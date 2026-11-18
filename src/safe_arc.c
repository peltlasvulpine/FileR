/* Archiving without the OS garbage-collect prompt on FileR's own screen.
 *
 * Ported from CEAShell (GPL-3.0, Copyright RoccoLox Programs and TIny_Hacker):
 *   asm/utils.s  _asm_utils_willNotGC  -> has_arc_room (asks FindFreeArcSpot if it fits)
 *   utility.c    util_SafeArchive      -> safe_set_archived
 *   asm/utils.s  _asm_utils_arcOnGC    -> safe_run_or_relaunch (clean screen, do it, come back)
 *   asm/apps.s   _asm_apps_executeApp  -> relaunch_app (ported from FileRExec, see below)
 *   asm/apps.s   app search            -> find_self (ported from FileRExec, same as calc.c's list)
 *
 * FileR-specific: the margin added to the size, and the archive-direction only. Unarchiving
 * still goes straight to the OS (see var_info.c). */

#include <tice.h>
#include <fileioc.h>
#include <graphx.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "cache.h"
#include "safe_arc.h"

#define APP_NAME    "FileR"  /* must match NAME in the makefile */
#define ARC_MARGIN  64       /* extra bytes asked of FindFreeArcSpot. Too much only means a relaunch */

static volatile uint32_t arc_need;
static volatile uint8_t  arc_ok;
static volatile uint32_t app_last;
static volatile uint8_t  app_end;
static volatile uint32_t relaunch_addr;

/* ---- finding our own app in flash (copied from FileRExec/main.c) ---- */

static void app_search_start(void) {
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "call $020384\n"        /* ti.ZeroOP1 */
        "ld a, $14\n"           /* ti.AppObj */
        "ld ($0D005F8), a\n"    /* ti.OP1 */
        "pop iy\n"
        "pop ix\n"
        "pop hl\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
}

static void app_search_next(void) {
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "ld a, $0D\n"
        "call $0217C0\n"        /* ti.FindAppCustom */
        "ld (_app_last), de\n"
        "sbc a, a\n"
        "ld (_app_end), a\n"
        "pop iy\n"
        "pop ix\n"
        "pop hl\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
}

/* Our own app's base address in flash, or 0 if it is not found. */
static uint32_t find_self(void) {
    int tries;

    app_search_start();
    for (tries = 0; tries < 64; tries++) {
        app_search_next();
        if (app_end) break;
        {
            const uint8_t *p = (const uint8_t *)(uintptr_t)(app_last & 0xFFFFFF);
            char name[9];
            int i;
            for (i = 0; i < 8; i++) name[i] = (char)p[0x103 + i];
            name[8] = 0;
            if (strncmp(name, APP_NAME, 8) == 0) return app_last & 0xFFFFFF;
        }
    }
    return 0;
}

/* ---- room in the archive (CEAShell's willNotGC) ---- */

/* True if FindFreeArcSpot finds a free spot of need bytes. */
static bool has_arc_room(uint32_t need) {
    arc_need = need;
    arc_ok = 0;
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "ld bc, (_arc_need)\n"
        "call $022078\n"        /* ti.FindFreeArcSpot: NZ when a free spot was found */
        "ld a, 0\n"             /* ld does not touch the flags */
        "jr z, 1f\n"
        "ld a, 1\n"
    "1:\n"
        "ld (_arc_ok), a\n"
        "pop iy\n"
        "pop ix\n"
        "pop hl\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
    return arc_ok != 0;
}

/* ---- relaunch (copied from FileRExec/main.c, which was ported from CEAShell) ---- */

/* Ported from CEAShell's _asm_apps_executeApp. Never returns on success: it jumps into the
 * app's own entry point, which restarts crt0. */
