/* Host test for tap-to-walk's route finder (SOURCES/TAPMOVE_PATH.CPP).
 *
 * The finder is pure -- the caller says which cells can be stood on -- so it is checked on invented
 * maps: the hand-made ones pin the behaviour a player would notice (round a wall, stop at a wall,
 * never cut a corner), and a sweep over thousands of random grids checks the two claims everything
 * else rests on against a plain Dijkstra: a route that is returned is legal, and one that reaches
 * the goal is the cheapest there is.
 */
#include "TAPMOVE.H"
#include "test_harness.h"

#include <stdlib.h>
#include <string.h>

#define DIM 64

static int g_w, g_h;
static unsigned char g_blocked[DIM][DIM];
static int g_startX, g_startZ;

static void clear_map(int w, int h) {
    g_w = w;
    g_h = h;
    memset(g_blocked, 0, sizeof(g_blocked));
    g_startX = g_startZ = -1;
}

static S32 walkable_cb(void *ctx, S32 x, S32 z) {
    (void)ctx;
    return !g_blocked[z][x];
}

/* What the finder is entitled to assume: the cell the asker stands on is walkable, whatever the
 * grid says about it. The reference below uses the same rule. */
static int walk(int x, int z) {
    if (x < 0 || z < 0 || x >= g_w || z >= g_h)
        return 0;
    if (x == g_startX && z == g_startZ)
        return 1;
    return !g_blocked[z][x];
}

static int step_cost(int dx, int dz) {
    return (dx != 0 && dz != 0) ? 14 : 10;
}

/* The cost of a route, or -1 if any step of it is not a legal one. */
static int route_cost(const S16 *x, const S16 *z, int n) {
    int i, cost = 0;
    for (i = 1; i < n; i++) {
        int dx = x[i] - x[i - 1], dz = z[i] - z[i - 1];
        if (dx < -1 || dx > 1 || dz < -1 || dz > 1 || (dx == 0 && dz == 0))
            return -1;
        if (!walk(x[i], z[i]))
            return -1;
        if (dx != 0 && dz != 0 && (!walk(x[i - 1] + dx, z[i - 1]) || !walk(x[i - 1], z[i - 1] + dz)))
            return -1; /* cut a corner */
        cost += step_cost(dx, dz);
    }
    return cost;
}

/* Reference: cheapest cost from the start to every cell, by relaxing until nothing changes. Slow
 * and obvious, which is the point of it. */
static void dijkstra(int sx, int sz, int dist[DIM][DIM]) {
    static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int DZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int x, z, d, changed = 1;

    for (z = 0; z < DIM; z++)
        for (x = 0; x < DIM; x++)
            dist[z][x] = -1;
    dist[sz][sx] = 0;

    while (changed) {
        changed = 0;
        for (z = 0; z < g_h; z++) {
            for (x = 0; x < g_w; x++) {
                if (dist[z][x] < 0)
                    continue;
                for (d = 0; d < 8; d++) {
                    int nx = x + DX[d], nz = z + DZ[d], c;
                    if (!walk(nx, nz))
                        continue;
                    if (DX[d] != 0 && DZ[d] != 0 && (!walk(x + DX[d], z) || !walk(x, z + DZ[d])))
                        continue;
                    c = dist[z][x] + step_cost(DX[d], DZ[d]);
                    if (dist[nz][nx] < 0 || c < dist[nz][nx]) {
                        dist[nz][nx] = c;
                        changed = 1;
                    }
                }
            }
        }
    }
}

static int octile(int x, int z, int gx, int gz) {
    int dx = abs(gx - x), dz = abs(gz - z), lo = dx < dz ? dx : dz;
    return 10 * (dx + dz) - 6 * lo;
}

static S32 find(int sx, int sz, int gx, int gz, S16 *x, S16 *z, S32 *reached) {
    g_startX = sx;
    g_startZ = sz;
    TapPath_Begin(walkable_cb, NULL, g_w, g_h);
    return TapPath_Find(sx, sz, gx, gz, x, z, 600, reached);
}

static void test_open_ground_route_is_the_cheapest(void) {
    S16 x[600], z[600];
    S32 reached;
    S32 n;

    clear_map(16, 16);
    n = find(2, 2, 9, 5, x, z, &reached);
    /* three diagonals and four straights: 3 * 14 + 4 * 10 */
    ASSERT_EQ_INT(8, n);
    ASSERT_EQ_INT(1, reached);
    ASSERT_EQ_INT(82, route_cost(x, z, n));
    ASSERT_EQ_INT(2, x[0]);
    ASSERT_EQ_INT(2, z[0]);
    ASSERT_EQ_INT(9, x[n - 1]);
    ASSERT_EQ_INT(5, z[n - 1]);
}

