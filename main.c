/*
 * xmb_gamecache - persistent XMB game-list cache for ARK-4
 *
 * module_start resolves VshCtrl's exported XMB IO handlers and patches
 * the syscall table entries that point at them, so our wrappers run
 * first and can serve ISO games from the persistent cache.  VshCtrl's
 * own view of the /ISO directory is faked empty (via import hooks on
 * its kernel IO), so its slow per-ISO parser never runs.
 *
 * GPL-3.0-or-later.  Portions derived from ARK-4 / PRO CFW.
 */
#include <string.h>
#include "gamecache.h"

PSP_MODULE_INFO("XMB_GameCache", 0x1000, 1, 1);

/* original VshCtrl handlers */
SceUID (*org_gamedopen)(const char *dirname) = NULL;
int    (*org_gamedread)(SceUID fd, SceIoDirent *dir) = NULL;
int    (*org_gamedclose)(SceUID fd) = NULL;
SceUID (*org_gameopen)(const char *file, int flags, SceMode mode) = NULL;
int    (*org_gameread)(SceUID fd, void *data, SceSize size) = NULL;
int    (*org_gameclose)(SceUID fd) = NULL;
SceOff (*org_gamelseek)(SceUID fd, SceOff offset, int whence) = NULL;
int    (*org_gamegetstat)(const char *file, SceIoStat *stat) = NULL;
int    (*org_gameremove)(const char *file) = NULL;
int    (*org_gamermdir)(const char *path) = NULL;
int    (*org_gamerename)(const char *oldname, const char *newname) = NULL;
int    (*org_gamechstat)(const char *file, SceIoStat *stat, int bits) = NULL;
int    (*org_umdemuloadexec)(char *file, struct SceKernelLoadExecVSHParam *param) = NULL;
int    (*org_homebrewloadexec)(char *file, struct SceKernelLoadExecVSHParam *param) = NULL;

/* raw (unpatched) kernel directory IO */
SceUID (*k_dopen)(const char *dirname) = NULL;
int    (*k_dread)(SceUID fd, SceIoDirent *dir) = NULL;
int    (*k_dclose)(SceUID fd) = NULL;

static STMOD_HANDLER g_previous = NULL;

/* ------------------------------------------------------------------ */
/* fake (empty) /ISO directory descriptors handed to VshCtrl           */
/* ------------------------------------------------------------------ */

SceUID g_last_fake_fd = -1;
static SceUID g_fake_fds[GC_MAX_ISODFD];
static u32 g_fake_seq = 0;

static int fake_find(SceUID fd)
{
    int i;
    if ((u32)fd < GC_ISODFD_BASE || (u32)fd > GC_ISODFD_BASE + 0xFFFF)
        return -1;
    for (i = 0; i < GC_MAX_ISODFD; i++) {
        if (g_fake_fds[i] == fd)
            return i;
    }
    return -1;
}

void release_fake_isodfd(SceUID fd)
{
    int i = fake_find(fd);
    if (i >= 0)
        g_fake_fds[i] = -1;
}

/* exact /ISO path my_gamedopen expects VshCtrl to open; only that one
 * gets faked (never e.g. /ISO/VIDEO from the video virtualization) */
char g_expect_isopath[128];

static SceUID hook_kDopen(const char *dirname)
{
    if (g_in_gamedopen && dirname != NULL && g_expect_isopath[0] != '\0' &&
        0 == strcmp(dirname, g_expect_isopath)) {
        int i;
        for (i = 0; i < GC_MAX_ISODFD; i++) {
            if (g_fake_fds[i] < 0) {
                SceUID fd = GC_ISODFD_BASE + (g_fake_seq++ & 0xFFFF);
                g_fake_fds[i] = fd;
                g_last_fake_fd = fd;
                return fd;
            }
        }
        /* no free slot: recycle the first (stale handles read as empty) */
        g_fake_fds[0] = GC_ISODFD_BASE + (g_fake_seq++ & 0xFFFF);
        g_last_fake_fd = g_fake_fds[0];
        return g_fake_fds[0];
    }
    return k_dopen(dirname);
}

