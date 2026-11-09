#include "TOUCH_INPUT.H"

#include "JOYSTICK.H" /* JoystickIsPresent: the pad half of the visibility rule */
#include "MESSAGE.H"  /* Language: whether the labels are drawn in Spanish */
#include "INPUT_BINDINGS.H"
#include "TAPMOVE.H" /* tap-to-walk: what a finger on empty glass does */

#include <SYSTEM/ANDROID.H>
#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOGPRINT.H>
#include <SVGA/SCREEN.H> /* ModeDesiredX/Y: the logical pixels the projection works in */
#include <SYSTEM/LOG.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/WINDOW.H>

#include <PLATFORM/PLATFORM.H>

#include <string.h>
#include <math.h>

/* Embedded 8x8 bitmap font from SVGA module — used to draw button labels. */
extern U8 Font8x8[];
extern U32 SizeChar;

#ifdef __cplusplus
extern "C" {
#endif

/* Last-input device flags (C linkage, defined in LIB386/SYSTEM/KEYBOARD.CPP).
 *
 * LastInputWasKeyboard is the older of the two and answers "can we prompt for
 * typed text": a touch tap is not a physical keyboard, so clear it and the save
 * menu uses the auto-name flow instead of a prompt we cannot fill.
 *
 * LastInputWasTouch answers a question that one could not, because a pad and a
 * finger both leave it 0: which of the two the player is holding right now.
 * That is what lets the overlay step aside for a pad on a phone -- the case a
 * device-class check for Android TV never covered. It is declared in
 * SYSTEM/KEYBOARD.H, beside the module that defines it. */
extern S32 LastInputWasKeyboard;

/* ---------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------------
 * Layout constants in normalised 0..1 coordinate space of the game picture, tuned
 * for a landscape phone held in both hands. The four controls a player reaches for
 * all the time (action, use, holomap, inventory) are big, coloured and spelt out in
 * words, and sit together on the right where a thumb rests. The walking finger has
 * the rest of the glass.
 *
 *    +--(ESC)---------(HIDE)---------(MEN)(CAM)-+
 *    |                                           |
 *    |                 (MODE)(THROW)(TALK)       |
 *    |                    [   HOLOMAP    ]       |
 *    |                    [  INVENTORY   ]       |
 *    |   (U)         (USE)    [  ACTION  ]       |
 *    |(L)(D)(R)               [          ]       |
 *    +-------------------------------------------+
 *
 * The D-pad is only drawn while it is wanted: see ButtonShown.
 */

#define NUM_TOUCH_BUTTONS 15

/* Maximum simultaneous touch points we track. */
#define MAX_FINGERS 8

enum TouchButtonId {
    BTN_ESC,
    BTN_HIDE,
    BTN_MENU,
    BTN_CAMERA,
    BTN_DPAD_UP,
    BTN_DPAD_DOWN,
    BTN_DPAD_LEFT,
    BTN_DPAD_RIGHT,
    BTN_ACTION,
    BTN_SPACE,
    BTN_ENTER,
    BTN_ALT,
    BTN_SHIFT,
    BTN_CTRL,
    BTN_HOLOMAP,
};

typedef struct {
    enum TouchButtonId id;
    const char *label;   /* English */
    const char *labelEs; /* Spanish, drawn when the game is in Spanish */
    float cx;            /* normalised centre X (0..1) */
    float cy;            /* normalised centre Y (0..1) */
    float rx;            /* normalised half-width */
    float ry;            /* normalised half-height */
    U32 scanCode;        /* SDL scancode to inject */
    U8 r, g, b;          /* body colour: the four main controls each have their own */
} TouchButton;

/* Language is the game's own language setting; 3 is Spanish (TabLanguage in MESSAGE.CPP). */
#define LANGUAGE_SPANISH 3

/* In a label, this byte puts an acute accent over the letter before it: the game's 8x8 font is
   the old PC one, which has no capital O with an accent. */
#define ACCENT_ACUTE '\001'

static const TouchButton kButtons[NUM_TOUCH_BUTTONS] = {
    /* Top-left: ESC */
    {BTN_ESC, "ESC", "ESC", 0.065f, 0.045f, 0.055f, 0.038f, K_ESC, 96, 96, 108},
    /* Top-centre: dismiss the overlay until the next touch */
    {BTN_HIDE, "HIDE", "OCULTAR", 0.48f, 0.045f, 0.060f, 0.038f, 0, 96, 96, 108},
    /* Top-right: Menu (F10) + Camera toggle */
    {BTN_MENU, "MEN", "MEN", 0.85f, 0.045f, 0.055f, 0.038f, K_F10, 96, 96, 108},
    {BTN_CAMERA, "CAM", "CAM", 0.955f, 0.045f, 0.040f, 0.038f, K_BACKSPACE, 96, 96, 108},
    /* Bottom-left: D-pad (thumb zone) */
    {BTN_DPAD_UP, "U", "U", 0.10f, 0.65f, 0.055f, 0.048f, K_UP, 96, 96, 108},
    {BTN_DPAD_DOWN, "D", "D", 0.10f, 0.85f, 0.055f, 0.048f, K_DOWN, 96, 96, 108},
    {BTN_DPAD_LEFT, "L", "L", 0.04f, 0.75f, 0.040f, 0.048f, K_LEFT, 96, 96, 108},
    {BTN_DPAD_RIGHT, "R", "R", 0.16f, 0.75f, 0.040f, 0.048f, K_RIGHT, 96, 96, 108},
    /* Right-hand column, top to bottom: the less used three, then the four main controls */
    {BTN_CTRL, "MODE", "MODO", 0.735f, 0.40f, 0.055f, 0.050f, K_CTRL, 110, 110, 128},
    {BTN_ALT, "THROW", "LANZAR", 0.85f, 0.40f, 0.050f, 0.050f, K_ALT, 110, 110, 128},
    {BTN_ACTION, "TALK", "HABLAR", 0.952f, 0.40f, 0.044f, 0.050f, K_W, 110, 110, 128},
    {BTN_HOLOMAP, "HOLOMAP", "HOLOMAPA", 0.84f, 0.545f, 0.150f, 0.055f, K_H, 50, 170, 120},
    {BTN_SHIFT, "INVENTORY", "INVENTARIO", 0.84f, 0.675f, 0.150f, 0.055f, K_SHIFT, 150, 100, 210},
    {BTN_ENTER, "USE", "USAR", 0.675f, 0.875f, 0.080f, 0.085f, K_ENTER, 70, 140, 230},
    {BTN_SPACE, "ACTION", "ACCIO\001N", 0.885f, 0.855f, 0.100f, 0.100f, K_SPACE, 240, 150, 40},
};

/* ---------------------------------------------------------------------------
 * Visibility policy
 * ---------------------------------------------------------------------------
 * One flag used to carry three unrelated facts -- whether the device has a
 * touchscreen, whether the player wants the pills drawn, and whether touch
 * input is accepted -- so there was nowhere to put a policy answer. They are
 * separate now:
 *
 *   s_supported   this build can draw an overlay at all: Android only.
 *   s_tvDevice    leanback, so assume no touchscreen until a finger proves otherwise.
 *   s_lastTouchMs when a finger was last down, which drives the fade.
 *
 * From those, one rule covers both the touch-only phone and the phone with a
 * pad clipped to it: THE OVERLAY FOLLOWS THE LAST INPUT THE PLAYER ACTUALLY
 * USED. Touch shows it, a pad or a key hides it, and going idle fades it out.
 * Nobody configures anything, and the pad case needs no device-class check --
 * Android TV is just a screen nobody has ever touched.
 *
 * Why a fade rather than a mode check: this game holds a direction to walk, so
 * a thumb is on the D-pad more or less continuously during play and the timer
 * never fires. It fires during dialogue, cutscenes and FMVs -- which is a fair
 * approximation of "the game is not asking for input" for the price of a
 * timer, instead of the per-modal mode stack the engine does not have (each
 * modal runs its own inner loop with its own exit sentinel; see docs/CONTROL.md).
 *
 * HIDE dismisses the overlay immediately rather than waiting out the fade, for
 * when the player wants to see what is under it now. It is not a mode: the next
 * touch brings the controls back, like any other touch. That is what removed the
 * SHOW pill this used to need -- a widget whose whole job was to undo a state
 * that touch already undoes, sitting on screen permanently in a design whose
 * point is to keep the screen clear. It also made the misfire cheap. HIDE is at
 * cx 0.48, cy 0.045, which is where a player taps to skip dialogue, so it does
 * get hit by accident; costing them one touch is fine, stranding them in an
 * unexplained state was not.
 */

/* Wall-clock, not the managed game clock: a UI fade should run in real time
   whether or not the simulation is paused, and staying off the game clock
   keeps this out of the recorder's determinism surface entirely. The overlay
   only ever injects keys from a real finger, so a replay never sees it. */
#define TOUCH_FULL_MS 1500u /* full opacity for this long after the last touch */
#define TOUCH_DIM_MS 6000u  /* then recessive; then gone, unless the game is being played (see CurrentLevel) */

/* How strong the controls are drawn when recessive, out of 255. They used to fade to 30%, which on a
   phone in daylight is gone: a button that cannot be seen is a button that cannot be found. */
#define OVL_DIM_SCALE 165

typedef enum {
    OVL_OFF = 0, /* not drawn */
    OVL_DIM,     /* drawn faintly: still findable, no longer competing with the art */
    OVL_FULL
} OverlayLevel;

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */

static S32 s_supported = 0;   /* Android build: an overlay is possible here */
static S32 s_tvDevice = 0;    /* leanback: start off, but a finger still overrides */
static S32 s_dismissed = 0;   /* HIDE was pressed; cleared by the next finger down */
static U32 s_lastTouchMs = 0; /* g_platform.system.ticks_ms at the last finger-down/motion; 0 = never */
static S32 s_pressed[NUM_TOUCH_BUTTONS];
static U8 s_virtual_keys[256]; /* virtual key states in 0x80-bit format */
static S32 s_gameplay = 0;
static S32 s_gameplayPoll = 0;
static S32 s_focused = 1;
static S32 s_recenterPending = 0;
static U32 s_injectedIndex = 0;
static U8 s_injectedBit = 0;

/* Per-finger tracking: which button (or -1) each finger is on. */
static struct {
    S32 buttonIndex; /* -1 = not touching any button */
    S32 touchId;     /* SDL finger ID */
    S32 active;      /* 1 = slot in use */
    S32 actuate;     /* 0 = this finger only woke the overlay, it presses nothing */
    S32 tap;         /* 1 = this finger landed on empty glass and is steering tap-to-walk */
} s_fingers[MAX_FINGERS];

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

static S32 FindFreeFingerSlot(void) {
    for (S32 i = 0; i < MAX_FINGERS; i++) {
        if (!s_fingers[i].active)
            return i;
    }
    return -1;
}

static S32 FindFingerByTouchId(S32 touchId) {
    for (S32 i = 0; i < MAX_FINGERS; i++) {
        if (s_fingers[i].active && s_fingers[i].touchId == touchId)
            return i;
    }
    return -1;
}

/* Any finger currently on the glass counts as activity for as long as it is
   held. A held button fires one FINGER_DOWN and then nothing, so timing the
   fade from events alone would dim the overlay in the middle of a walk. */
static S32 AnyFingerDown(void) {
    for (S32 i = 0; i < MAX_FINGERS; i++) {
        if (s_fingers[i].active)
            return 1;
    }
    return 0;
}

static void NoteTouchActivity(void) {
    s_lastTouchMs = g_platform.system.ticks_ms();
    LastInputWasTouch = 1;
    /* A touch is not a physical keyboard: keep the save menu on the auto-name
       flow, which is the one thing we cannot type into. */
    LastInputWasKeyboard = 0;
}

/* What to draw this frame. The order is the priority order: an unsupported
   device and an explicit HIDE both beat everything, then the pad, then time. */
static OverlayLevel CurrentLevel(void) {
    if (!s_supported)
        return OVL_OFF;

    /* HIDE, until the next finger clears it. */
    if (s_dismissed)
        return OVL_OFF;

    /* Nobody has touched the glass yet. A pad attached makes it a pad session
       and the pills would be 14 dead buttons over the art; a TV is a screen
       nobody is going to touch. Otherwise it is a player who has not found the
       controls yet, so leave a hint.

       This is where leanback belongs, rather than in s_supported: as a hard
       gate it was a hidden control stuck at off -- a leanback device that does
       have a touchscreen could never raise the overlay, and there is no
       setting to say otherwise. A first touch is better evidence than a
       manifest feature flag, so let it win. */
    if (s_lastTouchMs == 0)
        return (s_tvDevice || JoystickIsPresent()) ? OVL_OFF : OVL_DIM;

    /* The player picked up the pad. Step aside until a finger says otherwise --
       and the pad drives menus through fixed UI directions, so nothing here
       is reachable only by touch. */
    if (!LastInputWasTouch)
        return OVL_OFF;

    if (AnyFingerDown())
        return OVL_FULL;

    U32 idleMs = g_platform.system.ticks_ms() - s_lastTouchMs;
    if (idleMs < TOUCH_FULL_MS)
        return OVL_FULL;
    if (idleMs < TOUCH_DIM_MS)
        return OVL_DIM;

    /* Idle in a dialogue, a cutscene or a video, the controls go: the game is not asking for input.
       Idle in the game itself they stay, recessive. A player walking with one finger and reaching for
       Action with the other should find it where it was, not have to touch the glass to make it
       reappear first. */
    return TapMove_GameplayRunning() ? OVL_DIM : OVL_OFF;
}

/* Map a touch coordinate from display-normalised space (0..1 of the full
 * screen) to game-area-normalised space (0..1 of the letterboxed 4:3
 * region). This is the inverse of ComputePresentationRect. */
static void MapToGameCoords(float *nx, float *ny) {
    if (ModeResX == 0 || ModeResY == 0)
        return;

    S32 winW, winH;
    if (!g_platform.window.size(&winW, &winH))
        return;
    if (winW <= 0 || winH <= 0)
        return;

    float pixelX = *nx * (float)winW;
    float pixelY = *ny * (float)winH;

    S32 surfX, surfY;
    WindowToSurfaceCoords((S32)pixelX, (S32)pixelY, &surfX, &surfY);

    /* Clamp to valid surface area */
    if (surfX < 0)
        surfX = 0;
    if (surfY < 0)
        surfY = 0;
    if (surfX >= (S32)ModeResX)
        surfX = (S32)ModeResX - 1;
    if (surfY >= (S32)ModeResY)
        surfY = (S32)ModeResY - 1;

    *nx = (float)surfX / (float)ModeResX;
    *ny = (float)surfY / (float)ModeResY;
}

/* Whether a pill is on offer right now. The D-pad steps aside while tap-to-walk
   has the hero, and comes back with every modal screen: menus, the inventory and
   the save list are driven by direction keys, and on a touchscreen this is the only
   way to press them. Hidden also means untouchable, so the glass it covered is free
   to walk on. */
static S32 ButtonShown(S32 i) {
    if (kButtons[i].id >= BTN_DPAD_UP && kButtons[i].id <= BTN_DPAD_RIGHT)
        return !TapMove_ReplacesDpad();
    return 1;
}

static S32 HitTest(float nx, float ny) {
    for (S32 i = 0; i < NUM_TOUCH_BUTTONS; i++) {
        if (!ButtonShown(i))
            continue;
        /* The rectangle that is drawn, corners included: a big button with dead corners is a button
           the player misses with a thumb. */
        float dx = (nx - kButtons[i].cx) / kButtons[i].rx;
        float dy = (ny - kButtons[i].cy) / kButtons[i].ry;
        if (dx >= -1.0f && dx <= 1.0f && dy >= -1.0f && dy <= 1.0f)
            return i;
    }
    return -1;
}

static void SetButtonState(S32 idx, S32 pressed) {
    if (idx < 0 || idx >= NUM_TOUCH_BUTTONS)
        return;
    if (s_pressed[idx] == pressed)
        return;
    s_pressed[idx] = pressed;
    U32 sc = kButtons[idx].scanCode;
    if (sc < 256) {
        if (pressed) {
            TabKeys[sc] |= 0x80;
            s_virtual_keys[sc] = 0x80;
        } else {
            TabKeys[sc] &= ~0x80;
            s_virtual_keys[sc] = 0;
        }
    }
}

/* Re-apply virtual key states after UpdateKeyboardState clears TabKeys[0..255].
   Called from the weak hook in KEYBOARD.CPP:UpdateKeyboardState. */
void ApplyVirtualKeys(void) {
    if (s_recenterPending && s_gameplayPoll) {
        s_recenterPending = 0;
        U32 keys[4];
        const S32 count = Bindings_KeysForSlot(9, keys, 4);
        S32 injected = 0;
        for (S32 i = 0; i < count; ++i) {
            const U32 index = keys[i] < 256 ? keys[i] : 256 - 32 + (keys[i] >> 3);
            const U8 bit = keys[i] < 256 ? 0x80 : (U8)(1u << (keys[i] & 7));
            if (index >= sizeof(TabKeys))
                continue;
            if (!(TabKeys[index] & bit)) {
                s_injectedIndex = index;
                s_injectedBit = bit;
                TabKeys[index] |= bit;
            }
            injected = 1;
            break;
        }
        if (!injected)
            Log_Warn("Touch recenter has no usable camera-recenter binding.");
    }
    for (S32 i = 0; i < NUM_TOUCH_BUTTONS; i++) {
        U32 sc = kButtons[i].scanCode;
        if (sc < 256 && s_virtual_keys[sc])
            TabKeys[sc] |= 0x80;
    }
}

void TouchInput_EndGameplayPoll(void) {
    s_gameplayPoll = 0;
    if (s_injectedBit) {
        TabKeys[s_injectedIndex] &= ~s_injectedBit;
        s_injectedBit = 0;
    }
}

void TouchInput_SetGameplay(S32 gameplay) {
    s_gameplay = gameplay != 0;
    s_gameplayPoll = s_gameplay;
    if (!s_gameplay) {
        s_recenterPending = 0;
        TouchInput_EndGameplayPoll();
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void TouchOverlay_Init(void) {
    TouchInput_SetGameplay(0);
    s_focused = 1;
    s_supported = g_platform.capabilities.touch_overlay ? 1 : 0;
    s_tvDevice = 0;
    if (s_supported) {
        s_tvDevice = IsAndroidTVDevice();
        if (s_tvDevice) {
            LogPrintf("TouchOverlay: Android TV detected, overlay starts off "
                      "(use gamepad/remote; a touch still brings it up)\n");
        } else {
            LogPrintf("TouchOverlay: initialised for touchscreen (%d buttons)\n",
                      NUM_TOUCH_BUTTONS);
        }
    }
    s_dismissed = 0;
    s_lastTouchMs = 0;
    /* s_tvDevice is set above and is not cleared here: it is a property of the
       device, not of this session's input. */
    memset(s_pressed, 0, sizeof(s_pressed));
    memset(s_virtual_keys, 0, sizeof(s_virtual_keys));
    memset(s_fingers, 0, sizeof(s_fingers));
}

/* Let go of every key this overlay is holding. Called when it stops accepting
   input, so a finger that was on a direction when the overlay went away does
   not leave that direction latched in TabKeys forever. */
static void ReleaseAllButtons(void) {
    for (S32 i = 0; i < NUM_TOUCH_BUTTONS; i++) {
        if (s_pressed[i])
            SetButtonState(i, 0);
    }
    /* The finger that was steering a walk is forgotten with the rest, so its lift
       will never arrive to say it is over. */
    TapMove_Touch(0.0f, 0.0f, TAPMOVE_FINGER_UP);
    memset(s_fingers, 0, sizeof(s_fingers));
}

void TouchOverlay_Shutdown(void) {
    TouchInput_SetGameplay(0);
    ReleaseAllButtons();
    TapMove_Cancel();
    s_supported = 0;
}

S32 TouchOverlay_IsSupported(void) {
    return s_supported;
}

S32 TouchOverlay_HandleEvent(const PlatformEvent *event) {
    if (event == NULL)
        return 0;

    if (g_platform.capabilities.touch_recenter) {
        if (event->type == PLATFORM_EVENT_WINDOW_FOCUS_LOST) {
            s_focused = 0;
            s_recenterPending = 0;
        } else if (event->type == PLATFORM_EVENT_WINDOW_FOCUS_GAINED) {
            s_focused = 1;
        } else if (event->type == PLATFORM_EVENT_FINGER_DOWN && !event->tfinger.indirect) {
            if (s_gameplay && s_focused) {
                s_recenterPending = 1;
                LastInputWasKeyboard = 0;
                return 1;
            }
        }
    }
    if (!s_supported)
        return 0;

    const PlatformEvent *ev = event;
    S32 consumed = 0;

    switch (ev->type) {
    case PLATFORM_EVENT_FINGER_DOWN: {
        S32 slot = FindFreeFingerSlot();
        if (slot < 0)
            return 1;
        float nx = ev->tfinger.x;
        float ny = ev->tfinger.y;
        MapToGameCoords(&nx, &ny);
        S32 idx = HitTest(nx, ny);

        /* Any finger anywhere brings the controls back: touch is the only
           "show" this needs, which is why there is no pill. */
        s_dismissed = 0;

        /* Does this touch press what it landed on, or only wake the overlay?
           Only a player already using touch can have been aiming at a button:
           coming back from a pad or a keyboard there was nothing on screen to
           aim at. That is also what protects a phone in a controller clip,
           where the glass sits under the player's palms and a brush would
           otherwise latch a direction. Read before NoteTouchActivity, which is
           what makes the answer 1 from here on. */
        const S32 actuate = LastInputWasTouch;
        NoteTouchActivity();

        s_fingers[slot].active = 1;
        s_fingers[slot].touchId = (S32)ev->tfinger.fingerID;
        s_fingers[slot].buttonIndex = idx;
        s_fingers[slot].actuate = actuate;
        s_fingers[slot].tap = 0;

        if (idx < 0) {
            /* Empty glass. For a player already using touch it is an order: walk
               there. One finger owns the walk at a time, the newest. */
            if (actuate && TapMove_Touch(nx, ny, TAPMOVE_FINGER_DOWN)) {
                for (S32 i = 0; i < MAX_FINGERS; i++)
                    s_fingers[i].tap = 0;
                s_fingers[slot].tap = 1;
                consumed = 1;
            }
            break; /* otherwise the wake above was the whole effect */
        }

        if (kButtons[idx].id == BTN_HIDE) {
            /* Gated like any other button: a palm brush from a pad session
               wakes the overlay rather than dismissing it again. */
            if (actuate) {
                ReleaseAllButtons();
                s_dismissed = 1;
            }
            consumed = 1;
            break;
        }

        if (actuate) {
            SetButtonState(idx, 1);
            consumed = 1;
        }
        break;
    }
    case PLATFORM_EVENT_FINGER_UP: {
        S32 slot = FindFingerByTouchId((S32)ev->tfinger.fingerID);
        if (slot < 0)
            break;
        S32 idx = s_fingers[slot].buttonIndex;
        if (idx >= 0)
            SetButtonState(idx, 0);
        if (s_fingers[slot].tap) {
            TapMove_Touch(0.0f, 0.0f, TAPMOVE_FINGER_UP);
            s_fingers[slot].tap = 0;
        }
        s_fingers[slot].active = 0;
        s_fingers[slot].actuate = 0;
        /* The lift is what starts the idle clock: until it happens the finger
           is still down and the overlay is held at full. */
        s_lastTouchMs = g_platform.system.ticks_ms();
        consumed = 1;
        break;
    }
    case PLATFORM_EVENT_FINGER_MOTION: {
        S32 slot = FindFingerByTouchId((S32)ev->tfinger.fingerID);
        if (slot < 0)
            break;
        float nx = ev->tfinger.x;
        float ny = ev->tfinger.y;
        MapToGameCoords(&nx, &ny);
        if (s_fingers[slot].tap) {
            /* A finger steering the walk stays one wherever it goes: sliding it
               over a pill must not press it. */
            TapMove_Touch(nx, ny, TAPMOVE_FINGER_MOVE);
            consumed = 1;
            break;
        }
        S32 newIdx = HitTest(nx, ny);
        S32 oldIdx = s_fingers[slot].buttonIndex;
        if (newIdx != oldIdx) {
            if (oldIdx >= 0)
                SetButtonState(oldIdx, 0);
            /* A finger that only woke the overlay keeps pressing nothing for as
               long as it is down, however far it is dragged: the press it would
               start was never aimed. */
            if (newIdx >= 0 && s_fingers[slot].actuate)
                SetButtonState(newIdx, 1);
            s_fingers[slot].buttonIndex = newIdx;
        }
        consumed = 1;
        break;
    }
    default:
        break;
    }
    return consumed;
}

/* ---------------------------------------------------------------------------
 * Overlay rendering
 * ---------------------------------------------------------------------------
 * Draws semi-transparent rounded-rect buttons onto the ARGB framebuffer
 * just before present. This runs after the 8-bit→ARGB conversion so we
 * have direct access to the final pixel buffer.
 */

/* Forward declarations for helpers defined below. */
static void BlendPixel(U32 *pixel, U8 r, U8 g, U8 b, U8 a);

/* Clamp integer to [0, 255]. */
static U8 Clamp255(S32 v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (U8)v;
}

/* Blends a solid colour onto the framebuffer with alpha. */
static void BlendPixel(U32 *pixel, U8 r, U8 g, U8 b, U8 a) {
    U32 src = *pixel;
    U8 sr = (U8)(src >> 16);
    U8 sg = (U8)(src >> 8);
    U8 sb = (U8)(src);
    U8 ia = (U8)(255 - a);
    *pixel = ((U32)0xFF << 24) | ((U32)((sr * ia + r * a) / 255) << 16) | ((U32)((sg * ia + g * a) / 255) << 8) | ((U32)((sb * ia + b * a) / 255));
}

/* Is (x, y) inside the rounded rectangle [x0..x1] x [y0..y1] with corner radius `radius`? */
static S32 InsideRoundRect(S32 x, S32 y, S32 x0, S32 y0, S32 x1, S32 y1, S32 radius) {
    if (x < x0 || x > x1 || y < y0 || y > y1)
        return 0;
    if (radius <= 0)
        return 1;
    S32 cx = x, cy = y;
    if (x < x0 + radius)
        cx = x0 + radius;
    else if (x > x1 - radius)
        cx = x1 - radius;
    if (y < y0 + radius)
        cy = y0 + radius;
    else if (y > y1 - radius)
        cy = y1 - radius;
    const S32 dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

/* A rounded rectangle with a body colour and a ring of border colour `border` pixels wide inside its
   edge. Both blend over the picture, so the art shows through the body and the ring stays crisp. */
static void DrawRoundRect(U32 *fb, S32 pitchInPixels, S32 x0, S32 y0, S32 x1, S32 y1, S32 radius, S32 border,
                          U8 br, U8 bg, U8 bb, U8 ba, U8 er, U8 eg, U8 eb, U8 ea) {
    if (radius > (x1 - x0) / 2)
        radius = (x1 - x0) / 2;
    if (radius > (y1 - y0) / 2)
        radius = (y1 - y0) / 2;
    const S32 innerRadius = (radius > border) ? radius - border : 0;

    for (S32 y = y0; y <= y1; y++) {
        for (S32 x = x0; x <= x1; x++) {
            if (!InsideRoundRect(x, y, x0, y0, x1, y1, radius))
                continue;
            if (InsideRoundRect(x, y, x0 + border, y0 + border, x1 - border, y1 - border, innerRadius))
                BlendPixel(&fb[y * pitchInPixels + x], br, bg, bb, ba);
            else
                BlendPixel(&fb[y * pitchInPixels + x], er, eg, eb, ea);
        }
    }
}

/* Scale every alpha in a pass by one factor, so the dim level is the same
   drawing at lower contrast rather than a second set of tuned constants. */
static U8 ScaleAlpha(U8 a, U8 scale) {
    return (U8)(((U32)a * (U32)scale) / 255u);
}

/* Pixel rect for a button, clamped to the framebuffer. Returns 0 if nothing of
   it is on screen. Shared by the buttons and the pill so the thing the player
   taps and the thing they see cannot drift apart. */
static S32 ButtonRect(const TouchButton *btn, float shrink, U32 width, U32 height,
                      S32 *x0, S32 *y0, S32 *x1, S32 *y1) {
    *x0 = (S32)((btn->cx - btn->rx * shrink) * (float)width);
    *y0 = (S32)((btn->cy - btn->ry * shrink) * (float)height);
    *x1 = (S32)((btn->cx + btn->rx * shrink) * (float)width);
    *y1 = (S32)((btn->cy + btn->ry * shrink) * (float)height);
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 >= (S32)width)
        *x1 = (S32)width - 1;
    if (*y1 >= (S32)height)
        *y1 = (S32)height - 1;
    return (*x0 <= *x1 && *y0 <= *y1);
}

/* A solid block of the framebuffer, clipped. The label's "pixels" are scale x scale of these. */
static void FillBlock(U32 *fb, S32 pitchInPixels, S32 x, S32 y, S32 w, S32 h, U8 r, U8 g, U8 b, U8 a, U32 fbW, U32 fbH) {
    for (S32 py = y; py < y + h; py++) {
        if (py < 0 || (U32)py >= fbH)
            continue;
        for (S32 px = x; px < x + w; px++) {
            if (px >= 0 && (U32)px < fbW)
                BlendPixel(&fb[py * pitchInPixels + px], r, g, b, a);
        }
    }
}

/* One glyph of the game's 8x8 font, each of its pixels drawn as a scale x scale block. */
static void DrawGlyphScaled(U32 *fb, S32 pitchInPixels, S32 x, S32 y, U8 character, S32 scale,
                            U8 r, U8 g, U8 b, U8 a, U32 fbW, U32 fbH) {
    const U8 *glyph = &Font8x8[character * SizeChar];
    for (U32 row = 0; row < SizeChar; row++) {
        for (U32 col = 0; col < SizeChar; col++) {
            if (glyph[row] & (0x80u >> col))
                FillBlock(fb, pitchInPixels, x + (S32)col * scale, y + (S32)row * scale, scale, scale, r, g, b, a, fbW, fbH);
        }
    }
}

/* An acute accent over a glyph whose top row is at y: three blocks climbing to the right, in the
   two rows above it. */
static void DrawAcuteScaled(U32 *fb, S32 pitchInPixels, S32 x, S32 y, S32 scale, U8 r, U8 g, U8 b, U8 a, U32 fbW, U32 fbH) {
    FillBlock(fb, pitchInPixels, x + 3 * scale, y - 1 * scale, scale, scale, r, g, b, a, fbW, fbH);
    FillBlock(fb, pitchInPixels, x + 4 * scale, y - 1 * scale, scale, scale, r, g, b, a, fbW, fbH);
    FillBlock(fb, pitchInPixels, x + 5 * scale, y - 2 * scale, scale, scale, r, g, b, a, fbW, fbH);
}

static const char *LabelOf(const TouchButton *btn) {
    return (Language == LANGUAGE_SPANISH && btn->labelEs != NULL) ? btn->labelEs : btn->label;
}

/* How many glyphs a label has, and whether any of them carries an accent. */
static S32 LabelGlyphs(const char *label, S32 *accent) {
    S32 glyphs = 0;
    *accent = 0;
    for (; *label != 0; label++) {
        if (*label == ACCENT_ACUTE)
            *accent = 1;
        else
            glyphs++;
    }
    return glyphs;
}

/* The largest whole-number scale at which the label fits the button with a margin, no larger than
   what suits the framebuffer (the 8x8 font is 8 pixels of a 480-line picture at scale 1: too small to
   read on a phone, so labels are drawn as big as the button allows). */
static S32 LabelScale(const char *label, S32 buttonW, S32 buttonH, U32 fbHeight) {
    S32 accent;
    const S32 glyphs = LabelGlyphs(label, &accent);
    const S32 margin = 4;
    S32 scale = (S32)(fbHeight / 240u);
    if (scale < 1)
        scale = 1;
    while (scale > 1 &&
           (glyphs * (S32)SizeChar * scale > buttonW - 2 * margin ||
            ((S32)SizeChar + (accent ? 2 : 0)) * scale > buttonH - 2 * margin))
        scale--;
    return scale;
}

/* A label centred in the button, white with a dark shadow so it reads over any art. */
static void DrawLabelCentred(U32 *fb, S32 pitchPixels, S32 x0, S32 y0, S32 x1, S32 y1,
                             const char *label, U8 alpha, U32 width, U32 height) {
    if (label == NULL || label[0] == 0)
        return;

    S32 accent;
    const S32 glyphs = LabelGlyphs(label, &accent);
    const S32 scale = LabelScale(label, x1 - x0 + 1, y1 - y0 + 1, height);
    const S32 textW = glyphs * (S32)SizeChar * scale;
    const S32 textH = ((S32)SizeChar + (accent ? 2 : 0)) * scale;
    const S32 left = (x0 + x1 + 1) / 2 - textW / 2;
    const S32 top = (y0 + y1 + 1) / 2 - textH / 2 + (accent ? 2 * scale : 0); /* top of the glyphs */
    const S32 shadow = (scale >= 3) ? 2 : 1;

    for (S32 pass = 0; pass < 2; pass++) {
        const S32 off = (pass == 0) ? shadow : 0;
        const U8 c = (pass == 0) ? 0 : 255;
        const U8 a = (pass == 0) ? ScaleAlpha(alpha, 190) : alpha;
        S32 x = left + off;
        S32 previousX = x;
        for (const char *p = label; *p != 0; p++) {
            if (*p == ACCENT_ACUTE) {
                DrawAcuteScaled(fb, pitchPixels, previousX, top + off, scale, c, c, c, a, width, height);
                continue;
            }
            previousX = x;
            DrawGlyphScaled(fb, pitchPixels, x, top + off, (U8)*p, scale, c, c, c, a, width, height);
            x += (S32)SizeChar * scale;
        }
    }
}

static U8 Lighten(U8 v) {
    return (v > 185) ? 255 : (U8)(v + 70);
}

/* One button: a body in its own colour, a white ring, and the label. Pressed is brighter and more
   opaque, so a thumb can see it has landed. */
static void DrawButton(U32 *fb, S32 pitchPixels, const TouchButton *btn, S32 pressed, U8 scale, U32 width, U32 height) {
    S32 x0, y0, x1, y1;
    if (!ButtonRect(btn, 1.0f, width, height, &x0, &y0, &x1, &y1))
        return;

    const S32 shortSide = ((x1 - x0) < (y1 - y0)) ? (x1 - x0) : (y1 - y0);
    S32 radius = shortSide / 4;
    if (radius < 6)
        radius = 6;
    S32 border = (S32)(height / 160u);
    if (border < 2)
        border = 2;

    const U8 r = pressed ? Lighten(btn->r) : btn->r;
    const U8 g = pressed ? Lighten(btn->g) : btn->g;
    const U8 b = pressed ? Lighten(btn->b) : btn->b;
    const U8 bodyAlpha = pressed ? 240 : 165;

    DrawRoundRect(fb, pitchPixels, x0, y0, x1, y1, radius, border,
                  r, g, b, ScaleAlpha(bodyAlpha, scale),
                  255, 255, 255, ScaleAlpha(235, scale));

    /* The body recedes when the controls are idle; the word on it does not, or it could not be read. */
    DrawLabelCentred(fb, pitchPixels, x0, y0, x1, y1, LabelOf(btn), (scale > 225) ? scale : 225, width, height);
}

/* Where the walk is headed: a ring on the floor under the finger.
 *
 * Drawn whatever the overlay's fade says. The pills are gone a few seconds into a
 * walk, which is the point of the fade; the spot the hero is walking to is what the
 * player is watching, and it has to stay up for as long as the finger does. (x, y) arrive in
 * the logical pixels the projection works in, so they are scaled to the
 * framebuffer the way ButtonRect scales the pills. */
static void DrawTapMarker(U32 *fb, S32 pitchPixels, U32 width, U32 height) {
    S32 mx, my;
    if (!TapMove_GetMarker(&mx, &my) || ModeDesiredX == 0 || ModeDesiredY == 0)
        return;

    const S32 cx = (S32)((float)mx * (float)width / (float)ModeDesiredX);
    const S32 cy = (S32)((float)my * (float)height / (float)ModeDesiredY);

    S32 radius = (S32)(height / 32u);
    if (radius < 8)
        radius = 8;

    /* A slow pulse, so it reads as "going here" and not as part of the art. */
    const U32 phase = (U32)(g_platform.system.ticks_ms() % 900u);
    const S32 pulse = (phase < 450u) ? (S32)(phase / 90u) : (S32)((900u - phase) / 90u); /* 0..5..0 */
    const S32 r = radius + pulse * radius / 12;
    const S32 thick = (radius / 5 > 2) ? radius / 5 : 2;
    const S32 rIn = r - thick;

    for (S32 y = cy - r - 2; y <= cy + r + 2; y++) {
        if (y < 0 || y >= (S32)height)
            continue;
        for (S32 x = cx - r - 2; x <= cx + r + 2; x++) {
            if (x < 0 || x >= (S32)width)
                continue;
            const S32 dx = x - cx;
            const S32 dy = y - cy;
            const S32 d2 = dx * dx + dy * dy;
            U32 *px = &fb[y * pitchPixels + x];

            if ((d2 <= r * r && d2 >= rIn * rIn) || d2 <= thick * thick) {
                BlendPixel(px, 255, 255, 255, 230); /* the ring and the dot */
            } else if ((d2 > r * r && d2 <= (r + 2) * (r + 2)) ||
                       (d2 < rIn * rIn && d2 >= (rIn - 2) * (rIn - 2))) {
                BlendPixel(px, 0, 0, 0, 120); /* a dark edge, so it shows on pale floors too */
            }
        }
    }
}

void TouchOverlay_Render(U32 *frameBuffer, U32 pitch, U32 width, U32 height,
                         const U8 *palette_rgb) {
    (void)palette_rgb;
    if (frameBuffer == NULL || width == 0 || height == 0)
        return;

    S32 pitchPixels = (S32)(pitch / sizeof(U32));

    DrawTapMarker(frameBuffer, pitchPixels, width, height);

    const OverlayLevel level = CurrentLevel();
    if (level == OVL_OFF)
        return;

    /* Full while a finger is down or just was; recessive otherwise (OVL_DIM_SCALE), still easy to find. */
    const U8 scale = (level == OVL_FULL) ? 255 : OVL_DIM_SCALE;

    for (S32 i = 0; i < NUM_TOUCH_BUTTONS; i++) {
        if (!ButtonShown(i))
            continue;
        DrawButton(frameBuffer, pitchPixels, &kButtons[i], s_pressed[i], scale, width, height);
    }
}

/* ---------------------------------------------------------------------------
 * Event dispatch (strong override for the weak default in EVENTS.CPP)
 * --------------------------------------------------------------------------- */
S32 HandleEventsTouch(const PlatformEvent *event) {
    return TouchOverlay_HandleEvent(event);
}

#ifdef __cplusplus
}
#endif
