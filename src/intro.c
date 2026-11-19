/* FileR's startup intro.
 *
 * The idea of a centered title drawn letter by letter, with a colored title screen shown
 * for a moment before the app starts, comes from FlowCE (Jonathan Bush, GPL-3.0): its
 * displayTitleScreen() in src/main.c. The motion is FileR's own.
 *
 * Sequence:
 *   1. The top-bar colour fills the screen; the title flies in from the right.
 *   2. The bottom text (the subtitle) flies in from the bottom.
 *   3. Pause.
 *   4. The bottom text flies out (down) and the icon flies in from the right.
 *   5. The screen is drawn ahead of time (into the back buffer, out of sight). The title
 *      moves to its normal spot and the top bar shrinks from the bottom up. The rest of the
 *      screen rides up under the bar, as if the bar were dragging it.
 *
 * Steps 1 to 4 draw into the back buffer and swap it onto the screen. Step 5 copies the
 * pre-drawn screen onto the visible screen, so nothing is stored between frames except the
 * pre-drawn screen itself, which lives in the graphics back buffer.
 *
 * The timing comes from the Intro settings (intro_fps, intro_pause, intro_in, intro_out in
 * filer.h). The durations below are worked out in frames when the intro starts. */

#include <tice.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdbool.h>
#include <stdint.h>
#include "filer.h"
#include "intro.h"

#define TITLE_TEXT  "FileR"
#define SUB_TEXT    "A file manager for your calculator"
#define NLETTERS    5

#define BIG         3          /* title text scale at the start (1 is normal size) */
#define STRIP_W     24         /* hiding strip down the left edge, 24 px wide */

#define ENTRY_X     (SCREEN_W - 1)  /* letters start at the right edge */
#define TITLE_Y     108        /* top of the big title: about the vertical middle */
#define SUB_Y       160        /* where the subtitle stops, below the title */
#define FINAL_X     22         /* title text at its normal spot, after the icon */
#define FINAL_Y     6          /* same y as the title bar's text */
#define TBAR_ICON_X 2          /* folder icon at its normal spot */
#define TBAR_ICON_Y 2

/* Frame counts, set by intro_timing(). */
static int trav;        /* frames one letter takes to fly in (the in time) */
static int stagger;     /* frames between one letter starting and the next */
static int rise;        /* frames the subtitle takes to come up */
static int hold;        /* frames everything stays still (the pause) */
static int outf;        /* frames the subtitle takes to fly out, and the icon to fly in (the out time) */
static int shrinkf;     /* frames the title and the top bar take to shrink */
/* Frame where each part ends. */
static int end_fly, end_rise, end_hold, end_out, end_shrink;

static int land_x[NLETTERS];   /* where each letter stops at the big size, centered */
static int title_x0;           /* left edge of the whole word at the big size */
static bool ui_ready = false;  /* the screen has been drawn ahead of time into the buffer */

/* Turns the Intro settings into frame counts. Tenths of a second times frames a second
 * gives frames; each part is at least one frame, except the pause, which may be none. */
static void intro_timing(void) {
    long fps = intro_fps;

    trav = (int)((long)intro_in * fps / 10);
    if (trav < 1) trav = 1;
    stagger = trav / 4;
    if (stagger < 1) stagger = 1;
    rise = trav * 3 / 4;
    if (rise < 1) rise = 1;
    hold = (int)((long)intro_pause * fps / 10);
    outf = (int)((long)intro_out * fps / 10);
    if (outf < 1) outf = 1;
    shrinkf = outf * 5 / 2;          /* the shrink is 2.5 times the out time */
    if (shrinkf < 2) shrinkf = 2;

    end_fly = (NLETTERS - 1) * stagger + trav;
    end_rise = end_fly + rise;
    end_hold = end_rise + hold;
    end_out = end_hold + outf;
    end_shrink = end_out + shrinkf;
}

/* dist * (u / L) squared, the ease-out used for every move here: fast at first, slowing
 * into the spot. u and L are frames. Done in long so the longer settings can't overflow. */
static int ease(int dist, int u, int L) {
    return (int)((long)dist * u * u / ((long)L * L));
}

