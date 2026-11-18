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
 * pre-drawn screen itself, which lives in the graphics back buffer. */

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
#define STAGGER     4          /* frames between one letter starting to fly and the next */
#define TRAVEL      16         /* frames one letter takes to fly in */
#define RISE        12         /* frames the subtitle takes to come up */
#define HOLD        33         /* frames both stay still (33 x 30 ms, about the old 1 second) */
#define OUT         12         /* frames the subtitle takes to fly out, and the icon to fly in */
#define SHRINK      30         /* frames the title takes to move up and the bar to shrink */
#define FRAME_MS    30         /* delay between frames */
#define STRIP_W     24         /* hiding strip down the left edge, 24 px wide */

#define ENTRY_X     (SCREEN_W - 1)  /* letters start at the right edge */
#define TITLE_Y     108        /* top of the big title: about the vertical middle */
#define SUB_Y       160        /* where the subtitle stops, below the title */
#define FINAL_X     22         /* title text at its normal spot, after the icon */
#define FINAL_Y     6          /* same y as the title bar's text */
#define TBAR_ICON_X 2          /* folder icon at its normal spot */
#define TBAR_ICON_Y 2

/* Frame where each part ends. */
#define END_FLY     ((NLETTERS - 1) * STAGGER + TRAVEL)
#define END_RISE    (END_FLY + RISE)
#define END_HOLD    (END_RISE + HOLD)
#define END_OUT     (END_HOLD + OUT)
#define END_SHRINK  (END_OUT + SHRINK)

static int land_x[NLETTERS];   /* where each letter stops at the big size, centered */
static int title_x0;           /* left edge of the whole word at the big size */
static bool ui_ready = false;  /* the screen has been drawn ahead of time into the buffer */

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
    int t = f - i * STAGGER;
    int u;

    if (t < 0) return -1;
    if (t >= TRAVEL) return land_x[i];
    u = TRAVEL - t;                                   /* distance left, in frames */
    return land_x[i] + (ENTRY_X - land_x[i]) * u * u / (TRAVEL * TRAVEL);
}

/* y of the subtitle on frame f, or -1 if it is not on screen. */
static int sub_y(int f) {
    int p;

    if (f < END_FLY) return -1;
    if (f < END_RISE) {                               /* rises, easing out */
        int u = END_RISE - f;
        return SUB_Y + (SCREEN_H - SUB_Y) * u * u / (RISE * RISE);
    }
    if (f < END_HOLD) return SUB_Y;
    if (f < END_OUT) {                                /* flies out, speeding up */
        p = f - END_HOLD;
        return SUB_Y + (SCREEN_H - SUB_Y) * p * p / (OUT * OUT);
    }
    return -1;
}

/* x of the icon on frame f, or -1 if it is not on screen yet. It flies in from the right
 * during the subtitle's exit, then stays at its normal spot. */
static int icon_x(int f) {
    int v;

    if (f < END_HOLD) return -1;
    if (f >= END_OUT) return TBAR_ICON_X;
    v = END_OUT - f;                                  /* frames left in the fly-in */
    return TBAR_ICON_X + (SCREEN_W - TBAR_ICON_X) * v * v / (OUT * OUT);
}

/* Draws the normal screen into the back buffer, out of sight, so step 5 can use it. */
static void render_ui(void) {
    gfx_SetDrawBuffer();
    draw_body();
    ui_ready = true;
}

/* Draws the whole intro frame f. */
static void draw_frame(int f) {
    int i;
    int x;
    int y;

    if (f < END_OUT) {
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
        if (f < END_FLY) {
            gfx_SetColor(T_TOP);
            gfx_FillRectangle(0, 0, STRIP_W, SCREEN_H);
        }
        gfx_SwapDraw();
    } else {
        /* Step 5: the title moves to its normal spot, the scale steps down 3 -> 2 -> 1, and
         * the top bar shrinks from the bottom up to its normal height (easing out). The rest
         * of the pre-drawn screen rides up under the bar. */
        int s = f - END_OUT;                          /* 0 .. SHRINK-1 */
        int L = SHRINK - 1;
        int u = L - s;                                /* frames left in the shrink */
        int h = TITLE_H + (SCREEN_H - TITLE_H) * u * u / (L * L);  /* bar height */
        uint8_t scale = s < SHRINK / 3 ? 3 : (s < 2 * SHRINK / 3 ? 2 : 1);
        /* Same ease-out as the bar: fast at first, slowing into the spot. */
        int ax = FINAL_X + (title_x0 - FINAL_X) * u * u / (L * L);
        int ay = FINAL_Y + (TITLE_Y - FINAL_Y) * u * u / (L * L);

        if (!ui_ready) render_ui();

        /* Body of the pre-drawn screen (everything below its own title bar), moved so its
         * top edge sits at the bottom of our bar. Its own title bar is not copied. */
        gfx_SetDrawScreen();
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
    bool armed = false;     /* true once the key that started FileR has been let go */

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

    for (f = 0; f < END_SHRINK; f++) {
        if (!kb_AnyKey()) armed = true;
        if (armed && kb_AnyKey()) break;
        draw_frame(f);
        delay(FRAME_MS);
    }

    /* Leave the screen in its final state, whether the intro ran out or was skipped. The
     * main loop draws its own top bar next. */
    draw_frame(END_SHRINK - 1);
    gfx_SetTextScale(1, 1);
    gfx_SetDrawBuffer();
    while (kb_AnyKey()) {}  /* let go of a skip key before the menu reads the keyboard */
}
