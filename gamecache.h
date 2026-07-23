/*
 * xmb_gamecache - persistent XMB game-list cache plugin for ARK-4
 *
 * Speeds up the XMB Game menu on large memory cards by caching, on the
 * memory card, the virtual EBOOT.PBP data (PARAM.SFO + ICON0.PNG etc.)
 * that ARK-4's VshCtrl normally re-extracts from every ISO on every boot.
 *
 * Portions derived from ARK-4 / PRO CFW (core/vshctrl), GPL-3.0-or-later.
 * This plugin is likewise licensed GPL-3.0-or-later.
 */
#ifndef GAMECACHE_H
#define GAMECACHE_H

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psploadcore.h>
#include <psputilsforkernel.h>
#include <psploadexec_kernel.h>
#include "module2.h"

/* ---- SystemControl / VshCtrl imports (resolved at module load) ---- */
typedef int (*STMOD_HANDLER)(SceModule2 *);

unsigned int sctrlHENFindFunction(const char *modname, const char *libname, unsigned int nid);
void sctrlHENPatchSyscall(void *addr, void *newaddr);
STMOD_HANDLER sctrlHENSetStartModuleHandler(STMOD_HANDLER new_handler);
int sctrlHookImportByNID(SceModule2 *pMod, const char *library, unsigned int nid, void *func);
int sctrlKernelLoadExecVSHWithApitype(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param);
void sctrlSESetUmdFile(const char *file);
void sctrlSESetDiscType(int type);
void sctrlSESetBootConfFileIndex(int index);
unsigned int sctrlHENGetVersion(void);
unsigned int sctrlHENGetMinorVersion(void);

/* ISO access goes through the plugin's own private reader (piso.h) —
 * never VshCtrl's shared global reader, which other VshCtrl threads
 * (video virtualization) use concurrently. */

/* sceKernelGetGameInfo comes from pspsysmem_kernel.h (weak stub import) */
#include <pspsysmem_kernel.h>

/* ---- NIDs of VshCtrl's "xmbiso" export lib (SHA1 name hashes) ---- */
#define NID_gamedopen      0xD286414F
#define NID_gamedread      0x3AD99572
#define NID_gamedclose     0x105A3208
#define NID_gameopen       0x5630DFED
#define NID_gameread       0xB1A1436E
#define NID_gameclose      0x3BA3B174
#define NID_gamelseek      0xBF1FEA83
#define NID_gamegetstat    0xB544667D
#define NID_gameremove     0x869838D4
#define NID_gamermdir      0x7C251E13
#define NID_gamerename     0x4E666201
#define NID_gamechstat     0xA6C20C6E
#define NID_umdemuloadexec 0xF3FF4453
#define NID_homebrewloadexec 0x77806967

/* ---- constants ---- */
#define GC_MARKER      "@XGC@"          /* virtual folder prefix shown to the XMB  */
#define GC_MARKER_LEN  5
#define GC_SFO_SIZE    408              /* size of the synthesized PARAM.SFO       */
#define GC_PBP_HDR     0x28

#define GC_FD_BASE     0x7C300000       /* virtual EBOOT.PBP file descriptors      */
#define GC_MAX_OPEN    16
#define GC_DFD_DEL_A   0x7C100000       /* magic dfd: browsing a virtual game dir  */
#define GC_DFD_DEL_B   0x7C100001       /* magic dfd: browsing the _DEL_ temp dir  */
#define GC_ISODFD_BASE 0x7C500000       /* fake (empty) /ISO dfds fed to VshCtrl   */
#define GC_MAX_ISODFD  8

#define GC_MAX_DIRS    8                /* concurrently tracked game-dir handles   */
#define GC_MAX_EMIT    2048             /* max ISOs listed per directory           */

#define ISO_RUNLEVEL           0x123
#define ISO_RUNLEVEL_GO        0x125
#define ISO_PBOOT_RUNLEVEL     0x124
#define ISO_PBOOT_RUNLEVEL_GO  0x126
#define MODE_INFERNO           3
#define GC_PSP_UMD_TYPE_GAME   0x10

/* ---- cache entry: one per ISO, stored verbatim in the cache file ---- */
typedef struct __attribute__((packed)) {
    u32 flags;                  /* bit0 = valid */
    u32 iso_size;
    u32 pbp_total_size;
    u32 opnssmp_type;
    char path[128];             /* full ISO path, e.g. ms0:/ISO/Game.iso */
    u32 header[10];             /* PBP header: magic, ver, section offsets */
    u32 sect_lba[8];            /* location of each section inside the ISO */
    u32 sect_size[8];           /* SFO slot holds GC_SFO_SIZE */
    u32 blob_off[8];            /* offset of cached bytes in cache file, 0 = uncached */
    ScePspDateTime ctime;
    ScePspDateTime mtime;
    char disc_id[12];
    u32 reserved[5];
} CacheEntry;

