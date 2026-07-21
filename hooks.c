/*
 * hooks.c - wrappers installed over VshCtrl's XMB IO handlers
 *
 * Flow mirrors ARK-4's core/vshctrl/xmbiso.c, but ISO games are served
 * from the persistent cache instead of being re-parsed per boot.
 * Everything that is not ours is passed through to the original
 * handlers untouched (real game folders, videos, savedata, GAME150...).
 *
 * Portions derived from ARK-4 / PRO CFW, GPL-3.0-or-later.
 */
#include <string.h>
#include "gamecache.h"
#include "piso.h"

/* per-directory listing state */
typedef struct {
    int active;
    SceUID fd;                  /* fd the XMB uses for this game dir */
    SceUID fake_iso_fd;         /* fake /ISO dfd handed to VshCtrl (or -1) */
    int cnt, pos;
    u32 *emit;                  /* combined ids to serve after real entries */
    SceUID emit_id;
    char gamedir[136];          /* the real dir path, for MRU lookups */
} DirTrack;

static DirTrack g_dirs[GC_MAX_DIRS];

/* open virtual EBOOT.PBP descriptors */
typedef struct {
    int used;
    u32 comb;
    u32 gen;                    /* entry generation at open time */
    u32 fp;
} OpenTrack;

static OpenTrack g_open[GC_MAX_OPEN];

/* XMB delete flow state (mirrors xmbiso.c's g_iso_dir/g_temp_delete_dir) */
static u32 g_del_comb = (u32)-1;
static char g_tempdir[128];
static int g_injected_a = 0, g_injected_b = 0;

volatile int g_in_gamedopen = 0;

void release_fake_isodfd(SceUID fd);    /* main.c */

/* ---------------- path classification ---------------- */

static const char *find_marker(const char *path)
{
    const char *p;
    if (path == NULL)
        return NULL;
    p = strchr(path, '/');
    if (p == NULL || p <= path + 1 || p[-1] != ':')
        return NULL;
    return strstr(p, GC_MARKER);
}

static int is_our_path(const char *path)
{
    return find_marker(path) != NULL;
}

static u32 parse_comb(const char *path)
{
    const char *m = find_marker(path);
    if (m == NULL)
        return (u32)-1;
    return x_parsehex8(m + GC_MARKER_LEN);
}

static int is_our_dir(const char *path)
{
    const char *m = find_marker(path);
    if (m == NULL)
        return 0;
    if (x_parsehex8(m + GC_MARKER_LEN) == (u32)-1)
        return 0;
    m += GC_MARKER_LEN + 8;
    while (*m == '/')
        m++;
    return *m == '\0';
}

static int is_our_file(const char *path, const char *file)
{
    const char *m = find_marker(path);
    if (m == NULL)
        return 0;
    if (x_parsehex8(m + GC_MARKER_LEN) == (u32)-1)
        return 0;
    return 0 == strcmp(m + GC_MARKER_LEN + 8, file);
}

/* game-list dir? (mirrors xmbiso.c is_game_dir) */
static int rep_is_game_dir(const char *dirname)
{
    static const char *game_files[] = { "/EBOOT.PBP", "/PBOOT.PBP", "/PARAM.PBP" };
    char path[256];
    SceIoStat st;
    const char *p;
    int i;
    u32 k1;

    p = strchr(dirname, '/');
    if (p == NULL)
        return 0;
    if (0 != strncmp(p, "/PSP/GAME", 9))
        return 0;
    if (0 == strncmp(p, "/PSP/GAME/_DEL_", 15))
        return 0;
    if (strlen(dirname) > 200)
        return 0;

    k1 = pspSdkSetK1(0);
    for (i = 0; i < 3; i++) {
        strcpy(path, dirname);
        strcat(path, game_files[i]);
        if (0 == sceIoGetstat(path, &st)) {
            pspSdkSetK1(k1);
            return 0;               /* it's a single game folder */
        }
    }
    pspSdkSetK1(k1);
    return 1;
}

/* "ms0:/PSP/GAME<sub>" -> "ms0:/ISO<sub>" */
static int make_iso_path(const char *dirname, char *out, int outlen)
{
    const char *p = strchr(dirname, '/');
    int devlen;
    if (p == NULL || p[-1] != ':')
        return -1;
    devlen = p - dirname;
    if (devlen + 4 >= outlen)
        return -1;
    memcpy(out, dirname, devlen);
    out[devlen] = '\0';
    strcat(out, "/ISO");
    p = strstr(dirname, "/PSP/GAME");
    if (p != NULL) {
        p += sizeof("/PSP/GAME") - 1;
        if ((int)(strlen(out) + strlen(p)) >= outlen)
            return -1;
        strcat(out, p);
    }
    return 0;
}

