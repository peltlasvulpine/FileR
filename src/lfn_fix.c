/* Long-name and raw directory helpers. The FAT driver only sees 8.3 names and
 * can't move or rename entries, so these read and write the directory sectors
 * directly. See lfn_fix.h. The volume must be closed (or flushed) before they
 * run. FAT32 only. */

#include <tice.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <msddrvce.h>
#include "lfn_fix.h"

#define SECTOR      512
#define ENTRY       32
#define PER_SECTOR  (SECTOR / ENTRY)
#define ATTR_DIR    0x10
#define ATTR_VOLID  0x08
#define ATTR_LFN    0x0F
#define DELETED     0xE5

typedef struct {
    uint32_t part;        /* partition start, LBA */
    uint32_t reserved;    /* sectors before the first FAT */
    uint32_t fatsz;       /* sectors per FAT */
    uint32_t data_lba;    /* LBA of cluster 2 */
    uint32_t root;        /* root directory cluster */
    uint32_t max_cluster; /* highest valid cluster number */
    uint32_t fsinfo;      /* FSInfo sector, relative to the partition */
    uint8_t  spc;         /* sectors per cluster */
    uint8_t  nfats;
} geo_t;

/* Scratch sectors. Only one of each is in use at a time. */
static uint8_t dir_sec[SECTOR];
static uint8_t fat_sec[SECTOR];

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return rd16(p) | (rd16(p + 2) << 16);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool geo_load(msd_t *msd, uint32_t part, geo_t *g) {
    uint32_t data_rel;

    if (msd_Read(msd, part, 1, dir_sec) != 1) return false;
    if (dir_sec[510] != 0x55 || dir_sec[511] != 0xAA) return false;
    if (rd16(dir_sec + 11) != SECTOR) return false;      /* bytes per sector */

    g->part = part;
    g->spc = dir_sec[13];
    g->reserved = rd16(dir_sec + 14);
    g->nfats = dir_sec[16];
    g->fatsz = rd32(dir_sec + 36);
    g->root = rd32(dir_sec + 44);
    g->fsinfo = rd16(dir_sec + 48);
    if (g->spc == 0 || g->nfats == 0 || g->fatsz == 0 || g->root < 2) return false;

    data_rel = g->reserved + g->nfats * g->fatsz;
    g->data_lba = part + data_rel;
    g->max_cluster = (rd32(dir_sec + 32) - data_rel) / g->spc + 1;
    return true;
}

/* Reads the FAT entry for cluster c. Returns the next cluster in the chain. */
static bool fat_next(msd_t *msd, const geo_t *g, uint32_t c, uint32_t *next) {
    uint32_t byte = c * 4;

    if (msd_Read(msd, g->part + g->reserved + byte / SECTOR, 1, fat_sec) != 1) return false;
    *next = rd32(fat_sec + byte % SECTOR) & 0x0FFFFFFF;
    return true;
}

/* Walks a directory's entries in order, across its cluster chain. */
typedef struct {
    msd_t *msd;
    const geo_t *g;
    uint32_t cluster;
    uint8_t sec;    /* sector within the cluster */
    uint8_t idx;    /* entry within the sector */
} cur_t;

static uint32_t cur_lba(const cur_t *c) {
    return c->g->data_lba + (c->cluster - 2) * c->g->spc + c->sec;
}

static bool cur_load(cur_t *c) {
    return msd_Read(c->msd, cur_lba(c), 1, dir_sec) == 1;
}

static bool cur_start(cur_t *c, msd_t *msd, const geo_t *g, uint32_t first) {
    c->msd = msd;
    c->g = g;
    c->cluster = first;
    c->sec = 0;
    c->idx = 0;
    if (first < 2 || first > g->max_cluster) return false;
    return cur_load(c);
}

