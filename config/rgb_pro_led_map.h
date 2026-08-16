/*
 * FlatCat Split — LED map for zmk-pro-rgb.
 *
 * Two identical FlatCat PCBs side by side: 79 keys each, 158 total.
 * Both halves share the same serpentine LED wiring, so key_to_led is
 * identical. Only the visual columns differ — the right half continues at
 * column 14 so left-to-right effects sweep across both halves.
 *
 * Key positions are LOCAL to each half (0-78 on both sides).
 */

#pragma once
#include <zephyr/kernel.h>

#define LED_LAYOUT(                                                          \
    k00, k01, k02, k03, k04, k05, k06, k07, k08, k09, k10, k11, k12, k13,   \
    k14, k15, k16, k17, k18, k19, k20, k21, k22, k23, k24, k25, k26, k27,   \
    k28, k29, k30, k31, k32, k33, k34, k35, k36, k37, k38, k39, k40, k41,   \
    k42, k43, k44, k45, k46, k47, k48, k49, k50, k51, k52, k53, k54, k55,   \
    k56, k57, k58, k59, k60, k61, k62, k63, k64, k65, k66, k67, k68, k69,   \
    k70, k71, k72,            k73,            k74, k75, k76, k77, k78        \
) {                                                                          \
    k00, k01, k02, k03, k04, k05, k06, k07, k08, k09, k10, k11, k12, k13,   \
    k14, k15, k16, k17, k18, k19, k20, k21, k22, k23, k24, k25, k26, k27,   \
    k28, k29, k30, k31, k32, k33, k34, k35, k36, k37, k38, k39, k40, k41,   \
    k42, k43, k44, k45, k46, k47, k48, k49, k50, k51, k52, k53, k54, k55,   \
    k56, k57, k58, k59, k60, k61, k62, k63, k64, k65, k66, k67, k68, k69,   \
    k70, k71, k72, k73, k74, k75, k76, k77, k78                              \
}

/* Both halves carry the full 79-LED chain and the same serpentine wiring,
   so key_to_led is identical. Only the visual columns differ: the right half
   continues at column 14 so left-to-right effects sweep across both boards.

   Note this table covers all 79 LED positions on both sides. The keymap only
   wires up the right half's first 7 columns — that limit is about how many
   key positions ZMK's split protocol can carry, not about LEDs. Every LED
   still animates. */

const uint8_t key_to_led[] = LED_LAYOUT(
     22,  21,  20,  19,  18,  17,  16,  15,  14,  13,  12,  11,  10,   9,
     23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,   8,
     48,  47,  46,  45,  44,  43,  42,  41,  40,  39,  38,  37,  36,   7,
     49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,   6,
     74,  73,  72,  71,  70,  69,  68,  67,  66,  65,  64,  63,  62,   5,
     75,  76,  77,  78,   0,   1,   2,   3,   4
);

const uint8_t key_row[] = LED_LAYOUT(
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,
      4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      5,   5,   5,   5,   5,   5,   5,   5,   5
);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Left half: columns 0-13 */
const uint8_t key_col[] = LED_LAYOUT(
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,
      0,   1,   2,   6,   9,  10,  11,  12,  13
);
#else
/* Right half: columns 14-27 */
const uint8_t key_col[] = LED_LAYOUT(
     14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
     14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
     14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
     14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
     14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,
     14,  15,  16,  20,  23,  24,  25,  26,  27
);
#endif
