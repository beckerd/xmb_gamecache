/*
 * gcache.c - persistent cache engine
 *
 * Two files per device:
 *   XMBGC.BIN  = [DataHeader 64B][blobs, append-only]
 *   XMBGC.IDX  = [IdxHeader][count * CacheEntry] (rewritten whole on flush)
 * Committed blobs are immutable and appends only ever go past the
 * committed end, so a crash can at worst orphan a few uncommitted
 * bytes — never corrupt the index.  Any inconsistency fails checksum
 * validation and the cache silently rebuilds.
 *
 * Entries are created as cheap placeholders from the directory listing
 * alone; the expensive part (ISO walk + SFO synthesis + icon bytes) is
 * built lazily the first time the XMB asks for that specific game, so
 * there is never one giant blocking scan.
 *
 * Portions derived from ARK-4 / PRO CFW (core/vshctrl/virtual_pbp.c),
 * GPL-3.0-or-later.
 */
#include <string.h>
#include <pspsysmem_kernel.h>
#include "gamecache.h"
#include "piso.h"

#define GC_FILE_MAGIC   0x32434758      /* "XGC2" */
#define GC_FILE_VERSION 2
#define GC_HDR_SIZE     64
#define GC_IOBUF_SIZE   (32 * 1024)
#define GC_INIT_CAP     256
#define GC_FLUSH_EVERY  8

/*
 * Size budget.  The cache file is capped in absolute bytes, scaled to
 * the card it lives on: a 2 GB card must not carry the same 50 MB of
 * cache that is noise on a 128 GB one.  The budget bounds *total*
 * bytes (live + dead) — bounding only the dead part lets a library
 * that is merely added to, never deleted from, grow without limit.
 *
 * GC_SIZE_DIV of the card, clamped.  The floor keeps the budget large
 * enough to hold the list-critical blobs (PARAM.SFO + ICON0, ~30 KB a
 * game) for a big library even on a small card; the ceiling stops a
 * huge card from handing out a budget nothing needs.
 */
#define GC_SIZE_DIV     200             /* 0.5% of the card             */
#define GC_SIZE_MIN     (8 * 1024 * 1024)
#define GC_SIZE_MAX     (48 * 1024 * 1024)
#define GC_RECLAIM_PCT  75              /* compact down to this % of it */

/*
 * The ceiling is set by what reclamation costs, not by what the card
 * could spare: ctx_compact rewrites the surviving data, so the stall
 * scales with how much is kept (~0.33 s per MB on a Memory Stick).
 * 48 MB puts the worst case around 16 s, and it only comes up after
 * roughly a quarter of the budget has churned.  It is also the figure
 * this plugin previously allowed for dead bytes *alone*, so the whole
 * file is now held under what the garbage used to be.
 *
 * Little is lost by stopping there.  Metadata is what makes the list
 * instant and costs ~30 KB a game, so even 400 games fit in 12 MB;
 * the rest is media, and since eviction drops the coldest games
 * first, the games actually being played keep theirs.  A larger cache
 * would only buy instant backgrounds for games rarely opened.
 */

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 ark_magic;
    u32 blob_end;
    u32 waste;
    u32 rsvd[11];
} GcDataHeader;

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 ark_magic;
    u32 count;
    u32 index_sum;
    u32 rsvd[3];
} GcIdxHeader;

typedef struct {
    const char *dev;        /* "ms0:" / "ef0:" */
    const char *data_path;
    const char *idx_path;
    const char *tmp_path;   /* scratch file used by compaction */
    SceUID fd;              /* data file fd */
    int loaded;
    u32 count, cap;
    CacheEntry *e;
    SceUID e_id;
    u32 blob_end;
    u32 waste;
    u32 limit;              /* absolute byte budget for this device */
    int pend;               /* mutations since last flush */
    u8 **mir;               /* [cap*GC_MIR_SECS] RAM-mirror ptrs, NULL = none */
    SceUID mir_id;
} CacheCtx;

static CacheCtx g_ctx[2] = {
    { "ms0:", "ms0:/PSP/SYSTEM/XMBGC.BIN", "ms0:/PSP/SYSTEM/XMBGC.IDX",
      "ms0:/PSP/SYSTEM/XMBGC.TMP",
      -1, 0, 0, 0, NULL, -1, GC_HDR_SIZE, 0, GC_SIZE_MAX, 0, NULL, -1 },
    { "ef0:", "ef0:/PSP/SYSTEM/XMBGC.BIN", "ef0:/PSP/SYSTEM/XMBGC.IDX",
      "ef0:/PSP/SYSTEM/XMBGC.TMP",
      -1, 0, 0, 0, NULL, -1, GC_HDR_SIZE, 0, GC_SIZE_MAX, 0, NULL, -1 },
};

static SceUID g_sema = -1;
static u8 *g_iobuf = NULL;      /* 64-byte aligned scratch buffer */
static SceUID g_iobuf_id = -1;
static char g_openiso[128];     /* currently-open ISO (piso_open affinity) */
static u32 g_gen = 0;           /* session-unique entry generations */

/* SFO parsing (mirrors virtual_pbp.c) */
typedef struct __attribute__((packed)) {
    u32 signature;
    u32 version;
    u32 fields_table_offs;
    u32 values_table_offs;
    int nitems;
} SFOHeader;

typedef struct __attribute__((packed)) {
    u16 field_offs;
    u8  unk;
    u8  type;               /* 0x2 string, 0x4 number */
    u32 unk2;
    u32 unk3;
    u16 val_offs;
    u16 unk4;
} SFODir;

/* 408-byte PARAM.SFO template from ARK-4 (title @0x118, disc id @0xF0,
 * parental level @0x108) */
