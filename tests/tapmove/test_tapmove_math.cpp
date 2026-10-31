/* Host test for tap-to-walk's maths (SOURCES/TAPMOVE_MATH.CPP): the inversion of the live camera
 * that turns a touch into a spot on the floor, and the steering that turns a heading into key
 * presses.
 *
 * The inversion is checked on the engine's own projection (the 3D library), not on a model of it:
 * the whole reason it works through LongWorldRotatePoint/LongProjectPoint is that it cannot drift
 * from what is drawn, so the test must not be a second opinion on what is drawn either. What is
 * asked of it is the thing a player sees: the point it hands back, projected again, lands under the
 * finger.
 */
#include "TAPMOVE.H"
#include "test_harness.h"

#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LPROJ.H>
#include <3D/LROT3D.H>
#include <3D/PROJ.H>

#include <math.h>
#include <stdlib.h>

/* What the 3D library reaches for outside itself, on paths this test does not drive. */
extern "C" {
volatile U32 TimerRefHR = 0;
S32 ScreenXMin = 0, ScreenXMax = 639, ScreenYMin = 0, ScreenYMax = 479;
}

#define SCREEN_W 640
#define SCREEN_H 480

static S32 project(S32 x, S32 y, S32 z, S32 *sx, S32 *sy) {
    LongWorldRotatePoint(x, y, z);
    if (!LongProjectPoint(X0, Y0, Z0))
        return 0;
    *sx = Xp;
    *sy = Yp;
    return 1;
}

static unsigned g_seed = 1234567u;
static int rnd(int n) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int)((g_seed >> 8) % (unsigned)n);
}

/* Project a scatter of floor points, ask for each back from its pixel, and count what goes wrong:
 * answers that do not land back within two pixels of the finger (the failure that matters), points
 * it declined to answer for, and the worst the answer was out in the world. */
static void sweep(S32 hx, S32 hy, S32 hz, S32 range, S32 *tried, S32 *pixelFail, S32 *refused, double *worstWorld) {
    int i;
    *tried = *pixelFail = *refused = 0;
    *worstWorld = 0;
    for (i = 0; i < 1500; i++) {
        S32 x = hx + rnd(2 * range + 1) - range;
        S32 z = hz + rnd(2 * range + 1) - range;
        S32 sx, sy, gx, gz, rx, ry;
        double err;

        if (!project(x, hy, z, &sx, &sy) || sx < 0 || sx >= SCREEN_W || sy < 0 || sy >= SCREEN_H)
            continue;
        (*tried)++;
        if (!TapMove_SolveGround(sx, sy, hy, hx, hz, &gx, &gz)) {
            (*refused)++;
            continue;
        }
        err = hypot((double)(gx - x), (double)(gz - z));
        if (err > *worstWorld)
            *worstWorld = err;
        if (!project(gx, hy, gz, &rx, &ry) || abs(rx - sx) > 2 || abs(ry - sy) > 2)
            (*pixelFail)++;
    }
}

static void test_isometric_camera_inverts_exactly(void) {
    const S32 hx = 20000, hy = 2048, hz = 15000;
    S32 tried, pixelFail, refused;
    double worst;

    SetIsoProjection(SCREEN_W / 2 - 9, SCREEN_H / 2);
    SetPosCamera(hx, hy, hz);
    SetAngleCamera(0, 0, 0);
    sweep(hx, hy, hz, 6000, &tried, &pixelFail, &refused, &worst);
    ASSERT_TRUE(tried > 500);
    ASSERT_EQ_INT(0, pixelFail);
    ASSERT_EQ_INT(0, refused);
    /* a pixel is 21 world units wide in x and about 43 deep, so one pixel of rounding is the floor */
    ASSERT_TRUE(worst <= 64.0);

    /* and with the camera somewhere other than on the hero */
    SetPosCamera(hx + 3000, hy + 512, hz - 2000);
    sweep(hx, hy, hz, 4000, &tried, &pixelFail, &refused, &worst);
    ASSERT_TRUE(tried > 300);
    ASSERT_EQ_INT(0, pixelFail);
    ASSERT_EQ_INT(0, refused);
    ASSERT_TRUE(worst <= 64.0);
}

static void test_perspective_camera_inverts_across_the_auto_cameras_range(void) {
    const S32 hx = 20000, hy = 2048, hz = 15000;
    const S32 fovs[2] = {600, 1000};
    const S32 alphas[3] = {150, 300, 600}; /* elevation: the Auto camera's range is about 150..600 */
    const S32 betas[4] = {0, 700, 2048, 3300};
    const S32 dists[3] = {7000, 12000, 20000};
    int a, b, c, d;
    S32 totalTried = 0, totalFail = 0, totalRefused = 0;
    double worstAny = 0;

    for (a = 0; a < 2; a++)
        for (b = 0; b < 3; b++)
            for (c = 0; c < 4; c++)
                for (d = 0; d < 3; d++) {
                    S32 tried, pixelFail, refused;
                    double worst;
                    SetProjection(SCREEN_W / 2, SCREEN_H / 2, 128, fovs[a], fovs[a]);
                    SetFollowCamera(hx, hy, hz, alphas[b], betas[c], 0, dists[d]);
                    sweep(hx, hy, hz, 5000, &tried, &pixelFail, &refused, &worst);
                    totalTried += tried;
                    totalFail += pixelFail;
                    totalRefused += refused;
                    if (worst > worstAny)
                        worstAny = worst;
                }

    ASSERT_TRUE(totalTried > 50000);
    ASSERT_EQ_INT(0, totalFail);                  /* never an answer that is not under the finger */
    ASSERT_TRUE(totalRefused * 100 < totalTried); /* declines are rare: under 1% */
    /* The shallowest views at the longest range put hundreds of units under one pixel, so the world
     * error is bounded by a brick, not by a few pixels' worth. */
    ASSERT_TRUE(worstAny < 600.0);
}