/* rewrite ".../@XGC@XXXXXXXX/FILE" -> ".../<DISCID>/FILE" in place
 * (mirrors vpbp_fixisopath; used for DOCUMENT.DAT / PBOOT.PBP / PARAM.PBP) */
static void fix_our_path(char *path)
{
    char fnbuf[64], discid[16];
    char *tmp, *filename;
    CacheEntry *e;
    u32 comb = parse_comb(path);

    gc_lock();
    e = gc_entry(comb);
    if (e == NULL) {
        gc_unlock();
        return;
    }
    memset(discid, 0, sizeof(discid));
    memcpy(discid, e->disc_id, sizeof(e->disc_id));
    discid[sizeof(discid) - 1] = '\0';
    gc_unlock();
    if (discid[0] == '\0')
        return;

    tmp = strrchr(path, '/');
    if (tmp == NULL)
        return;
    filename = tmp + 1;
    if (strlen(filename) >= sizeof(fnbuf))
        return;
    strcpy(fnbuf, filename);
    *tmp = '\0';
    tmp = strrchr(path, '/');
    if (tmp == NULL)
        return;
    strcpy(tmp + 1, discid);
    strcat(tmp + 1, "/");
    strcat(tmp + 1, fnbuf);
}

/* ---------------- directory handlers ---------------- */

static DirTrack *dir_find(SceUID fd)
{
    int i;
    for (i = 0; i < GC_MAX_DIRS; i++) {
        if (g_dirs[i].active && g_dirs[i].fd == fd)
            return &g_dirs[i];
    }
    return NULL;
}

SceUID my_gamedopen(const char *dirname)
{
    char isopath[128];
    SceUID fd;
    u32 k1;
    int i;

    if (is_our_dir(dirname)) {
        g_del_comb = parse_comb(dirname);
        g_injected_a = 0;
        return GC_DFD_DEL_A;
    }
    if (g_tempdir[0] != '\0' && 0 == strcmp(dirname, g_tempdir)) {
        g_injected_b = 0;
        return GC_DFD_DEL_B;
    }
    if (is_our_path(dirname))
        return org_gamedopen(dirname);

    if (!rep_is_game_dir(dirname) || make_iso_path(dirname, isopath, sizeof(isopath)) < 0)
        return org_gamedopen(dirname);

    /* Game Categories Lite relies on VshCtrl's own /ISO enumeration
     * (subfolders as categories); with it installed, fall back to stock
     * behavior rather than hide categorized games */
    {
        u32 k1c = pspSdkSetK1(0);
        void *gcl = sceKernelFindModuleByName("Game_Categories_Light");
        pspSdkSetK1(k1c);
        if (gcl != NULL)
            return org_gamedopen(dirname);
    }

    k1 = pspSdkSetK1(0);
    gc_lock();

    extern SceUID g_last_fake_fd;
    extern char g_expect_isopath[128];
    g_last_fake_fd = -1;
    strcpy(g_expect_isopath, isopath);
    g_in_gamedopen = 1;
    fd = org_gamedopen(dirname);
    g_in_gamedopen = 0;
    g_expect_isopath[0] = '\0';

    if (fd >= 0) {
        DirTrack *t = NULL;
        for (i = 0; i < GC_MAX_DIRS; i++) {
            if (!g_dirs[i].active) {
                t = &g_dirs[i];
                break;
            }
        }
        if (t == NULL)
            t = &g_dirs[0];         /* shouldn't happen; recycle */
        if (t->emit == NULL)
            t->emit = gc_alloc(GC_MAX_EMIT * sizeof(u32), &t->emit_id);
        t->fd = fd;
        t->fake_iso_fd = g_last_fake_fd;
        t->pos = 0;
        t->cnt = 0;
        strncpy(t->gamedir, dirname, sizeof(t->gamedir) - 1);
        t->gamedir[sizeof(t->gamedir) - 1] = '\0';
        mru_load();
        if (t->emit != NULL) {
            int cnt = gc_scan_dir(isopath, t->emit, GC_MAX_EMIT);
            t->cnt = (cnt > 0) ? cnt : 0;
        }
        t->active = 1;
    }

    gc_unlock();
    pspSdkSetK1(k1);
    return fd;
}