static unsigned char virtualsfo[GC_SFO_SIZE] = {
    0x00, 0x50, 0x53, 0x46, 0x01, 0x01, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00, 0xe8, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x04, 0x02, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x12, 0x00, 0x04, 0x02, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x04, 0x02, 0x05, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x27, 0x00, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x36, 0x00, 0x04, 0x02, 0x05, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x45, 0x00, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x2c, 0x00, 0x00, 0x00, 0x4c, 0x00, 0x04, 0x02, 0x40, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x42, 0x4f, 0x4f, 0x54, 0x41, 0x42, 0x4c, 0x45, 0x00, 0x43, 0x41, 0x54,
    0x45, 0x47, 0x4f, 0x52, 0x59, 0x00, 0x44, 0x49, 0x53, 0x43, 0x5f, 0x49, 0x44, 0x00, 0x44, 0x49,
    0x53, 0x43, 0x5f, 0x56, 0x45, 0x52, 0x53, 0x49, 0x4f, 0x4e, 0x00, 0x50, 0x41, 0x52, 0x45, 0x4e,
    0x54, 0x41, 0x4c, 0x5f, 0x4c, 0x45, 0x56, 0x45, 0x4c, 0x00, 0x50, 0x53, 0x50, 0x5f, 0x53, 0x59,
    0x53, 0x54, 0x45, 0x4d, 0x5f, 0x56, 0x45, 0x52, 0x00, 0x52, 0x45, 0x47, 0x49, 0x4f, 0x4e, 0x00,
    0x54, 0x49, 0x54, 0x4c, 0x45, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x45, 0x47, 0x00, 0x00,
    0x55, 0x43, 0x4a, 0x53, 0x31, 0x30, 0x30, 0x34, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x31, 0x2e, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x31, 0x2e, 0x30, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
    0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const char *pbp_files[6] = {
    "/PSP_GAME/PARAM.SFO",
    "/PSP_GAME/ICON0.PNG",
    "/PSP_GAME/ICON1.PMF",
    "/PSP_GAME/PIC0.PNG",
    "/PSP_GAME/PIC1.PNG",
    "/PSP_GAME/SND0.AT3",
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

int x_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

void x_hex8(char *out, u32 v)
{
    static const char digits[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 8; i++)
        out[i] = digits[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = '\0';
}

u32 x_parsehex8(const char *s)
{
    u32 v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return (u32)-1;
        v = (v << 4) | d;
    }
    return v;
}

#if XGC_DEBUG
void gc_log(const char *a, const char *b, u32 v)
{
    char line[224], rev[12], num[12];
    const char *p;
    int i = 0, n = 0;
    int k1 = pspSdkSetK1(0);
    u32 t = sceKernelGetSystemTimeLow() / 1000;
    SceUID fd;

    if (t == 0)
        rev[i++] = '0';
    while (t > 0 && i < 12) {
        rev[i++] = '0' + (t % 10);
        t /= 10;
    }
    while (i > 0)
        line[n++] = rev[--i];
    line[n++] = ' ';
    for (p = a; *p && n < 130; p++)
        line[n++] = *p;
    if (b != NULL) {
        line[n++] = ' ';
        for (p = b; *p && n < 200; p++)
            line[n++] = *p;
    }
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    x_hex8(num, v);
    for (i = 0; i < 8; i++)
        line[n++] = num[i];
    line[n++] = '\n';

    fd = sceIoOpen("ms0:/XGC_LOG.TXT", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
    pspSdkSetK1(k1);
}
#else
void gc_log(const char *a, const char *b, u32 v) { (void)a; (void)b; (void)v; }
#endif

void *gc_alloc(u32 size, SceUID *id)
{
    SceUID uid = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "xgc",
                                               PSP_SMEM_Low, size, NULL);
    if (uid < 0) {
        *id = -1;
        return NULL;
    }
    *id = uid;
    return sceKernelGetBlockHeadAddr(uid);
}

void gc_free(SceUID id)
{
    if (id >= 0)
        sceKernelFreePartitionMemory(id);
}

void gc_lock(void)
{
    if (g_sema >= 0)
        sceKernelWaitSema(g_sema, 1, 0);
}

void gc_unlock(void)
{
    if (g_sema >= 0)
        sceKernelSignalSema(g_sema, 1);
}

int gc_init(void)
{
    g_sema = sceKernelCreateSema("XGCSema", 0, 1, 1, NULL);
    return (g_sema >= 0) ? 0 : -1;
}

static int ensure_iobuf(void)
{
    if (g_iobuf != NULL)
        return 0;
    u8 *p = gc_alloc(GC_IOBUF_SIZE + 64, &g_iobuf_id);
    if (p == NULL)
        return -1;
    g_iobuf = (u8 *)(((u32)p + 63) & ~63);
    return 0;
}

/* piso_open affinity: keep the last ISO open across consecutive reads */
static int aff_open(const char *path)
{
    if (g_openiso[0] != '\0' && 0 == strcmp(g_openiso, path))
        return 0;
    if (g_openiso[0] != '\0') {
        piso_close();
        g_openiso[0] = '\0';
    }
    int ret = piso_open(path);
    if (ret < 0)
        return ret;
    strncpy(g_openiso, path, sizeof(g_openiso) - 1);
    g_openiso[sizeof(g_openiso) - 1] = '\0';
    return 0;
}

void gc_aff_close(void)
{
    if (g_openiso[0] != '\0') {
        piso_close();
        g_openiso[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* RAM mirror of the list-critical blobs (PARAM.SFO + ICON0)           */
/*                                                                     */
/* The XMB re-reads every game's SFO and icon on every list build, and */
/* right after resume those cache-file reads cost seconds (cold FAT    */
/* chains on a big card).  RAM survives suspend, so once a session's   */
/* first list build has run, rebuilds are served from here with        */
/* almost no card IO.  Bump-allocated in chunks, capped; on any        */
/* allocation failure mirroring quietly stops and reads fall back to   */
/* the cache file.                                                     */
/* ------------------------------------------------------------------ */

#define GC_MIR_SECS      2              /* sections 0 (SFO) and 1 (ICON0) */
#define GC_MIR_CHUNK     (256 * 1024)
#define GC_MIR_MAXCHUNKS 8              /* 2 MB total cap */

static u8 *g_mir_chunk[GC_MIR_MAXCHUNKS];
static int g_mir_nchunks = 0;
static u32 g_mir_used = 0;              /* bytes used in the last chunk */
static int g_mir_dead = 0;              /* out of budget/memory: stop */

static u8 *mir_alloc(u32 size)
{
    if (g_mir_dead || size > GC_MIR_CHUNK)
        return NULL;
    if (g_mir_nchunks == 0 || g_mir_used + size > GC_MIR_CHUNK) {
        SceUID id;
        u8 *p;
        if (g_mir_nchunks >= GC_MIR_MAXCHUNKS) {
            g_mir_dead = 1;
            return NULL;
        }
        p = gc_alloc(GC_MIR_CHUNK, &id);
        if (p == NULL) {
            g_mir_dead = 1;
            return NULL;
        }
        g_mir_chunk[g_mir_nchunks++] = p;
        g_mir_used = 0;
    }
    g_mir_used += size;
    return g_mir_chunk[g_mir_nchunks - 1] + g_mir_used - size;
}

static u8 *ctx_mir_get(CacheCtx *c, CacheEntry *e, int sec)
{
    u32 idx = e - c->e;
    if (c->mir == NULL || idx >= c->cap)
        return NULL;
    return c->mir[idx * GC_MIR_SECS + sec];
}

static void ctx_mir_set(CacheCtx *c, CacheEntry *e, int sec, u8 *m)
{
    u32 idx = e - c->e;
    if (c->mir == NULL || idx >= c->cap)
        return;
    c->mir[idx * GC_MIR_SECS + sec] = m;
}

static void ctx_mir_drop(CacheCtx *c, CacheEntry *e)
{
    int s;
    for (s = 0; s < GC_MIR_SECS; s++)
        ctx_mir_set(c, e, s, NULL);     /* arena bytes leak; bounded by cap */
}

/* ------------------------------------------------------------------ */
/* cache file primitives                                               */
/* ------------------------------------------------------------------ */

static u32 index_sum(CacheCtx *c)
{
    u32 sum = 0x58474331;
    u32 n = c->count * sizeof(CacheEntry) / 4;
    u32 *p = (u32 *)c->e;
    u32 i;
    for (i = 0; i < n; i++)
        sum += p[i];
    return sum;
}

static u32 gc_ark_magic(void)
{
    u32 v = sctrlHENGetVersion() & 0xF;
    v = (v << 16) | sctrlHENGetMinorVersion();
    return v ^ 0x58474331;
}

static int ctx_open_file(CacheCtx *c, int create)
{
    if (c->fd >= 0)
        return 0;
    int flags = PSP_O_RDWR | (create ? PSP_O_CREAT : 0);
    c->fd = sceIoOpen(c->data_path, flags, 0777);
    if (c->fd < 0 && create) {
        /* PSP/SYSTEM may not exist yet */
        char tmp[32];
        strcpy(tmp, c->dev); strcat(tmp, "/PSP");
        sceIoMkdir(tmp, 0777);
        strcat(tmp, "/SYSTEM");
        sceIoMkdir(tmp, 0777);
        c->fd = sceIoOpen(c->data_path, flags, 0777);
    }
    return (c->fd >= 0) ? 0 : -1;
}

static void ctx_close_file(CacheCtx *c)
{
    if (c->fd >= 0) {
        sceIoClose(c->fd);
        c->fd = -1;
    }
}

static int ctx_pread_once(CacheCtx *c, u32 off, void *data, u32 size)
{
    if (c->fd < 0 && ctx_open_file(c, 0) < 0)
        return -1;
    if (sceIoLseek32(c->fd, off, PSP_SEEK_SET) != (int)off)
        return -2;
    int r = sceIoRead(c->fd, data, size);
    return (r == (int)size) ? 0 : -3;
}

static int ctx_pread(CacheCtx *c, u32 off, void *data, u32 size)
{
    int ret = ctx_pread_once(c, off, data, size);
    if (ret < 0) {
        /* fd may have gone stale (USB transfer, card remount): retry once */
        ctx_close_file(c);
        ret = ctx_pread_once(c, off, data, size);
    }
    return ret;
}

static int ctx_pwrite_once(CacheCtx *c, u32 off, const void *data, u32 size)
{
    if (c->fd < 0 && ctx_open_file(c, 1) < 0)
        return -1;
    if (sceIoLseek32(c->fd, off, PSP_SEEK_SET) != (int)off)
        return -2;
    int r = sceIoWrite(c->fd, data, size);
    return (r == (int)size) ? 0 : -3;
}

static int ctx_pwrite(CacheCtx *c, u32 off, const void *data, u32 size)
{
    int ret = ctx_pwrite_once(c, off, data, size);
    if (ret < 0) {
        ctx_close_file(c);
        ret = ctx_pwrite_once(c, off, data, size);
    }
    return ret;
}

static int ctx_grow(CacheCtx *c, u32 need)
{
    if (need <= c->cap)
        return 0;
    u32 newcap = c->cap ? c->cap : GC_INIT_CAP;
    while (newcap < need)
        newcap *= 2;
    SceUID nid;
    CacheEntry *ne = gc_alloc(newcap * sizeof(CacheEntry), &nid);
    if (ne == NULL)
        return -1;
    if (c->e != NULL) {
        memcpy(ne, c->e, c->count * sizeof(CacheEntry));
        gc_free(c->e_id);
    }
    /* keep the RAM-mirror table the same size as the entry table; if it
     * can't grow, drop it (mirroring off for this ctx) rather than risk
     * out-of-bounds indexing */
    {
        SceUID mid;
        u8 **nm = gc_alloc(newcap * GC_MIR_SECS * sizeof(u8 *), &mid);
        if (nm != NULL) {
            memset(nm, 0, newcap * GC_MIR_SECS * sizeof(u8 *));
            if (c->mir != NULL)
                memcpy(nm, c->mir, c->cap * GC_MIR_SECS * sizeof(u8 *));
        }
        if (c->mir != NULL)
            gc_free(c->mir_id);
        c->mir = nm;
        c->mir_id = (nm != NULL) ? mid : -1;
    }
    c->e = ne;
    c->e_id = nid;
    c->cap = newcap;
    return 0;
}

static int write_data_header(CacheCtx *c)
{
    GcDataHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = GC_FILE_MAGIC;
    h.version = GC_FILE_VERSION;
    h.ark_magic = gc_ark_magic();
    h.blob_end = c->blob_end;
    h.waste = c->waste;
    return ctx_pwrite(c, 0, &h, sizeof(h));
}

static void ctx_reset(CacheCtx *c)
{
    if (c->mir != NULL)
        memset(c->mir, 0, c->cap * GC_MIR_SECS * sizeof(u8 *));
    c->count = 0;
    c->blob_end = GC_HDR_SIZE;
    c->waste = 0;
    c->pend = 0;
    if (c->fd >= 0)
        ctx_close_file(c);
    sceIoRemove(c->idx_path);
    sceIoRemove(c->tmp_path);
    c->fd = sceIoOpen(c->data_path, PSP_O_RDWR | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    write_data_header(c);
    gc_log("cache reset", c->dev, 0);
}

static int ctx_flush(CacheCtx *c)
{
    GcIdxHeader ih;
    SceUID fd;

    if (ctx_open_file(c, 1) < 0)
        return -1;
    if (write_data_header(c) < 0)
        return -2;

    fd = sceIoOpen(c->idx_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return -3;
    memset(&ih, 0, sizeof(ih));
    ih.magic = GC_FILE_MAGIC;
    ih.version = GC_FILE_VERSION;
    ih.ark_magic = gc_ark_magic();
    ih.count = c->count;
    ih.index_sum = index_sum(c);
    sceIoWrite(fd, &ih, sizeof(ih));
    if (c->count > 0)
        sceIoWrite(fd, c->e, c->count * sizeof(CacheEntry));
    sceIoClose(fd);
    c->pend = 0;
    return 0;
}

static void ctx_maybe_flush(CacheCtx *c)
{
    if (c->pend >= GC_FLUSH_EVERY)
        ctx_flush(c);
}

void gc_flush_all(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_ctx[i].loaded && g_ctx[i].pend > 0)
            ctx_flush(&g_ctx[i]);
    }
}

static void ctx_load(CacheCtx *c)
{
    GcDataHeader h;
    GcIdxHeader ih;
    SceUID fd;
    int ok = 0;

    if (c->loaded)
        return;
    c->loaded = 1;
    c->count = 0;
    c->blob_end = GC_HDR_SIZE;
    c->waste = 0;
    c->pend = 0;

    /* scratch left behind by a compaction that was cut short */
    sceIoRemove(c->tmp_path);

    if (ctx_open_file(c, 1) < 0)
        return;

    if (ctx_pread(c, 0, &h, sizeof(h)) < 0) {
        /* brand new data file: commit a fresh header */
        write_data_header(c);
        sceIoRemove(c->idx_path);
        gc_log("cache new", c->dev, 0);
        return;
    }
    if (h.magic != GC_FILE_MAGIC || h.version != GC_FILE_VERSION ||
        h.ark_magic != gc_ark_magic() || h.blob_end < GC_HDR_SIZE) {
        ctx_reset(c);
        return;
    }
    c->blob_end = h.blob_end;
    c->waste = h.waste;

    fd = sceIoOpen(c->idx_path, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        if (sceIoRead(fd, &ih, sizeof(ih)) == sizeof(ih) &&
            ih.magic == GC_FILE_MAGIC && ih.version == GC_FILE_VERSION &&
            ih.ark_magic == gc_ark_magic() && ih.count <= 8192) {
            if (ih.count == 0) {
                ok = 1;
            } else if (ctx_grow(c, ih.count) >= 0 &&
                       sceIoRead(fd, c->e, ih.count * sizeof(CacheEntry)) ==
                           (int)(ih.count * sizeof(CacheEntry))) {
                c->count = ih.count;
                if (index_sum(c) == ih.index_sum)
                    ok = 1;
            }
        }
        sceIoClose(fd);
    }
    if (!ok) {
        ctx_reset(c);
        return;
    }
    /* sanitize loaded entries and stamp session-unique generations */
    {
        u32 i;
        for (i = 0; i < c->count; i++) {
            c->e[i].path[sizeof(c->e[i].path) - 1] = '\0';
            c->e[i].reserved[0] = ++g_gen;
        }
    }
    gc_log("cache loaded", c->dev, c->count);
}

static int blob_append(CacheCtx *c, const void *data, u32 size, u32 *off_out)
{
    if (ctx_pwrite(c, c->blob_end, data, size) < 0)
        return -1;
    *off_out = c->blob_end;
    c->blob_end += size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry building                                                      */
/* ------------------------------------------------------------------ */

static int get_sfo_string(const char *sfo, const char *name, char *output, int output_size)
{
    SFOHeader *header = (SFOHeader *)sfo;
    SFODir *entries = (SFODir *)(sfo + 0x14);
    int i;

    if (header->signature != 0x46535000)
        return -39;

    for (i = 0; i < header->nitems; i++) {
        if (0 == strcmp(sfo + header->fields_table_offs + entries[i].field_offs, name)) {
            if (entries[i].type != 0x02)
                return -41;
            memset(output, 0, output_size);
            strncpy(output, sfo + header->values_table_offs + entries[i].val_offs, output_size);
            output[output_size - 1] = '\0';
            return 0;
        }
    }
    return -40;
}

static int get_sfo_u32(const char *sfo, const char *name, u32 *output)
{
    SFOHeader *header = (SFOHeader *)sfo;
    SFODir *entries = (SFODir *)(sfo + 0x14);
    int i;

    if (header->signature != 0x46535000)
        return -39;

    for (i = 0; i < header->nitems; i++) {
        if (0 == strcmp(sfo + header->fields_table_offs + entries[i].field_offs, name)) {
            if (entries[i].type != 0x04)
                return -41;
            *output = *(u32 *)(sfo + header->values_table_offs + entries[i].val_offs);
            return 0;
        }
    }
    return -40;
}

/* bytes this entry occupies in the data file */
static u32 entry_sect_bytes(CacheEntry *e, int i)
{
    if (!e->blob_off[i])
        return 0;
    return (i == 0) ? GC_SFO_SIZE : e->sect_size[i];
}

static u32 entry_blob_bytes(CacheEntry *e)
{
    u32 n = 0;
    int i;
    for (i = 0; i < 8; i++)
        n += entry_sect_bytes(e, i);
    return n;
}

static void entry_invalidate(CacheCtx *c, CacheEntry *e)
{
    if (!(e->flags & GC_ENTRY_VALID))
        return;
    c->waste += entry_blob_bytes(e);
    ctx_mir_drop(c, e);
    memset(e, 0, sizeof(*e));
}

/* ------------------------------------------------------------------ */
/* size budget and reclamation                                         */
/*                                                                     */
/* Three mechanisms, cheapest first:                                   */
/*   trim  - roll the append point back over blobs that ended up at    */
/*           the tail, so add-then-remove churn costs nothing          */
/*   evict - drop the bulky media sections (ICON1/PIC0/PIC1/SND0) from */
/*           the coldest games, keeping the SFO+ICON0 that the list    */
/*           view actually needs; they re-cache from the ISO on demand */
/*   compact - copy the surviving blobs into a fresh file, which is    */
/*           the only step that physically shrinks it                  */
/*                                                                     */
/* The old behaviour — truncate everything once dead bytes passed a    */
/* fixed 48 MB — is now only the fallback for a failed compaction.     */
/* ------------------------------------------------------------------ */

static u32 ctx_live_bytes(CacheCtx *c)
{
    u32 i, n = 0;
    for (i = 0; i < c->count; i++) {
        if (c->e[i].flags & GC_ENTRY_VALID)
            n += entry_blob_bytes(&c->e[i]);
    }
    return n;
}

/* byte budget for this device, from the free-space snapshot */
static void ctx_update_limit(CacheCtx *c)
{
    u64 cap = gc_dev_capacity(c->dev);
    u32 lim;

    if (cap == 0) {
        /* capacity not sampled yet: assume the permissive ceiling and
         * re-derive on a later scan, once the XMB has asked for free
         * space at least once */
        c->limit = GC_SIZE_MAX;
        return;
    }
    cap /= GC_SIZE_DIV;
    if (cap > GC_SIZE_MAX)
        lim = GC_SIZE_MAX;
    else
        lim = (u32)cap;
    if (lim < GC_SIZE_MIN)
        lim = GC_SIZE_MIN;
    c->limit = lim;
}

/* would caching `size` more bytes put us over budget? */
static int ctx_over_budget(CacheCtx *c, u32 size)
{
    if (c->blob_end >= c->limit)
        return 1;
    return size > c->limit - c->blob_end;
}

/*
 * Roll blob_end back over any trailing free space.  Blobs are
 * immutable and only ever appended, so everything above the highest
 * live blob is dead and can be handed straight back to the allocator.
 * One pass over the index, run after a batch of invalidations rather
 * than per entry.  The file keeps its high-water size on the card
 * until a compaction rewrites it; the reclaimed range is reused by
 * the next append.
 */
static void ctx_trim_tail(CacheCtx *c)
{
    u32 i, hi = GC_HDR_SIZE;
    int s;

    for (i = 0; i < c->count; i++) {
        CacheEntry *e = &c->e[i];
        if (!(e->flags & GC_ENTRY_VALID))
            continue;
        for (s = 0; s < 8; s++) {
            u32 end;
            if (!e->blob_off[s])
                continue;
            end = e->blob_off[s] + entry_sect_bytes(e, s);
            if (end > hi)
                hi = end;
        }
    }
    if (hi >= c->blob_end)
        return;
    {
        u32 freed = c->blob_end - hi;
        c->waste = (c->waste > freed) ? c->waste - freed : 0;
        c->blob_end = hi;
        c->pend++;
        gc_log("cache trim", c->dev, freed);
    }
}

/* drop one cached section, returning its bytes to the waste pool */
static void evict_section(CacheCtx *c, CacheEntry *e, int s)
{
    u32 n = entry_sect_bytes(e, s);
    if (n == 0)
        return;
    c->waste += n;
    e->blob_off[s] = 0;
    if (s < GC_MIR_SECS)
        ctx_mir_set(c, e, s, NULL);
    c->pend++;
}

/*
 * Shed cached bytes until live data fits in `target`.  Media first and
 * coldest first: a game that is not in the recently-played list loses
 * its animated icon, backgrounds and menu music before a recent one
 * does, and every game keeps its SFO+ICON0 until the last resort —
 * those are what make the list itself instant, and they are ~30 KB a
 * game against megabytes for the media.
 */
static void ctx_evict(CacheCtx *c, u32 target)
{
    static const int media[4] = { 2, 3, 4, 5 };   /* ICON1 PIC0 PIC1 SND0 */
    u32 live = ctx_live_bytes(c);
    int pass, k;
    u32 i;

    for (pass = 0; pass < 3 && live > target; pass++) {
        for (i = 0; i < c->count && live > target; i++) {
            CacheEntry *e = &c->e[i];
            if (!(e->flags & GC_ENTRY_VALID))
                continue;
            /* pass 0: cold games only.  pass 1: any game.  pass 2:
             * also give up ICON0, which only a huge library on a tiny
             * card should ever reach. */
            if (pass == 0 && mru_rank(e->path) >= 0)
                continue;
            for (k = 0; k < 4 && live > target; k++) {
                u32 n = entry_sect_bytes(e, media[k]);
                if (n == 0)
                    continue;
                evict_section(c, e, media[k]);
                live -= n;
            }
            if (pass == 2 && live > target) {
                u32 n = entry_sect_bytes(e, 1);
                if (n > 0) {
                    evict_section(c, e, 1);
                    live -= n;
                }
            }
        }
    }
    gc_log("cache evict", c->dev, live);
}

/* whole-file copy through the scratch buffer (compaction fallback) */
static int copy_file(const char *src, const char *dst)
{
    SceUID s, d;
    int ret = 0;

    s = sceIoOpen(src, PSP_O_RDONLY, 0777);
    if (s < 0)
        return -1;
    d = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (d < 0) {
        sceIoClose(s);
        return -1;
    }
    for (;;) {
        int n = sceIoRead(s, g_iobuf, GC_IOBUF_SIZE);
        if (n < 0) {
            ret = -1;
            break;
        }
        if (n == 0)
            break;
        if (sceIoWrite(d, g_iobuf, n) != n) {
            ret = -1;
            break;
        }
    }
    sceIoClose(d);
    sceIoClose(s);
    return ret;
}

/*
 * Copy every surviving blob into a fresh file and swap it in.  This is
 * the incremental alternative to wiping the cache: entries keep their
 * built state, their synthesized SFO and their icons, so nothing goes
 * cold and no ISO has to be re-walked.
 *
 * Crash safety comes from the ordering, not from a journal.  The index
 * is deleted before the data file is swapped, so an interruption at
 * any point leaves either the old data file or the new one with no
 * index beside it — and ctx_load treats a missing/mismatched index as
 * "rebuild", which is exactly the old behaviour.  A half-written
 * scratch file is never visible under the real name.
 */
static int ctx_compact(CacheCtx *c)
{
    GcDataHeader h;
    SceUID tfd;
    u32 i, newend = GC_HDR_SIZE;
    int s, ret = -1;

    if (ensure_iobuf() < 0)
        return -1;
    if (ctx_open_file(c, 0) < 0)
        return -1;

    sceIoRemove(c->tmp_path);
    tfd = sceIoOpen(c->tmp_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (tfd < 0)
        return -1;

    memset(&h, 0, sizeof(h));
    if (sceIoWrite(tfd, &h, sizeof(h)) != (int)sizeof(h))
        goto fail;

    for (i = 0; i < c->count; i++) {
        CacheEntry *e = &c->e[i];
        if (!(e->flags & GC_ENTRY_VALID))
            continue;
        for (s = 0; s < 8; s++) {
            u32 size = entry_sect_bytes(e, s);
            u32 src = e->blob_off[s];
            u32 pos = 0;
            if (size == 0)
                continue;
            while (pos < size) {
                u32 n = size - pos;
                if (n > GC_IOBUF_SIZE)
                    n = GC_IOBUF_SIZE;
                if (ctx_pread(c, src + pos, g_iobuf, n) < 0)
                    goto fail;
                if (sceIoWrite(tfd, g_iobuf, n) != (int)n)
                    goto fail;
                pos += n;
            }
            e->blob_off[s] = newend;    /* src already consumed */
            newend += size;
        }
    }

    h.magic = GC_FILE_MAGIC;
    h.version = GC_FILE_VERSION;
    h.ark_magic = gc_ark_magic();
    h.blob_end = newend;
    h.waste = 0;
    if (sceIoLseek32(tfd, 0, PSP_SEEK_SET) != 0)
        goto fail;
    if (sceIoWrite(tfd, &h, sizeof(h)) != (int)sizeof(h))
        goto fail;
    sceIoClose(tfd);
    tfd = -1;

    /* swap: index first, so any crash from here on lands on "rebuild" */
    ctx_close_file(c);
    sceIoRemove(c->idx_path);
    sceIoRemove(c->data_path);
    if (sceIoRename(c->tmp_path, c->data_path) < 0) {
        /* rename is not dependable across every PSP filesystem; the
         * old file is already gone, so copy the scratch into place
         * rather than lose a cache we just spent the IO to build */
        if (copy_file(c->tmp_path, c->data_path) < 0) {
            sceIoRemove(c->tmp_path);
            return -1;
        }
    }
    sceIoRemove(c->tmp_path);

    c->blob_end = newend;
    c->waste = 0;
    c->pend = 1;
    ret = ctx_flush(c);
    gc_log("cache compact", c->dev, newend);
    return ret;

fail:
    if (tfd >= 0)
        sceIoClose(tfd);
    sceIoRemove(c->tmp_path);
    return -1;
}

/*
 * Bring the cache back inside its budget.  Called at list-build time,
 * which is the only safe point: no virtual PBP read is in flight, so
 * blob offsets can move.
 */
static void ctx_reclaim(CacheCtx *c)
{
    u32 target;

    ctx_update_limit(c);
    if (c->blob_end <= c->limit && c->waste <= (c->limit >> 1))
        return;

    target = (u32)(((u64)c->limit * GC_RECLAIM_PCT) / 100);
    if (ctx_live_bytes(c) > target)
        ctx_evict(c, target);
    ctx_trim_tail(c);

    /* Rolling back the tail is free and often enough on its own —
     * rewriting the file is the one step here that costs real IO, so
     * only pay for it if holes are actually left behind.  The caller
     * flushes the index either way. */
    if (c->blob_end <= c->limit && c->waste <= (c->limit >> 1))
        return;

    if (ctx_compact(c) < 0)
        ctx_reset(c);       /* last resort: the old wipe-everything path */
}

/* copy an entire ISO section into the cache file, chunked */
static int cache_section_from_iso(CacheCtx *c, CacheEntry *e, int i)
{
    u32 pos = 0, start = 0, first = 1;
    u32 size = e->sect_size[i];

    while (pos < size) {
        u32 n = size - pos;
        if (n > GC_IOBUF_SIZE)
            n = GC_IOBUF_SIZE;
        if (piso_read(g_iobuf, e->sect_lba[i], pos, n) < 0)
            return -1;
        u32 off;
        if (blob_append(c, g_iobuf, n, &off) < 0)
            return -2;
        if (first) {
            start = off;
            first = 0;
        }
        pos += n;
    }
    e->blob_off[i] = start;
    return 0;
}

/* initialize a placeholder entry from directory-listing info alone —
 * no ISO access whatsoever (this is what keeps scans instant) */
static void placeholder_entry(CacheEntry *e, const char *path, SceIoStat *st)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->iso_size = (u32)st->st_size;
    memcpy(&e->ctime, &st->sce_st_ctime, sizeof(ScePspDateTime));
    memcpy(&e->mtime, &st->sce_st_mtime, sizeof(ScePspDateTime));
    e->reserved[0] = ++g_gen;
    e->flags = GC_ENTRY_VALID;
}

/* the expensive part, run once per game on first use: ISO9660 walks +
 * SFO synthesis.  Icon/pic/sound bytes stay lazy (cached on first read). */
static int ensure_built_entry(CacheCtx *c, CacheEntry *e)
{
    char title[64];
    char disc_id[12];
    u32 parental = 1;
    u32 off, i;
    int ret;

    if (e->flags & GC_ENTRY_BUILT)
        return 0;
    if (e->flags & GC_ENTRY_BAD)
        return -1;
    if (ensure_iobuf() < 0)
        return -2;

    if (aff_open(e->path) < 0) {
        gc_log("build: open failed", e->path, 0);
        e->flags |= GC_ENTRY_BAD;
        c->pend++;
        return -3;
    }

    e->header[0] = 0x50425000;      /* "\0PBP" */
    e->header[1] = 0x10000;

    off = GC_PBP_HDR;
    for (i = 0; i < 8; i++) {
        e->header[2 + i] = off;
        if (i >= 6)
            continue;               /* DATA.PSP / DATA.PSAR placeholders */
        u32 size = 0, lba = 0;
        ret = piso_getfileinfo((char *)pbp_files[i], &size, &lba);
        if (ret < 0) {
            if (i == 0) {
                gc_log("build: no PARAM.SFO", e->path, ret);
                e->flags |= GC_ENTRY_BAD;
                c->pend++;
                return -4;          /* not a PSP game image */
            }
            e->sect_lba[i] = 0;
            e->sect_size[i] = 0;
            continue;
        }
        e->sect_lba[i] = lba;
        if (i == 0) {
            e->sect_size[i] = GC_SFO_SIZE;
            off += GC_SFO_SIZE;
        } else {
            e->sect_size[i] = size;
            off += size;
        }
    }
    e->pbp_total_size = e->header[9];

    /* read + synthesize PARAM.SFO */
    if (piso_read(g_iobuf, e->sect_lba[0], 0, 2048) < 0) {
        gc_log("build: sfo read failed", e->path, 0);
        e->flags |= GC_ENTRY_BAD;
        c->pend++;
        return -5;
    }
    if (get_sfo_string((char *)g_iobuf, "TITLE", title, sizeof(title)) < 0) {
        /* fall back to the file name so the game still shows up */
        const char *base = strrchr(e->path, '/');
        base = base ? base + 1 : e->path;
        memset(title, 0, sizeof(title));
        strncpy(title, base, sizeof(title) - 1);
    }
    if (get_sfo_string((char *)g_iobuf, "DISC_ID", disc_id, sizeof(disc_id)) < 0) {
        memset(disc_id, 0, sizeof(disc_id));
        strncpy(disc_id, "XGC00000", sizeof(disc_id) - 1);
    }
    get_sfo_u32((char *)g_iobuf, "PARENTAL_LEVEL", &parental);
    get_sfo_u32((char *)g_iobuf, "HRKGMP_VER", &e->opnssmp_type);
    memcpy(e->disc_id, disc_id, sizeof(e->disc_id));

    {
        static unsigned char sfo[GC_SFO_SIZE];
        memcpy(sfo, virtualsfo, sizeof(sfo));
        memcpy(sfo + 0x118, title, 64);
        memcpy(sfo + 0xF0, disc_id, 12);
        memcpy(sfo + 0x108, &parental, sizeof(parental));
        if (blob_append(c, sfo, sizeof(sfo), &e->blob_off[0]) < 0) {
            gc_log("build: sfo append failed", e->path, 0);
            e->flags |= GC_ENTRY_BAD;
            c->pend++;
            return -6;
        }
    }

    e->flags |= GC_ENTRY_BUILT;
    c->pend++;
    ctx_maybe_flush(c);
    return 0;
}

int gc_ensure_built(u32 comb)
{
    u32 ci = GC_CTX(comb);
    CacheEntry *e = gc_entry(comb);
    int ret;

    if (ci >= 2 || e == NULL)
        return -1;
    ret = ensure_built_entry(&g_ctx[ci], e);
    gc_aff_close();
    return ret;
}

/* ------------------------------------------------------------------ */
/* public API (global lock held by callers)                            */
/* ------------------------------------------------------------------ */

static CacheCtx *ctx_for_path(const char *path)
{
    if (path[0] == 'e' && path[1] == 'f')
        return &g_ctx[1];
    if (path[0] == 'm' && path[1] == 's')
        return &g_ctx[0];
    return NULL;
}

CacheEntry *gc_entry(u32 comb)
{
    u32 ci = GC_CTX(comb), idx = GC_IDX(comb);
    if (ci >= 2)
        return NULL;
    CacheCtx *c = &g_ctx[ci];
    if (idx >= c->count)
        return NULL;
    CacheEntry *e = &c->e[idx];
    if (!(e->flags & GC_ENTRY_VALID))
        return NULL;
    return e;
}

void gc_make_name(char *out, u32 comb)
{
    strcpy(out, GC_MARKER);
    x_hex8(out + GC_MARKER_LEN, comb);
}

/* does this dirent look like an ISO/CSO/...? (mirrors vshctrl's is_iso) */
static int is_iso(SceIoDirent *dir)
{
    if (!FIO_S_ISREG(dir->d_stat.st_mode))
        return 0;
    if (dir->d_name[0] == '.' && dir->d_stat.st_size < 0x8000)
        return 0;                   /* macOS metadata files */
    int len = strlen(dir->d_name);
    if (len < 5)
        return 0;
    const char *ext = dir->d_name + len - 4;
    return (x_stricmp(ext, ".iso") == 0 ||
            x_stricmp(ext, ".img") == 0 ||
            x_stricmp(ext, ".cso") == 0 ||
            x_stricmp(ext, ".zso") == 0 ||
            x_stricmp(ext, ".dax") == 0 ||
            x_stricmp(ext, ".jso") == 0);
}

static int find_entry(CacheCtx *c, const char *path)
{
    u32 i;
    for (i = 0; i < c->count; i++) {
        if ((c->e[i].flags & GC_ENTRY_VALID) && 0 == strcmp(c->e[i].path, path))
            return (int)i;
    }
    return -1;
}

/* is `dir` the immediate parent directory of entry path? */
static int entry_in_dir(CacheEntry *e, const char *dir, int dirlen)
{
    if (0 != strncmp(e->path, dir, dirlen))
        return 0;
    if (e->path[dirlen] != '/')
        return 0;
    return strchr(e->path + dirlen + 1, '/') == NULL;
}

/*
 * Scan an ISO directory ("ms0:/ISO" or "ms0:/ISO/<category>").
 * Validates cache entries against the directory listing (size+mtime, no
 * per-file opens on a warm cache), rebuilds changed/new ISOs, drops
 * removed ones, and fills `emit` with combined ids in directory order.
 */
int gc_scan_dir(const char *isopath, u32 *emit, int cap)
{
    static SceIoDirent sdir;
    static char path[160];
    CacheCtx *c = ctx_for_path(isopath);
    int cnt = 0;
    int dirlen = strlen(isopath);
    SceUID d;
    u32 i;
    /* bitmap of referenced entries, to detect deletions */
    static u8 seen[8192 / 8];

    if (c == NULL)
        return -1;

    ctx_load(c);
    ctx_reclaim(c);

    d = k_dopen(isopath);
    if (d < 0) {
        /* directory gone: drop its entries */
        for (i = 0; i < c->count; i++) {
            if ((c->e[i].flags & GC_ENTRY_VALID) && entry_in_dir(&c->e[i], isopath, dirlen)) {
                entry_invalidate(c, &c->e[i]);
                c->pend++;
            }
        }
        ctx_trim_tail(c);
        if (c->pend > 0)
            ctx_flush(c);
        gc_log("scan: no iso dir", isopath, (u32)d);
        return 0;
    }

    memset(seen, 0, sizeof(seen));
    memset(&sdir, 0, sizeof(sdir));

    while (k_dread(d, &sdir) > 0) {
        if (!is_iso(&sdir)) {
            memset(&sdir, 0, sizeof(sdir));
            continue;
        }
        if (dirlen + 1 + (int)strlen(sdir.d_name) >= (int)sizeof(((CacheEntry *)0)->path)) {
            memset(&sdir, 0, sizeof(sdir));
            continue;               /* path too long for cache entry */
        }
        strcpy(path, isopath);
        strcat(path, "/");
        strcat(path, sdir.d_name);

        int idx = find_entry(c, path);
        if (idx >= 0 &&
            c->e[idx].iso_size == (u32)sdir.d_stat.st_size &&
            0 == memcmp(&c->e[idx].mtime, &sdir.d_stat.sce_st_mtime, sizeof(ScePspDateTime))) {
            /* warm hit, zero extra IO */
        } else {
            /* new or changed ISO: reset to a cheap placeholder; the
             * heavy build happens on first use (gc_ensure_built) */
            if (idx >= 0) {
                entry_invalidate(c, &c->e[idx]);
            } else {
                /* reuse a hole if possible */
                for (i = 0; i < c->count; i++) {
                    if (!(c->e[i].flags & GC_ENTRY_VALID)) {
                        idx = (int)i;
                        break;
                    }
                }
                if (idx < 0) {
                    if (ctx_grow(c, c->count + 1) < 0) {
                        memset(&sdir, 0, sizeof(sdir));
                        continue;
                    }
                    idx = (int)c->count;
                    memset(&c->e[idx], 0, sizeof(CacheEntry));
                    c->count++;
                }
            }
            placeholder_entry(&c->e[idx], path, &sdir.d_stat);
            c->pend++;
        }
        if (idx >= 0 && idx < 8192) {
            seen[idx >> 3] |= 1 << (idx & 7);
            if (cnt < cap)
                emit[cnt++] = GC_COMBINE(c - g_ctx, idx);
        }
        memset(&sdir, 0, sizeof(sdir));
    }
    k_dclose(d);

    /* invalidate entries whose ISO vanished from this directory */
    for (i = 0; i < c->count && i < 8192; i++) {
        if ((c->e[i].flags & GC_ENTRY_VALID) &&
            entry_in_dir(&c->e[i], isopath, dirlen) &&
            !(seen[i >> 3] & (1 << (i & 7)))) {
            entry_invalidate(c, &c->e[i]);
            c->pend++;
        }
    }

    ctx_trim_tail(c);
    if (c->pend > 0)
        ctx_flush(c);
    gc_log("scan done", isopath, (u32)cnt);
    return cnt;
}

/* serve a read of the virtual EBOOT.PBP at *fp, advancing it */
static int gc_read_inner(u32 comb, u32 gen, u32 *fp, void *data, int size)
{
    CacheCtx *c = &g_ctx[GC_CTX(comb)];
    CacheEntry *e = gc_entry(comb);
    u8 *out = (u8 *)data;
    int done = 0;

    if (e == NULL || e->reserved[0] != gen)
        return -4;                  /* entry replaced since open */
    if (ensure_iobuf() < 0)
        return -5;
    if (!(e->flags & GC_ENTRY_BUILT)) {
        if (ensure_built_entry(c, e) < 0)
            return -9;
    }

    while (done < size) {
        u32 pos = *fp;
        u32 n = 0;

        if (pos >= e->pbp_total_size)
            break;

        if (pos < GC_PBP_HDR) {
            n = GC_PBP_HDR - pos;
            if (n > (u32)(size - done))
                n = size - done;
            memcpy(out, ((u8 *)e->header) + pos, n);
        } else {
            /* find the section containing pos */
            int i, sec = -1;
            for (i = 7; i >= 0; i--) {
                if (pos >= e->header[2 + i] && e->sect_size[i] > 0 &&
                    pos < e->header[2 + i] + e->sect_size[i]) {
                    sec = i;
                    break;
                }
            }
            if (sec < 0) {
                /* gap (all remaining sections empty): skip to end */
                break;
            }
            u32 spos = pos - e->header[2 + sec];
            n = e->sect_size[sec] - spos;
            if (n > (u32)(size - done))
                n = size - done;

            /* hot section already mirrored in RAM: no card IO at all */
            if (sec < GC_MIR_SECS) {
                u8 *m = ctx_mir_get(c, e, sec);
                if (m != NULL) {
                    memcpy(out, m + spos, n);
                    goto advanced;
                }
            }

            if (e->blob_off[sec] == 0) {
                /* lazily cache this section from the ISO, then serve it */
                if (aff_open(e->path) < 0)
                    return -6;
                /* Over budget: serve the bytes but don't grow the file.
                 * Reclaiming here is not an option — this read holds a
                 * blob offset that compaction would move — so the cap
                 * is enforced by refusing to append until the next
                 * list build, which runs ctx_reclaim at a safe point. */
                if (ctx_over_budget(c, e->sect_size[sec]) ||
                    cache_section_from_iso(c, e, sec) < 0) {
                    /* can't cache (over budget, or card full): stream
                     * straight from the ISO */
                    u32 p2 = 0;
                    while (p2 < n) {
                        u32 m = n - p2;
                        if (m > GC_IOBUF_SIZE)
                            m = GC_IOBUF_SIZE;
                        if (piso_read(g_iobuf, e->sect_lba[sec], spos + p2, m) < 0)
                            return -7;
                        memcpy(out + p2, g_iobuf, m);
                        p2 += m;
                    }
                    goto advanced;
                }
                c->pend++;
                ctx_maybe_flush(c);
            }
            /* first touch this session: pull the whole section into the
             * mirror in one read and serve from there */
            if (sec < GC_MIR_SECS) {
                u8 *m = mir_alloc(e->sect_size[sec]);
                if (m != NULL) {
                    if (ctx_pread(c, e->blob_off[sec], m, e->sect_size[sec]) == 0) {
                        ctx_mir_set(c, e, sec, m);
                        memcpy(out, m + spos, n);
                        goto advanced;
                    }
                    /* read failed: leave the slice unused, fall through */
                }
            }
            if (ctx_pread(c, e->blob_off[sec] + spos, out, n) < 0)
                return -8;
        }
advanced:
        *fp += n;
        out += n;
        done += n;
        if (n == 0)
            break;
    }
    return done;
}

int gc_read(u32 comb, u32 gen, u32 *fp, void *data, int size)
{
    int ret = gc_read_inner(comb, gen, fp, data, size);
    /* never leave the shared VshCtrlLib ISO reader open between calls:
     * VshCtrl's video virtualization uses the same global context */
    gc_aff_close();
    return ret;
}

int gc_delete(u32 comb)
{
    CacheCtx *c = &g_ctx[GC_CTX(comb)];
    CacheEntry *e = gc_entry(comb);
    int ret;

    if (e == NULL)
        return -14;
    gc_aff_close();
    ret = sceIoRemove(e->path);
    entry_invalidate(c, e);
    ctx_flush(c);
    return ret;
}