/* Moves to the next entry. Returns false at the end of the chain. */
static bool cur_advance(cur_t *c) {
    if (++c->idx < PER_SECTOR) return true;
    c->idx = 0;
    if (++c->sec < c->g->spc) return cur_load(c);
    c->sec = 0;
    if (!fat_next(c->msd, c->g, c->cluster, &c->cluster)) return false;
    if (c->cluster < 2 || c->cluster > c->g->max_cluster) return false;   /* end of chain */
    return cur_load(c);
}

/* The 8.3 name of a short entry, as FileR shows it: "NAME.EXT". */
static void fmt_short(const uint8_t *e, char *out) {
    int i, n = 0;

    for (i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)toupper(e[i]);
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)toupper(e[i]);
    }
    out[n] = 0;
}

static bool name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* Checksum of the 11-byte short name, stored in each long-name entry. */
static uint8_t lfn_checksum(const uint8_t *name11) {
    uint8_t s = 0;
    int i;

    for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + name11[i]);
    return s;
}

/* Looks through the directory at cluster first for the short entry called
 * name. If want_dir is set, only folders match. On a match, *cluster (if
 * given) gets its first cluster, run_out (if given) gets its long-name entries
 * when their checksum matches, and spos (if given) gets the entry's position. */
static bool dir_search(msd_t *msd, const geo_t *g, uint32_t first, const char *name,
                       bool want_dir, uint32_t *cluster, lfn_list_t *run_out,
                       lfn_pos_t *spos) {
    cur_t c;
    lfn_list_t run;
    uint8_t chk = 0;
    char fmt[16];
    const uint8_t *e;

    run.n = 0;
    if (!cur_start(&c, msd, g, first)) return false;

    for (;;) {
        e = dir_sec + c.idx * ENTRY;
        if (e[0] == 0x00) return false;                  /* end of directory */

        if (e[0] == DELETED) {
            run.n = 0;
        } else if (e[11] == ATTR_LFN) {
            if ((e[0] & 0x40) || run.n == 0) {           /* first part of a new name */
                run.n = 0;
                chk = e[13];
            }
            if (run.n < LFN_MAX_PARTS) {
                run.pos[run.n].lba = cur_lba(&c);
                run.pos[run.n].off = (uint16_t)(c.idx * ENTRY);
                run.n++;
            }
        } else {
            if (!(e[11] & ATTR_VOLID)) {
                fmt_short(e, fmt);
                if (name_eq(fmt, name) && (!want_dir || (e[11] & ATTR_DIR))) {
                    if (spos) {
                        spos->lba = cur_lba(&c);
                        spos->off = (uint16_t)(c.idx * ENTRY);
                    }
                    if (cluster) *cluster = (rd16(e + 20) << 16) | rd16(e + 26);
                    if (run_out) {
                        run_out->n = 0;
                        if (run.n && lfn_checksum(e) == chk) *run_out = run;
                    }
                    return true;
                }
            }
            run.n = 0;                                    /* a short entry ends the run */
        }

        if (!cur_advance(&c)) return false;
    }
}

/* Resolves a folder path ("/" or "/A/B") to its first cluster. */
static bool resolve_dir(msd_t *msd, const geo_t *g, const char *path, uint32_t *cluster) {
    const char *p = path + (path[0] == '/');
    const char *slash;
    char comp[16];
    size_t n;

    *cluster = g->root;
    while (*p) {
        slash = strchr(p, '/');
        n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || n >= sizeof(comp)) return false;
        memcpy(comp, p, n);
        comp[n] = 0;
        if (!dir_search(msd, g, *cluster, comp, true, cluster, NULL, NULL)) return false;
        p = slash ? slash + 1 : p + n;
    }
    return true;
}

