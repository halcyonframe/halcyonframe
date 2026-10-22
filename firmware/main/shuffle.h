// Which photo comes next. Plain C with no ESP-IDF dependencies, so it is tested on a PC (tools/test_shuffle.c).
#pragma once
#include <stdint.h>

typedef enum { ORDER_SEQUENTIAL = 0, ORDER_SHUFFLE = 1, ORDER_ADVANCED = 2 } order_mode_t;

// What the frame remembers about each photo.
typedef struct {
    uint32_t count; // times shown
    uint32_t last;  // play sequence number of the latest showing (higher = more recent; 0 = never shown)
} play_t;

// Index of the photo to show next, out of n (sorted in upload order; n >= 1). cur is the index of the photo on the
// panel now, or -1 if unknown (deleted, or the frame lost power). rnd is any 32 bits that differ each call.
//
// ORDER_SEQUENTIAL: the next in upload order, wrapping. With cur unknown, continues after the most recently shown.
// ORDER_SHUFFLE:    a random photo other than cur; may repeat soon, like drawing with replacement.
// ORDER_ADVANCED:   only photos shown the fewest times are candidates, so every photo is shown as often as every
//                   other. Of those, a random one of the least recently shown tenth of the library, so a photo
//                   comes back about one round later, spread evenly, in a slightly different order each round.
int shuffle_pick(order_mode_t mode, int n, const play_t *p, int cur, uint32_t rnd);
