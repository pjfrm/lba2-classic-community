/* The pure half of tap-to-walk: nothing here reads the game, only the camera the
 * projection already holds. See TAPMOVE.H for the whole design. */

#include "TAPMOVE.H"

#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LPROJ.H>
#include <3D/LROT3D.H>

#include <math.h>

/* Steering thresholds, in 1/4096 of a turn.
 *
 * TURN_ON and TURN_OFF are the dead band: a heading within OFF of the bearing is
 * left alone, and a turn already going is kept until it gets there, but one is not
 * begun for an error under ON, so a heading that has just settled is not set
 * turning by the first stray frame. OFF is 2 degrees: walking straight at a spot
 * that far off the bearing misses it by under a third of a brick from 3000 units.
 *
 * The turn rate is proportional to the error (GAIN a second per unit of it), within
 * a floor and a ceiling, in the same units as the 1024 a second the turn keys give.
 * At gain 8 the heading closes on the bearing by 8 x the frame time a frame, so it
 * can never step over it at any frame rate above 8 fps; the floor keeps the last
 * degrees from taking forever; the ceiling is one full turn a second.
 *
 * WALK_CONE is how far off the bearing the hero may be and still walk. Inside it he
 * walks an arc towards the spot, outside it he turns on the spot first, which keeps
 * him from striding off the wrong way. It is wide when the spot is far, where the
 * arc is a small correction, and narrow when it is near, where a wide cone would
 * have him circle it. */
#define TAP_TURN_ON 64
#define TAP_TURN_OFF 24
#define TAP_TURN_GAIN 8
#define TAP_TURN_MIN 256
#define TAP_TURN_MAX 4096
#define TAP_CONE_NEAR 320
#define TAP_CONE_FAR 640
#define TAP_CONE_NEAR_DIST 400
#define TAP_CONE_FAR_DIST 1500

/* The solver's finite-difference probe, in world units (half a brick), the
 * largest single correction it may take, and how many it gets. */
#define TAP_PROBE 256
#define TAP_MAX_STEP 4096.0
#define TAP_SOLVE_ITERS 14

/* How far, in pixels, the answer may land from the finger and still count. The
 * isometric projection quantises to whole pixels, so ~1 is the floor. */
#define TAP_CONVERGED_PX 1.0
#define TAP_ACCEPT_PX 2.0

/* Round to nearest, halves away from zero. The project builds as C++98, which has no lround. */
static S32 Round(double v) {
    return (S32)(v < 0.0 ? v - 0.5 : v + 0.5);
}

S32 TapMove_AngleError(S32 beta, S32 bearing) {
    return (((bearing - beta) + 2048) & 4095) - 2048;
}

T_TAP_STEER TapMove_Aim(S32 beta, S32 bearing, S32 prevTurn, S32 dist) {
    const S32 e = TapMove_AngleError(beta, bearing);
    const S32 ae = (e < 0) ? -e : e;
    S32 cone;
    S32 rate;
    T_TAP_STEER s;

    s.turn = prevTurn;
    if (ae > TAP_TURN_ON) {
        s.turn = (e > 0) ? 1 : -1;
    } else if (ae < TAP_TURN_OFF) {
        s.turn = 0;
    } else if (s.turn != 0 && ((e > 0) != (s.turn > 0))) {
        /* Overshot into the dead band on the far side: stop, do not chase it. */
        s.turn = 0;
    }

    if (dist <= TAP_CONE_NEAR_DIST)
        cone = TAP_CONE_NEAR;
    else if (dist >= TAP_CONE_FAR_DIST)
        cone = TAP_CONE_FAR;
    else
        cone = TAP_CONE_NEAR + (TAP_CONE_FAR - TAP_CONE_NEAR) * (dist - TAP_CONE_NEAR_DIST) / (TAP_CONE_FAR_DIST - TAP_CONE_NEAR_DIST);
    s.forward = (ae < cone) ? 1 : 0;

    rate = TAP_TURN_GAIN * ae;
    if (rate < TAP_TURN_MIN)
        rate = TAP_TURN_MIN;
    if (rate > TAP_TURN_MAX)
        rate = TAP_TURN_MAX;
    s.rate = rate;
    return s;
}