static int hook_kDread(SceUID fd, SceIoDirent *dir)
{
    if (fake_find(fd) >= 0)
        return 0;                   /* always empty */
    return k_dread(fd, dir);
}

static int hook_kDclose(SceUID fd)
{
    if (fake_find(fd) >= 0) {
        release_fake_isodfd(fd);
        return 0;
    }
    return k_dclose(fd);
}

/* ------------------------------------------------------------------ */
/* launch hook                                                         */
/*                                                                     */
/* VshCtrl hooks vsh_module's LoadExec imports to its umdemuloadexec   */
/* via sceKernelQuerySystemCall (it's a 0x4001 export, so it has a     */
/* syscall table entry).  We take over by patching that syscall table  */
/* entry to point at my_umdemuloadexec instead — but only AFTER        */
/* VshCtrl has done its QuerySystemCall (patching earlier would make   */
/* its lookup fail and break the launch path entirely).                */
/* ------------------------------------------------------------------ */

static int g_loadexec_patched = 0;

static void patch_loadexec_syscall(void)
{
    if (g_loadexec_patched)
        return;
    g_loadexec_patched = 1;
    sctrlHENPatchSyscall((void *)org_umdemuloadexec, (void *)my_umdemuloadexec);
    sctrlHENPatchSyscall((void *)org_homebrewloadexec, (void *)my_homebrewloadexec);
    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();
}

static int stmod_handler(SceModule2 *mod)
{
    int ret = 0;
    if (g_previous)
        ret = g_previous(mod);      /* let VshCtrl install its hooks first */
    if (0 == strcmp(mod->modname, "vsh_module"))
        patch_loadexec_syscall();
    return ret;
}

/* ------------------------------------------------------------------ */
/* instant free-space display                                          */
/*                                                                     */
/* The FAT driver re-counts every free cluster on every free-space     */
/* devctl — nothing caches it — so on a big card the XMB's query       */
/* stalls for seconds every time it asks, most visibly after           */
/* suspend/resume.  We hook the user sceIoDevctl syscall: the first    */
/* query per device after boot passes through (slow, but folded into   */
/* the boot the user already waits for) and its result is remembered;  */
/* every later query this session — including after resume — is        */
/* answered instantly from that snapshot.  The number can drift if     */
/* games are added/deleted mid-session; it self-corrects on the next   */
/* boot, and every game launch reboots the VSH anyway.  Safety floor:  */
/* once the last known free space drops below GC_FAKE_MIN_FREE the     */
/* XMB always gets the real count, so a nearly-full card is never      */
/* misreported.                                                        */
/* ------------------------------------------------------------------ */

#define DEVCTL_GET_FREE_CLUSTERS  0x02425818
#define NID_sceIoDevctl           0x54F5FB11
#define GC_FAKE_MIN_FREE          (1024ULL * 1024 * 1024)   /* 1 GB */

typedef struct {
    u32 max_clusters, free_clusters, max_sectors, sector_size, sector_count;
} MsSpaceInfo;

static struct {
    int valid;
    MsSpaceInfo info;
} g_space[2];                       /* [0] = ms0:, [1] = ef0: */

static int (*org_iodevctl)(const char *dev, unsigned int cmd, void *indata,
                           int inlen, void *outdata, int outlen) = NULL;

static int space_dev_index(const char *dev)
{
    if (dev == NULL)
        return -1;
    if (dev[0] == 'm' && dev[1] == 's' && dev[2] == '0')
        return 0;
    if (dev[0] == 'e' && dev[1] == 'f' && dev[2] == '0')
        return 1;
    return -1;
}

static u64 space_free_bytes(const MsSpaceInfo *info)
{
    return (u64)info->free_clusters * info->sector_size * info->sector_count;
}

