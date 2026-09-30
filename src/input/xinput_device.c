/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prompt_activity.h"
static PromptPadActivity g_prompt_pad_activity[2];
#ifdef _WIN32
static volatile LONG g_prompt_device;
static volatile LONG g_prompt_controller=-1;
int xbox_InputPromptController(void) { int d=(int)InterlockedCompareExchange(&g_prompt_controller,0,0);return d<0?XBOX_PROMPT_XBOX:d; }
int xbox_InputPromptDevice(void) { return (int)InterlockedCompareExchange(&g_prompt_device,0,0); }
static void input_prompt_device(int device) {
    if(device!=XBOX_PROMPT_KEYBOARD)InterlockedExchange(&g_prompt_controller,device);
    InterlockedExchange(&g_prompt_device,device);
}
#else
static int g_prompt_device;
static int g_prompt_controller=-1;
int xbox_InputPromptController(void) { return g_prompt_controller<0?XBOX_PROMPT_XBOX:g_prompt_controller; }
int xbox_InputPromptDevice(void) { return g_prompt_device; }
static void input_prompt_device(int device) { if(device!=XBOX_PROMPT_KEYBOARD)g_prompt_controller=device;g_prompt_device=device; }
#endif
void xbox_InputNotifyKeyboardActivity(void) { input_prompt_device(XBOX_PROMPT_KEYBOARD); }
static void input_prompt_pad(unsigned family,const XBOX_GAMEPAD *pad) {
    /* A connected pad supplies the editor family even before its first press.
     * Keyboard activity must not erase the last deliberate pad selection. */
#ifdef _WIN32
    if(family<2)InterlockedCompareExchange(&g_prompt_controller,(LONG)family,-1);
#else
    if(family<2 && g_prompt_controller<0)g_prompt_controller=(int)family;
#endif
    int16_t axes[4]={pad->sThumbLX,pad->sThumbLY,pad->sThumbRX,pad->sThumbRY};
    if(family<2 && prompt_pad_activity(&g_prompt_pad_activity[family],pad->wButtons,pad->bAnalogButtons,axes))
        input_prompt_device((int)family);
}

static XBOX_INPUT_MAPPER g_input_mapper;
void xbox_InputSetMapper(XBOX_INPUT_MAPPER mapper) { g_input_mapper = mapper; }

static BOOL g_test_auto_a_stopped;
#ifdef _WIN32
static volatile LONG g_overlay_input_capture;
static const int g_overlay_close_keys[] = { VK_F9, VK_ESCAPE, VK_RETURN, VK_LBUTTON };
static volatile LONG g_overlay_close_held[4];
static BOOL test_input_isolated(void);
void xbox_InputSetOverlayCapture(BOOL captured)
{
    if (captured) {
        InterlockedExchange(&g_overlay_input_capture, 1);
        return;
    }
    /* Consume only the keys/buttons that can close the panel. Waiting for all
     * 255 virtual keys to become neutral also blocked controllers indefinitely.
     * Keep each closing key suppressed until release, without blocking a pad
     * or unrelated keyboard movement. Publish after initializing the latches. */
    for (unsigned i=0; i<4; ++i)
        InterlockedExchange(&g_overlay_close_held[i],
            !test_input_isolated() && (GetAsyncKeyState(g_overlay_close_keys[i]) & 0x8000));
    InterlockedExchange(&g_overlay_input_capture, 0);
}
static BOOL overlay_captures_input(void)
{
    if (InterlockedCompareExchange(&g_overlay_input_capture,0,0)) return TRUE;
    for (unsigned i=0; i<4; ++i)
        if (InterlockedCompareExchange(&g_overlay_close_held[i],0,0) &&
            !(GetAsyncKeyState(g_overlay_close_keys[i]) & 0x8000))
            InterlockedExchange(&g_overlay_close_held[i],0);
    return FALSE;
}
BOOL xbox_InputKeyDownContinuous(int key)
{
    if (key<=0 || key>=256 || test_input_isolated() ||
        InterlockedCompareExchange(&g_overlay_input_capture,0,0)) return FALSE;
    return (GetAsyncKeyState(key) & 0x8000)!=0;
}
BOOL xbox_InputKeyDown(int key)
{
    if (key<=0 || key>=256 || test_input_isolated()) return FALSE;
    if (InterlockedCompareExchange(&g_overlay_input_capture,0,0)) return FALSE;
    for (unsigned i=0; i<4; ++i)
        if (key==g_overlay_close_keys[i] &&
            InterlockedCompareExchange(&g_overlay_close_held[i],0,0)) return FALSE;
    return (GetAsyncKeyState(key) & 0x8000)!=0;
}
#else
static BOOL g_overlay_input_capture;
void xbox_InputSetOverlayCapture(BOOL captured) { g_overlay_input_capture=captured; }
static BOOL overlay_captures_input(void) { return g_overlay_input_capture; }
BOOL xbox_InputKeyDown(int key) { (void)key; return FALSE; }
BOOL xbox_InputKeyDownContinuous(int key) { (void)key; return FALSE; }
#endif

/* Input is polled far more often than the game renders. Environment switches
 * are immutable process-launch configuration, so cache both present and
 * absent values per input thread instead of repeatedly entering the CRT's
 * environment lock on every controller poll. */
