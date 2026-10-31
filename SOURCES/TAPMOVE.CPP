/* Tap-to-walk, the half that touches the game. The design, and the reasons for
 * it, are in TAPMOVE.H; the pure maths is in TAPMOVE_MATH.CPP. */

#include "TAPMOVE.H"

#include "C_EXTERN.H"
#include "CONTROL.H"
#include "GRILLE.H"
#include "INPUT.H"
#include "RECORD.H"

#include <3D/GETANG2D.H>
#include <PLATFORM/PLATFORM.H>
#include <SVGA/SCREEN.H>
#include <SYSTEM/TIMER.H>

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How close counts as arrived: a third of a brick. Tight enough that he stops
 * where the finger was, loose enough that a hero who cannot quite reach the exact
 * point (a narrow gap, a ledge) is not left walking on the spot. */
#define TAP_ARRIVE_RADIUS 170

/* A waypoint on the way is passed at this distance: a little over half a brick, so
 * the hero rounds a corner instead of walking up to its exact centre and then
 * having to turn on the spot. */
#define TAP_WAYPOINT_RADIUS 260

/* While a finger is held and drags the destination across the floor, the route is
 * worked out again when it enters another cell, but no more often than this (game
 * clock, ms). */
#define TAP_REPLAN_MS 150

/* The most waypoints one route keeps. A cube is 64 x 64 bricks and smoothing
 * leaves a handful of corners, so this is a ceiling, not an expectation. */
#define TAP_MAX_WP 160

/* A tap further than this from the hero is pulled back along the line to it
 * (about 47 bricks: past anything that is on screen with the camera zoomed out). */
#define TAP_MAX_DIST 24000

/* No gameplay frame for this long (wall clock) means a modal screen, a pause or a
 * load was up. The walk is dropped on the way back rather than resumed: the
 * player's intent was about a scene that has since changed. */
#define TAP_RESUME_GAP_MS 250

/* Blocked detection, on the game clock: over a window in which the hero spent most
 * of the time holding forward, he must have covered at least MIN_MOVE units, or he
 * is blocked. The route is then planned again from where he stands (the grid can be
 * wrong about a door, a pushed block, somebody standing in the way). The walk is
 * not dropped: the finger is still down, and the player can see he is stuck. */
#define TAP_STUCK_WINDOW_MS 450
#define TAP_STUCK_MIN_MOVE 70

typedef struct {
    S32 active;  /* a destination is set */
    S32 down;    /* the finger is on the glass: the walk lasts exactly as long as this */
    S32 fresh;   /* a touch has arrived since the destination was last worked out */
    S32 restart; /* the next resolve starts a new order: reset the timers */

    float nx, ny; /* the finger, game-normalised */

    S32 tx, ty, tz; /* the tapped spot, world */
    S32 turn;       /* TapMove_Aim's memory of last frame's turn */
    S32 turnRate;   /* the turn speed chosen this frame (TapMove_TurnRate); 0 = none */

    S32 wpX[TAP_MAX_WP]; /* the route: waypoints, world; the last is the destination */
    S32 wpZ[TAP_MAX_WP];
    S32 wpN, wpI;       /* how many, and which one the hero is heading for */
    S32 planned;        /* wpX/wpZ hold a route for the current destination */
    S32 replan;         /* plan again at the next opportunity */
    S32 partial;        /* the route ends short of the tapped spot (it is walled off) */
    S32 planCx, planCz; /* the destination's cell when the route was planned */
    U32 planMs;         /* game clock when it was planned */

    U32 sampleMs;   /* game clock: start of the current blocked-detection window */
    U32 lastGameMs; /* game clock at the previous TapMove_Steer */
    S32 sampleX, sampleZ;
    S32 walkedMs; /* time spent holding forward in this window */

    U32 lastFrameMs; /* wall clock at the previous TapMove_Steer; 0 = never */

    S32 markerOk;
    S32 markerX, markerY;
} T_TAPMOVE;