/*
 * Total capacity of a device in bytes, 0 if not known yet.  Read from
 * the snapshot the free-space hook already collected, never by issuing
 * a fresh devctl: that call re-counts every free cluster on the card
 * and is precisely the stall this plugin exists to remove.
 *
 * Caller must hold the global lock (gcache.c calls this from paths
 * that already do), which is what serialises it against the writes in
 * my_iodevctl below.
 */
u64 gc_dev_capacity(const char *dev)
{
    int i = space_dev_index(dev);

    if (i < 0 || !g_space[i].valid)
        return 0;
    return (u64)g_space[i].info.max_clusters *
           g_space[i].info.sector_size * g_space[i].info.sector_count;
}

int my_iodevctl(const char *dev, unsigned int cmd, void *indata, int inlen,
                void *outdata, int outlen)
{
    int i, ret, served;
    MsSpaceInfo *user_info;
    u32 k1;

    if (cmd != DEVCTL_GET_FREE_CLUSTERS)
        return org_iodevctl(dev, cmd, indata, inlen, outdata, outlen);
    i = space_dev_index(dev);
    if (i < 0 || indata == NULL || inlen < 4)
        return org_iodevctl(dev, cmd, indata, inlen, outdata, outlen);

    /* indata is a pointer to a pointer to the result struct */
    user_info = *(MsSpaceInfo **)indata;
    if (user_info == NULL)
        return org_iodevctl(dev, cmd, indata, inlen, outdata, outlen);

    k1 = pspSdkSetK1(0);

    served = 0;
    gc_lock();
    if (g_space[i].valid && space_free_bytes(&g_space[i].info) >= GC_FAKE_MIN_FREE) {
        memcpy(user_info, &g_space[i].info, sizeof(MsSpaceInfo));
        served = 1;
    }
    gc_unlock();
    if (served) {
        pspSdkSetK1(k1);
        return 0;
    }

    /* no snapshot yet, or space is low: real count, and remember it */
    ret = org_iodevctl(dev, cmd, indata, inlen, outdata, outlen);
    if (ret >= 0) {
        gc_lock();
        memcpy(&g_space[i].info, user_info, sizeof(MsSpaceInfo));
        g_space[i].valid = 1;
        gc_unlock();
    }
    pspSdkSetK1(k1);
    return ret;
}

/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int nid;
    void *hook;
    void **org;
} HookEnt;