#define INPUT_ENV_CACHE_CAPACITY 64
typedef struct InputEnvCacheEntry {
    const char *name;
    const char *value;
} InputEnvCacheEntry;
#if defined(_MSC_VER)
__declspec(thread) static InputEnvCacheEntry g_input_env_cache[INPUT_ENV_CACHE_CAPACITY];
__declspec(thread) static unsigned int g_input_env_cache_count;
#else
static _Thread_local InputEnvCacheEntry g_input_env_cache[INPUT_ENV_CACHE_CAPACITY];
static _Thread_local unsigned int g_input_env_cache_count;
#endif

static const char *input_cached_getenv(const char *name)
{
    unsigned int i;
    const char *value;
    for (i = 0; i < g_input_env_cache_count; ++i) {
        if (strcmp(g_input_env_cache[i].name, name) == 0)
            return g_input_env_cache[i].value;
    }
    value = getenv(name);
    if (g_input_env_cache_count < INPUT_ENV_CACHE_CAPACITY) {
        g_input_env_cache[g_input_env_cache_count].name = name;
        g_input_env_cache[g_input_env_cache_count].value = value;
        ++g_input_env_cache_count;
    }
    return value;
}


void xbox_InputStopTestAutoA(void)
{
    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
        fprintf(stderr, "[TEST-INPUT] auto-A stopped\n");
    g_test_auto_a_stopped = TRUE;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#include "dualsense_sdl.h"
#include <stdlib.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };
static BOOL g_sdl_controller_logged;
static BOOL g_host_stick_outer_rim_latched[XBOX_MAX_CONTROLLERS][2];
static ULONGLONG g_input_start_ms;
static ULONGLONG g_test_auto_a_last_slot = ~(ULONGLONG)0;
static BOOL g_test_auto_y_emitted;
static BOOL g_test_auto_y_logged;
static unsigned int g_test_exact_new_game_emitted;
static unsigned int g_test_exact_new_game_active_slot;
static ULONGLONG g_test_exact_new_game_pulse_start_ms;
static unsigned int g_test_exact_load_game_emitted;
static unsigned int g_test_exact_load_game_active_slot;
static ULONGLONG g_test_exact_load_game_pulse_start_ms;
static BOOL g_test_movie_skip_b_emitted;
static ULONGLONG g_test_movie_skip_b_pulse_start_ms;
static ULONGLONG g_test_auto_y_armed_ms;
static ULONGLONG g_test_auto_y_emitted_ms;
static ULONGLONG g_test_auto_move_armed_ms;
static BOOL g_test_auto_move_started;
static BOOL g_test_auto_move_ended;
static BOOL g_test_auto_move_turn_logged;
static BOOL g_test_auto_move_walk_logged;

/* Diagnostic-only sampling of the host controller range.  The retail game
 * expects the original Xbox stick to reach its full signed-16-bit range;
 * modern controllers can report a smaller physical outer radius.  Report the
 * unmodified host values so calibration can be derived from measurements
 * without changing the game's authored movement thresholds. */
static void trace_controller_axes(DWORD port, const char *source,
                                  const XBOX_INPUT_STATE *state)
{
    static ULONGLONG last_report_ms;
    static SHORT min_lx, max_lx, min_ly, max_ly;
    const ULONGLONG now = GetTickCount64();
    const SHORT lx = state->Gamepad.sThumbLX;
    const SHORT ly = state->Gamepad.sThumbLY;
    const int active = lx < -16384 || lx > 16384 ||
                       ly < -16384 || ly > 16384;

    if (port != 0u || input_cached_getenv("MERCENARIES_TRACE_CONTROLLER_AXES") == NULL)
        return;
    if (lx < min_lx) min_lx = lx;
    if (lx > max_lx) max_lx = lx;
    if (ly < min_ly) min_ly = ly;
    if (ly > max_ly) max_ly = ly;
    if (!active || now - last_report_ms < 250u)
        return;
    last_report_ms = now;
    fprintf(stderr,
            "[INPUT-AXES] source=%s raw=%d,%d extrema=%d..%d,%d..%d "
            "magnitude2=%lld\n",
            source, (int)lx, (int)ly,
            (int)min_lx, (int)max_lx, (int)min_ly, (int)max_ly,
            (long long)lx * lx + (long long)ly * ly);
    fflush(stderr);
}

/* SDL exposes the controller's physical stick range.  Some PlayStation pads
 * stop a little short of the original Xbox signed-16-bit radial range (the
 * measured DualSense rim is about 30,200) and can jitter slightly while held
 * there.  Calibrate only that physical outer rim to the Xbox rim, preserving
 * direction and the complete response below it.  A narrow release hysteresis
 * prevents a held stick from falling off the rim for a single poll.  This is a
 * host-controller correction; guest-authored movement thresholds stay intact. */
#define HOST_STICK_OUTER_RIM_ENTER 30000
#define HOST_STICK_OUTER_RIM_RELEASE 29500
#define XBOX_STICK_RADIAL_MAX 32767

static SHORT clamp_scaled_stick_axis(double value)
{
    long rounded = (long)(value + (value >= 0.0 ? 0.5 : -0.5));
    if (rounded > 32767)
        return 32767;
    if (rounded < -32768)
        return -32768;
    return (SHORT)rounded;
}

static void normalize_host_stick_outer_rim(SHORT *x, SHORT *y, BOOL *latched)
{
    const long long magnitude_sq =
        (long long)(*x) * (*x) + (long long)(*y) * (*y);
    const long long enter_sq =
        (long long)HOST_STICK_OUTER_RIM_ENTER * HOST_STICK_OUTER_RIM_ENTER;
    const long long release_sq =
        (long long)HOST_STICK_OUTER_RIM_RELEASE * HOST_STICK_OUTER_RIM_RELEASE;
    const long long xbox_max_sq =
        (long long)XBOX_STICK_RADIAL_MAX * XBOX_STICK_RADIAL_MAX;

    if (!*latched) {
        if (magnitude_sq < enter_sq)
            return;
        *latched = TRUE;
    } else if (magnitude_sq < release_sq) {
        *latched = FALSE;
        return;
    }

    /* Square-gated diagonals can already exceed the Xbox radial maximum. */
    if (magnitude_sq > 0 && magnitude_sq < xbox_max_sq) {
        const double scale =
            (double)XBOX_STICK_RADIAL_MAX / sqrt((double)magnitude_sq);
        *x = clamp_scaled_stick_axis((double)(*x) * scale);
        *y = clamp_scaled_stick_axis((double)(*y) * scale);
    }
}

static void normalize_host_stick_outer_rims(DWORD port,
                                             XBOX_INPUT_STATE *state)
{
    normalize_host_stick_outer_rim(&state->Gamepad.sThumbLX,
                                   &state->Gamepad.sThumbLY,
                                   &g_host_stick_outer_rim_latched[port][0]);
    normalize_host_stick_outer_rim(&state->Gamepad.sThumbRX,
                                   &state->Gamepad.sThumbRY,
                                   &g_host_stick_outer_rim_latched[port][1]);
}
void xbox_InputArmTestAutoY(void)
{
    g_test_auto_y_armed_ms = GetTickCount64();
    g_test_auto_y_emitted = FALSE;
    g_test_auto_y_logged = FALSE;
    g_test_auto_y_emitted_ms = 0u;
    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
        fprintf(stderr, "[TEST-INPUT] auto-Y armed after movie close\n");
}

void xbox_InputDisarmTestAutoY(void)
{
    g_test_auto_y_armed_ms = 0u;
    g_test_auto_y_emitted = TRUE;
    g_test_auto_y_emitted_ms = 0u;
    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
        fprintf(stderr, "[TEST-INPUT] auto-Y disarmed after completed stand-up\n");
}

void xbox_InputArmTestAutoMove(void)
{
    if (g_test_auto_move_armed_ms != 0u)
        return;
    g_test_auto_move_armed_ms = GetTickCount64();
    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
        fprintf(stderr, "[TEST-INPUT] auto-move armed after stand-up\n");
}

static BOOL keyboard_fallback_enabled(void)
{
    const char *value = input_cached_getenv("MERCENARIES_DISABLE_KEYBOARD");
    return value == NULL || value[0] == '\0' || value[0] == '0';
}

/* Unattended tests must not consume the user's controller or send vibration
 * to it while another preview is running. Keep scripted/local-file input,
 * but bypass physical devices entirely. Never enabled by normal previews. */
static BOOL test_input_isolated(void)
{
    static int initialized;
    static BOOL isolated;
    if (!initialized) {
        const char *value = input_cached_getenv("MERCENARIES_TEST_ISOLATE_INPUT");
        isolated = value && value[0] && value[0] != '0';
        initialized = 1;
    }
    return isolated;
}

static BOOL key_down(int virtual_key)
{
    if (test_input_isolated())
        return FALSE;
    HWND foreground = GetForegroundWindow();
    DWORD foreground_process = 0;

    if (foreground == NULL)
        return FALSE;
    GetWindowThreadProcessId(foreground, &foreground_process);
    if (foreground_process != GetCurrentProcessId())
        return FALSE;
    return xbox_InputKeyDown(virtual_key);
}

static ULONGLONG test_input_delay_ms(const char *name, ULONGLONG fallback)
{
    const char *value = input_cached_getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0')
        return fallback;
    return (ULONGLONG)parsed;
}

static BOOL apply_test_exact_load_game_sequence(XBOX_INPUT_STATE *state,
                                                WORD *buttons,
                                                ULONGLONG elapsed,
                                                BOOL guest_poll)
{
    static const char *const delay_names[6] = {
        "MERCENARIES_TEST_EXACT_LOAD_START_DELAY_MS",
        "MERCENARIES_TEST_EXACT_LOAD_DOWN1_DELAY_MS",
        "MERCENARIES_TEST_EXACT_LOAD_DOWN2_DELAY_MS",
        "MERCENARIES_TEST_EXACT_LOAD_OPEN_DELAY_MS",
        "MERCENARIES_TEST_EXACT_LOAD_SELECT_DELAY_MS",
        "MERCENARIES_TEST_EXACT_LOAD_CONFIRM_DELAY_MS"
    };
    static const ULONGLONG fallback_delays[6] = {
        90000u, 105000u, 108000u, 112000u, 118000u, 121000u
    };
    const BOOL single_poll =
        input_cached_getenv("MERCENARIES_TEST_EXACT_LOAD_SINGLE_POLL") != NULL;
    const ULONGLONG pulse = test_input_delay_ms(
        "MERCENARIES_TEST_EXACT_LOAD_PULSE_MS", 300u);
    unsigned int slot;

    if (input_cached_getenv("MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE") == NULL ||
        input_cached_getenv("MERCENARIES_TEST_EXACT_NEW_GAME_SEQUENCE") != NULL ||
        single_poll != guest_poll)
        return FALSE;

    for (slot = 0; slot < 6u; ++slot) {
        const unsigned int bit = 1u << slot;
        const ULONGLONG delay = test_input_delay_ms(
            delay_names[slot], fallback_delays[slot]);
        const ULONGLONG now = GetTickCount64();
        if ((g_test_exact_load_game_emitted & bit) != 0u)
            continue;
        if (elapsed < delay)
            break;
        if (g_test_exact_load_game_active_slot != slot) {
            g_test_exact_load_game_active_slot = slot;
            g_test_exact_load_game_pulse_start_ms = now;
            if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                fprintf(stderr,
                        "[TEST-INPUT] exact-load-game slot=%u elapsed=%llu\n",
                        slot, (unsigned long long)elapsed);
        }
        if (now - g_test_exact_load_game_pulse_start_ms < pulse) {
            if (slot == 0u)
                *buttons |= XBOX_GAMEPAD_START;
            else if (slot == 1u || slot == 2u)
                *buttons |= XBOX_GAMEPAD_DPAD_DOWN;
            else
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            if (single_poll && slot != 0u) {
                g_test_exact_load_game_emitted |= bit;
                g_test_exact_load_game_active_slot = 6u;
                g_test_exact_load_game_pulse_start_ms = 0u;
            }
        } else {
            g_test_exact_load_game_emitted |= bit;
            g_test_exact_load_game_active_slot = 6u;
            g_test_exact_load_game_pulse_start_ms = 0u;
        }
        break;
    }
    return TRUE;
}

BOOL xbox_InputApplyTestExactLoadGame(XBOX_INPUT_STATE *state)
{
    WORD buttons;
    if (state == NULL)
        return FALSE;
    buttons = state->Gamepad.wButtons;
    if (!apply_test_exact_load_game_sequence(
            state, &buttons, GetTickCount64() - g_input_start_ms, TRUE))
        return FALSE;
    state->Gamepad.wButtons = buttons;
    return TRUE;
}
static void read_keyboard_state(XBOX_INPUT_STATE *state)
{
    if (g_input_mapper && (!test_input_isolated() ||
        input_cached_getenv("MERCENARIES_TEST_KEYS_FILE") != NULL) &&
        g_input_mapper(state, TRUE)) {
        state->dwPacketNumber = ++g_last_packet[0];
        return;
    }

    WORD buttons = 0;
    const SHORT stick_max = 32767;
    const ULONGLONG elapsed = GetTickCount64() - g_input_start_ms;

    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = ++g_last_packet[0];
    if (key_down(VK_UP))    buttons |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(VK_DOWN))  buttons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(VK_LEFT))  buttons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(VK_RIGHT)) buttons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(VK_RETURN)) buttons |= XBOX_GAMEPAD_START;
    if (key_down(VK_ESCAPE)) buttons |= XBOX_GAMEPAD_BACK;

    /* Diagnostic-only, process-local navigation.  Unlike host SendInput this
     * can never leak keystrokes into another application.  Emit one short tap
     * for Start, then exactly three A taps: New Game, agent, Accept. */
    if (input_cached_getenv("MERCENARIES_TEST_EXACT_NEW_GAME_SEQUENCE") != NULL) {
        static const char *const delay_names[4] = {
            "MERCENARIES_TEST_EXACT_START_DELAY_MS",
            "MERCENARIES_TEST_EXACT_A1_DELAY_MS",
            "MERCENARIES_TEST_EXACT_A2_DELAY_MS",
            "MERCENARIES_TEST_EXACT_A3_DELAY_MS"
        };
        static const ULONGLONG fallback_delays[4] = {
            45000u, 65000u, 70000u, 75000u
        };
        const ULONGLONG pulse = test_input_delay_ms(
            "MERCENARIES_TEST_EXACT_PULSE_MS", 300u);
        unsigned int slot;

        for (slot = 0; slot < 4u; ++slot) {
            const unsigned int bit = 1u << slot;
            const ULONGLONG delay = test_input_delay_ms(
                delay_names[slot], fallback_delays[slot]);
            const ULONGLONG now = GetTickCount64();
            if ((g_test_exact_new_game_emitted & bit) != 0u)
                continue;
            if (elapsed < delay)
                break;
            if (g_test_exact_new_game_active_slot != slot) {
                g_test_exact_new_game_active_slot = slot;
                g_test_exact_new_game_pulse_start_ms = now;
                if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                    fprintf(stderr,
                            "[TEST-INPUT] exact-new-game slot=%u elapsed=%llu\n",
                            slot, (unsigned long long)elapsed);
            }
            if (now - g_test_exact_new_game_pulse_start_ms < pulse) {
                if (slot == 0u)
                    buttons |= XBOX_GAMEPAD_START;
                else
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            } else {
                g_test_exact_new_game_emitted |= bit;
                g_test_exact_new_game_active_slot = 4u;
                g_test_exact_new_game_pulse_start_ms = 0u;
            }
            break;
        }
    }
    /* Single-sample load navigation is applied only by the retail guest poll
     * wrapper. Background host-device queries must not consume those edges. */
    apply_test_exact_load_game_sequence(state, &buttons, elapsed, FALSE);
    /* Test-only input lets unattended retail-XBE runs cross the title prompt.
     * It is opt-in and is never enabled by the preview launcher. */
    if (input_cached_getenv("MERCENARIES_TEST_AUTO_START") != NULL) {
        const ULONGLONG delay = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_START_DELAY_MS", 45000u);
        const ULONGLONG duration = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_START_DURATION_MS", 12000u);
        if (elapsed >= delay && elapsed - delay < duration &&
            (elapsed - delay) % 2000u < 150u)
            buttons |= XBOX_GAMEPAD_START;
    }
    state->Gamepad.wButtons = buttons;

    if (key_down('Z'))
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    /* Test-only menu navigation for unattended progression runs. */
    if (!g_test_auto_a_stopped &&
        input_cached_getenv("MERCENARIES_TEST_AUTO_A") != NULL) {
        const ULONGLONG delay = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_A_DELAY_MS", 70000u);
        const ULONGLONG duration = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_A_DURATION_MS", 60000u);
        const ULONGLONG pulse = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_A_PULSE_MS", 150u);
        if (elapsed >= delay && elapsed - delay < duration) {
            const ULONGLONG offset = elapsed - delay;
            const ULONGLONG slot = offset / 3000u;
            if (input_cached_getenv("MERCENARIES_TEST_AUTO_A_SINGLE_POLL") != NULL) {
                /* A wall-clock pulse can span several very fast guest input
                 * polls and carry the menu accept into a newly opened movie.
                 * This opt-in mode emits one sample per navigation slot. */
                if (slot != g_test_auto_a_last_slot) {
                    g_test_auto_a_last_slot = slot;
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
                    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                        fprintf(stderr,
                                "[TEST-INPUT] auto-A slot=%llu elapsed=%llu\n",
                                (unsigned long long)slot,
                                (unsigned long long)elapsed);
                }
            } else if (offset % 3000u < pulse) {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            }
        }
    }
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = key_down('X') ? 255 : 0;
    /* Diagnostic-only movie skip through the game's real B-button path. */
    if (!g_test_movie_skip_b_emitted &&
        input_cached_getenv("MERCENARIES_TEST_MOVIE_SKIP_B_DELAY_MS") != NULL) {
        const ULONGLONG delay = test_input_delay_ms(
            "MERCENARIES_TEST_MOVIE_SKIP_B_DELAY_MS", 80000u);
        const ULONGLONG pulse = test_input_delay_ms(
            "MERCENARIES_TEST_MOVIE_SKIP_B_PULSE_MS", 300u);
        const ULONGLONG now = GetTickCount64();

        if (elapsed >= delay) {
            if (g_test_movie_skip_b_pulse_start_ms == 0u) {
                g_test_movie_skip_b_pulse_start_ms = now;
                if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                    fprintf(stderr,
                            "[TEST-INPUT] movie-skip B elapsed=%llu\n",
                            (unsigned long long)elapsed);
            }
            if (now - g_test_movie_skip_b_pulse_start_ms < pulse)
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
            else
                g_test_movie_skip_b_emitted = TRUE;
        }
    }
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = key_down('C') ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = key_down('V') ? 255 : 0;
    /* Test-only scripted-use input for unattended gameplay validation. */
    if (input_cached_getenv("MERCENARIES_TEST_AUTO_Y") != NULL) {
        ULONGLONG auto_y_elapsed = elapsed;
        const BOOL after_movie =
            input_cached_getenv("MERCENARIES_TEST_AUTO_Y_AFTER_MOVIE") != NULL;
        const BOOL after_use =
            input_cached_getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL;
        const BOOL after_collision =
            input_cached_getenv("MERCENARIES_TEST_AUTO_Y_AFTER_COLLISION_DISABLED") != NULL;
        const BOOL after_armed_event =
            after_movie || after_use || after_collision;
        const ULONGLONG delay = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_Y_DELAY_MS",
            after_armed_event ? 2500u : 100000u);
        const ULONGLONG duration = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_Y_DURATION_MS", 20000u);
        if (after_armed_event && g_test_auto_y_armed_ms != 0u)
            auto_y_elapsed = GetTickCount64() - g_test_auto_y_armed_ms;
        if ((!after_armed_event || g_test_auto_y_armed_ms != 0u) &&
            auto_y_elapsed >= delay && auto_y_elapsed - delay < duration) {
            if (!g_test_auto_y_logged) {
                g_test_auto_y_logged = TRUE;
                if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                    fprintf(stderr,
                            "[TEST-INPUT] auto-Y tap elapsed=%llu "
                            "after-movie=%u after-use=%u after-collision=%u\n",
                            (unsigned long long)auto_y_elapsed,
                            after_movie, after_use, after_collision);
            }
            if (input_cached_getenv("MERCENARIES_TEST_AUTO_Y_SINGLE_POLL") != NULL) {
                if (!g_test_auto_y_emitted) {
                    g_test_auto_y_emitted = TRUE;
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                }
            } else if (input_cached_getenv("MERCENARIES_TEST_AUTO_Y_ONCE") != NULL) {
                const ULONGLONG pulse = test_input_delay_ms(
                    "MERCENARIES_TEST_AUTO_Y_PULSE_MS", 750u);
                const ULONGLONG now = GetTickCount64();
                if (!g_test_auto_y_emitted) {
                    /* Start the pulse on the first eligible guest poll. The
                     * aircraft load can leave input unpolled long enough to
                     * miss a wall-clock window that began at `delay`. */
                    if (g_test_auto_y_emitted_ms == 0u)
                        g_test_auto_y_emitted_ms = now;
                    if (now - g_test_auto_y_emitted_ms < pulse)
                        state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                    else
                        g_test_auto_y_emitted = TRUE;
                }
            } else if ((auto_y_elapsed - delay) % 2000u < 750u) {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            }
            if (state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] != 0u &&
                g_test_auto_y_emitted_ms == 0u)
                g_test_auto_y_emitted_ms = GetTickCount64();
        }
    }
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = key_down(VK_SHIFT) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = key_down(VK_CONTROL) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('Q') ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('E') ? 255 : 0;

    state->Gamepad.sThumbLX = key_down('D') ? stick_max : (key_down('A') ? -stick_max : 0);
    state->Gamepad.sThumbLY = key_down('W') ? stick_max : (key_down('S') ? -stick_max : 0);
    state->Gamepad.sThumbRX = key_down('L') ? stick_max : (key_down('J') ? -stick_max : 0);
    state->Gamepad.sThumbRY = key_down('I') ? stick_max : (key_down('K') ? -stick_max : 0);
    /* Opt-in gameplay probe: hold forward for a bounded interval so an
     * unattended run can prove that game/play consumes player movement. */
    if (input_cached_getenv("MERCENARIES_TEST_AUTO_MOVE") != NULL) {
        static ULONGLONG auto_move_use_last_slot = ~(ULONGLONG)0;
        const BOOL after_standup =
            input_cached_getenv("MERCENARIES_TEST_AUTO_MOVE_AFTER_STANDUP") != NULL;
        const BOOL after_y =
            input_cached_getenv("MERCENARIES_TEST_AUTO_MOVE_AFTER_Y") != NULL;
        const BOOL no_stick =
            input_cached_getenv("MERCENARIES_TEST_AUTO_MOVE_NO_STICK") != NULL;
        ULONGLONG auto_move_elapsed = elapsed;
        const ULONGLONG delay = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_MOVE_DELAY_MS",
            after_standup ? 1000u : (after_y ? 8000u : 55000u));
        const ULONGLONG duration = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_MOVE_DURATION_MS", 8000u);
        const ULONGLONG turn_duration = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_MOVE_TURN_MS", 0u);
        const ULONGLONG use_delay = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_MOVE_USE_DELAY_MS", 1000u);
        const ULONGLONG use_pulse = test_input_delay_ms(
            "MERCENARIES_TEST_AUTO_MOVE_USE_PULSE_MS", 200u);
        if (after_standup && g_test_auto_move_armed_ms != 0u)
            auto_move_elapsed = GetTickCount64() - g_test_auto_move_armed_ms;
        else if (after_y && g_test_auto_y_emitted_ms != 0u)
            auto_move_elapsed = GetTickCount64() - g_test_auto_y_emitted_ms;
        const BOOL ready =
            (!after_standup || g_test_auto_move_armed_ms != 0u) &&
            (!after_y || g_test_auto_y_emitted_ms != 0u);
        if (ready &&
            auto_move_elapsed >= delay &&
            auto_move_elapsed - delay < duration) {
            if (!g_test_auto_move_started) {
                g_test_auto_move_started = TRUE;
                if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                    fprintf(stderr,
                            "[TEST-INPUT] auto-move start elapsed=%llu\n",
                            (unsigned long long)auto_move_elapsed);
            }
            const ULONGLONG move_offset = auto_move_elapsed - delay;
            if (!no_stick && move_offset < turn_duration) {
                state->Gamepad.sThumbRX = stick_max;
                if (!g_test_auto_move_turn_logged) {
                    g_test_auto_move_turn_logged = TRUE;
                    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                        fprintf(stderr,
                                "[TEST-INPUT] auto-move turn start duration=%llu\n",
                                (unsigned long long)turn_duration);
                }
            } else if (!no_stick) {
                state->Gamepad.sThumbLY = stick_max;
                if (!g_test_auto_move_walk_logged) {
                    g_test_auto_move_walk_logged = TRUE;
                    if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                        fprintf(stderr,
                                "[TEST-INPUT] auto-move walk start elapsed=%llu\n",
                                (unsigned long long)auto_move_elapsed);
                }
            }
            if (input_cached_getenv("MERCENARIES_TEST_AUTO_Y_DURING_MOVE") != NULL) {
                if (move_offset >= turn_duration + use_delay &&
                    (move_offset - turn_duration - use_delay) % 2000u <
                        use_pulse) {
                    const ULONGLONG slot =
                        (move_offset - turn_duration - use_delay) / 2000u;
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                    if (slot != auto_move_use_last_slot &&
                        input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL) {
                        auto_move_use_last_slot = slot;
                        fprintf(stderr,
                                "[TEST-INPUT] auto-move Y slot=%llu "
                                "elapsed=%llu\n",
                                (unsigned long long)slot,
                                (unsigned long long)auto_move_elapsed);
                    }
                }
            }
        } else if (ready && g_test_auto_move_started &&
                   !g_test_auto_move_ended &&
                   auto_move_elapsed >= delay + duration) {
            g_test_auto_move_ended = TRUE;
            if (input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL)
                fprintf(stderr,
                        "[TEST-INPUT] auto-move end elapsed=%llu\n",
                        (unsigned long long)auto_move_elapsed);
        }
    }
}

