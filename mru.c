/*
 * mru.c - most-recently-launched ordering
 *
 * Persists the last-launched game paths (PSP/SYSTEM/XMBMRU.BIN) and
 * hands out fake "newest" timestamps by rank.  The XMB sorts the game
 * list by folder date, so ranked games surface to the top of the list
 * in launch order; everything else keeps its real dates below them.
 *
 * Part of xmb_gamecache, GPL-3.0-or-later.
 */
#include <string.h>
#include "gamecache.h"

#define MRU_MAX   20
#define MRU_MAGIC 0x31524D58        /* "XMR1" */

static char g_mru[MRU_MAX][128];
static int g_mru_cnt = 0;
static int g_mru_loaded = 0;

static const char *mru_files[2] = {
    "ms0:/PSP/SYSTEM/XMBMRU.BIN",
    "ef0:/PSP/SYSTEM/XMBMRU.BIN",
};

void mru_load(void)
{
    u32 magic = 0;
    int i, fd = -1;

    if (g_mru_loaded)
        return;
    g_mru_loaded = 1;
    g_mru_cnt = 0;

    for (i = 0; i < 2 && fd < 0; i++)
        fd = sceIoOpen(mru_files[i], PSP_O_RDONLY, 0777);
    if (fd < 0)
        return;
    if (sceIoRead(fd, &magic, 4) == 4 && magic == MRU_MAGIC &&
        sceIoRead(fd, &g_mru_cnt, 4) == 4 &&
        g_mru_cnt >= 0 && g_mru_cnt <= MRU_MAX) {
        if (sceIoRead(fd, g_mru, g_mru_cnt * sizeof(g_mru[0])) !=
            (int)(g_mru_cnt * sizeof(g_mru[0])))
            g_mru_cnt = 0;
    } else {
        g_mru_cnt = 0;
    }
    sceIoClose(fd);
    for (i = 0; i < g_mru_cnt; i++)
        g_mru[i][sizeof(g_mru[0]) - 1] = '\0';
    gc_log("mru loaded", NULL, (u32)g_mru_cnt);
}

static void mru_save(void)
{
    u32 magic = MRU_MAGIC;
    int i;
    SceUID fd = -1;

    for (i = 0; i < 2 && fd < 0; i++)
        fd = sceIoOpen(mru_files[i], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, &magic, 4);
    sceIoWrite(fd, &g_mru_cnt, 4);
    if (g_mru_cnt > 0)
        sceIoWrite(fd, g_mru, g_mru_cnt * sizeof(g_mru[0]));
    sceIoClose(fd);
}

/* 0 = most recent, -1 = not in the list */
int mru_rank(const char *path)
{
    int i;
    mru_load();
    for (i = 0; i < g_mru_cnt; i++) {
        if (0 == strcmp(g_mru[i], path))
            return i;
    }
    return -1;
}

/* record a launch: move-to-front, drop the oldest beyond MRU_MAX */
void mru_record(const char *path)
{
    int i, at = -1;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(g_mru[0]))
        return;
    mru_load();
    for (i = 0; i < g_mru_cnt; i++) {
        if (0 == strcmp(g_mru[i], path)) {
            at = i;
            break;
        }
    }
    if (at == 0)
        return;                     /* already the most recent */
    if (at < 0) {
        if (g_mru_cnt < MRU_MAX)
            g_mru_cnt++;
        at = g_mru_cnt - 1;
    }
    for (i = at; i > 0; i--)
        strcpy(g_mru[i], g_mru[i - 1]);
    strncpy(g_mru[0], path, sizeof(g_mru[0]) - 1);
    g_mru[0][sizeof(g_mru[0]) - 1] = '\0';
    mru_save();
    gc_log("mru record", path, 0);
}

/* fake "newest" timestamp for a rank (the XMB lists newest dates first) */
void mru_faketime(int rank, ScePspDateTime *t)
{
    memset(t, 0, sizeof(*t));
    t->year = 2049;
    t->month = 1;
    t->day = 1;
    t->hour = 23;
    t->minute = (unsigned short)(59 - rank);
    t->second = 0;
}
