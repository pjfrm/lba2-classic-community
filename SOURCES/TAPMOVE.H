#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

/* Touch-to-walk for the touch overlay.
 *
 * Put a finger on the empty part of the screen and Twinsen walks to the spot under
 * it; move the finger and he follows; lift it and he stops. The walk belongs to the
 * finger and ends with it. The D-pad this replaces made the player steer a tank
 * (turn, then walk) with two thumbs on a glass screen, which is the part of the 1997
 * controls that suits a touchscreen least.
 *
 * HOW IT WORKS
 * ------------
 * It does not move the hero. It presses the same keys a player would: each
 * gameplay frame TapMove_Steer returns I_UP / I_LEFT / I_RIGHT bits that are ORed
 * into Input, right where the replay harness already ORs its own. Everything
 * after that -- animations, speed, collisions, climbing, swimming, the
 * behaviour modes -- is the engine's existing manual-movement code, untouched.
 *
 * The finger becomes a spot on the ground by inverting the live camera: the engine's
 * own world->screen projection (LongWorldRotatePoint + LongProjectPoint, the
 * pair every actor label and shadow goes through) is solved for the point on the
 * floor, at the hero's feet height, that lands under the finger. Doing it through
 * the projection the game is drawing with, rather than re-deriving the camera,
 * is what makes it right for the classic isometric rooms and the perspective
 * Auto camera alike.
 *
 * The steering itself (TapMove_Aim) is a pure function of the hero's heading and
 * the bearing to the spot, with hysteresis so a held turn key is not toggled every
 * frame -- the manual-movement code restarts the walk animation whenever the
 * movement bits change.
 *
 * WHAT STOPS IT
 * -------------
 * Lifting the finger. Also: any real movement input (pad, keyboard); the hero
 * leaving manual control (cutscene, script, death); and a modal screen (dialogue,
 * menu, inventory: they run their own loops, so TapMove_Steer simply stops being
 * called and the walk is dropped when it resumes). At the finger he waits for it to
 * move on; blocked for about half a second, the route is worked out again.
 *
 * It is off during replays and the scripted harness: a recording already holds the
 * bits this produced, and generating them again would double-drive the hero.
 *
 * Setting: TouchTapToMove in lba2.cfg / touch_tap_move on the console (default on;
 * 0 brings the on-screen D-pad back).
 */

/* ---------------------------------------------------------------------------
 * Pure helpers (TAPMOVE_MATH.CPP). No game state beyond the camera globals the
 * projection already reads, so they are unit tested on their own.
 * --------------------------------------------------------------------------- */

/* Steering decision for one frame. */
typedef struct {
    S32 turn;    /* +1: heading must grow (hold I_LEFT), -1: shrink (I_RIGHT), 0: hold */
    S32 forward; /* 1: hold I_UP */
    S32 rate;    /* how fast to turn, in 1/4096 turn a second: see TapMove_TurnRate */
} T_TAP_STEER;

/* Signed shortest angle from `beta` to `bearing`, in 1/4096 turn units, in
 * [-2048, 2047]. Positive means the heading has to grow. */
S32 TapMove_AngleError(S32 beta, S32 bearing);

/* One frame of steering. `prevTurn` is last frame's `turn`: it is the memory the
 * hysteresis needs to keep a turn going until the heading is nearly right instead
 * of releasing it the moment the error dips under the threshold that started it.
 * `dist` is how far the hero still has to go: close to the spot he turns on the
 * spot before walking, far from it he may walk an arc while he turns.
 *
 * The turn is quick when the heading is far off and gentle as it closes: the rate
 * is proportional to the error, within a floor and a ceiling. A fixed rate is
 * either slow for a half turn or overshoots the last few degrees. */
T_TAP_STEER TapMove_Aim(S32 beta, S32 bearing, S32 prevTurn, S32 dist);

/* World point -> screen pixel with the camera and projection the game is drawing
 * with, leaving the projection's scratch globals (X0, Y0, Z0, Xp, Yp) as they
 * were. Returns 0 when the point is not on screen (behind the camera). */
S32 TapMove_Project(S32 x, S32 y, S32 z, S32 *sx, S32 *sy);

/* The point on the horizontal plane y = groundY that the live camera projects to
 * the screen pixel (px, py), in the same logical pixels the projection uses.
 * (startX, startZ) seeds the search; the hero's own position is the natural seed.
 * Returns 1 and fills *outX / *outZ, or 0 when there is no such point on screen
 * (above the horizon, behind the camera) or the answer does not hold up when
 * projected back. */
S32 TapMove_SolveGround(S32 px, S32 py, S32 groundY, S32 startX, S32 startZ, S32 *outX, S32 *outZ);