static void test_a_wall_with_a_gap_is_gone_round(void) {
    S16 x[600], z[600];
    S32 reached, n;
    int i, through = 0;

    clear_map(16, 16);
    for (i = 0; i < 12; i++)
        g_blocked[i][8] = 1; /* a wall down column 8, open from row 12 */
    n = find(4, 5, 12, 5, x, z, &reached);
    ASSERT_EQ_INT(1, reached);
    for (i = 0; i < n; i++)
        if (x[i] == 8)
            through = (z[i] >= 12);
    ASSERT_EQ_INT(1, through);
    ASSERT_TRUE(route_cost(x, z, n) > 0);
}

static void test_a_goal_inside_a_wall_ends_at_the_nearest_reachable_cell(void) {
    S16 x[600], z[600];
    S32 reached, n;
    int ok;

    clear_map(16, 16);
    g_blocked[6][12] = 1; /* the goal itself is solid */
    n = find(2, 6, 12, 6, x, z, &reached);
    ASSERT_EQ_INT(0, reached);
    /* it walks up to the wall: the cell just short of it, on the near side */
    ok = (x[n - 1] == 11 && z[n - 1] == 6);
    ASSERT_EQ_INT(1, ok);
}

static void test_a_goal_walled_in_is_approached_not_entered(void) {
    S16 x[600], z[600];
    S32 reached, n;
    int dx, dz, ok;

    clear_map(24, 24);
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++)
            if (dx != 0 || dz != 0)
                g_blocked[12 + dz][12 + dx] = 1; /* a ring round (12, 12) */
    n = find(2, 12, 12, 12, x, z, &reached);
    ASSERT_EQ_INT(0, reached);
    /* two cells from the goal along an axis is as close as the ring allows */
    ok = (octile(x[n - 1], z[n - 1], 12, 12) == 20);
    ASSERT_EQ_INT(1, ok);
}

static void test_a_diagonal_never_cuts_a_corner(void) {
    S16 x[600], z[600];
    S32 reached, n;

    /* both neighbours of the diagonal are solid: the goal cannot be got to */
    clear_map(4, 4);
    g_blocked[0][1] = 1;
    g_blocked[1][0] = 1;
    n = find(0, 0, 1, 1, x, z, &reached);
    ASSERT_EQ_INT(0, reached);
    ASSERT_EQ_INT(1, n); /* boxed in: it stays where it is */

    /* one is solid: the diagonal is still refused, the way round the other is taken */
    clear_map(4, 4);
    g_blocked[0][1] = 1;
    n = find(0, 0, 1, 1, x, z, &reached);
    ASSERT_EQ_INT(1, reached);
    ASSERT_EQ_INT(3, n);
    ASSERT_EQ_INT(0, x[1]);
    ASSERT_EQ_INT(1, z[1]);
}

static void test_the_start_cell_is_always_walkable(void) {
    S16 x[600], z[600];
    S32 reached, n;

    clear_map(8, 8);
    g_blocked[3][3] = 1; /* the grid says he is inside a wall (a ramp, a doorway) */
    n = find(3, 3, 6, 3, x, z, &reached);
    ASSERT_EQ_INT(1, reached);
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_INT(3, x[0]);
}

static void test_a_goal_off_the_grid_is_the_nearest_cell_on_it(void) {
    S16 x[600], z[600];
    S32 reached, n;

    clear_map(10, 10);
    n = find(2, 5, 100, 5, x, z, &reached);
    ASSERT_EQ_INT(1, reached);
    ASSERT_EQ_INT(9, x[n - 1]);
    ASSERT_EQ_INT(5, z[n - 1]);
}

static void test_a_route_longer_than_the_room_keeps_its_beginning(void) {
    S16 x[4], z[4];
    S32 reached, n;

    clear_map(16, 16);
    g_startX = 0;
    g_startZ = 0;
    TapPath_Begin(walkable_cb, NULL, 16, 16);
    n = TapPath_Find(0, 0, 10, 0, x, z, 4, &reached);
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_INT(0, reached);
    ASSERT_EQ_INT(0, x[0]);
    ASSERT_EQ_INT(3, x[3]);
}