int module_start(SceSize args, void *argp)
{
    int i;

    static HookEnt hooks[] = {
        { NID_gamedopen,   (void *)my_gamedopen,   (void **)&org_gamedopen },
        { NID_gamedread,   (void *)my_gamedread,   (void **)&org_gamedread },
        { NID_gamedclose,  (void *)my_gamedclose,  (void **)&org_gamedclose },
        { NID_gameopen,    (void *)my_gameopen,    (void **)&org_gameopen },
        { NID_gameread,    (void *)my_gameread,    (void **)&org_gameread },
        { NID_gameclose,   (void *)my_gameclose,   (void **)&org_gameclose },
        { NID_gamelseek,   (void *)my_gamelseek,   (void **)&org_gamelseek },
        { NID_gamegetstat, (void *)my_gamegetstat, (void **)&org_gamegetstat },
        { NID_gameremove,  (void *)my_gameremove,  (void **)&org_gameremove },
        { NID_gamermdir,   (void *)my_gamermdir,   (void **)&org_gamermdir },
        { NID_gamerename,  (void *)my_gamerename,  (void **)&org_gamerename },
        { NID_gamechstat,  (void *)my_gamechstat,  (void **)&org_gamechstat },
    };

    for (i = 0; i < GC_MAX_ISODFD; i++)
        g_fake_fds[i] = -1;

    /* resolve every original handler; if anything is missing (not ARK-4,
     * or an incompatible version), stay completely inert */
    for (i = 0; i < 12; i++) {
        *hooks[i].org = (void *)sctrlHENFindFunction("VshCtrl", "xmbiso", hooks[i].nid);
        if (*hooks[i].org == NULL)
            return 0;
    }
    org_umdemuloadexec = (void *)sctrlHENFindFunction("VshCtrl", "xmbiso", NID_umdemuloadexec);
    org_homebrewloadexec = (void *)sctrlHENFindFunction("VshCtrl", "xmbiso", NID_homebrewloadexec);
    if (org_umdemuloadexec == NULL || org_homebrewloadexec == NULL) {
        gc_log("init: xmbiso exports missing, inert", NULL, 0);
        return 0;
    }

    k_dopen  = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", 0xB29DDF9C);
    k_dread  = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", 0xE3EB004C);
    k_dclose = (void *)sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", 0xEB092469);
    if (k_dopen == NULL || k_dread == NULL || k_dclose == NULL) {
        gc_log("init: kernel io missing, inert", NULL, 0);
        return 0;
    }

    if (gc_init() < 0)
        return 0;

    /* fake out VshCtrl's view of the /ISO dir; if any hook fails, stay
     * inert (otherwise both VshCtrl and us would list the ISOs) */
    {
        SceModule2 *mod = (SceModule2 *)sceKernelFindModuleByName("VshCtrl");
        if (mod == NULL)
            return 0;
        if (sctrlHookImportByNID(mod, "IoFileMgrForKernel", 0xB29DDF9C, hook_kDopen) < 0 ||
            sctrlHookImportByNID(mod, "IoFileMgrForKernel", 0xE3EB004C, hook_kDread) < 0 ||
            sctrlHookImportByNID(mod, "IoFileMgrForKernel", 0xEB092469, hook_kDclose) < 0) {
            gc_log("init: import hook failed, inert", NULL, 0);
            return 0;
        }
    }

    /* take over the XMB IO syscalls (currently pointing at VshCtrl) */
    for (i = 0; i < 12; i++)
        sctrlHENPatchSyscall(*hooks[i].org, hooks[i].hook);

    /* catch vsh_module when it loads, to own the ISO launch path */
    g_previous = sctrlHENSetStartModuleHandler(stmod_handler);
    if (sceKernelFindModuleByName("vsh_module") != NULL)
        patch_loadexec_syscall();   /* already up: VshCtrl hooked it long ago */

    /* trampoline SystemControl's central launch function so PS1/POPS
     * launches (stock bridge path, invisible to every other hook) are
     * recorded for recently-played ordering (Davee's hijack technique,
     * as used by ARK itself) */
    {
        u32 a = sctrlHENFindFunction("SystemControl", "SystemCtrlForKernel", 0x2D10FB28);
        if (a != 0) {
            static u32 springboard[5];
            springboard[0] = *(volatile u32 *)(a);
            springboard[1] = *(volatile u32 *)(a + 4);
            springboard[2] = 0;                                     /* nop */
            springboard[3] = 0x08000000 | ((((u32)(a) + 8) & 0x0FFFFFFC) >> 2); /* j a+8 */
            springboard[4] = 0;                                     /* delay nop */
            *(volatile u32 *)(a) = 0x08000000 | ((((u32)my_vsh_loadexec_apitype) >> 2) & 0x03FFFFFF);
            *(volatile u32 *)(a + 4) = 0;                           /* delay nop */
            org_vsh_loadexec_apitype = (void *)springboard;
        } else {
            gc_log("init: launch funnel not found (no PS1 MRU)", NULL, 0);
        }
    }

    /* take over the user free-space devctl so the Memory Stick icon
     * shows instantly (kernel callers import the function directly and
     * bypass the syscall table, so they always get the real count) */
    org_iodevctl = (void *)sctrlHENFindFunction("sceIOFileManager",
                                                "IoFileMgrForUser",
                                                NID_sceIoDevctl);
    if (org_iodevctl != NULL)
        sctrlHENPatchSyscall((void *)org_iodevctl, (void *)my_iodevctl);
    else
        gc_log("init: IoFileMgrForUser devctl not found", NULL, 0);

    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();

    gc_log("init: hooks installed", NULL, 0);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    return 0;
}
