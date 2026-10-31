/* Route finding for tap-to-walk, on the brick grid of an interior cube. The
 * design, and where this fits, are in TAPMOVE.H.
 *
 * Pure: it knows nothing of the game. The caller says which cells can be stood on
 * through a callback, which is what lets it be tested on invented maps. */

#include "TAPMOVE.H"

#include <string.h>

#define PATH_DIM TAPPATH_GRID_MAX
#define PATH_NODES (PATH_DIM * PATH_DIM)

/* Integer costs of an orthogonal step and a diagonal one (1 : sqrt 2 to 1%). */
#define COST_ORTH 10
#define COST_DIAG 14

/* A node can be pushed again each time a cheaper way to it is found; eight
 * neighbours is the most a node can be relaxed from. */
#define HEAP_MAX (PATH_NODES * 8)

static T_TAP_WALKABLE s_walkable;
static void *s_ctx;
static S32 s_w, s_h;

static U8 s_known[PATH_NODES]; /* 0 = not asked yet, 1 = walkable, 2 = blocked */
static U8 s_state[PATH_NODES]; /* 0 = unseen, 1 = open, 2 = closed */
static S32 s_g[PATH_NODES];
static S32 s_f[PATH_NODES];
static S16 s_parent[PATH_NODES];

static S32 s_heapF[HEAP_MAX];
static S16 s_heapN[HEAP_MAX];
static S32 s_heapLen;

void TapPath_Begin(T_TAP_WALKABLE walkable, void *ctx, S32 gridW, S32 gridH) {
    s_walkable = walkable;
    s_ctx = ctx;
    s_w = (gridW > PATH_DIM) ? PATH_DIM : gridW;
    s_h = (gridH > PATH_DIM) ? PATH_DIM : gridH;
    memset(s_known, 0, sizeof(s_known));
}

/* Walkable, asked of the callback at most once per cell per query. */
static S32 Walkable(S32 x, S32 z) {
    if (x < 0 || z < 0 || x >= s_w || z >= s_h)
        return 0;
    U8 *k = &s_known[z * PATH_DIM + x];
    if (*k == 0)
        *k = s_walkable(s_ctx, x, z) ? 1 : 2;
    return *k == 1;
}

static S32 Abs(S32 v) {
    return v < 0 ? -v : v;
}

/* Octile distance: exact for a grid with no obstacles, never an over-estimate
 * with them, so the first time A* reaches the goal it has the cheapest way. */
static S32 Heuristic(S32 x, S32 z, S32 gx, S32 gz) {
    const S32 dx = Abs(gx - x);
    const S32 dz = Abs(gz - z);
    const S32 lo = (dx < dz) ? dx : dz;
    return COST_ORTH * (dx + dz) - (2 * COST_ORTH - COST_DIAG) * lo;
}

static void HeapPush(S32 f, S16 node) {
    if (s_heapLen >= HEAP_MAX)
        return;
    S32 i = s_heapLen++;
    while (i > 0) {
        const S32 p = (i - 1) / 2;
        if (s_heapF[p] <= f)
            break;
        s_heapF[i] = s_heapF[p];
        s_heapN[i] = s_heapN[p];
        i = p;
    }
    s_heapF[i] = f;
    s_heapN[i] = node;
}

static S16 HeapPop(S32 *f) {
    const S16 top = s_heapN[0];
    *f = s_heapF[0];
    s_heapLen--;
    if (s_heapLen > 0) {
        const S32 lf = s_heapF[s_heapLen];
        const S16 ln = s_heapN[s_heapLen];
        S32 i = 0;
        for (;;) {
            S32 c = 2 * i + 1;
            if (c >= s_heapLen)
                break;
            if (c + 1 < s_heapLen && s_heapF[c + 1] < s_heapF[c])
                c++;
            if (s_heapF[c] >= lf)
                break;
            s_heapF[i] = s_heapF[c];
            s_heapN[i] = s_heapN[c];
            i = c;
        }
        s_heapF[i] = lf;
        s_heapN[i] = ln;
    }
    return top;
}