bool lfn_find(msd_t *msd, uint32_t part, const char *full_path, lfn_list_t *out) {
    geo_t g;
    const char *fname;
    char dirp[160];
    size_t dl;
    uint32_t cluster;

    out->n = 0;
    if (!geo_load(msd, part, &g)) return false;

    fname = strrchr(full_path, '/');
    if (!fname) return false;
    dl = (size_t)(fname - full_path);
    if (dl == 0) dl = 1;
    if (dl >= sizeof(dirp)) return false;
    memcpy(dirp, full_path, dl);
    dirp[dl] = 0;
    fname++;

    if (!resolve_dir(msd, &g, dirp, &cluster)) return false;
    return dir_search(msd, &g, cluster, fname, false, NULL, out, NULL);
}

/* Turns "NAME.EXT" into the 11-byte 8.3 form. Returns false if it doesn't fit. */
static bool to_short(const char *name, uint8_t out[11]) {
    int i = 0, n = 0;

    if (!name[0] || name[0] == '.') return false;
    memset(out, ' ', 11);
    while (name[i] && name[i] != '.') {
        if (n >= 8) return false;
        out[n++] = (uint8_t)toupper((unsigned char)name[i++]);
    }
    if (name[i] == '.') {
        i++;
        n = 8;
        while (name[i]) {
            if (n >= 11) return false;
            out[n++] = (uint8_t)toupper((unsigned char)name[i++]);
        }
    }
    return true;
}

/* True if the n characters at s contain at least one letter and no uppercase letters. */
static bool all_lower(const char *s, size_t n) {
    bool any = false;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') return false;
        if (s[i] >= 'a' && s[i] <= 'z') any = true;
    }
    return any;
}

/* Case flags for a short name: 0x08 if the base is all lowercase, 0x10 if the
 * extension is. */
static uint8_t name_case_flags(const char *s) {
    const char *dot = strchr(s, '.');
    uint8_t f = 0;

    if (all_lower(s, dot ? (size_t)(dot - s) : strlen(s))) f |= 0x08;
    if (dot && all_lower(dot + 1, strlen(dot + 1))) f |= 0x10;
    return f;
}

bool lfn_rename(msd_t *msd, uint32_t part, const char *full_path, const char *new_name) {
    geo_t g;
    const char *fname;
    char dirp[160];
    size_t dl;
    uint32_t cluster, dummy;
    lfn_list_t run;
    lfn_pos_t spos;
    uint8_t short11[11];

    if (!to_short(new_name, short11)) return false;
    if (!geo_load(msd, part, &g)) return false;

    fname = strrchr(full_path, '/');
    if (!fname) return false;
    dl = (size_t)(fname - full_path);
    if (dl == 0) dl = 1;
    if (dl >= sizeof(dirp)) return false;
    memcpy(dirp, full_path, dl);
    dirp[dl] = 0;
    fname++;

    if (!resolve_dir(msd, &g, dirp, &cluster)) return false;

    /* The new name must not be taken already. */
    if (dir_search(msd, &g, cluster, new_name, false, &dummy, NULL, NULL)) return false;
    /* Finds the entry to rename, and the long-name entries in front of it. */
    if (!dir_search(msd, &g, cluster, fname, false, &dummy, &run, &spos)) return false;

    /* Overwrite the 11 name bytes. The cluster, size and attributes stay as they are. */
    if (msd_Read(msd, spos.lba, 1, dir_sec) != 1) return false;
    memcpy(dir_sec + spos.off, short11, 11);
    /* Byte 12 holds the case flags: a part whose letters are all lowercase is shown
     * in lowercase. Bits 0x08 (base) and 0x10 (extension); other bits are kept. */
    dir_sec[spos.off + 12] = (uint8_t)((dir_sec[spos.off + 12] & ~0x18) | name_case_flags(new_name));
    if (msd_Write(msd, spos.lba, 1, dir_sec) != 1) return false;

    /* The old long-name entries no longer match the name, so remove them. */
    return lfn_mark_deleted(msd, &run);
}