int my_gamedread(SceUID fd, SceIoDirent *dir)
{
    DirTrack *t;
    int result;
    u32 k1;

    if (fd == GC_DFD_DEL_A || fd == GC_DFD_DEL_B) {
        int *injected = (fd == GC_DFD_DEL_A) ? &g_injected_a : &g_injected_b;
        if (*injected)
            return 0;
        k1 = pspSdkSetK1(0);
        gc_lock();
        CacheEntry *e = gc_entry(g_del_comb);
        memset(dir, 0, sizeof(*dir));
        if (e != NULL) {
            dir->d_stat.st_mode = 0x21FF;
            dir->d_stat.st_attr = 0x20;
            dir->d_stat.st_size = e->iso_size;
            memcpy(&dir->d_stat.sce_st_ctime, &e->ctime, sizeof(ScePspDateTime));
            memcpy(&dir->d_stat.sce_st_mtime, &e->ctime, sizeof(ScePspDateTime));
            memcpy(&dir->d_stat.sce_st_atime, &e->ctime, sizeof(ScePspDateTime));
        }
        strcpy(dir->d_name, (fd == GC_DFD_DEL_A) ? "EBOOT.PBP" : "_EBOOT.PBP");
        gc_unlock();
        pspSdkSetK1(k1);
        *injected = 1;
        return 1;
    }

    t = dir_find(fd);
    if (t == NULL)
        return org_gamedread(fd, dir);

    k1 = pspSdkSetK1(0);
    result = org_gamedread(fd, dir);
    if (result > 0) {
        /* real entry: if this folder was launched recently, hand the XMB
         * a fake "newest" date so it sorts to the top */
        if (dir->d_name[0] != '\0' && dir->d_name[0] != '.' &&
            strchr(dir->d_name, '%') == NULL) {
            char full[288];
            if (strlen(t->gamedir) + 1 + strlen(dir->d_name) < sizeof(full)) {
                int r;
                strcpy(full, t->gamedir);
                strcat(full, "/");
                strcat(full, dir->d_name);
                gc_lock();
                r = mru_rank(full);
                gc_unlock();
                if (r >= 0) {
                    mru_faketime(r, &dir->d_stat.sce_st_ctime);
                    memcpy(&dir->d_stat.sce_st_mtime, &dir->d_stat.sce_st_ctime,
                           sizeof(ScePspDateTime));
                    memcpy(&dir->d_stat.sce_st_atime, &dir->d_stat.sce_st_ctime,
                           sizeof(ScePspDateTime));
                }
            }
        }
        pspSdkSetK1(k1);
        return result;
    }

    /* real entries exhausted: serve cached ISO games */
    gc_lock();
    while (t->pos < t->cnt) {
        u32 comb = t->emit[t->pos++];
        CacheEntry *e = gc_entry(comb);
        if (e == NULL)
            continue;               /* deleted during this session */
        memset(dir, 0, sizeof(*dir));
        gc_make_name(dir->d_name, comb);
        dir->d_stat.st_mode = 0x11FF;
        dir->d_stat.st_attr = 0x10;
        dir->d_stat.st_size = e->iso_size;
        memcpy(&dir->d_stat.sce_st_ctime, &e->ctime, sizeof(ScePspDateTime));
        memcpy(&dir->d_stat.sce_st_mtime, &e->ctime, sizeof(ScePspDateTime));
        memcpy(&dir->d_stat.sce_st_atime, &e->ctime, sizeof(ScePspDateTime));
        {
            int r = mru_rank(e->path);
            if (r >= 0) {
                mru_faketime(r, &dir->d_stat.sce_st_ctime);
                memcpy(&dir->d_stat.sce_st_mtime, &dir->d_stat.sce_st_ctime,
                       sizeof(ScePspDateTime));
                memcpy(&dir->d_stat.sce_st_atime, &dir->d_stat.sce_st_ctime,
                       sizeof(ScePspDateTime));
            }
        }
        gc_unlock();
        pspSdkSetK1(k1);
        return 1;
    }
    gc_unlock();
    pspSdkSetK1(k1);
    return result;
}