S32 TapPath_Find(S32 sx, S32 sz, S32 gx, S32 gz, S16 *outX, S16 *outZ, S32 maxOut, S32 *reachedGoal) {
    static const S8 kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const S8 kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    *reachedGoal = 0;
    if (s_walkable == NULL || maxOut < 1 || sx < 0 || sz < 0 || sx >= s_w || sz >= s_h)
        return 0;

    /* A goal off the grid is the nearest cell on it. */
    if (gx < 0)
        gx = 0;
    if (gz < 0)
        gz = 0;
    if (gx >= s_w)
        gx = s_w - 1;
    if (gz >= s_h)
        gz = s_h - 1;

    memset(s_state, 0, sizeof(s_state));
    s_heapLen = 0;

    /* Whoever is asking is standing on the start cell: it is walkable by
       definition, whatever the grid says about the spot they are on (a ramp, a
       doorway, a block they are pushing). */
    const S32 start = sz * PATH_DIM + sx;
    s_known[start] = 1;
    s_g[start] = 0;
    s_parent[start] = -1;
    s_f[start] = Heuristic(sx, sz, gx, gz);
    s_state[start] = 1;
    HeapPush(s_f[start], (S16)start);

    S32 best = start;
    S32 bestH = Heuristic(sx, sz, gx, gz);

    while (s_heapLen > 0) {
        S32 f;
        const S32 n = HeapPop(&f);
        if (s_state[n] == 2 || f > s_f[n])
            continue; /* a stale entry: a cheaper way to this cell was found since */
        s_state[n] = 2;

        const S32 nx = n % PATH_DIM;
        const S32 nz = n / PATH_DIM;

        const S32 h = Heuristic(nx, nz, gx, gz);
        if (h < bestH || (h == bestH && s_g[n] < s_g[best])) {
            best = n;
            bestH = h;
        }
        if (nx == gx && nz == gz) {
            *reachedGoal = 1;
            break;
        }

        for (S32 d = 0; d < 8; d++) {
            const S32 x = nx + kDx[d];
            const S32 z = nz + kDz[d];
            if (!Walkable(x, z))
                continue;
            /* No cutting corners: the hero is wider than a point, so a diagonal
               needs both of the cells it brushes to be free. */
            if (kDx[d] != 0 && kDz[d] != 0 && (!Walkable(nx + kDx[d], nz) || !Walkable(nx, nz + kDz[d])))
                continue;

            const S32 m = z * PATH_DIM + x;
            if (s_state[m] == 2)
                continue;
            const S32 g = s_g[n] + ((kDx[d] != 0 && kDz[d] != 0) ? COST_DIAG : COST_ORTH);
            if (s_state[m] == 0 || g < s_g[m]) {
                s_g[m] = g;
                s_parent[m] = (S16)n;
                s_f[m] = g + Heuristic(x, z, gx, gz);
                s_state[m] = 1;
                HeapPush(s_f[m], (S16)m);
            }
        }
    }

    /* Walk back from the goal -- or, if it cannot be reached, from the reachable
       cell nearest to it -- and lay the route out start first. */
    S32 total = 0;
    for (S32 t = best; t >= 0; t = s_parent[t])
        total++;
    const S32 len = (total > maxOut) ? maxOut : total;

    /* A route longer than the room the caller has keeps its beginning. */
    S32 c = best;
    for (S32 skip = total - len; skip > 0; skip--)
        c = s_parent[c];
    if (total > len)
        *reachedGoal = 0;

    for (S32 i = len - 1; i >= 0; i--) {
        outX[i] = (S16)(c % PATH_DIM);
        outZ[i] = (S16)(c / PATH_DIM);
        c = s_parent[c];
    }
    return len;
}

S32 TapPath_LineClear(S32 ax, S32 az, S32 bx, S32 bz) {
    const S32 dx = Abs(bx - ax);
    const S32 dz = Abs(bz - az);
    const S32 sx = (ax < bx) ? 1 : -1;
    const S32 sz = (az < bz) ? 1 : -1;
    S32 err = dx - dz;
    S32 x = ax;
    S32 z = az;

    for (;;) {
        if (!Walkable(x, z))
            return 0;
        if (x == bx && z == bz)
            return 1;

        const S32 e2 = 2 * err;
        S32 stepX = 0;
        S32 stepZ = 0;
        if (e2 > -dz) {
            err -= dz;
            stepX = sx;
        }
        if (e2 < dx) {
            err += dx;
            stepZ = sz;
        }
        if (stepX != 0 && stepZ != 0 && (!Walkable(x + stepX, z) || !Walkable(x, z + stepZ)))
            return 0;
        x += stepX;
        z += stepZ;
    }
}

S32 TapPath_Smooth(S16 *x, S16 *z, S32 n) {
    if (n <= 2)
        return n;

    S32 kept = 1; /* x[0], z[0] stay where they are */
    S32 i = 0;
    while (i < n - 1) {
        S32 j = n - 1;
        while (j > i + 1 && !TapPath_LineClear(x[i], z[i], x[j], z[j]))
            j--;
        x[kept] = x[j];
        z[kept] = z[j];
        kept++;
        i = j;
    }
    return kept;
}
