#include "shuffle.h"
#include <stdlib.h>

static uint32_t next_rand(uint32_t *s) { // xorshift32: plenty for picking photos
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return *s = x;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    return x < y ? -1 : x > y;
}

int shuffle_pick(order_mode_t mode, int n, const play_t *p, int cur, uint32_t rnd) {
    if (n <= 1) return 0;
    uint32_t s = rnd * 2654435761u + 0x7F4A7C15u; // spread entropy that only lives in a few bits
    next_rand(&s), next_rand(&s);

    if (mode == ORDER_SEQUENTIAL) {
        if (cur >= 0) return (cur + 1) % n;
        int best = -1;
        for (int i = 0; i < n; i++)
            if (p[i].last && (best < 0 || p[i].last > p[best].last)) best = i;
        return best < 0 ? 0 : (best + 1) % n;
    }

    if (mode == ORDER_SHUFFLE) {
        int pick = (int) (next_rand(&s) % (uint32_t) (n - 1)); // among the n-1 others
        return cur >= 0 && pick >= cur ? pick + 1 : pick;
    }

    // Advanced: the fewest-shown group...
    uint32_t min = UINT32_MAX;
    for (int i = 0; i < n; i++)
        if (i != cur && p[i].count < min) min = p[i].count;
    int pool = 0;
    for (int i = 0; i < n; i++)
        if (i != cur && p[i].count == min) pool++;
    // ...and of it the least recently shown tenth of the library (at least one photo, at most the whole group).
    int window = (n + 9) / 10;
    if (window > pool) window = pool;
    uint32_t *last = malloc(pool * sizeof(uint32_t));
    if (!last) return cur >= 0 ? (cur + 1) % n : 0; // out of memory: plain order beats no photo
    int k = 0;
    for (int i = 0; i < n; i++)
        if (i != cur && p[i].count == min) last[k++] = p[i].last;
    qsort(last, pool, sizeof(uint32_t), cmp_u32);
    uint32_t limit = last[window - 1]; // the window-th smallest "last"
    free(last);
    int eligible = 0;
    for (int i = 0; i < n; i++)
        if (i != cur && p[i].count == min && p[i].last <= limit) eligible++;
    int want = (int) (next_rand(&s) % (uint32_t) eligible);
    for (int i = 0; i < n; i++)
        if (i != cur && p[i].count == min && p[i].last <= limit && want-- == 0) return i;
    return 0; // not reached
}