/* ---------------------------------------------------------------------------
 * Route finding (TAPMOVE_PATH.CPP): A* on the cell grid of an interior cube, so a
 * tap on the far side of a wall walks round it instead of into it. Pure: the
 * caller says which cells can be stood on.
 *
 * A query is TapPath_Begin, then TapPath_Find, then (optionally) TapPath_Smooth,
 * which reuses what Find learned about the grid.
 * --------------------------------------------------------------------------- */

/* The largest grid it will search: an interior cube is 64 x 64 bricks. */
#define TAPPATH_GRID_MAX 64

/* Can a hero stand in cell (cx, cz)? */
typedef S32 (*T_TAP_WALKABLE)(void *ctx, S32 cx, S32 cz);

void TapPath_Begin(T_TAP_WALKABLE walkable, void *ctx, S32 gridW, S32 gridH);

/* The cheapest route from (sx, sz) to (gx, gz) over 8-connected cells, no corner
 * cutting, written start first into outX / outZ (room for maxOut cells). Returns
 * the number of cells, 0 if the query is unusable. *reachedGoal is 1 if the last
 * cell is the goal. If the goal cannot be reached (it is a wall, or walled in)
 * the route ends at the reachable cell nearest to it: walk as close as you can.
 * The start cell is always taken as walkable. */
S32 TapPath_Find(S32 sx, S32 sz, S32 gx, S32 gz, S16 *outX, S16 *outZ, S32 maxOut, S32 *reachedGoal);

/* Whether a straight run between two cells stays on walkable ones (and cuts no
 * corner). */
S32 TapPath_LineClear(S32 ax, S32 az, S32 bx, S32 bz);

/* Drops the cells of a route that a straight line can skip, in place, keeping
 * both ends. Returns the new count. */
S32 TapPath_Smooth(S16 *x, S16 *z, S32 n);

/* ---------------------------------------------------------------------------
 * Game side (TAPMOVE.CPP)
 * --------------------------------------------------------------------------- */

#define TAPMOVE_FINGER_DOWN 0
#define TAPMOVE_FINGER_MOVE 1
#define TAPMOVE_FINGER_UP 2

/* The TouchTapToMove setting (lba2.cfg key, touch_tap_move on the console): 1 = a
 * touch on empty glass walks there, 0 = the on-screen D-pad, as before. Defined in
 * TAPMOVE.CPP with the rest of the module's state; CONFIG_FILE.CPP lists it. */
extern S32 TouchTapToMove;

/* Whether the player has touch-to-walk on. */
S32 TapMove_IsEnabled(void);

/* Whether the game is being played right now: the main loop has asked for steering lately, so no
 * dialogue, menu, video or load screen is up. The overlay keeps its controls on screen, recessive,
 * while it is, and lets them fade away when it is not. True whether or not touch-to-walk is on. */
S32 TapMove_GameplayRunning(void);

/* Whether the overlay should leave the D-pad out right now: touch-to-walk is on
 * AND gameplay is running. Two ways to steer on one screen is the clutter this
 * exists to remove -- but menus, the inventory and the save list are still driven
 * by direction keys, and on a touchscreen the D-pad is the only way to press them,
 * so it comes back whenever a modal screen is up (no gameplay frame for a moment). */
S32 TapMove_ReplacesDpad(void);

/* A finger on empty glass: down, moving, or lifted (which stops the walk). nx, ny
 * are game-area normalised (0..1), the space TOUCH_INPUT.CPP's MapToGameCoords
 * produces. Returns 1 if the touch was taken, 0 if it was ignored (feature off, or
 * no gameplay running: a touch on a dialogue or a menu must not hold a walk for
 * after it closes). */
S32 TapMove_Touch(float nx, float ny, S32 phase);

/* Drop the current walk. */
void TapMove_Cancel(void);

/* Once per gameplay frame, after the real input has been read and before the
 * simulation runs. Returns the movement bits to OR into Input. */
U32 TapMove_Steer(void);

/* How fast the hero's turn key turns him this frame, in 1/4096 turn a second.
 * ManualRealAngle asks this with the speed the keys give (1024: a quarter turn a
 * second). When the turn bits this frame came from TapMove_Steer the answer is the
 * faster, error-proportional rate it chose; for a player on the keys or a pad it is
 * `keyRate`, so nothing changes for them. The hero's own turning code does the
 * turning either way: this only says how fast. */
S32 TapMove_TurnRate(S32 keyRate);

/* Where the hero is being walked to, on screen in logical pixels, for the overlay
 * to mark. Returns 0 when there is no walk in progress. */
S32 TapMove_GetMarker(S32 *sx, S32 *sy);

#ifdef __cplusplus
}
#endif
