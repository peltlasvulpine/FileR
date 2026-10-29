#include <tice.h>
#include <graphx.h>
#include <stdbool.h>
#include <stdint.h>
#include "icon/gfx.h"
#include "keys.h"

static bool shift_on = false;   /* 2nd */
static bool alpha_on = false;   /* alpha, for the next key */
static bool alock_on = false;   /* alpha lock, until alpha is pressed again */

typedef struct {
    uint8_t key;
    char ch;
} letter_key_t;

static const letter_key_t letter_keys[] = {
    { sk_Math, 'a' },  { sk_Apps, 'b' },   { sk_Prgm, 'c' },   { sk_Recip, 'd' },
    { sk_Sin, 'e' },   { sk_Cos, 'f' },    { sk_Tan, 'g' },    { sk_Power, 'h' },
    { sk_Square, 'i' },{ sk_Comma, 'j' },  { sk_LParen, 'k' }, { sk_RParen, 'l' },
    { sk_Div, 'm' },   { sk_Log, 'n' },    { sk_7, 'o' },      { sk_8, 'p' },
    { sk_9, 'q' },     { sk_Mul, 'r' },    { sk_Ln, 's' },     { sk_4, 't' },
    { sk_5, 'u' },     { sk_6, 'v' },      { sk_Sub, 'w' },    { sk_Store, 'x' },
    { sk_1, 'y' },     { sk_2, 'z' },
};

void keys_process(uint8_t key) {
    if (key == sk_2nd) {
        shift_on = !shift_on;
        return;
    }
    if (key == sk_Alpha) {
        if (alock_on) {
            alock_on = false;
            alpha_on = false;
            shift_on = false;
        } else if (shift_on) {
            alock_on = true;
            alpha_on = false;
            shift_on = false;
        } else {
            alpha_on = !alpha_on;
        }
        return;
    }
    if (shift_on && alpha_on && !alock_on) {
        shift_on = false;     /* with both on, shift goes first and alpha stays for the next key */
        return;
    }
    /* Any other key uses up 2nd and a one-shot alpha. */
    shift_on = false;
    if (!alock_on) alpha_on = false;
}

char keys_char(uint8_t key) {
    unsigned i;
    for (i = 0; i < sizeof(letter_keys) / sizeof(letter_keys[0]); i++) {
        if (key == letter_keys[i].key) return letter_keys[i].ch;
    }
    return 0;
}

char keys_letter(uint8_t key) {
    if (!alpha_on && !alock_on) return 0;
    return keys_char(key);
}

bool keys_2nd_active(void) {
    return shift_on;
}

bool keys_alpha_active(void) {
    return alpha_on || alock_on;
}

bool keys_alock_active(void) {
    return alock_on;
}

void keys_alpha_off(void) {
    alpha_on = false;
    alock_on = false;
}

/* Icons sit just left of the battery (battery_x), the same size as the battery
 * icon. 2nd is next to the battery; alpha or alpha lock is the slot after it. */
void keys_draw(int battery_x, int y) {
    int gap = 2;
    int x2 = battery_x - gap - batt_width;          /* 2nd */
    int x3 = x2 - gap - batt_width;                 /* alpha / alpha lock */

    if (shift_on) gfx_RLETSprite(shift, x2, y);
    if (alock_on) {
        gfx_RLETSprite(alphalock, x3, y);
    } else if (alpha_on) {
        gfx_RLETSprite(alpha, x3, y);
    }
}