/* World point -> screen pixel with the camera and projection the game is drawing
 * with. The same pair of calls every label, shadow and flow particle uses, so it
 * is right for the isometric rooms and the perspective camera alike. */
static S32 ProjectWorld(S32 x, S32 y, S32 z, S32 *sx, S32 *sy) {
    LongWorldRotatePoint(x, y, z);
    if (!LongProjectPoint(X0, Y0, Z0))
        return 0;
    *sx = Xp;
    *sy = Yp;
    return 1;
}

S32 TapMove_Project(S32 x, S32 y, S32 z, S32 *sx, S32 *sy) {
    const S32 saveX0 = X0, saveY0 = Y0, saveZ0 = Z0, saveXp = Xp, saveYp = Yp;
    const S32 ok = ProjectWorld(x, y, z, sx, sy);
    X0 = saveX0;
    Y0 = saveY0;
    Z0 = saveZ0;
    Xp = saveXp;
    Yp = saveYp;
    return ok;
}

S32 TapMove_SolveGround(S32 px, S32 py, S32 groundY, S32 startX, S32 startZ, S32 *outX, S32 *outZ) {
    /* The projection leaves its result in these scratch globals. Nothing is
     * supposed to rely on them across a frame boundary, but whoever called us
     * did not ask for them to change. */
    const S32 saveX0 = X0, saveY0 = Y0, saveZ0 = Z0, saveXp = Xp, saveYp = Yp;

    double x = (double)startX;
    double z = (double)startZ;
    S32 ok = 0;

    /* Newton's method on the two-variable map (x, z) -> (pixel x, pixel y) at a
     * fixed height. The Jacobian comes from probing the projection itself, so
     * there is no camera model to keep in step with the engine's: for the
     * isometric camera the map is affine and one step lands it; for the
     * perspective camera it is a homography and a few steps do. */
    for (S32 it = 0; it < TAP_SOLVE_ITERS; it++) {
        const S32 xi = Round(x);
        const S32 zi = Round(z);
        S32 sx, sy, ax, ay, bx, by;

        if (!ProjectWorld(xi, groundY, zi, &sx, &sy))
            break;

        const double ex = (double)px - (double)sx;
        const double ey = (double)py - (double)sy;
        if (fabs(ex) <= TAP_CONVERGED_PX && fabs(ey) <= TAP_CONVERGED_PX) {
            ok = 1;
            break;
        }

        if (!ProjectWorld(xi + TAP_PROBE, groundY, zi, &ax, &ay) ||
            !ProjectWorld(xi, groundY, zi + TAP_PROBE, &bx, &by))
            break;

        const double a = (double)(ax - sx) / TAP_PROBE;
        const double b = (double)(bx - sx) / TAP_PROBE;
        const double c = (double)(ay - sy) / TAP_PROBE;
        const double d = (double)(by - sy) / TAP_PROBE;
        const double det = a * d - b * c;
        if (fabs(det) < 1e-12)
            break;

        double dx = (d * ex - b * ey) / det;
        double dz = (a * ey - c * ex) / det;
        const double len = sqrt(dx * dx + dz * dz);
        if (!(len == len)) /* NaN */
            break;
        if (len > TAP_MAX_STEP) {
            dx *= TAP_MAX_STEP / len;
            dz *= TAP_MAX_STEP / len;
        }
        x += dx;
        z += dz;
    }

    S32 rx = Round(x);
    S32 rz = Round(z);

    /* Out of iterations is not the same as wrong: take the last point if it
     * projects back to within a couple of pixels of the finger. A point that does
     * not (above the horizon, behind the camera) is refused. */
    if (!ok) {
        S32 sx, sy;
        if (ProjectWorld(rx, groundY, rz, &sx, &sy) &&
            fabs((double)px - (double)sx) <= TAP_ACCEPT_PX && fabs((double)py - (double)sy) <= TAP_ACCEPT_PX)
            ok = 1;
    }

    X0 = saveX0;
    Y0 = saveY0;
    Z0 = saveZ0;
    Xp = saveXp;
    Yp = saveYp;

    if (!ok)
        return 0;
    *outX = rx;
    *outZ = rz;
    return 1;
}
