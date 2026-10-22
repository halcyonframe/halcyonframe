// Host tests for main/shuffle.c: cc -O2 -I main tools/test_shuffle.c main/shuffle.c -o /tmp/test_shuffle && /tmp/test_shuffle
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shuffle.h"

static uint32_t seed = 12345;
static uint32_t rnd(void) { return seed = seed * 1664525u + 1013904223u; } // the test's own entropy

typedef struct { int n, cur; uint32_t seq; play_t *p; } world_t;

static world_t make(int n) { return (world_t) {n, -1, 0, calloc(n + 64, sizeof(play_t))}; }
static int step(world_t *w, order_mode_t m) {
    int i = shuffle_pick(m, w->n, w->p, w->cur, rnd());
    assert(i >= 0 && i < w->n);
    if (w->n > 1) assert(i != w->cur); // never the photo already on the panel
    w->p[i].count++, w->p[i].last = ++w->seq, w->cur = i;
    return i;
}

int main(void) {
    // ---- in order ----
    world_t w = make(5);
    int seq[7];
    for (int k = 0; k < 7; k++) seq[k] = step(&w, ORDER_SEQUENTIAL);
    assert(seq[0] == 0 && seq[1] == 1 && seq[4] == 4 && seq[5] == 0 && seq[6] == 1); // wraps
    w.cur = -1; // power was lost: carry on after the photo shown last (1)
    assert(shuffle_pick(ORDER_SEQUENTIAL, 5, w.p, -1, 1) == 2);
    play_t fresh[5] = {{0}};
    assert(shuffle_pick(ORDER_SEQUENTIAL, 5, fresh, -1, 1) == 0); // nothing shown yet: the first
    assert(shuffle_pick(ORDER_ADVANCED, 1, fresh, -1, 7) == 0 && shuffle_pick(ORDER_SHUFFLE, 1, fresh, 0, 7) == 0);
    puts("in order: ok");

    // ---- plain shuffle: random with replacement, so repeats happen (this is what you were seeing) ----
    const int N = 660, DRAWS = 72, TRIALS = 400;
    double repeats = 0;
    for (int t = 0; t < TRIALS; t++) {
        world_t s = make(N);
        int seen[660] = {0};
        for (int k = 0; k < DRAWS; k++) {
            int i = step(&s, ORDER_SHUFFLE);
            if (seen[i]++) repeats++;
        }
        free(s.p);
    }
    repeats /= TRIALS;
    printf("shuffle: %.1f repeated photos in %d changes of %d (a few days at one an hour); chance predicts about %.1f\n",
           repeats, DRAWS, N, DRAWS * (DRAWS - 1) / 2.0 / N);
    assert(repeats > 2.5 && repeats < 5.5);

    // ---- advanced shuffle ----
    world_t a = make(N);
    int first[660] = {0}, lastpos[660];
    long gap_min = 1 << 30, gap_max = 0, gap_sum = 0, gaps = 0;
    for (int i = 0; i < N; i++) lastpos[i] = -1;
    for (int k = 0; k < 40 * N; k++) {
        int i = step(&a, ORDER_ADVANCED);
        if (k < N) assert(!first[i]++); // round one shows every photo exactly once
        uint32_t lo = UINT32_MAX, hi = 0;
        for (int j = 0; j < N; j++) lo = a.p[j].count < lo ? a.p[j].count : lo, hi = a.p[j].count > hi ? a.p[j].count : hi;
        assert(hi - lo <= 1); // every photo has been shown as often as every other (give or take one)
        if (lastpos[i] >= 0 && k > 5 * N) { // after a few rounds
            long g = k - lastpos[i];
            gap_min = g < gap_min ? g : gap_min, gap_max = g > gap_max ? g : gap_max, gap_sum += g, gaps++;
        }
        lastpos[i] = k;
    }
    printf("advanced: gap between showings of a photo, in changes (library %d): min %ld, mean %.0f, max %ld\n", N, gap_min,
           (double) gap_sum / gaps, gap_max);
    assert(gap_min > N / 3 && gap_max < N * 2); // never soon again, and never left out for long
    for (int j = 0; j < N; j++) assert(a.p[j].count == 40 || a.p[j].count == 40 + 0); // 40 rounds: exactly equal
    free(a.p);

    // new photos jump in at once; deleted ones just vanish from the library
    world_t b = make(N);
    for (int k = 0; k < 3 * N + 100; k++) step(&b, ORDER_ADVANCED);
    b.n += 10; // ten new photos: count 0, never shown
    int shown_new = 0;
    for (int k = 0; k < 10; k++) shown_new += step(&b, ORDER_ADVANCED) >= N;
    printf("advanced: %d of 10 newly added photos shown in the next 10 changes\n", shown_new);
    assert(shown_new == 10);
    b.n -= 100; // 100 photos removed from the end
    for (int k = 0; k < 500; k++) step(&b, ORDER_ADVANCED);
    puts("tests passed");
    return 0;
}