void xbox_InputInit(void)
{
    g_input_start_ms = GetTickCount64();
    g_test_auto_a_stopped = FALSE;
    g_test_auto_a_last_slot = ~(ULONGLONG)0;
    g_test_auto_y_emitted = FALSE;
    g_test_auto_y_logged = FALSE;
    g_test_exact_new_game_emitted = 0u;
    g_test_exact_new_game_active_slot = 4u;
    g_test_exact_new_game_pulse_start_ms = 0u;
    g_test_exact_load_game_emitted = 0u;
    g_test_exact_load_game_active_slot = 4u;
    g_test_exact_load_game_pulse_start_ms = 0u;
    g_test_movie_skip_b_emitted = FALSE;
    g_test_movie_skip_b_pulse_start_ms = 0u;
    g_test_auto_y_armed_ms = 0u;
    g_test_auto_y_emitted_ms = 0u;
    g_test_auto_move_armed_ms = 0u;
    g_test_auto_move_started = FALSE;
    g_test_auto_move_ended = FALSE;
    g_test_auto_move_turn_logged = FALSE;
    g_test_auto_move_walk_logged = FALSE;
    if (test_input_isolated()) {
        for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; ++i)
            g_controller_connected[i] = i == 0u;
        return;
    }
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
    dualsense_sdl_init(g_controller_connected);
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; ++i) {
        if (dualsense_sdl_is_connected(i))
            g_controller_connected[i] = TRUE;
    }
    if (keyboard_fallback_enabled())
        g_controller_connected[0] = TRUE;
}

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState, BOOL overlay_camera)
{
    XINPUT_STATE xi_state;
    XBOX_INPUT_STATE sdl_state;
    DWORD result;
    DWORD sdl_result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (overlay_captures_input() && !overlay_camera) {
        memset(pState, 0, sizeof(*pState));
        return ERROR_SUCCESS;
    }

    if (test_input_isolated()) {
        if (dwPort != 0u)
            return ERROR_DEVICE_NOT_CONNECTED;
        g_controller_connected[0] = TRUE;
        read_keyboard_state(pState);
        return ERROR_SUCCESS;
    }

    if (dwPort == 0 &&
        input_cached_getenv("MERCENARIES_TEST_FORCE_KEYBOARD") != NULL) {
        static BOOL force_keyboard_poll_logged;
        g_controller_connected[0] = TRUE;
        if (!force_keyboard_poll_logged &&
            input_cached_getenv("MERCENARIES_TRACE_TEST_INPUT") != NULL) {
            force_keyboard_poll_logged = TRUE;
            fprintf(stderr, "[TEST-INPUT] first forced-keyboard poll\n");
            fflush(stderr);
        }
        read_keyboard_state(pState);
        return ERROR_SUCCESS;
    }

    result = XInputGetState(dwPort, &xi_state);
    sdl_result = dualsense_sdl_get_state(dwPort, &sdl_state);
    if (sdl_result == ERROR_SUCCESS && !g_sdl_controller_logged &&
        input_cached_getenv("MERCENARIES_TRACE_CONTROLLER") != NULL) {
        g_sdl_controller_logged = TRUE;
        fprintf(stderr,
                "[INPUT-SDL] PlayStation controller active on Xbox port %lu "
                "axes=%d,%d,%d,%d triggers=%u,%u\n",
                (unsigned long)dwPort,
                sdl_state.Gamepad.sThumbLX, sdl_state.Gamepad.sThumbLY,
                sdl_state.Gamepad.sThumbRX, sdl_state.Gamepad.sThumbRY,
                sdl_state.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER],
                sdl_state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]);
        fflush(stderr);
    }
    if (result != ERROR_SUCCESS && sdl_result != ERROR_SUCCESS) {
        if(dwPort==0)memset(g_prompt_pad_activity,0,sizeof(g_prompt_pad_activity));
        if (dwPort == 0 && keyboard_fallback_enabled()) {
            g_controller_connected[0] = TRUE;
            read_keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        g_controller_connected[dwPort] = FALSE;
        memset(g_host_stick_outer_rim_latched[dwPort], 0,
               sizeof(g_host_stick_outer_rim_latched[dwPort]));
        return result;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    if (result == ERROR_SUCCESS) {
        g_last_packet[dwPort] = xi_state.dwPacketNumber;
        pState->dwPacketNumber = xi_state.dwPacketNumber;
        pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
            xi_state.Gamepad.bLeftTrigger;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
            xi_state.Gamepad.bRightTrigger;
        pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
        pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
        pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
        pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;
    }

    if(dwPort==0 && result==ERROR_SUCCESS)input_prompt_pad(XBOX_PROMPT_XBOX,&pState->Gamepad);
    else if(dwPort==0)memset(&g_prompt_pad_activity[XBOX_PROMPT_XBOX],0,sizeof(PromptPadActivity));
    if(dwPort==0 && sdl_result==ERROR_SUCCESS)input_prompt_pad(XBOX_PROMPT_PLAYSTATION,&sdl_state.Gamepad);
    else if(dwPort==0)memset(&g_prompt_pad_activity[XBOX_PROMPT_PLAYSTATION],0,sizeof(PromptPadActivity));
    if (sdl_result == ERROR_SUCCESS) {
        unsigned int i;
        pState->dwPacketNumber = sdl_state.dwPacketNumber;
        pState->Gamepad.wButtons |= sdl_state.Gamepad.wButtons;
        for (i = 0; i < sizeof(pState->Gamepad.bAnalogButtons); ++i) {
            if (sdl_state.Gamepad.bAnalogButtons[i] >
                pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] =
                    sdl_state.Gamepad.bAnalogButtons[i];
        }
        /* A native PlayStation controller owns player one's axes even when a
         * stale virtual XInput device also occupies slot zero. */
        pState->Gamepad.sThumbLX = sdl_state.Gamepad.sThumbLX;
        pState->Gamepad.sThumbLY = sdl_state.Gamepad.sThumbLY;
        pState->Gamepad.sThumbRX = sdl_state.Gamepad.sThumbRX;
        pState->Gamepad.sThumbRY = sdl_state.Gamepad.sThumbRY;
    }
    g_controller_connected[dwPort] = TRUE;
    if (dwPort == 0 && g_input_mapper) g_input_mapper(pState, FALSE);

    if (dwPort == 0 && keyboard_fallback_enabled()) {
        XBOX_INPUT_STATE keyboard_state;
        unsigned int i;
        read_keyboard_state(&keyboard_state);
        pState->dwPacketNumber = keyboard_state.dwPacketNumber;
        pState->Gamepad.wButtons |= keyboard_state.Gamepad.wButtons;
        for (i = 0; i < sizeof(pState->Gamepad.bAnalogButtons); ++i) {
            if (keyboard_state.Gamepad.bAnalogButtons[i] >
                pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] =
                    keyboard_state.Gamepad.bAnalogButtons[i];
        }
        if (keyboard_state.Gamepad.sThumbLX)
            pState->Gamepad.sThumbLX = keyboard_state.Gamepad.sThumbLX;
        if (keyboard_state.Gamepad.sThumbLY)
            pState->Gamepad.sThumbLY = keyboard_state.Gamepad.sThumbLY;
        if (keyboard_state.Gamepad.sThumbRX)
            pState->Gamepad.sThumbRX = keyboard_state.Gamepad.sThumbRX;
        if (keyboard_state.Gamepad.sThumbRY)
            pState->Gamepad.sThumbRY = keyboard_state.Gamepad.sThumbRY;
    }

    trace_controller_axes(dwPort,
                          sdl_result == ERROR_SUCCESS ? "sdl-playstation" :
                          (result == ERROR_SUCCESS ? "xinput" : "keyboard"),
                          pState);
    normalize_host_stick_outer_rims(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get_state(dwPort, pState, FALSE);
}

/* Host camera only. The caller consumes this separate packet; retail still
 * receives neutral input while the overlay owns normal input capture. */
DWORD xbox_InputGetOverlayState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return input_get_state(dwPort, pState, TRUE);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (test_input_isolated())
        return dwPort == 0u ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    result = XInputSetState(dwPort, &xi_vib);
    if (result != ERROR_SUCCESS &&
        dualsense_sdl_set_vibration(dwPort, pVibration) == ERROR_SUCCESS)
        return ERROR_SUCCESS;
    if (result != ERROR_SUCCESS && dwPort == 0 && keyboard_fallback_enabled())
        return ERROR_SUCCESS;
    return result;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    if (test_input_isolated()) return dwPort == 0u;
    if (dwPort == 0 && keyboard_fallback_enabled()) return TRUE;
    return g_controller_connected[dwPort] ||
           dualsense_sdl_is_connected(dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (test_input_isolated()) {
        if (dwPort != 0u)
            return ERROR_DEVICE_NOT_CONNECTED;
        memset(pCaps, 0, sizeof(*pCaps));
        pCaps->Type = 1;
        pCaps->SubType = 1;
        return ERROR_SUCCESS;
    }

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) {
        if (dualsense_sdl_is_connected(dwPort)) {
            memset(pCaps, 0, sizeof(*pCaps));
            pCaps->Type = 1;
            pCaps->SubType = 1;
            return ERROR_SUCCESS;
        }
        if (dwPort == 0 && keyboard_fallback_enabled()) {
            memset(pCaps, 0, sizeof(*pCaps));
            pCaps->Type = 1;
            pCaps->SubType = 1;
            return ERROR_SUCCESS;
        }
        return result;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}
/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
BOOL xbox_InputApplyTestExactLoadGame(XBOX_INPUT_STATE *state)
{
    (void)state;
    return FALSE;
}
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputGetOverlayState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    return xbox_InputGetState(dwPort, pState);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