bool lfn_mark_deleted(msd_t *msd, const lfn_list_t *list) {
    uint8_t i;
    bool ok = true;

    for (i = 0; i < list->n; i++) {
        uint32_t lba = list->pos[i].lba;

        /* Read-modify-write, one sector at a time, so two entries in the
         * same sector both survive. */
        if (msd_Read(msd, lba, 1, dir_sec) != 1) {
            ok = false;
            continue;
        }
        dir_sec[list->pos[i].off] = DELETED;
        if (msd_Write(msd, lba, 1, dir_sec) != 1) ok = false;
    }
    return ok;
}

bool lfn_fix_fsinfo(msd_t *msd, uint32_t part) {
    geo_t g;
    uint32_t free_count = 0;
    uint32_t s, j, c;

    if (!geo_load(msd, part, &g)) return false;
    if (g.fsinfo == 0 || g.fsinfo == 0xFFFF) return false;

    /* Count the free clusters the FAT actually has. */
    for (s = 0; s < g.fatsz; s++) {
        if (msd_Read(msd, part + g.reserved + s, 1, fat_sec) != 1) return false;
        for (j = 0; j < SECTOR / 4; j++) {
            c = s * (SECTOR / 4) + j;
            if (c < 2 || c > g.max_cluster) continue;
            if ((rd32(fat_sec + j * 4) & 0x0FFFFFFF) == 0) free_count++;
        }
    }

    if (msd_Read(msd, part + g.fsinfo, 1, dir_sec) != 1) return false;
    if (rd32(dir_sec) != 0x41615252) return false;       /* FSInfo lead signature */
    if (rd32(dir_sec + 488) == free_count) return true;  /* already right */

    wr32(dir_sec + 488, free_count);
    return msd_Write(msd, part + g.fsinfo, 1, dir_sec) == 1;
}

/* The character at index k (0..12) of a long-name entry, as UCS-2. */
static uint16_t lfn_char(const uint8_t *e, int k) {
    int off = k < 5 ? 1 + 2 * k : (k < 11 ? 14 + 2 * (k - 5) : 28 + 2 * (k - 11));
    return (uint16_t)(e[off] | (e[off + 1] << 8));
}

/* Reads the long name held by a run of long-name entries into out (ASCII). */
static bool decode_run(msd_t *msd, const lfn_list_t *run, char *out, unsigned outsz) {
    unsigned n = 0;
    int i, k;

    if (outsz < 2) return false;
    out[0] = 0;
    /* The run is stored highest part first, so read it from the last entry back. */
    for (i = run->n - 1; i >= 0; i--) {
        const uint8_t *e;
        if (msd_Read(msd, run->pos[i].lba, 1, dir_sec) != 1) return false;
        e = dir_sec + run->pos[i].off;
        for (k = 0; k < 13; k++) {
            uint16_t u = lfn_char(e, k);
            if (u == 0 || u == 0xFFFF) {
                out[n] = 0;
                return n > 0;
            }
            if (n + 1 >= outsz) return false;
            out[n++] = u < 128 ? (char)u : '_';
        }
    }
    out[n] = 0;
    return n > 0;
}

bool lfn_long_name(msd_t *msd, uint32_t part, const char *full_path, char *out, unsigned outsz) {
    geo_t g;
    uint32_t cluster, dummy;
    lfn_list_t run;
    const char *fname;
    char dirp[160];
    size_t dl;

    if (outsz < 2) return false;
    out[0] = 0;
    if (!geo_load(msd, part, &g)) return false;

    fname = strrchr(full_path, '/');
    if (!fname) return false;
    dl = (size_t)(fname - full_path);
    if (dl == 0) dl = 1;
    if (dl >= sizeof(dirp)) return false;
    memcpy(dirp, full_path, dl);
    dirp[dl] = 0;
    fname++;

    if (!resolve_dir(msd, &g, dirp, &cluster)) return false;
    if (!dir_search(msd, &g, cluster, fname, false, &dummy, &run, NULL)) return false;
    if (run.n == 0) return false;
    return decode_run(msd, &run, out, outsz);
}