#define GC_ENTRY_VALID 1            /* path/size/times from dirent are good */
#define GC_ENTRY_BUILT 2            /* sections + SFO synthesized */
#define GC_ENTRY_BAD   4            /* build failed; don't retry this session */

/* combined id passed around as the hex part of the fake folder name */
#define GC_COMBINE(ctx, idx)   ((((u32)(ctx)) << 24) | ((u32)(idx) & 0xFFFFFF))
#define GC_CTX(comb)           (((comb) >> 24) & 0xFF)
#define GC_IDX(comb)           ((comb) & 0xFFFFFF)

/* ---- gcache.c API (caller must hold the global lock) ---- */
int  gc_scan_dir(const char *isopath, u32 *emit, int cap);
CacheEntry *gc_entry(u32 comb);
int  gc_ensure_built(u32 comb);
int  gc_read(u32 comb, u32 gen, u32 *fp, void *data, int size);
int  gc_delete(u32 comb);
void gc_make_name(char *out, u32 comb);
void gc_aff_close(void);
void gc_flush_all(void);

/* debug logging to <dev>:/XGC_LOG.TXT (compiled in when XGC_DEBUG=1) */
#define XGC_DEBUG 0
void gc_log(const char *a, const char *b, u32 v);

void gc_lock(void);
void gc_unlock(void);
int  gc_init(void);

/* main.c — card capacity in bytes from the free-space snapshot, 0 if
 * not sampled yet.  Call with the lock held; never issues a devctl. */
u64 gc_dev_capacity(const char *dev);

void *gc_alloc(u32 size, SceUID *id);
void gc_free(SceUID id);

/* small libc helpers (avoid depending on sprintf etc.) */
int  x_stricmp(const char *a, const char *b);
void x_hex8(char *out, u32 v);
u32  x_parsehex8(const char *s);

/* mru.c — recently-launched-first ordering (callers hold the lock) */
void mru_load(void);
int  mru_rank(const char *path);
void mru_record(const char *path);
void mru_faketime(int rank, ScePspDateTime *t);

/* original VshCtrl handlers, resolved in main.c */
extern SceUID (*org_gamedopen)(const char *dirname);
extern int    (*org_gamedread)(SceUID fd, SceIoDirent *dir);
extern int    (*org_gamedclose)(SceUID fd);
extern SceUID (*org_gameopen)(const char *file, int flags, SceMode mode);
extern int    (*org_gameread)(SceUID fd, void *data, SceSize size);
extern int    (*org_gameclose)(SceUID fd);
extern SceOff (*org_gamelseek)(SceUID fd, SceOff offset, int whence);
extern int    (*org_gamegetstat)(const char *file, SceIoStat *stat);
extern int    (*org_gameremove)(const char *file);
extern int    (*org_gamermdir)(const char *path);
extern int    (*org_gamerename)(const char *oldname, const char *newname);
extern int    (*org_gamechstat)(const char *file, SceIoStat *stat, int bits);
extern int    (*org_umdemuloadexec)(char *file, struct SceKernelLoadExecVSHParam *param);
extern int    (*org_homebrewloadexec)(char *file, struct SceKernelLoadExecVSHParam *param);

/* raw kernel IO (unpatched originals, resolved in main.c) */
extern SceUID (*k_dopen)(const char *dirname);
extern int    (*k_dread)(SceUID fd, SceIoDirent *dir);
extern int    (*k_dclose)(SceUID fd);

/* hooks.c */
SceUID my_gamedopen(const char *dirname);
int    my_gamedread(SceUID fd, SceIoDirent *dir);
int    my_gamedclose(SceUID fd);
SceUID my_gameopen(const char *file, int flags, SceMode mode);
int    my_gameread(SceUID fd, void *data, SceSize size);
int    my_gameclose(SceUID fd);
SceOff my_gamelseek(SceUID fd, SceOff offset, int whence);
int    my_gamegetstat(const char *file, SceIoStat *stat);
int    my_gameremove(const char *file);
int    my_gamermdir(const char *path);
int    my_gamerename(const char *oldname, const char *newname);
int    my_gamechstat(const char *file, SceIoStat *stat, int bits);
int    my_umdemuloadexec(char *file, struct SceKernelLoadExecVSHParam *param);
int    my_homebrewloadexec(char *file, struct SceKernelLoadExecVSHParam *param);

/* trampoline over SystemControl's central launch function (catches PS1) */
extern int (*org_vsh_loadexec_apitype)(int apitype, const char *file,
                                       struct SceKernelLoadExecVSHParam *param);
int my_vsh_loadexec_apitype(int apitype, const char *file,
                            struct SceKernelLoadExecVSHParam *param);

extern volatile int g_in_gamedopen;   /* set while original gamedopen runs */

#endif