/* Draws one character at (x,y) at the given scale, on the given background. */
static void draw_char(char c, int x, int y, uint8_t scale, uint8_t fg, uint8_t bg) {
    gfx_SetTextScale(scale, scale);
    gfx_SetTextFGColor(fg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextXY(x, y);
    gfx_PrintChar(c);
}

/* Draws the title at (x,y) at the given scale, one letter at a time. */
static void draw_word(int x, int y, uint8_t scale, uint8_t fg, uint8_t bg) {
    int i;
    for (i = 0; TITLE_TEXT[i]; i++) {
        draw_char(TITLE_TEXT[i], x, y, scale, fg, bg);
        gfx_SetTextScale(scale, scale);
        x += gfx_GetCharWidth(TITLE_TEXT[i]);
    }
}

/* x of letter i on frame f, or -1 if it has not started yet. Each letter starts at the
 * right edge and eases to its landing spot, so it slows as it arrives. */
static int letter_x(int i, int f) {
    int t = f - i * stagger;
    int u;

    if (t < 0) return -1;
    if (t >= trav) return land_x[i];
    u = trav - t;                                     /* distance left, in frames */
    return land_x[i] + ease(ENTRY_X - land_x[i], u, trav);
}

/* y of the subtitle on frame f, or -1 if it is not on screen. */
static int sub_y(int f) {
    int p;

    if (f < end_fly) return -1;
    if (f < end_rise) {                               /* rises, easing out */
        int u = end_rise - f;
        return SUB_Y + ease(SCREEN_H - SUB_Y, u, rise);
    }
    if (f < end_hold) return SUB_Y;
    if (f < end_out) {                                /* flies out, speeding up */
        p = f - end_hold;
        return SUB_Y + ease(SCREEN_H - SUB_Y, p, outf);
    }
    return -1;
}

/* x of the icon on frame f, or -1 if it is not on screen yet. It flies in from the right
 * during the subtitle's exit, then stays at its normal spot. */
static int icon_x(int f) {
    int v;

    if (f < end_hold) return -1;
    if (f >= end_out) return TBAR_ICON_X;
    v = end_out - f;                                  /* frames left in the fly-in */
    return TBAR_ICON_X + ease(SCREEN_W - TBAR_ICON_X, v, outf);
}

/* Draws the normal screen into the back buffer, out of sight, so step 5 can use it. */
static void render_ui(void) {
    gfx_SetDrawBuffer();
    gfx_SetTextScale(1, 1);   /* the last intro frame may have left the title's big scale set */
    draw_body();
    ui_ready = true;
}

/* Draws the whole intro frame f. */
static void draw_frame(int f) {
    int i;
    int x;
    int y;

    if (f < end_out) {
        /* Steps 1 to 4: the top-bar colour is the background. */
        gfx_SetDrawBuffer();
        gfx_FillScreen(T_TOP);
        for (i = 0; i < NLETTERS; i++) {
            x = letter_x(i, f);
            if (x >= 0) draw_char(TITLE_TEXT[i], x, TITLE_Y, BIG, T_TOPTXT, T_TOP);
        }
        y = sub_y(f);
        if (y >= 0) {
            gfx_SetTextScale(1, 1);
            gfx_SetTextFGColor(T_TOPTXT);
            gfx_SetTextBGColor(T_TOP);
            gfx_PrintStringXY(SUB_TEXT, (SCREEN_W - gfx_GetStringWidth(SUB_TEXT)) / 2, y);
        }
        x = icon_x(f);
        if (x >= 0) gfx_RLETSprite(dirfilled, x, TBAR_ICON_Y);

        /* Hiding strip down the left edge, the top-bar colour. It is drawn last so it covers
         * text that wraps over from the right edge, and it goes away when the title's fly-in
         * ends. */
        if (f < end_fly) {
            gfx_SetColor(T_TOP);
            gfx_FillRectangle(0, 0, STRIP_W, SCREEN_H);
        }
        gfx_SwapDraw();
    } else {
        /* Step 5: the title moves to its normal spot, the scale steps down 3 -> 2 -> 1, and
         * the top bar shrinks from the bottom up to its normal height (easing out). The rest
         * of the pre-drawn screen rides up under the bar. */
        int s = f - end_out;                          /* 0 .. shrinkf-1 */
        int L = shrinkf - 1;
        int u = L - s;                                /* frames left in the shrink */
        int h = TITLE_H + ease(SCREEN_H - TITLE_H, u, L) - ease(9, u, L);  /* bar height */
        uint8_t scale = s < shrinkf / 3 ? 3 : (s < 2 * shrinkf / 3 ? 2 : 1);
        /* Same ease-out as the bar: fast at first, slowing into the spot. */
        int ax = FINAL_X + ease(title_x0 - FINAL_X, u, L);
        int ay = FINAL_Y + ease(TITLE_Y - FINAL_Y, u, L);

        if (!ui_ready) render_ui();

        /* Body of the pre-drawn screen (everything below its own title bar), moved so its
         * top edge sits at the bottom of the top bar. Its own title bar is not copied. */
        gfx_SetDrawScreen();
        /* Rows 0 to TITLE_H-1 of the pre-drawn screen are its own title bar, so the copy starts
         * at row TITLE_H, just below it. */
        gfx_CopyRectangle(gfx_buffer, gfx_screen, 0, TITLE_H, 0, h, SCREEN_W, SCREEN_H - h);

        gfx_SetColor(T_TOP);
        gfx_FillRectangle(0, 0, SCREEN_W, h);
        draw_indicators();
        draw_word(ax, ay, scale, T_TOPTXT, T_TOP);
        gfx_RLETSprite(dirfilled, TBAR_ICON_X, TBAR_ICON_Y);
        gfx_SetDrawBuffer();
    }
}

void intro_run(void) {
    int f;
    int i;
    int fps = intro_fps;
    bool armed = false;     /* true once the key that started FileR has been let go */

    intro_timing();
    ui_ready = false;

    /* Work out where the big title lands: the word is centered, and each letter sits after
     * the ones before it. */
    gfx_SetTextScale(BIG, BIG);
    title_x0 = (SCREEN_W - gfx_GetStringWidth(TITLE_TEXT)) / 2;
    {
        int x = title_x0;
        for (i = 0; i < NLETTERS; i++) {
            land_x[i] = x;
            gfx_SetTextScale(BIG, BIG);
            x += gfx_GetCharWidth(TITLE_TEXT[i]);
        }
    }

    for (f = 0; f < end_shrink; f++) {
        if (!kb_AnyKey()) armed = true;
        if (armed && kb_AnyKey()) break;
        draw_frame(f);
        delay(1000 / fps);
    }

    /* Leave the screen in its final state, whether the intro ran out or was skipped. The
     * main loop draws its own top bar next. */
    draw_frame(end_shrink - 1);
    gfx_SetTextScale(1, 1);
    gfx_SetDrawBuffer();
    while (kb_AnyKey()) {}  /* let go of a skip key before the menu reads the keyboard */
}