static T_TAPMOVE s_tap;

S32 TouchTapToMove = 1;

static U32 WallMs(void) {
    return (U32)g_platform.system.ticks_ms();
}

/* Gameplay is running if the main loop has asked us for steering recently. Every
 * modal screen runs its own inner loop and never gets here, so the absence of
 * calls is the signal -- no per-screen mode stack needed. */
static S32 GameplayRunning(void) {
    return s_tap.lastFrameMs != 0 && (U32)(WallMs() - s_tap.lastFrameMs) <= TAP_RESUME_GAP_MS;
}

S32 TapMove_IsEnabled(void) {
    return TouchTapToMove ? 1 : 0;
}

S32 TapMove_GameplayRunning(void) {
    return GameplayRunning();
}

S32 TapMove_ReplacesDpad(void) {
    return TapMove_IsEnabled() && GameplayRunning();
}

void TapMove_Cancel(void) {
    s_tap.active = 0;
    s_tap.down = 0;
    s_tap.fresh = 0;
    s_tap.restart = 0;
    s_tap.turn = 0;
    s_tap.turnRate = 0;
    s_tap.walkedMs = 0;
    s_tap.planned = 0;
    s_tap.replan = 0;
    s_tap.wpN = 0;
    s_tap.wpI = 0;
    s_tap.markerOk = 0;
}

S32 TapMove_Touch(float nx, float ny, S32 phase) {
    if (!TapMove_IsEnabled())
        return 0;

    switch (phase) {
    case TAPMOVE_FINGER_DOWN:
        /* A touch on a dialogue, a menu or a loading screen is not a walk order:
           holding it would send the hero off the moment the screen closed. */
        if (!GameplayRunning())
            return 0;
        s_tap.nx = nx;
        s_tap.ny = ny;
        s_tap.down = 1;
        s_tap.fresh = 1;
        s_tap.restart = 1;
        return 1;

    case TAPMOVE_FINGER_MOVE:
        if (!s_tap.down)
            return 0;
        s_tap.nx = nx;
        s_tap.ny = ny;
        s_tap.fresh = 1;
        return 1;

    case TAPMOVE_FINGER_UP:
        if (!s_tap.down)
            return 0;
        /* Let go and he stops. The walk belongs to the finger; it does not outlive it. */
        TapMove_Cancel();
        return 1;
    }
    return 0;
}

S32 TapMove_GetMarker(S32 *sx, S32 *sy) {
    if (!s_tap.active || !s_tap.markerOk)
        return 0;
    *sx = s_tap.markerX;
    *sy = s_tap.markerY;
    return 1;
}

/* The finger -> a point on the floor at the hero's feet, through the camera the
 * frame was drawn with. Returns 0 for a tap that is not on the ground (the sky,
 * behind the camera). */
static S32 ResolveFinger(const T_OBJET *hero, S32 *gx, S32 *gz) {
    if (ModeDesiredX == 0 || ModeDesiredY == 0)
        return 0;

    const S32 px = (S32)(s_tap.nx * (float)ModeDesiredX + 0.5f);
    const S32 py = (S32)(s_tap.ny * (float)ModeDesiredY + 0.5f);

    if (!TapMove_SolveGround(px, py, hero->Obj.Y, hero->Obj.X, hero->Obj.Z, gx, gz))
        return 0;

    const double dx = (double)(*gx - hero->Obj.X);
    const double dz = (double)(*gz - hero->Obj.Z);
    const double dist = sqrt(dx * dx + dz * dz);
    if (dist > (double)TAP_MAX_DIST) {
        *gx = hero->Obj.X + (S32)(dx * (double)TAP_MAX_DIST / dist);
        *gz = hero->Obj.Z + (S32)(dz * (double)TAP_MAX_DIST / dist);
    }
    return 1;
}

/* The ring goes where the route ends: the tapped spot, or the nearest the hero can
 * get to it when that is walled off. */