int my_gamedclose(SceUID fd)
{
    DirTrack *t;
    int result;

    if (fd == GC_DFD_DEL_A || fd == GC_DFD_DEL_B) {
        if (fd == GC_DFD_DEL_A)
            g_injected_a = 0;
        else
            g_injected_b = 0;
        return 0;
    }

    t = dir_find(fd);
    if (t == NULL)
        return org_gamedclose(fd);

    {
        u32 k1 = pspSdkSetK1(0);
        gc_lock();
        result = org_gamedclose(fd);
        if (t->fake_iso_fd >= 0)
            release_fake_isodfd(t->fake_iso_fd);
        t->active = 0;
        gc_flush_all();
        gc_unlock();
        pspSdkSetK1(k1);
    }
    return result;
}

/* ---------------- file handlers ---------------- */

SceUID my_gameopen(const char *file, int flags, SceMode mode)
{
    if (is_our_file(file, "/EBOOT.PBP")) {
        int i;
        u32 comb = parse_comb(file);
        u32 k1 = pspSdkSetK1(0);
        gc_lock();
        if ((flags & (PSP_O_WRONLY | PSP_O_TRUNC | PSP_O_CREAT)) || !(flags & PSP_O_RDONLY)) {
            gc_unlock();
            pspSdkSetK1(k1);
            return -6;
        }
        CacheEntry *e = gc_entry(comb);
        if (e == NULL || gc_ensure_built(comb) < 0) {
            gc_unlock();
            pspSdkSetK1(k1);
            return -12;
        }
        for (i = 0; i < GC_MAX_OPEN; i++) {
            if (!g_open[i].used) {
                g_open[i].used = 1;
                g_open[i].comb = comb;
                g_open[i].gen = e->reserved[0];
                g_open[i].fp = 0;
                gc_unlock();
                pspSdkSetK1(k1);
                return GC_FD_BASE + i;
            }
        }
        gc_unlock();
        pspSdkSetK1(k1);
        return -26;
    }

    if (is_our_file(file, "/DOCUMENT.DAT") || is_our_file(file, "/PBOOT.PBP") ||
        is_our_file(file, "/PARAM.PBP")) {
        fix_our_path((char *)file);
    }
    return org_gameopen(file, flags, mode);
}

static OpenTrack *open_find(SceUID fd)
{
    int i = fd - GC_FD_BASE;
    if (i < 0 || i >= GC_MAX_OPEN || !g_open[i].used)
        return NULL;
    return &g_open[i];
}

int my_gameread(SceUID fd, void *data, SceSize size)
{
    OpenTrack *o = open_find(fd);
    int ret;
    u32 k1;

    if (o == NULL)
        return org_gameread(fd, data, size);

    k1 = pspSdkSetK1(0);
    gc_lock();
    ret = gc_read(o->comb, o->gen, &o->fp, data, size);
    gc_unlock();
    pspSdkSetK1(k1);
    return ret;
}

SceOff my_gamelseek(SceUID fd, SceOff offset, int whence)
{
    OpenTrack *o = open_find(fd);
    CacheEntry *e;
    u32 k1;

    if (o == NULL)
        return org_gamelseek(fd, offset, whence);

    k1 = pspSdkSetK1(0);
    gc_lock();
    e = gc_entry(o->comb);
    if (e == NULL || e->reserved[0] != o->gen) {
        gc_unlock();
        pspSdkSetK1(k1);
        return -3;
    }
    switch (whence) {
    case PSP_SEEK_SET:
        o->fp = (u32)offset;
        break;
    case PSP_SEEK_CUR:
        o->fp += (int)offset;
        break;
    case PSP_SEEK_END:
        o->fp = e->pbp_total_size + (int)offset;
        break;
    }
    gc_unlock();
    pspSdkSetK1(k1);
    return o->fp;
}

int my_gameclose(SceUID fd)
{
    OpenTrack *o = open_find(fd);
    if (o == NULL)
        return org_gameclose(fd);
    gc_lock();
    o->used = 0;
    gc_unlock();
    return 0;
}