static bool write_entry(msd_t *msd, const lfn_pos_t *p, const uint8_t *e) {
    if (msd_Read(msd, p->lba, 1, dir_sec) != 1) return false;
    memcpy(dir_sec + p->off, e, ENTRY);
    return msd_Write(msd, p->lba, 1, dir_sec) == 1;
}

/* Writes a new entry into the folder at dir_cluster: the short entry sc, with
 * long-name entries in front of it if long_name is given. Uses a run of free
 * entries anywhere in the folder. */
static bool place_run(msd_t *msd, const geo_t *g, uint32_t dir_cluster,
                      const uint8_t *sc, const char *long_name) {
    lfn_pos_t slots[LFN_MAX_PARTS + 1];
    uint8_t entry[ENTRY];
    cur_t c;
    size_t len = long_name ? strlen(long_name) : 0;
    int chunks = long_name ? (int)((len + 1 + 12) / 13) : 0;   /* characters plus terminator */
    int need = chunks + 1;
    int found = 0, i, j, q;
    uint8_t chk;

    if (long_name && (len == 0 || len > 13 * LFN_MAX_PARTS - 1)) return false;
    chk = lfn_checksum(sc);

    if (!cur_start(&c, msd, g, dir_cluster)) return false;
    for (;;) {
        const uint8_t *e = dir_sec + c.idx * ENTRY;
        if (e[0] == 0x00 || e[0] == DELETED) {
            slots[found].lba = cur_lba(&c);
            slots[found].off = (uint16_t)(c.idx * ENTRY);
            if (++found == need) break;
        } else {
            found = 0;
        }
        if (!cur_advance(&c)) return false;
    }

    /* Long-name entries first, highest part first; part j+1 goes in slot i with
     * j = chunks - 1 - i. The first slot carries the "last part" flag 0x40. */
    for (i = 0; i < chunks; i++) {
        uint16_t u[13];

        j = chunks - 1 - i;
        for (q = 0; q < 13; q++) {
            int ci = j * 13 + q;
            if (ci < (int)len) u[q] = (uint8_t)long_name[ci];
            else if (ci == (int)len) u[q] = 0;
            else u[q] = 0xFFFF;
        }
        memset(entry, 0, ENTRY);
        entry[0] = (uint8_t)((j + 1) | (i == 0 ? 0x40 : 0));
        for (q = 0; q < 5; q++) {
            entry[1 + 2 * q] = (uint8_t)u[q];
            entry[2 + 2 * q] = (uint8_t)(u[q] >> 8);
        }
        entry[11] = ATTR_LFN;
        entry[13] = chk;
        for (q = 0; q < 6; q++) {
            entry[14 + 2 * q] = (uint8_t)u[5 + q];
            entry[15 + 2 * q] = (uint8_t)(u[5 + q] >> 8);
        }
        for (q = 0; q < 2; q++) {
            entry[28 + 2 * q] = (uint8_t)u[11 + q];
            entry[29 + 2 * q] = (uint8_t)(u[11 + q] >> 8);
        }
        if (!write_entry(msd, &slots[i], entry)) return false;
    }

    return write_entry(msd, &slots[chunks], sc);
}

bool lfn_add_long(msd_t *msd, uint32_t part, const char *dir_path, const char *short_name,
                  const char *long_name) {
    geo_t g;
    uint32_t cluster, dummy;
    lfn_pos_t spos;
    uint8_t sc[ENTRY];

    if (!geo_load(msd, part, &g)) return false;
    if (!resolve_dir(msd, &g, dir_path, &cluster)) return false;
    if (!dir_search(msd, &g, cluster, short_name, false, &dummy, NULL, &spos)) return false;
    if (msd_Read(msd, spos.lba, 1, dir_sec) != 1) return false;
    memcpy(sc, dir_sec + spos.off, ENTRY);

    if (!place_run(msd, &g, cluster, sc, long_name)) return false;

    /* The old short entry goes; the copy written above replaces it. */
    if (msd_Read(msd, spos.lba, 1, dir_sec) != 1) return false;
    dir_sec[spos.off] = DELETED;
    return msd_Write(msd, spos.lba, 1, dir_sec) == 1;
}

