#ifndef LFN_FIX_H
#define LFN_FIX_H

#include <stdbool.h>
#include <stdint.h>
#include <msddrvce.h>

/* The FAT driver only sees 8.3 names. When it deletes a file it marks the
 * short entry deleted but leaves that file's long-name entries behind. These
 * functions find those leftover entries with raw directory reads and mark
 * them deleted. FAT32 only. */

#define LFN_MAX_PARTS 20   /* 255 characters / 13 per entry, rounded up */

typedef struct {
    uint32_t lba;      /* sector holding the entry */
    uint16_t off;      /* byte offset of the entry inside that sector */
} lfn_pos_t;

typedef struct {
    lfn_pos_t pos[LFN_MAX_PARTS];
    uint8_t n;
} lfn_list_t;

/* Finds the long-name entries in front of the short entry for full_path
 * (for example "/FOLDER/NEWTEX~1.TXT"). Call BEFORE the file is deleted.
 * out->n is 0 if there is no long name. Returns false if the lookup failed. */
bool lfn_find(msd_t *msd, uint32_t part, const char *full_path, lfn_list_t *out);

/* Marks the entries in list as deleted. Call AFTER the delete has been
 * flushed (volume closed and reopened), so the driver's cache can't overwrite it. */
bool lfn_mark_deleted(msd_t *msd, const lfn_list_t *list);

/* Renames a file or folder in place: overwrites its 8.3 name in the directory
 * entry and removes its long-name entries. new_name must be 8.3 ("NAME.EXT").
 * Nothing is copied, so this is fast for big files and works for folders.
 * Returns false if the name is taken or the entry can't be found. */
bool lfn_rename(msd_t *msd, uint32_t part, const char *full_path, const char *new_name);

/* Reads the long name held by the file at full_path into out. Returns false if
 * the file has no long name or it doesn't fit in outsz. */
bool lfn_long_name(msd_t *msd, uint32_t part, const char *full_path, char *out, unsigned outsz);

/* Gives the 8.3 entry short_name in dir_path a long name, by writing its
 * long-name entries into free slots of that folder. Call after the file is
 * written and the volume is flushed. Returns false if there's no room. */
bool lfn_add_long(msd_t *msd, uint32_t part, const char *dir_path, const char *short_name,
                  const char *long_name);

/* Moves the folder at src_path into the folder dst_dir, keeping its name and
 * long name. Raw directory edit: no data is copied, only the entries and the
 * folder's ".." pointer change. Refuses if dst_dir is inside the folder, the
 * name is taken, or src_path isn't a folder. Volume must be closed. */
bool lfn_move_entry(msd_t *msd, uint32_t part, const char *src_path, const char *dst_dir);

/* Recounts the free clusters from the FAT and writes the count into FSInfo.
 * Reads the whole FAT, so call it on unmount, not on every delete. The volume
 * must be closed. */
bool lfn_fix_fsinfo(msd_t *msd, uint32_t part);

#endif