int my_gamegetstat(const char *file, SceIoStat *stat)
{
    if (is_our_file(file, "/EBOOT.PBP") || is_our_dir(file)) {
        int isdir = is_our_dir(file);
        u32 comb = parse_comb(file);
        u32 k1 = pspSdkSetK1(0);
        gc_lock();
        CacheEntry *e = gc_entry(comb);
        if (e == NULL) {
            gc_unlock();
            pspSdkSetK1(k1);
            return -30;
        }
        memset(stat, 0, sizeof(*stat));
        stat->st_mode = isdir ? 0x11FF : 0x21FF;
        stat->st_attr = isdir ? 0x10 : 0x20;
        stat->st_size = e->iso_size;
        memcpy(&stat->sce_st_ctime, &e->ctime, sizeof(ScePspDateTime));
        memcpy(&stat->sce_st_mtime, &e->ctime, sizeof(ScePspDateTime));
        memcpy(&stat->sce_st_atime, &e->ctime, sizeof(ScePspDateTime));
        gc_unlock();
        pspSdkSetK1(k1);
        return 0;
    }

    if (is_our_file(file, "/DOCUMENT.DAT") || is_our_file(file, "/PBOOT.PBP") ||
        is_our_file(file, "/PARAM.PBP")) {
        fix_our_path((char *)file);
    }
    return org_gamegetstat(file, stat);
}

/* ---------------- delete flow ---------------- */

int my_gamerename(const char *oldname, const char *newname)
{
    if (is_our_dir(oldname)) {
        g_del_comb = parse_comb(oldname);
        strncpy(g_tempdir, newname, sizeof(g_tempdir) - 1);
        g_tempdir[sizeof(g_tempdir) - 1] = '\0';
        return 0;
    }
    if (g_tempdir[0] != '\0' && 0 == strncmp(oldname, g_tempdir, strlen(g_tempdir)))
        return 0;
    return org_gamerename(oldname, newname);
}

int my_gameremove(const char *file)
{
    if (g_tempdir[0] != '\0' && 0 == strncmp(file, g_tempdir, strlen(g_tempdir)))
        return 0;
    return org_gameremove(file);
}

int my_gamermdir(const char *path)
{
    if (g_tempdir[0] != '\0' && 0 == strcmp(path, g_tempdir)) {
        int result;
        u32 k1 = pspSdkSetK1(0);
        gc_lock();
        result = gc_delete(g_del_comb);
        gc_unlock();
        pspSdkSetK1(k1);
        g_del_comb = (u32)-1;
        g_tempdir[0] = '\0';
        return result;
    }
    return org_gamermdir(path);
}

int my_gamechstat(const char *file, SceIoStat *stat, int bits)
{
    if (g_tempdir[0] != '\0' && 0 == strncmp(file, g_tempdir, strlen(g_tempdir)))
        return 0;
    return org_gamechstat(file, stat, bits);
}

/* ---------------- game launch ---------------- */

/* does the ISO carry a Prometheus module? (private-reader port of
 * VshCtrl's has_prometheus_module) */
static int prom_check(const char *isopath)
{
    u32 size, lba;
    int ret;
    if (piso_open(isopath) < 0)
        return 0;
    ret = (piso_getfileinfo("/PSP_GAME/SYSDIR/EBOOT.OLD", &size, &lba) >= 0);
    piso_close();
    return ret;
}

/* is there a PBOOT.PBP update for this game? (port of has_update_file /
 * vpbp_gameid: game id lives at ISO offset 0x8373, "ULUS-01234" form) */
static int update_check(const char *isopath, char *update_file)
{
    static const char *devs[2] = { "ms0:", "ef0:" };
    char game_id[10];
    SceIoStat st;
    int i;

    if (piso_open(isopath) < 0)
        return 0;
    i = piso_read(game_id, 16, 883, 10);
    piso_close();
    if (i < 10)
        return 0;
    game_id[4] = game_id[5];
    game_id[5] = game_id[6];
    game_id[6] = game_id[7];
    game_id[7] = game_id[8];
    game_id[8] = game_id[9];
    game_id[9] = '\0';

    for (i = 0; i < 2; i++) {
        strcpy(update_file, devs[i]);
        strcat(update_file, "/PSP/GAME/");
        strcat(update_file, game_id);
        strcat(update_file, "/PBOOT.PBP");
        if (sceIoGetstat(update_file, &st) >= 0)
            return 1;
    }
    return 0;
}

/* record a real (non-ISO) game launch: strip the file name, keep the
 * game folder path.  Only EBOOT.PBP launches under PSP/GAME count
 * (skips PBOOT updates, updaters, ISO paths, discs). */