static void relaunch_app(void) {
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push hl\n"
        "push ix\n"
        "push iy\n"
        "ld hl, (_relaunch_addr)\n"   /* 24-bit app base, from find_self() */
        "push hl\n"
        "push hl\n"
    "1:\n"
        "call $002014C\n"             /* ti.GetCSC */
        "or a, a\n"
        "jr nz, 1b\n"                 /* debounce: wait for no key down */
        "ld a, $25\n"
        "ld ($0D02687), a\n"          /* copied from CEAShell; purpose not confirmed */
        "xor a, a\n"
        "ld ($0D00824), a\n"          /* ti.menuCurrent */
        "ld ($0D025A9), a\n"          /* ti.appErr1 */
        "ld ($0D0058D), a\n"          /* ti.kbdGetKy */
        "ld de, $0D1A881\n"           /* ti.userMem */
        "ld hl, ($0D0118C)\n"         /* ti.asm_prgm_size */
        "ex de, hl\n"
        "call $0020590\n"             /* ti.DelMem */
        "or a, a\n"
        "sbc hl, hl\n"
        "ld ($0D0118C), hl\n"
        "ld iy, $0D00080\n"
        "res 4, (iy+$3F)\n"           /* ti.clockFlags, useTokensInString */
        "res 4, (iy+$9)\n"            /* ti.onFlags, onInterrupt */
        "set 0, (iy+$3)\n"            /* ti.graphFlags, graphDraw */
        "call $0021DC4\n"             /* ti.ReloadAppEntryVecs */
        "call $0021148\n"             /* ti.AppSetup */
        "set 4, (iy+$28)\n"           /* ti.APIFlg, appRunning */
        "set 6, (iy+$28)\n"           /* copied from CEAShell; bit not named in my sources */
        "res 0, (iy+$2C)\n"           /* copied from CEAShell; byte not named in my sources */
        "set 0, (iy+$28)\n"           /* ti.APIFlg, appAllowContext */
        "ld hl, $D1787C\n"
        "ld bc, $FFF\n"
        "call $00210DC\n"             /* ti.MemClear */
        "call $0021528\n"             /* ti.ForceFullScreen */
        "call $0020814\n"             /* ti.ClrScrn */
        "call $0020828\n"             /* ti.HomeUp */
        "ld hl, $0D031F6\n"           /* ti.pixelShadow */
        "ld bc, 25200\n"              /* 8400 * 3 */
        "call $00210DC\n"             /* ti.MemClear */
        "call $0020818\n"             /* ti.ClrTxtShd */
        "ld hl, $0D006C0\n"           /* ti.textShadow */
        "ld de, $0D0232D\n"           /* ti.cmdShadow */
        "ld bc, $104\n"
        "ldir\n"
        "call $0021A3C\n"             /* ti.DrawStatusBar */
        "pop de\n"
        "ex (sp), hl\n"
        "push de\n"
        "ld sp, ($0D007FA)\n"         /* ti.onSP: discard our old stack, use the OS baseline */
        "push hl\n"
        "call $002103C\n"             /* ti.ResetStacks */
        "pop hl\n"
        "ld de, $100\n"
        "add hl, de\n"
        "push hl\n"                   /* code base = app base + 0x100 */
        "ex de, hl\n"
        "ld hl, $18\n"
        "add hl, de\n"
        "ld hl, (hl)\n"               /* data-size offset field, relative to code base */
        "add hl, de\n"
        "or a, a\n"
        "sbc hl, de\n"
        "jr z, 2f\n"
        "push hl\n"
        "pop bc\n"
        "ld hl, $15\n"
        "add hl, de\n"
        "ld hl, (hl)\n"               /* data-source offset field */
        "add hl, de\n"
        "ld de, $D1787C\n"
        "ldir\n"
    "2:\n"
        "pop hl\n"
        "push hl\n"
        "pop de\n"
        "ld bc, $1B\n"
        "add hl, bc\n"
        "ld hl, (hl)\n"               /* entry-point offset field */
        "add hl, de\n"
        "jp (hl)\n");                 /* never returns */
}

/* ---- the CEAShell sequence ---- */

/* Free user RAM in bytes, as CEAShell's getFreeRAM: ti.MemChk plus asm_prgm_size. */
static volatile uint32_t free_ram_out;

static uint32_t free_ram(void) {
    free_ram_out = 0;
    asm("push af\n"
        "push bc\n"
        "push de\n"
        "push ix\n"
        "push iy\n"
        "call $0204FC\n"              /* ti.MemChk: HL = free RAM */
        "ld bc, ($0D0118C)\n"         /* ti.asm_prgm_size */
        "add hl, bc\n"
        "ld (_free_ram_out), hl\n"
        "pop iy\n"
        "pop ix\n"
        "pop de\n"
        "pop bc\n"
        "pop af\n");
    return free_ram_out;
}

/* Set while a safe_run_or_relaunch op is running on the clean screen, so the checks inside
 * the op do not send it round again. */
static bool forced = false;

bool safe_var_fits(uint32_t size, bool archived) {
    if (forced) return true;
    if (free_ram() < size + ARC_MARGIN) return false;
    if (archived && !has_arc_room(size + ARC_MARGIN)) return false;
    return true;
}

/* Runs op(ctx) from a clean screen, then relaunches FileR. Never returns when the relaunch
 * works. If FileR cannot be found to come back to, op runs here and its result is returned. */
int safe_run_or_relaunch(safe_op_fn op, void *ctx) {
    int r;

    relaunch_addr = find_self();
    if (relaunch_addr == 0) {
        /* No way back: run it here. forced stops op's own checks from looping. */
        int err;
        forced = true;
        err = os_PushErrorHandler();
        r = err ? err : op(ctx);
        if (!err) os_PopErrorHandler();
        forced = false;
        return r;
    }

    cache_shutdown();                   /* deletes FileR's cache; the relaunch starts it again */
    while (os_GetCSC()) {}              /* keys up before the OS takes the screen, as CEAShell does */
    gfx_End();
    os_ClrLCDFull();
    os_HomeUp();
    os_ClrTxtShd();

    forced = true;
    r = -1;
    if (os_PushErrorHandler() == 0) {   /* an OS error comes back here, not as a reset */
        r = op(ctx);
        os_PopErrorHandler();
    }
    forced = false;
    relaunch_app();                     /* never returns */
    return r;
}

/* Archives the variable, from the clean screen if needed. */
struct arc_args {
    const char *name;
    uint8_t type;
};

static int archive_op(void *p) {
    struct arc_args *a = (struct arc_args *)p;
    uint8_t h = ti_OpenVar(a->name, "r", a->type);
    int ok = 0;
    if (h) { ok = ti_SetArchiveStatus(true, h) ? 1 : 0; ti_Close(h); }
    return ok;
}

bool safe_set_archived(const char *name, uint8_t type, bool to_arc) {
    uint8_t h;
    uint32_t size;
    bool ok = false;

    if (!to_arc) {
        /* Unarchiving needs RAM, not archive space. Not handled here yet. */
        h = ti_OpenVar(name, "r", type);
        if (h) { ok = ti_SetArchiveStatus(false, h); ti_Close(h); }
        return ok;
    }

    h = ti_OpenVar(name, "r", type);
    if (!h) return false;
    size = ti_GetSize(h);
    ti_Close(h);

    if (has_arc_room(size + ARC_MARGIN)) {
        h = ti_OpenVar(name, "r", type);
        if (h) { ok = ti_SetArchiveStatus(true, h); ti_Close(h); }
        return ok;
    }
    {
        struct arc_args a = { name, type };
        return safe_run_or_relaunch(archive_op, &a) != 0;
    }
}