bool lfn_move_entry(msd_t *msd, uint32_t part, const char *src_path, const char *dst_dir) {
    geo_t g;
    uint32_t src_parent, dst_cluster, dummy, folder_cluster, folder_lba;
    lfn_list_t run;
    lfn_pos_t spos;
    uint8_t sc[ENTRY];
    char dirp[160], long_name[256];
    const char *fname;
    size_t dl, sl;
    bool has_long;

    /* A folder can't go inside itself. */
    sl = strlen(src_path);
    if (strncmp(dst_dir, src_path, sl) == 0 && (dst_dir[sl] == '/' || dst_dir[sl] == 0)) return false;

    fname = strrchr(src_path, '/');
    if (!fname) return false;
    dl = (size_t)(fname - src_path);
    if (dl == 0) dl = 1;
    if (dl >= sizeof(dirp)) return false;
    memcpy(dirp, src_path, dl);
    dirp[dl] = 0;
    fname++;

    if (!geo_load(msd, part, &g)) return false;
    if (!resolve_dir(msd, &g, dirp, &src_parent)) return false;
    if (!dir_search(msd, &g, src_parent, fname, false, &dummy, &run, &spos)) return false;
    if (msd_Read(msd, spos.lba, 1, dir_sec) != 1) return false;
    memcpy(sc, dir_sec + spos.off, ENTRY);
    if (!(sc[11] & ATTR_DIR)) return false;                  /* folders only */

    folder_cluster = (rd16(sc + 20) << 16) | rd16(sc + 26);
    if (folder_cluster < 2 || folder_cluster > g.max_cluster) return false;
    folder_lba = g.data_lba + (folder_cluster - 2) * g.spc;

    /* The folder's own ".." entry must be there to be repointed. */
    if (msd_Read(msd, folder_lba, 1, dir_sec) != 1) return false;
    if (dir_sec[32] != '.' || dir_sec[33] != '.' || dir_sec[34] != ' ') return false;

    if (!resolve_dir(msd, &g, dst_dir, &dst_cluster)) return false;
    if (dir_search(msd, &g, dst_cluster, fname, false, &dummy, NULL, NULL)) return false;   /* taken */

    has_long = run.n > 0 && decode_run(msd, &run, long_name, sizeof(long_name));
    if (!place_run(msd, &g, dst_cluster, sc, has_long ? long_name : NULL)) return false;

    /* Point the folder's ".." at its new parent (cluster 0 means the root).
     * ".." is entry 1 of the folder's first sector: high cluster at bytes 20-21,
     * low cluster at bytes 26-27. Other bytes of the entry are left alone. */
    if (msd_Read(msd, folder_lba, 1, dir_sec) != 1) return false;
    {
        uint32_t parent = (dst_cluster == g.root) ? 0 : dst_cluster;
        dir_sec[32 + 20] = (uint8_t)(parent >> 16);
        dir_sec[32 + 21] = (uint8_t)(parent >> 24);
        dir_sec[32 + 26] = (uint8_t)parent;
        dir_sec[32 + 27] = (uint8_t)(parent >> 8);
    }
    if (msd_Write(msd, folder_lba, 1, dir_sec) != 1) return false;

    /* The source entries go last, so a failure above leaves the folder in place. */
    if (!lfn_mark_deleted(msd, &run)) return false;
    if (msd_Read(msd, spos.lba, 1, dir_sec) != 1) return false;
    dir_sec[spos.off] = DELETED;
    return msd_Write(msd, spos.lba, 1, dir_sec) == 1;
}