static void record_real_launch(const char *file)
{
    char folder[128];
    const char *p;
    int len;
    u32 k1;

    if (file == NULL || strstr(file, "/PSP/GAME") == NULL)
        return;
    p = strrchr(file, '/');
    if (p == NULL || 0 != strcmp(p, "/EBOOT.PBP"))
        return;
    len = p - file;
    if (len <= 0 || len >= (int)sizeof(folder))
        return;
    memcpy(folder, file, len);
    folder[len] = '\0';
    k1 = pspSdkSetK1(0);
    gc_lock();
    mru_record(folder);
    gc_unlock();
    pspSdkSetK1(k1);
}

int my_homebrewloadexec(char *file, struct SceKernelLoadExecVSHParam *param)
{
    record_real_launch(file);
    return org_homebrewloadexec(file, param);
}

/* Every VSH launch — including PS1/POPS, which uses a stock bridge path
 * that no other hook sees — funnels through SystemControl's
 * sctrlKernelLoadExecVSHWithApitype (ARK hijacks the firmware internals
 * to guarantee this).  We trampoline that function to record launches.
 * NOTE: my_umdemuloadexec must NOT hold gc_lock when it launches, or
 * this re-entry would deadlock (it releases the lock before launching). */
int (*org_vsh_loadexec_apitype)(int apitype, const char *file,
                                struct SceKernelLoadExecVSHParam *param) = NULL;

int my_vsh_loadexec_apitype(int apitype, const char *file,
                            struct SceKernelLoadExecVSHParam *param)
{
    record_real_launch(file);
    return org_vsh_loadexec_apitype(apitype, file, param);
}

int my_umdemuloadexec(char *file, struct SceKernelLoadExecVSHParam *param)
{
    static char pboot_path[256];
    static char iso_path[128];
    u32 opnssmp;
    CacheEntry *e;
    const char *loadexec_file;
    int apitype, ret;
    int has_prom, has_pboot;
    u32 k1;
    u32 comb;

    if (!is_our_file(file, "/EBOOT.PBP")) {
        record_real_launch(file);
        return org_umdemuloadexec(file, param);
    }

    comb = parse_comb(file);
    k1 = pspSdkSetK1(0);
    gc_lock();
    gc_ensure_built(comb);          /* for opnssmp; launch proceeds regardless */
    e = gc_entry(comb);
    if (e == NULL) {
        gc_unlock();
        pspSdkSetK1(k1);
        return -31;
    }
    strcpy(iso_path, e->path);
    opnssmp = e->opnssmp_type;
    has_prom = prom_check(iso_path);
    has_pboot = update_check(iso_path, pboot_path);
    gc_aff_close();
    mru_record(iso_path);
    gc_flush_all();
    gc_unlock();
    gc_log("loadexec", iso_path, comb);

    /* fix vsh args (mirrors vpbp_loadexec) */
    {
        u32 *vshargp = (u32 *)param->vshmain_args;
        int vshargs = param->vshmain_args_size;
        if (vshargp) {
            memset(vshargp, 0, vshargs);
            vshargp[0] = vshargs;
            vshargp[1] = 0x20;
            vshargp[16] = 1;
        }
    }
    /* fix sfo title */
    {
        u32 *sfo_field = (u32 *)((u32)param + 36);
        u8 *sfo = (u8 *)(sfo_field[1]);
        if (sfo)
            sfo[0x24] = 0;
    }

    sctrlSESetUmdFile(iso_path);
    sctrlSESetDiscType(GC_PSP_UMD_TYPE_GAME);
    sctrlSESetBootConfFileIndex(MODE_INFERNO);

    if (opnssmp) {
        u32 *info = (u32 *)sceKernelGetGameInfo();
        if (info)
            info[216 / 4] = opnssmp;
    }

    param->key = "umdemu";
    apitype = ISO_RUNLEVEL;

    if (has_pboot) {
        apitype = ISO_PBOOT_RUNLEVEL;
        param->argp = pboot_path;
        param->args = strlen(pboot_path) + 1;
        loadexec_file = pboot_path;
        if (0 == strncmp(pboot_path, "ef0", 3))
            apitype = ISO_PBOOT_RUNLEVEL_GO;
    } else {
        loadexec_file = iso_path;
        if (0 == strncmp(iso_path, "ef0", 3))
            apitype = ISO_RUNLEVEL_GO;
        if (has_prom)
            param->argp = "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD";
        else
            param->argp = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        param->args = 33;
    }

    ret = sctrlKernelLoadExecVSHWithApitype(apitype, (char *)loadexec_file, param);

    pspSdkSetK1(k1);
    return ret;
}