static void UpdateMarker(void) {
    if (s_tap.wpN < 1) {
        s_tap.markerOk = 0;
        return;
    }
    s_tap.markerOk = TapMove_Project(s_tap.wpX[s_tap.wpN - 1], s_tap.ty, s_tap.wpZ[s_tap.wpN - 1], &s_tap.markerX,
                                     &s_tap.markerY)
                         ? 1
                         : 0;
}

static S64 DistSq(S32 dx, S32 dz) {
    return (S64)dx * (S64)dx + (S64)dz * (S64)dz;
}

/* ---- Routes -------------------------------------------------------------- */

typedef struct {
    S32 y;    /* the hero's feet: the floor the route is on */
    S32 ymax; /* his height: what has to be clear above it */
} T_WALK_CTX;

/* Can the hero stand in this cell of the grid? The same two questions the
 * engine asks when it moves him: is the column at his height free (a result of
 * 1 is a solid brick; ramps and stairs are other codes and can be walked), and is
 * there a brick under his feet to stand on, so a route does not lead off a ledge or
 * across a hole. */
static S32 CellWalkable(void *vctx, S32 cx, S32 cz) {
    const T_WALK_CTX *c = (const T_WALK_CTX *)vctx;
    const S32 x = cx * SIZE_BRICK_XZ + SIZE_BRICK_XZ / 2;
    const S32 z = cz * SIZE_BRICK_XZ + SIZE_BRICK_XZ / 2;

    /* The probes leave the cell they looked at in these. */
    const S32 saveX = XMap, saveY = YMap, saveZ = ZMap;

    S32 ok = (WorldColBrickFull(x, c->y, z, c->ymax) != 1);
    if (ok && c->y > 0)
        ok = (WorldColBrick(x, c->y - 1, z) != 0);

    XMap = saveX;
    YMap = saveY;
    ZMap = saveZ;
    return ok;
}

static S32 ClampCell(S32 v) {
    return v < 0 ? 0 : (v >= SIZE_CUBE_X ? SIZE_CUBE_X - 1 : v);
}

/* Fill the route from the hero to the tapped spot, round whatever is in between.
 * Returns the number of waypoints, or 0 when there is nothing better than walking
 * straight at it: not an interior (an island is not on the brick grid), the hero
 * and the spot in one cell, or no usable route. */
static S32 BuildRoute(const T_OBJET *hero) {
    if (CubeMode != CUBE_INTERIEUR)
        return 0;

    const S32 sx = hero->Obj.X >> 9;
    const S32 sz = hero->Obj.Z >> 9;
    if (sx < 0 || sx >= SIZE_CUBE_X || sz < 0 || sz >= SIZE_CUBE_Z)
        return 0;

    const S32 gx = ClampCell(s_tap.tx >> 9);
    const S32 gz = ClampCell(s_tap.tz >> 9);
    if (sx == gx && sz == gz)
        return 0;

    T_WALK_CTX ctx;
    ctx.y = hero->Obj.Y;
    ctx.ymax = hero->YMax;

    S16 cx[TAP_MAX_WP], cz[TAP_MAX_WP];
    S32 reached = 0;
    TapPath_Begin(CellWalkable, &ctx, SIZE_CUBE_X, SIZE_CUBE_Z);
    S32 n = TapPath_Find(sx, sz, gx, gz, cx, cz, TAP_MAX_WP, &reached);
    if (n < 2)
        return 0;
    n = TapPath_Smooth(cx, cz, n);

    /* The hero is already in the first cell: the route is the ones after it. */
    S32 k = 0;
    for (S32 i = 1; i < n && k < TAP_MAX_WP; i++) {
        s_tap.wpX[k] = cx[i] * SIZE_BRICK_XZ + SIZE_BRICK_XZ / 2;
        s_tap.wpZ[k] = cz[i] * SIZE_BRICK_XZ + SIZE_BRICK_XZ / 2;
        k++;
    }

    s_tap.partial = reached ? 0 : 1;
    if (reached && (s_tap.tx >> 9) == gx && (s_tap.tz >> 9) == gz) {
        /* End on the spot that was touched, not the middle of its cell. */
        s_tap.wpX[k - 1] = s_tap.tx;
        s_tap.wpZ[k - 1] = s_tap.tz;
    }
    return k;
}