static void test_a_tap_above_the_horizon_is_refused(void) {
    const S32 hx = 20000, hy = 2048, hz = 15000;
    S32 gx, gz;

    SetProjection(SCREEN_W / 2, SCREEN_H / 2, 128, 1000, 1000);
    SetFollowCamera(hx, hy, hz, 150, 0, 0, 12000);
    ASSERT_EQ_INT(0, TapMove_SolveGround(SCREEN_W / 2, 2, hy, hx, hz, &gx, &gz));
}

static void test_project_and_solve_leave_the_scratch_globals_alone(void) {
    const S32 hx = 20000, hy = 2048, hz = 15000;
    S32 sx, sy, gx, gz;

    SetIsoProjection(SCREEN_W / 2 - 9, SCREEN_H / 2);
    SetPosCamera(hx, hy, hz);
    SetAngleCamera(0, 0, 0);

    X0 = 111;
    Y0 = 222;
    Z0 = 333;
    Xp = 444;
    Yp = 555;
    ASSERT_EQ_INT(1, TapMove_Project(hx + 1000, hy, hz + 500, &sx, &sy));
    ASSERT_TRUE(X0 == 111 && Y0 == 222 && Z0 == 333 && Xp == 444 && Yp == 555);
    ASSERT_EQ_INT(1, TapMove_SolveGround(sx, sy, hy, hx, hz, &gx, &gz));
    ASSERT_TRUE(X0 == 111 && Y0 == 222 && Z0 == 333 && Xp == 444 && Yp == 555);
}

static void test_angle_error_takes_the_short_way(void) {
    S32 a, b, bad = 0;

    ASSERT_EQ_INT(0, TapMove_AngleError(100, 100));
    ASSERT_EQ_INT(-16, TapMove_AngleError(10, 4090)); /* across 0, not round through 2048 */
    ASSERT_EQ_INT(16, TapMove_AngleError(4090, 10));
    ASSERT_EQ_INT(1024, TapMove_AngleError(0, 1024));
    ASSERT_EQ_INT(-1024, TapMove_AngleError(0, 3072));
    ASSERT_EQ_INT(-2048, TapMove_AngleError(0, 2048)); /* dead behind: a defined side, not a coin toss */

    for (a = 0; a < 4096; a += 7)
        for (b = 0; b < 4096; b += 11) {
            S32 e = TapMove_AngleError(a, b);
            if (e < -2048 || e > 2047 || ((a + e) & 4095) != b)
                bad++;
        }
    ASSERT_EQ_INT(0, bad);
}

static void test_aim_turns_toward_the_bearing_and_walks_when_close_to_it(void) {
    T_TAP_STEER s;

    s = TapMove_Aim(0, 1024, 0, 3000); /* the spot is a quarter turn to the side where the heading grows */
    ASSERT_EQ_INT(1, s.turn);
    ASSERT_EQ_INT(0, s.forward); /* turn on the spot first: do not stride off the wrong way */

    s = TapMove_Aim(0, 3072, 0, 3000);
    ASSERT_EQ_INT(-1, s.turn);
    ASSERT_EQ_INT(0, s.forward);

    s = TapMove_Aim(1000, 1010, 0, 3000); /* nearly there: no turn key, walk */
    ASSERT_EQ_INT(0, s.turn);
    ASSERT_EQ_INT(1, s.forward);

    s = TapMove_Aim(1000, 1200, 0, 3000); /* off by 200: an arc, turning and walking at once */
    ASSERT_EQ_INT(1, s.turn);
    ASSERT_EQ_INT(1, s.forward);
}

static void test_the_walk_cone_is_wide_far_away_and_narrow_close_to_the_spot(void) {
    /* Off by 500 (44 degrees): far from the spot that is an arc worth walking, close to it a
     * wide cone would have him circle it, so he turns on the spot first. */
    ASSERT_EQ_INT(1, TapMove_Aim(0, 500, 0, 3000).forward);
    ASSERT_EQ_INT(0, TapMove_Aim(0, 500, 0, 300).forward);

    /* In between it narrows steadily: the widest error that still walks never grows as he nears. */
    {
        S32 dist, widest, previous = 1 << 30;
        for (dist = 3000; dist >= 200; dist -= 100) {
            widest = 0;
            while (widest < 2047 && TapMove_Aim(0, widest + 1, 0, dist).forward)
                widest++;
            ASSERT_TRUE(widest <= previous);
            previous = widest;
        }
        ASSERT_TRUE(previous >= 300); /* never narrower than a quarter turn's worth of a third */
    }
}