static void test_smoothing_keeps_the_ends_and_the_corners_that_matter(void) {
    S16 x[600], z[600];
    S32 reached, n, m, i;

    /* open ground: a straight run collapses to its two ends */
    clear_map(16, 16);
    n = find(1, 1, 12, 1, x, z, &reached);
    ASSERT_EQ_INT(12, n);
    m = TapPath_Smooth(x, z, n);
    ASSERT_EQ_INT(2, m);
    ASSERT_EQ_INT(1, x[0]);
    ASSERT_EQ_INT(12, x[1]);

    /* a wall in the way: a corner survives, and every leg is clear */
    clear_map(16, 16);
    for (i = 0; i < 12; i++)
        g_blocked[i][8] = 1;
    n = find(4, 5, 12, 5, x, z, &reached);
    m = TapPath_Smooth(x, z, n);
    ASSERT_TRUE(m >= 3 && m < n);
    ASSERT_EQ_INT(4, x[0]);
    ASSERT_EQ_INT(12, x[m - 1]);
    for (i = 1; i < m; i++)
        ASSERT_TRUE(TapPath_LineClear(x[i - 1], z[i - 1], x[i], z[i]));
}

static void test_unusable_queries_are_refused(void) {
    S16 x[4], z[4];
    S32 reached = 5;

    clear_map(8, 8);
    TapPath_Begin(walkable_cb, NULL, 8, 8);
    ASSERT_EQ_INT(0, TapPath_Find(-1, 0, 3, 3, x, z, 4, &reached));
    ASSERT_EQ_INT(0, reached);
    ASSERT_EQ_INT(0, TapPath_Find(0, 99, 3, 3, x, z, 4, &reached));
    ASSERT_EQ_INT(0, TapPath_Find(0, 0, 3, 3, x, z, 0, &reached));
}

/* The sweep. Random grids from empty to 40% solid, 8x8 up to the 64x64 of a whole cube. */
static unsigned g_seed = 20260622u;
static int rnd(int n) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int)((g_seed >> 8) % (unsigned)n);
}

static void test_routes_match_a_reference_on_random_grids(void) {
    static int dist[DIM][DIM];
    S16 x[600], z[600];
    int iter, bad_route = 0, bad_cost = 0, bad_partial = 0, bad_smooth = 0;

    for (iter = 0; iter < 1500; iter++) {
        int w = (iter % 4 == 0) ? 64 : 8 + rnd(25);
        int h = (iter % 4 == 0) ? 64 : 8 + rnd(25);
        int density = (iter % 11) * 4;
        int i, j, sx, sz, gx, gz;
        S32 reached, n, m;

        clear_map(w, h);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
                g_blocked[j][i] = (rnd(100) < density);
        sx = rnd(w);
        sz = rnd(h);
        gx = rnd(w);
        gz = rnd(h);

        n = find(sx, sz, gx, gz, x, z, &reached);
        if (n < 1 || x[0] != sx || z[0] != sz || route_cost(x, z, n) < 0) {
            bad_route++;
            continue;
        }

        dijkstra(sx, sz, dist);
        if (dist[gz][gx] >= 0 && walk(gx, gz)) {
            if (!reached || x[n - 1] != gx || z[n - 1] != gz || route_cost(x, z, n) != dist[gz][gx])
                bad_cost++;
        } else {
            int best = 1 << 30;
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++)
                    if (dist[j][i] >= 0 && octile(i, j, gx, gz) < best)
                        best = octile(i, j, gx, gz);
            if (reached || octile(x[n - 1], z[n - 1], gx, gz) != best)
                bad_partial++;
        }

        m = TapPath_Smooth(x, z, n);
        if (m < 1 || m > n || x[0] != sx || z[0] != sz)
            bad_smooth++;
        for (i = 1; i < m; i++)
            if (!TapPath_LineClear(x[i - 1], z[i - 1], x[i], z[i]))
                bad_smooth++;
    }

    ASSERT_EQ_INT(0, bad_route);
    ASSERT_EQ_INT(0, bad_cost);
    ASSERT_EQ_INT(0, bad_partial);
    ASSERT_EQ_INT(0, bad_smooth);
}

int main(void) {
    RUN_TEST(test_open_ground_route_is_the_cheapest);
    RUN_TEST(test_a_wall_with_a_gap_is_gone_round);
    RUN_TEST(test_a_goal_inside_a_wall_ends_at_the_nearest_reachable_cell);
    RUN_TEST(test_a_goal_walled_in_is_approached_not_entered);
    RUN_TEST(test_a_diagonal_never_cuts_a_corner);
    RUN_TEST(test_the_start_cell_is_always_walkable);
    RUN_TEST(test_a_goal_off_the_grid_is_the_nearest_cell_on_it);
    RUN_TEST(test_a_route_longer_than_the_room_keeps_its_beginning);
    RUN_TEST(test_smoothing_keeps_the_ends_and_the_corners_that_matter);
    RUN_TEST(test_unusable_queries_are_refused);
    RUN_TEST(test_routes_match_a_reference_on_random_grids);
    TEST_SUMMARY();
    return test_failures != 0;
}