static void PlanRoute(const T_OBJET *hero, U32 gameNow) {
    s_tap.partial = 0;
    S32 n = BuildRoute(hero);
    if (n == 0) {
        s_tap.wpX[0] = s_tap.tx;
        s_tap.wpZ[0] = s_tap.tz;
        n = 1;
        s_tap.partial = 0;
    }
    s_tap.wpN = n;
    s_tap.wpI = 0;
    s_tap.planned = 1;
    s_tap.replan = 0;
    s_tap.planCx = s_tap.tx >> 9;
    s_tap.planCz = s_tap.tz >> 9;
    s_tap.planMs = gameNow;
}

S32 TapMove_TurnRate(S32 keyRate) {
    /* Set by TapMove_Steer in the frame whose turn bits it injected, cleared at the top of the next
       one: a turn the player makes on a key or a pad never sees it. */
    return (s_tap.turnRate > 0) ? s_tap.turnRate : keyRate;
}

U32 TapMove_Steer(void) {
    s_tap.turnRate = 0;
    const U32 wallNow = WallMs();
    const S32 resumed = s_tap.lastFrameMs != 0 && (U32)(wallNow - s_tap.lastFrameMs) > TAP_RESUME_GAP_MS;
    s_tap.lastFrameMs = wallNow;

    const U32 gameNow = TimerRefHR;
    const S32 frameMs = (S32)(gameNow - s_tap.lastGameMs);
    s_tap.lastGameMs = gameNow;

    /* Back from a modal screen, a pause or a load. */
    if (resumed)
        TapMove_Cancel();

    /* A replay already carries the bits this produced, and the scripted harness
       drives the hero itself; adding ours would double-drive him. Demo reels are
       not playable. */
    if (!TapMove_IsEnabled() || DemoSlide || Control_IsActive() || Record_IsReplaying()) {
        TapMove_Cancel();
        return 0;
    }

    const T_OBJET *hero = &ListObjet[NUM_PERSO];

    /* Only while the player really has the hero: a cutscene or a script moves him
       on a track, and a dead hero walks nowhere. */
    if (hero->Move != MOVE_MANUAL || hero->Obj.Body.Num == -1 || hero->LifePoint <= 0) {
        TapMove_Cancel();
        return 0;
    }

    /* No finger, no walk. */
    if (!s_tap.down) {
        TapMove_Cancel();
        return 0;
    }

    /* Real movement input (keyboard, pad) always wins over a walk order. Input
       holds only what was read from real devices so far this frame. */
    if (Input & I_JOY) {
        TapMove_Cancel();
        return 0;
    }

    /* Work out where the finger is. While it is down this runs every frame, not
       only when it moves: the camera follows the hero, so a finger that does not
       move still points at a different piece of floor from one frame to the next. */
    if (s_tap.fresh || s_tap.down) {
        S32 gx, gz;
        s_tap.fresh = 0;
        if (ResolveFinger(hero, &gx, &gz)) {
            s_tap.tx = gx;
            s_tap.tz = gz;
            s_tap.ty = hero->Obj.Y;
            if (!s_tap.active || s_tap.restart) {
                s_tap.active = 1;
                s_tap.planned = 0; /* a new order: a new route */
                s_tap.turn = 0;
                s_tap.walkedMs = 0;
                s_tap.sampleMs = gameNow;
                s_tap.sampleX = hero->Obj.X;
                s_tap.sampleZ = hero->Obj.Z;
            }
            s_tap.restart = 0;
        }
        /* A finger that is not on the ground (the sky, behind the camera) gives no
           destination yet. It is still followed: sliding it onto the floor starts the walk. */
    }

    if (!s_tap.active)
        return 0;

    /* Plan the route to where the finger points, or plan it again: the finger
       moved to another cell (a held finger drags the destination about), or the
       hero was found to be blocked. Inside the same cell the last waypoint just
       follows the finger. */
    {
        const S32 moved = (s_tap.tx >> 9) != s_tap.planCx || (s_tap.tz >> 9) != s_tap.planCz;
        if (!s_tap.planned || s_tap.replan || (moved && (U32)(gameNow - s_tap.planMs) >= (U32)TAP_REPLAN_MS)) {
            PlanRoute(hero, gameNow);
        } else if (!s_tap.partial && !moved && s_tap.wpN > 0) {
            s_tap.wpX[s_tap.wpN - 1] = s_tap.tx;
            s_tap.wpZ[s_tap.wpN - 1] = s_tap.tz;
        }
    }

    /* Pass the waypoints already reached; the last one is the destination. */
    while (s_tap.wpI < s_tap.wpN - 1 &&
           DistSq(s_tap.wpX[s_tap.wpI] - hero->Obj.X, s_tap.wpZ[s_tap.wpI] - hero->Obj.Z) <=
               (S64)TAP_WAYPOINT_RADIUS * (S64)TAP_WAYPOINT_RADIUS)
        s_tap.wpI++;

    const S32 aimX = s_tap.wpX[s_tap.wpI];
    const S32 aimZ = s_tap.wpZ[s_tap.wpI];
    const S32 isLast = (s_tap.wpI == s_tap.wpN - 1);

    UpdateMarker();

    const S32 arrived =
        isLast && DistSq(aimX - hero->Obj.X, aimZ - hero->Obj.Z) <= (S64)TAP_ARRIVE_RADIUS * (S64)TAP_ARRIVE_RADIUS;

    /* At the finger: wait there. He walks on as soon as it moves away again. */
    if (arrived) {
        s_tap.turn = 0;
        return 0;
    }

    /* Blocked? */
    if ((U32)(gameNow - s_tap.sampleMs) >= (U32)TAP_STUCK_WINDOW_MS) {
        const S32 mx = hero->Obj.X - s_tap.sampleX;
        const S32 mz = hero->Obj.Z - s_tap.sampleZ;
        const S32 spentWalking = s_tap.walkedMs * 4 >= TAP_STUCK_WINDOW_MS * 3;
        if (spentWalking && DistSq(mx, mz) < (S64)TAP_STUCK_MIN_MOVE * (S64)TAP_STUCK_MIN_MOVE)
            s_tap.replan = 1; /* blocked: look for another way from here */
        s_tap.sampleMs = gameNow;
        s_tap.sampleX = hero->Obj.X;
        s_tap.sampleZ = hero->Obj.Z;
        s_tap.walkedMs = 0;
    }

    const S32 bearing = GetAngle2D(hero->Obj.X, hero->Obj.Z, aimX, aimZ);
    const S32 toGo = (S32)sqrt((double)DistSq(aimX - hero->Obj.X, aimZ - hero->Obj.Z));
    const T_TAP_STEER st = TapMove_Aim(hero->Obj.Beta & 4095, bearing, s_tap.turn, toGo);
    s_tap.turn = st.turn;
    if (st.turn != 0)
        s_tap.turnRate = st.rate;

    U32 bits = 0;
    if (st.turn > 0)
        bits |= I_LEFT; /* the heading grows while I_LEFT is held (ManualRealAngle) */
    else if (st.turn < 0)
        bits |= I_RIGHT;
    if (st.forward) {
        bits |= I_UP;
        /* A hitch in the frame clock must not count as a long walk. */
        if (frameMs > 0 && frameMs < 100)
            s_tap.walkedMs += frameMs;
    }
    return bits;
}

#ifdef __cplusplus
}
#endif