static void test_aim_holds_a_turn_until_nearly_aligned_but_does_not_chase(void) {
    T_TAP_STEER s;

    s = TapMove_Aim(0, 40, 1, 3000); /* inside the band where a turn already going is kept */
    ASSERT_EQ_INT(1, s.turn);
    s = TapMove_Aim(0, 40, 0, 3000); /* ...and one not yet started is not begun there */
    ASSERT_EQ_INT(0, s.turn);
    s = TapMove_Aim(0, 10, 1, 3000); /* near enough: let go */
    ASSERT_EQ_INT(0, s.turn);
    s = TapMove_Aim(40, 0, 1, 3000); /* overshot into the band on the far side: stop, do not turn back */
    ASSERT_EQ_INT(0, s.turn);
}

static void test_the_turn_rate_is_quick_when_far_off_and_gentle_when_close(void) {
    T_TAP_STEER far_off = TapMove_Aim(0, 2000, 0, 3000);
    T_TAP_STEER mid = TapMove_Aim(0, 300, 0, 3000);
    T_TAP_STEER close = TapMove_Aim(0, 70, 0, 3000);
    S32 e;

    /* The keys turn at 1024 a second; a half turn must beat that, the last few degrees must be gentler. */
    ASSERT_TRUE(far_off.rate > 1024);
    ASSERT_TRUE(mid.rate > close.rate);
    ASSERT_TRUE(close.rate < 1024);

    /* Within the floor and the ceiling, rising with the error. */
    {
        S32 last = 0, bad = 0;
        for (e = 0; e <= 2047; e++) {
            S32 r = TapMove_Aim(0, e, 0, 3000).rate;
            if (r < 256 || r > 4096 || r < last)
                bad++;
            last = r;
        }
        ASSERT_EQ_INT(0, bad);
    }
}

/* Every start, every bearing, at the frame sizes of a 60, 30 and 15 fps device. The engine turns the hero
 * by the rate it was last given times the frame time, with no clamp at the bearing, so overshooting
 * is possible in principle: it must not happen, and the key must be pressed and released once rather than
 * chattering (the walk animation restarts whenever the movement bits change, so chatter would be seen). */
static void test_aim_converges_without_chatter_or_overshoot_at_any_frame_rate(void) {
    static const S32 frameMs[3] = {16, 33, 66};
    S32 f, start, bearing;
    S32 worstToggles = 0, nonConverged = 0, overshoots = 0;

    for (f = 0; f < 3; f++)
        for (start = 0; start < 4096; start += 37)
            for (bearing = 0; bearing < 4096; bearing += 53) {
                S32 beta = start, turn = 0, prev = 0, toggles = 0, frames, e;
                for (frames = 0; frames < 600; frames++) {
                    T_TAP_STEER s = TapMove_Aim(beta, bearing, turn, 3000);
                    S32 before = TapMove_AngleError(beta, bearing);
                    turn = s.turn;
                    if (turn != prev) {
                        toggles++;
                        prev = turn;
                    }
                    if (turn == 0 && s.forward)
                        break;
                    beta = (beta + turn * (s.rate * frameMs[f] / 1000)) & 4095;
                    /* stepping across the bearing: the sign of the error flips while it was big enough to matter */
                    e = TapMove_AngleError(beta, bearing);
                    if (turn != 0 && ((before > 0) != (e > 0)) && (before > 40 || before < -40) && (e > 40 || e < -40))
                        overshoots++;
                }
                e = TapMove_AngleError(beta, bearing);
                if (frames >= 600 || (e < 0 ? -e : e) > 640)
                    nonConverged++;
                if (toggles > worstToggles)
                    worstToggles = toggles;
            }
    ASSERT_EQ_INT(0, nonConverged);
    ASSERT_EQ_INT(0, overshoots);
    ASSERT_TRUE(worstToggles <= 3); /* on, off, and at most one reversal */
}

int main(void) {
    RUN_TEST(test_isometric_camera_inverts_exactly);
    RUN_TEST(test_perspective_camera_inverts_across_the_auto_cameras_range);
    RUN_TEST(test_a_tap_above_the_horizon_is_refused);
    RUN_TEST(test_project_and_solve_leave_the_scratch_globals_alone);
    RUN_TEST(test_angle_error_takes_the_short_way);
    RUN_TEST(test_aim_turns_toward_the_bearing_and_walks_when_close_to_it);
    RUN_TEST(test_the_walk_cone_is_wide_far_away_and_narrow_close_to_the_spot);
    RUN_TEST(test_aim_holds_a_turn_until_nearly_aligned_but_does_not_chase);
    RUN_TEST(test_the_turn_rate_is_quick_when_far_off_and_gentle_when_close);
    RUN_TEST(test_aim_converges_without_chatter_or_overshoot_at_any_frame_rate);
    TEST_SUMMARY();
    return test_failures != 0;
}
