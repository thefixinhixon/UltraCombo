// Windowed frontend: video, audio, input, and the quality-of-life features
// (pause, fast-forward, fullscreen, screenshots, auto-coin, volume).
//
// Everything that reaches the emulated machine goes through the input ports
// once per frame (hw.p1, hw.p2, hw.volume), so the emulation stays a pure
// function of those inputs (needed for netplay later).
#include "config.h"
#include "controller_labels.h"
#include "ide.h"
#include "machine.h"
#include "mem.h"
#include "osd.h"
#include "png.h"
#include "sound.h"
#include "state.h"
#include "netplay.h"
#include "training.h"
#include "launch.h"
#include "icon.h"
#include "adjust.h"
#include "rec_build.h"
#include "rec.h"
#include "game.h"
#include "video.h"
#include "shaders.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#define AUDIO_HZ 48000

static SDL_Window *win;
static SDL_AudioDeviceID adev;
static uint32_t px[320 * 240];     // game image
static uint32_t ov[320 * 240];     // overlay: messages, menu, input display

static bool paused, step_once, fullscreen, muted, vsync;
static bool vsync_lock;     // vsync on a ~60 Hz display: one game frame per refresh
static bool vsync_broken;   // vsync turned out not to wait for the display: audio pacing
static bool focused = true;
static bool apply_shader(void);
static int  volume;
static double display_hz;
static uint64_t frame_count;

/* ---------------------------------------------------------------- */
/* Audio                                                            */
/* ---------------------------------------------------------------- */

// Resample DCS output (usually 31.25 kHz) to the device rate and queue it.
// `speed` > 1 when the game runs faster than 58.96 Hz: the same audio is
// played in less time (and slightly higher pitch).
static void pump_audio(bool discard, double speed)
{
    static int16_t in[8192], out[24576];
    static double pos;      // fractional read position into the previous+current block
    static int16_t prev;
    int n = sound_fetch(in, 8192);
    if (!adev || discard) return;
    double step = sound_rate() / AUDIO_HZ * speed;
    int gain = muted || (!focused && cfg.mute_unfocused) ? 0 : volume * 256 / 100;
    int m = 0;
    while (pos < n && m < 24576) {
        int i = (int)pos;
        double f = pos - i;
        int a = i == 0 ? prev : in[i - 1];
        int b = in[i];
        out[m++] = (int16_t)((int)(a + (b - a) * f) * gain / 256);
        pos += step;
    }
    pos -= n;
    if (n) prev = in[n - 1];
    // after a long hitch, throw away the backlog instead of lagging behind
    if (SDL_GetQueuedAudioSize(adev) > AUDIO_HZ * 2 * 250 / 1000) SDL_ClearQueuedAudio(adev);
    SDL_QueueAudio(adev, out, (Uint32)m * 2);
}

static double queued_ms(void)
{
    return adev ? SDL_GetQueuedAudioSize(adev) * 1000.0 / (AUDIO_HZ * 2) : 0;
}

/* ---------------------------------------------------------------- */
/* Video                                                            */
/* ---------------------------------------------------------------- */

// On a ~60 Hz display, vsync runs one game frame per refresh (smooth, 1.7%
// fast). Vsync on any other display stays tear-free but the game is timed by
// the clock, at its own speed: one frame per refresh would run a 144 Hz
// display at 2.4x. Auto uses vsync on ~60 Hz displays only. If vsync turns
// out not to wait (a driver setting can force it off), audio pacing takes over.
static void choose_pacing(void)
{
    SDL_DisplayMode m;
    int idx = SDL_GetWindowDisplayIndex(win);
    display_hz = (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &m) == 0 && m.refresh_rate) ? m.refresh_rate : 60;
    if (getenv("KI2_DISPLAY_HZ")) display_hz = atof(getenv("KI2_DISPLAY_HZ"));   // testing
    bool sixty = display_hz >= 59 && display_hz <= 61;
    bool want = !vsync_broken && (cfg.pacing == PACING_VSYNC || (cfg.pacing == PACING_AUTO && sixty));
    bool lock = want && sixty;
    if (want != vsync || lock != vsync_lock || !frame_count) {
        vsync = want;
        vsync_lock = lock;
        video_set_vsync(vsync);
        fprintf(stderr, "video: %.0f Hz display, %s pacing\n", display_hz,
                lock ? "vsync" : vsync ? "vsync display, clock-timed" : "audio");
    }
}

// Called once per game frame in display-locked pacing. Vsync that works
// can't present faster than the display refreshes; seeing far more for two
// seconds running means it isn't waiting, and the game would run too fast.
static void check_vsync(void)
{
    static uint64_t t0;
    static int frames, strikes;
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (!t0 || !focused || now - t0 > 2 * f) { t0 = now; frames = strikes = 0; return; }   // fresh start
    frames++;
    if (now - t0 < f) return;
    double rate = frames * (double)f / (double)(now - t0);
    strikes = rate > display_hz * 1.25 + 5 ? strikes + 1 : 0;
    t0 = now;
    frames = 0;
    if (strikes >= 2) {
        fprintf(stderr, "video: vsync isn't waiting for the display (%.0f frames/s on %.0f Hz): audio pacing\n", rate, display_hz);
        osd_message("Vsync isn't working here: timing by audio instead");
        vsync_broken = true;
        strikes = 0;
        choose_pacing();
    }
}

static void set_fullscreen(bool on)
{
    fullscreen = on;
    if (on && cfg.fullscreen_exclusive) {
        // exclusive: switch the display to the chosen mode (or keep the desktop's)
        SDL_DisplayMode want = { 0 }, got;
        int d = SDL_GetWindowDisplayIndex(win);
        if (*cfg.fullscreen_mode && sscanf(cfg.fullscreen_mode, "%dx%d@%d", &want.w, &want.h, &want.refresh_rate) == 3 &&
            SDL_GetClosestDisplayMode(d, &want, &got))
            SDL_SetWindowDisplayMode(win, &got);
        else if (SDL_GetDesktopDisplayMode(d, &got) == 0)
            SDL_SetWindowDisplayMode(win, &got);
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
    } else SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
    choose_pacing();
}

// Menu changes to the display are applied once the choice has settled:
// switching at every step of a cycle, exclusive modes especially, can lose
// the window under XWayland on GNOME. want_display: -1 = none pending, else
// 0 off, 1 borderless, 2 exclusive (cfg.fullscreen_exclusive already says which).
static int want_display = -1;
static uint32_t display_due;

static int shown_display(void)
{
    if (want_display >= 0) return want_display;
    return !fullscreen ? 0 : cfg.fullscreen_exclusive ? 2 : 1;
}

static void request_display(int m)
{
    want_display = m;
    display_due = SDL_GetTicks() + 400;
}

// now: apply a pending change at once (closing the menu, quitting)
static void settle_display(bool now)
{
    if (want_display < 0 || (!now && !SDL_TICKS_PASSED(SDL_GetTicks(), display_due))) return;
    int m = want_display;
    want_display = -1;
    set_fullscreen(m != 0);                 // exclusive to exclusive: the mode changes in place
}

static void draw_menu(uint32_t *ov);
static void draw_inputs(uint32_t *ov);

// Performance overlay: measured over the last second.
static struct { double emu_ms, emu_acc, fps; int emu_n, frames; uint64_t t0; } perf;

static void perf_emu(uint64_t start)
{
    perf.emu_acc += (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    perf.emu_n++;
}

static double queued_ms(void);

static void draw_perf(uint32_t *ov)
{
    uint64_t now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    perf.frames++;
    if (!perf.t0) perf.t0 = now;
    if (now - perf.t0 >= f) {
        double s = (double)(now - perf.t0) / (double)f;
        perf.fps = perf.frames / s;
        perf.emu_ms = perf.emu_n ? perf.emu_acc / perf.emu_n : 0;
        perf.frames = perf.emu_n = 0;
        perf.emu_acc = 0;
        perf.t0 = now;
    }
    if (!cfg.perf_overlay) return;
    char l1[48], l2[48];
    snprintf(l1, sizeof l1, "%.1f FPS  AUDIO %.0f MS", perf.fps, queued_ms());
    // load: the share of each frame's time spent emulating it (under 100% keeps
    // up). Shown as a load rather than a speed, which read as the game running fast.
    snprintf(l2, sizeof l2, "EMU %.2f MS  LOAD %.0f%%", perf.emu_ms, perf.emu_ms * FRAME_HZ / 10.0);
    int w = (int)(strlen(l1) > strlen(l2) ? strlen(l1) : strlen(l2)) * 6 + 6;
    osd_box(ov, 320 - w - 2, 240 - 34, w, 20, 0xb0000000u);
    osd_text(ov, 320 - w + 1, 240 - 32, l1, 0x80ff80);
    osd_text(ov, 320 - w + 1, 240 - 23, l2, 0x80ff80);
}

static void present(const char *status)
{
    if (!status) status = training_status();
    to_rgb(px);
    osd_clear(ov);
    draw_inputs(ov);
    draw_perf(ov);
    draw_menu(ov);
    osd_draw(ov, status);           // after the menu: messages show over it too
    video_present(px, ov, cfg.filter, cfg.scale_mode);
}

static void screenshot(void)
{
    char dir[4096], path[4200];
    snprintf(dir, sizeof dir, "%sscreenshots", data_dir());
    make_dir(dir);
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&t));
    snprintf(path, sizeof path, "%s/%s_%s.png", dir, game.id, stamp);
    for (int i = 2; i < 100; i++) {         // several in one second
        FILE *f = fopen(path, "rb");
        if (!f) break;
        fclose(f);
        snprintf(path, sizeof path, "%s/%s_%s_%d.png", dir, game.id, stamp, i);
    }
    to_rgb(px);
    if (png_write(path, px, 320, 240)) { osd_message("Screenshot saved"); fprintf(stderr, "screenshot: %s\n", path); }
    else osd_message("Screenshot failed");
}

/* ---------------------------------------------------------------- */
/* Input                                                            */
/* ---------------------------------------------------------------- */

#define MAX_PADS 2
static SDL_GameController *pads[MAX_PADS];
static pad_prompt_t pad_prompt;

static int prompt_override(void)
{
    for (int p = 0; p < MAX_PADS; p++)
        if (pads[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[p])) == pad_prompt.last_pad)
            return cfg.pad_family[p];
    return PAD_FAMILY_AUTO;
}

static const char *pad_label(int button)
{
    return pad_button_name(SDL_GameControllerFromInstanceID(pad_prompt.last_pad), prompt_override(), button);
}

static void pad_added(int index)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    for (int i = 0; i < MAX_PADS; i++)                      // already a player (startup scan + its event)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return;
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i]) continue;
        pads[i] = SDL_GameControllerOpen(index);
        if (pads[i]) {
            fprintf(stderr, "pad: player %d = %s\n", i + 1, SDL_GameControllerName(pads[i]));
            osd_message("Player %d: %s", i + 1, SDL_GameControllerName(pads[i]));
        }
        return;
    }
}

static void pad_removed(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            osd_message("Player %d controller disconnected", i + 1);
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
}

static bool pad_pressed(SDL_GameController *c, int b)
{
    if (b == PAD_NONE) return false;
    if (b == PAD_LT) return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
    if (b == PAD_RT) return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b);
}

// Buttons held by each player this frame, one bit per BTN_*.
static void read_players(uint32_t held[2])
{
    const uint8_t *ks = SDL_GetKeyboardState(NULL);
    for (int p = 0; p < 2; p++) {
        held[p] = 0;
        for (int b = 0; b < NBUTTONS; b++)
            if (cfg.keys[p][b] && ks[cfg.keys[p][b]]) held[p] |= 1u << b;
        SDL_GameController *c = pads[p];
        if (!c) continue;
        for (int b = 0; b < NBUTTONS; b++)
            if (pad_pressed(c, cfg.pad[b])) held[p] |= 1u << b;
        int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (y < -cfg.pad_deadzone) held[p] |= 1u << BTN_UP;
        if (y > cfg.pad_deadzone) held[p] |= 1u << BTN_DOWN;
        if (x < -cfg.pad_deadzone) held[p] |= 1u << BTN_LEFT;
        if (x > cfg.pad_deadzone) held[p] |= 1u << BTN_RIGHT;
    }
    // SOCD cleaning: opposite directions together resolve the way fight sticks do
    for (int p = 0; p < 2 && cfg.socd; p++) {
        const uint32_t lr = 1u << BTN_LEFT | 1u << BTN_RIGHT, ud = 1u << BTN_UP | 1u << BTN_DOWN;
        if ((held[p] & lr) == lr) held[p] &= ~lr;
        if ((held[p] & ud) == ud) held[p] &= ~(1u << BTN_DOWN);
    }
}

// Auto-coin: pressing Start without enough credits drops in the missing
// coins first, then presses Start. Driven entirely through the coin and
// start inputs, so the game does its own credit bookkeeping.

static struct { int player, coins, t, wait; bool active; } ac;

static int credits(void)
{
    if (!game.credits) return -1;      // not known for this game
    uint32_t v;
    memcpy(&v, &ram2[game.credits], 4);
    return v < 100 ? (int)v : -1;   // garbage: different code version
}

static void auto_coin(uint32_t held[2])
{
    static uint32_t prev[2];
    if (!ac.active && cfg.auto_coin && !(hw.dsw & 0x100 && (hw.dsw & 0x3e00) == 0x3000))
        for (int p = 0; p < 2; p++) {
            int c = credits();
            bool press = (held[p] & ~prev[p]) >> BTN_START & 1;
            if (press && c >= 0 && c < game.credits_needed) {
                ac.player = p; ac.coins = game.credits_needed - c; ac.t = ac.wait = 0; ac.active = true;
                break;
            }
        }
    prev[0] = held[0]; prev[1] = held[1];
    if (!ac.active) return;

    // 8 frames coin down, 8 up, per coin; wait (up to 1.5 s) for the game to
    // count them; then 10 frames of Start. Early in boot the game ignores
    // coins, in which case this gives up quietly.
    held[ac.player] &= ~(1u << BTN_START);
    int coin_frames = ac.coins * 16;
    if (ac.t < coin_frames) {
        if (ac.t % 16 < 8) held[ac.player] |= 1u << BTN_COIN;
        ac.t++;
    } else if (ac.t == coin_frames) {
        if (credits() >= game.credits_needed) ac.t++;
        else if (++ac.wait > 90) ac.active = false;
    } else if (ac.t < coin_frames + 11) {
        held[ac.player] |= 1u << BTN_START;
        ac.t++;
    } else ac.active = false;
}

// Rumble when a player's health drops. In netplay only your own player counts.
static void rumble_check(void)
{
    static int last[2] = { -1, -1 };
    for (int p = 0; p < 2; p++) {
        int h = game_health(p);
        int prev = last[p];
        last[p] = h;
        if (!cfg.rumble || h < 0 || prev < 0 || h >= prev) continue;
        SDL_GameController *c = NULL;
        if (netplay_running()) { if (p == netplay_local_player()) c = pads[0]; }
        else c = pads[p];
        if (!c) continue;
        int d = prev - h > 40 ? 40 : prev - h;
        uint16_t strength = (uint16_t)(0x5000 + d * 0x2a0);
        SDL_GameControllerRumble(c, strength, (uint16_t)(strength / 2), (uint32_t)(90 + d * 6));
    }
}

/* ---------------------------------------------------------------- */
/* Input display                                                    */
/* ---------------------------------------------------------------- */

#define HIST 12
static struct { uint32_t held; int frames; } inhist[2][HIST];   // [0] is the newest

static void record_inputs(const uint32_t held[2])
{
    const uint32_t mask = (1u << BTN_START) - 1;               // directions and attacks
    for (int p = 0; p < 2; p++) {
        uint32_t h = held[p] & mask;
        if (inhist[p][0].frames && inhist[p][0].held == h) {
            if (inhist[p][0].frames < 99) inhist[p][0].frames++;
            continue;
        }
        memmove(&inhist[p][1], &inhist[p][0], (HIST - 1) * sizeof inhist[p][0]);
        inhist[p][0].held = h;
        inhist[p][0].frames = 1;
    }
}

static void describe(uint32_t h, char *buf, size_t n)
{
    int up = h >> BTN_UP & 1, down = h >> BTN_DOWN & 1, left = h >> BTN_LEFT & 1, right = h >> BTN_RIGHT & 1;
    int row = up ? 2 : down ? 0 : 1, col = left ? 0 : right ? 2 : 1;
    static const char *const names[6] = { "QP", "MP", "FP", "QK", "MK", "FK" };
    int len = snprintf(buf, n, "%d", row * 3 + col + 1);
    for (int b = 0; b < 6; b++)
        if (h >> b & 1) len += snprintf(buf + len, n - (size_t)len, " %s", names[b]);
}

static void draw_inputs(uint32_t *out)
{
    if (!cfg.input_display) return;
    for (int p = 0; p < 2; p++) {
        int y = 44;
        osd_box(out, p ? 320 - 62 : 0, y - 2, 62, HIST * 9 + 3, 0x90000000u);
        for (int i = 0; i < HIST && inhist[p][i].frames; i++, y += 9) {
            char a[32], line[48];
            describe(inhist[p][i].held, a, sizeof a);
            snprintf(line, sizeof line, "%2d %s", inhist[p][i].frames, a);
            uint32_t c = i == 0 ? 0xffffff : 0xa0a0a0;
            int x = p ? 320 - 4 - (int)strlen(line) * 6 : 3;
            osd_text(out, x, y, line, c);
        }
    }
}

static void set_ports(const uint32_t held[2])
{
    const uint8_t *ks = SDL_GetKeyboardState(NULL);
    hw.p1 = ~held[0];
    hw.p2 = ~held[1];
    if (cfg.hotkeys[HK_TEST] && ks[cfg.hotkeys[HK_TEST]]) hw.p1 &= ~0x1000u;
    if (cfg.hotkeys[HK_SERVICE] && ks[cfg.hotkeys[HK_SERVICE]]) hw.p2 &= ~0x2000u;
    hw.volume = 0xffffffffu;
    if (cfg.hotkeys[HK_GAME_VOL_UP] && ks[cfg.hotkeys[HK_GAME_VOL_UP]]) hw.volume &= ~0x4u;
    if (cfg.hotkeys[HK_GAME_VOL_DOWN] && ks[cfg.hotkeys[HK_GAME_VOL_DOWN]]) hw.volume &= ~0x8u;
}

static bool is_hotkey(SDL_Scancode sc, int hk) { return cfg.hotkeys[hk] && sc == cfg.hotkeys[hk]; }

static bool key_held(int hk)
{
    const uint8_t *ks = SDL_GetKeyboardState(NULL);
    return cfg.hotkeys[hk] && ks[cfg.hotkeys[hk]];
}

static bool pad_held(int b)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && pad_pressed(pads[i], b)) return true;
    return false;
}

static void set_volume(int v)
{
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    muted = false;
    osd_message("Volume %d%%", volume);
}

static void stop_audio(void) { if (adev) SDL_ClearQueuedAudio(adev); }

/* ---------------------------------------------------------------- */
/* Save states                                                      */
/* ---------------------------------------------------------------- */

static int slot(void) { return cfg.state_slot >= 1 && cfg.state_slot <= 9 ? cfg.state_slot : 1; }

static void slot_path(char *buf, size_t n, int s)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%sstates", data_dir());
    make_dir(dir);
    snprintf(buf, n, "%s/slot%d.%ss", dir, s, game.id);
}

static void save_state(void)
{
    char path[4200];
    slot_path(path, sizeof path, slot());
    if (state_save_file(path)) osd_message("Saved state %d", slot());
    else osd_message("Could not save state %d", slot());
}

static void load_state(void)
{
    char path[4200];
    slot_path(path, sizeof path, slot());
    FILE *f = fopen(path, "rb");
    if (!f) { osd_message("State %d is empty", slot()); return; }
    fclose(f);
    if (!state_load_file(path)) { osd_message("State %d is from another version", slot()); return; }
    hw.instant_disk = cfg.disk_speed == 1;      // a preference, not part of the moment saved
    ac.active = false;
    if (frame_count < (uint64_t)cfg.boot_skip_frames) frame_count = (uint64_t)cfg.boot_skip_frames;
    stop_audio();
    osd_message("Loaded state %d", slot());
}

static void apply_rewind_setting(void)
{
    state_set_history(cfg.rewind ? (size_t)(cfg.rewind_mb > 8 ? cfg.rewind_mb : 8) << 20 : 0);
}

/* ---------------------------------------------------------------- */
/* Menu                                                             */
/* ---------------------------------------------------------------- */

enum { PG_MAIN, PG_GRAPHICS, PG_PARAMS, PG_CONTROLS, PG_BIND, PG_GAME, PG_PRACTICE, PG_NETPLAY };

// Main page
static void apply_rewind_setting(void);
static void prepare_game_code(void);
static int boot_difficulty;          // as read when the machine last powered on

// Power-cycle the cabinet, as "Reset game" does.
static void hard_reset(void)
{
    hw.dsw = config_dsw();
    hw.instant_disk = cfg.disk_speed == 1;
    machine_reset();
    boot_difficulty = adjust_difficulty();
    apply_rewind_setting();
    ac.active = false;
    frame_count = 0;
    stop_audio();
}

static int version_pick = -1;          // version chosen in the menu, not switched to yet

static int versions_there(void)
{
    int n = 0;
    for (int v = 0; v < 4 && game.versions[v].label; v++) n += machine_version_available(v);
    return n;
}

enum { M_RESUME, M_SAVE, M_LOAD, M_SLOT, M_GRAPHICS, M_CONTROLS, M_GAME, M_PRACTICE, M_NETPLAY, M_RESET, M_CHANGE, M_QUIT, M_COUNT };
// Graphics
enum { G_FULLSCREEN, G_MODE, G_WINDOW, G_SCALE, G_SHADER, G_PARAMS, G_PACING, G_PERF, G_BACK, G_COUNT };
// Controls
enum { C_P1, C_P2, C_PAD, C_HOTKEYS, C_PADHOT, C_SOCD, C_RUMBLE, C_DEADZONE, C_P1LABELS, C_P2LABELS, C_BACK, C_COUNT };
// Game & system
enum { S_VOLUME, S_MUTE_BG, S_PAUSE_BG, S_VERSION, S_DIFFICULTY, S_FREEPLAY, S_BLOOD, S_BLOODCOL, S_FINISH, S_DEMO, S_DISK, S_AUTOCOIN, S_RUNAHEAD, S_REWIND, S_BACK, S_COUNT };
// Practice
enum { T_TRAINING, T_REFILL, T_P2HEALTH, T_POWER, T_INPUTS, T_RECORD, T_PLAY, T_BACK, T_COUNT };
// Netplay
enum { N_HOST, N_JOIN, N_ADDRESS, N_PORT, N_DELAY, N_DISCONNECT, N_BACK, N_COUNT };

static bool menu_open, menu_changed, quit_requested;
static int menu_page, menu_sel;
static int editing = -1;             // N_ADDRESS or N_PORT while typing into it
static char edit_buf[256];
static int bind_set;                 // PG_BIND: 0 P1 keys, 1 P2 keys, 2 pad buttons, 3 hotkeys, 4 pad hotkeys
static int capturing = -1;           // row waiting for a key or button

static bool in_netplay(void) { return netplay_running(); }
static void not_in_netplay(void) { osd_message("Not available during netplay"); }

static const char *const button_labels[NBUTTONS] = {
    "Quick punch", "Medium punch", "Fierce punch", "Quick kick", "Medium kick", "Fierce kick",
    "Up", "Down", "Left", "Right", "Start", "Coin",
};
static const char *const hotkey_labels[NHOTKEYS] = {
    "Menu", "Pause", "Frame advance", "Fast-forward (hold)", "Rewind (hold)", "Save state", "Load state",
    "Next state slot", "Fullscreen", "Screenshot", "Volume up", "Volume down", "Mute", "Test (operator)",
    "Service credit", "Cabinet volume up", "Cabinet volume down", "Input display", "Training mode",
    "Record dummy", "Play dummy", "Performance overlay", "Quit",
};
static const char *const pad_hotkey_labels[4] = { "Menu", "Rewind (hold)", "Fast-forward (hold)", "Quit" };
static int *pad_hotkey(int i) { return i == 0 ? &cfg.pad_menu : i == 1 ? &cfg.pad_rewind : i == 2 ? &cfg.pad_fast : &cfg.pad_quit; }

/* --- shader list --- */

#define MAX_SHADERS 256
static char shader_names[MAX_SHADERS][192];
static int nshaders = -1;

static void scan_shaders(void)
{
    char dir[2048];
    snprintf(dir, sizeof dir, "%sshaders", base_dir());
    nshaders = shader_list(dir, shader_names, MAX_SHADERS);
}

// 0-3 built-in filters, 4+ presets
static int shader_index(void)
{
    if (nshaders < 0) scan_shaders();
    if (!*cfg.shader) return cfg.filter;
    for (int i = 0; i < nshaders; i++) {
        char p[256];
        snprintf(p, sizeof p, "shaders/%s", shader_names[i]);
        if (!strcmp(p, cfg.shader)) return NFILTERS + i;
    }
    return NFILTERS + nshaders;          // set in ki2.ini to something outside shaders/
}

static void shader_label(int idx, char *buf, size_t n)
{
    static const char *const builtin[NFILTERS] = { "Sharp", "Smooth", "Scanlines", "CRT" };
    if (idx < NFILTERS) { snprintf(buf, n, "%s", builtin[idx]); return; }
    if (idx - NFILTERS >= nshaders) { snprintf(buf, n, "(custom)"); return; }
    const char *name = shader_names[idx - NFILTERS], *slash = strrchr(name, '/');
    snprintf(buf, n, "%s", slash ? slash + 1 : name);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = 0;
}

static void select_shader(int idx)
{
    if (!video_shaders_supported() && idx >= NFILTERS) { osd_message("Shaders need OpenGL"); return; }
    if (idx < NFILTERS) { cfg.filter = idx; cfg.shader[0] = 0; }
    else snprintf(cfg.shader, sizeof cfg.shader, "shaders/%s", shader_names[idx - NFILTERS]);
    cfg.shader_params[0] = 0;
    if (!apply_shader() && idx >= NFILTERS) cfg.shader[0] = 0;
}

/* --- display modes --- */

static int display_modes(SDL_DisplayMode *out, int max)
{
    int d = SDL_GetWindowDisplayIndex(win), n = 0;
    for (int i = 0; i < SDL_GetNumDisplayModes(d) && n < max; i++) {
        SDL_DisplayMode m;
        if (SDL_GetDisplayMode(d, i, &m) || m.w < 640) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (out[k].w == m.w && out[k].h == m.h && out[k].refresh_rate == m.refresh_rate) dup = true;
        if (!dup) out[n++] = m;
    }
    return n;
}

/* --- drawing --- */

// Rows that don't apply to this game.
static bool row_hidden(int row)
{
    if (menu_page == PG_MAIN) return row == M_CHANGE && (!machine_both_installed() || launch_active());   // a launcher picks the game
    if (menu_page == PG_PRACTICE) return row == T_POWER && !game.power_p1;
    if (menu_page != PG_GAME) return false;
    if (row == S_AUTOCOIN) return !game.credits;
    if (row == S_BLOODCOL) return !game.has_blood_colour;
    if (row == S_DIFFICULTY) return !game.adj_len;
    return false;
}

static int page_count(void)
{
    switch (menu_page) {
    case PG_GRAPHICS: return G_COUNT;
    case PG_PARAMS: return shader_param_count() + 2;
    case PG_CONTROLS: return C_COUNT;
    case PG_BIND: return (bind_set == 3 ? NHOTKEYS : bind_set == 4 ? 4 : NBUTTONS) + 1;
    case PG_GAME: return S_COUNT;
    case PG_PRACTICE: return T_COUNT;
    case PG_NETPLAY: return N_COUNT;
    default: return M_COUNT;
    }
}

static void draw_menu(uint32_t *out)
{
    if (!menu_open) return;
    static char b[96][72];
    const char *items[96];
    int n = page_count();
    if (n > 96) n = 96;
    char footer[96];
    pad_prompt_footer(&pad_prompt, prompt_override(), "SELECT", "BACK", footer, sizeof footer);
    const char *title = game.product, *foot = footer;   // the font draws capitals
    static const char *const onoff[2] = { "Off", "On" };
    char lbl[80];

    switch (menu_page) {
    case PG_MAIN:
        snprintf(b[M_RESUME], 72, "Resume");
        snprintf(b[M_SAVE], 72, "Save state\tslot %d", slot());
        snprintf(b[M_LOAD], 72, "Load state\tslot %d", slot());
        snprintf(b[M_SLOT], 72, "State slot\t< %d >", slot());
        snprintf(b[M_GRAPHICS], 72, "Graphics...");
        snprintf(b[M_CONTROLS], 72, "Controls...");
        snprintf(b[M_GAME], 72, "Game & system...");
        snprintf(b[M_PRACTICE], 72, "Practice...\t%s", cfg.training ? "training on" : "");
        snprintf(b[M_NETPLAY], 72, "Netplay...\t%s", in_netplay() ? "playing" : netplay_active() ? "connecting" : "");
        snprintf(b[M_RESET], 72, "Reset game");
        snprintf(b[M_CHANGE], 72, "Change game...");
        snprintf(b[M_QUIT], 72, "Quit");
        pad_prompt_footer(&pad_prompt, prompt_override(), "SELECT", "CLOSE", footer, sizeof footer);
        break;
    case PG_GRAPHICS: {
        static const char *const scale_names[] = { "Fit 4:3", "Integer", "Stretch" };
        static const char *const pacing_names[] = { "Auto", "Audio", "Vsync" };
        title = "GRAPHICS";
        static const char *const display_names[] = { "Off", "Borderless", "Exclusive" };
        snprintf(b[G_FULLSCREEN], 72, "Fullscreen\t< %s >", display_names[shown_display()]);
        snprintf(b[G_MODE], 72, "Exclusive mode\t< %s >", *cfg.fullscreen_mode ? cfg.fullscreen_mode : "Desktop");
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        snprintf(b[G_WINDOW], 72, "Window size\t< %dx%d >", ww, wh);
        snprintf(b[G_SCALE], 72, "Scaling\t< %s >", scale_names[cfg.scale_mode]);
        shader_label(shader_index(), lbl, sizeof lbl);
        snprintf(b[G_SHADER], 72, "Shader\t< %s >", lbl);
        snprintf(b[G_PARAMS], 72, shader_param_count() ? "Shader parameters...\t%d" : "Shader parameters\t(none)", shader_param_count());
        snprintf(b[G_PACING], 72, "Frame pacing\t< %s%s >", pacing_names[cfg.pacing],
                 cfg.pacing == PACING_AUDIO ? "" : vsync_broken ? ": not working" :
                 cfg.pacing == PACING_AUTO ? (vsync ? ": vsync" : ": audio") : "");
        snprintf(b[G_PERF], 72, "Performance overlay\t%s", onoff[cfg.perf_overlay]);
        snprintf(b[G_BACK], 72, "Back");
        pad_prompt_adjust_footer(&pad_prompt, prompt_override(), footer, sizeof footer);
        break;
    }
    case PG_PARAMS:
        title = "SHADER PARAMETERS";
        for (int i = 0; i < n - 2; i++) {
            shader_param_t *p = shader_param(i);
            snprintf(b[i], 72, "%.30s\t%s%.4g", p->desc, p->value != p->def ? "*" : "", p->value);
        }
        snprintf(b[n - 2], 72, "Reset all to defaults");
        snprintf(b[n - 1], 72, "Back");
        foot = "LEFT/RIGHT ADJUST   ENTER RESETS ONE";
        break;
    case PG_CONTROLS:
        title = "CONTROLS";
        snprintf(b[C_P1], 72, "Player 1 keys...");
        snprintf(b[C_P2], 72, "Player 2 keys...");
        snprintf(b[C_PAD], 72, "Gamepad buttons...");
        snprintf(b[C_HOTKEYS], 72, "Hotkeys...");
        snprintf(b[C_PADHOT], 72, "Gamepad hotkeys...");
        snprintf(b[C_SOCD], 72, "Left+right = neutral\t%s", onoff[cfg.socd]);
        snprintf(b[C_RUMBLE], 72, "Rumble when hit\t%s", onoff[cfg.rumble]);
        snprintf(b[C_DEADZONE], 72, "Stick deadzone\t< %d%% >", cfg.pad_deadzone * 100 / 32767);
        for (int p = 0; p < 2; p++)
            snprintf(b[p ? C_P2LABELS : C_P1LABELS], 72, "P%d button labels\t< %s%s%s >", p + 1,
                     pad_family_name(cfg.pad_family[p]), cfg.pad_family[p] == PAD_FAMILY_AUTO ? ": " : "",
                     cfg.pad_family[p] == PAD_FAMILY_AUTO ? pad_family_name(pad_label_family(pads[p], PAD_FAMILY_AUTO)) : "");
        snprintf(b[C_BACK], 72, "Back");
        break;
    case PG_BIND: {
        static const char *const titles[] = { "PLAYER 1 KEYS", "PLAYER 2 KEYS", "GAMEPAD BUTTONS", "HOTKEYS", "GAMEPAD HOTKEYS" };
        title = titles[bind_set];
        for (int i = 0; i < n - 1; i++) {
            const char *v;
            if (capturing == i) v = bind_set == 2 || bind_set == 4 ? "press a button" : "press a key";
            else if (bind_set <= 1) v = cfg.keys[bind_set][i] ? SDL_GetScancodeName(cfg.keys[bind_set][i]) : "-";
            else if (bind_set == 2) v = pad_label(cfg.pad[i]);
            else if (bind_set == 3) v = cfg.hotkeys[i] ? SDL_GetScancodeName(cfg.hotkeys[i]) : "-";
            else v = pad_label(*pad_hotkey(i));
            const char *name = bind_set <= 2 ? button_labels[i] : bind_set == 3 ? hotkey_labels[i] : pad_hotkey_labels[i];
            snprintf(b[i], 72, "%s\t%s", name, v);
        }
        snprintf(b[n - 1], 72, "Back");
        if (capturing >= 0) foot = "ESC CANCELS   BACKSPACE CLEARS";
        else pad_prompt_footer(&pad_prompt, prompt_override(), "CHANGE", "BACK", footer, sizeof footer);
        break;
    }
    case PG_GAME: {
        static const char *const blood_names[] = { "None", "Low", "Medium", "High" };
        static const char *const ra_names[] = { "Off", "1 frame", "2 frames" };
        bool pending = config_dsw() != hw.dsw && !in_netplay();
        title = "GAME & SYSTEM";
        if (muted) snprintf(b[S_VOLUME], 72, "Volume\t< Muted >");
        else snprintf(b[S_VOLUME], 72, "Volume\t< %d%% >", volume);
        snprintf(b[S_MUTE_BG], 72, "Mute in background\t%s", onoff[cfg.mute_unfocused]);
        snprintf(b[S_PAUSE_BG], 72, "Pause in background\t%s", onoff[cfg.pause_unfocused]);
        {
            int cur = machine_version();
            if (cur < 0) snprintf(b[S_VERSION], 72, "Game version\t(ROM set in %s.ini)", game.id);
            else if (version_pick >= 0 && version_pick != cur) {
                snprintf(b[S_VERSION], 72, "Game version\t< %s >  Enter: switch", game.versions[version_pick].label);
                foot = "SWITCHING RESETS THE GAME";
            } else if (versions_there() > 1) snprintf(b[S_VERSION], 72, "Game version\t< %s >", game.versions[cur].label);
            else snprintf(b[S_VERSION], 72, "Game version\t%s", game.versions[cur].label);
        }
        static const char *const diff_names[] = { "?", "Extra easy", "Easy", "Medium", "Hard", "Extra hard" };
        int dl = adjust_difficulty();
        snprintf(b[S_DIFFICULTY], 72, "CPU difficulty\t< %s >%s", dl < 0 ? "(unknown)" : diff_names[dl],
                 dl > 0 && dl != boot_difficulty ? " (on reset)" : "");
        if (dl > 0 && dl != boot_difficulty) foot = "CHANGES APPLY ON RESET GAME";
        snprintf(b[S_FREEPLAY], 72, "Free play\t%s%s", onoff[cfg.free_play], pending ? " (on reset)" : "");
        snprintf(b[S_BLOOD], 72, "Blood\t< %s >%s", blood_names[cfg.blood & 3], pending ? "*" : "");
        snprintf(b[S_BLOODCOL], 72, "Blood colour\t< %s >%s", cfg.blood_colour ? "White" : "Red", pending ? "*" : "");
        snprintf(b[S_FINISH], 72, "Finishing moves\t%s", onoff[cfg.finishing_moves]);
        snprintf(b[S_DEMO], 72, "Attract-mode sound\t%s", onoff[cfg.demo_sounds]);
        snprintf(b[S_DISK], 72, "Disk loading\t< %s >", cfg.disk_speed ? "Instant" : "Original drive");
        snprintf(b[S_AUTOCOIN], 72, "Auto-coin\t%s", cfg.free_play ? "(free play)" : onoff[cfg.auto_coin]);
        int ra = cfg.run_ahead < 0 ? 0 : cfg.run_ahead > 2 ? 2 : cfg.run_ahead;
        snprintf(b[S_RUNAHEAD], 72, "Run-ahead (less lag)\t< %s >", ra_names[ra]);
        snprintf(b[S_REWIND], 72, "Rewind\t%s", onoff[cfg.rewind]);
        snprintf(b[S_BACK], 72, "Back");
        if (pending) foot = "SWITCH CHANGES APPLY ON RESET";
        break;
    }
    case PG_PRACTICE:
        title = "PRACTICE";
        snprintf(b[T_TRAINING], 72, "Training mode\t%s", onoff[cfg.training]);
        snprintf(b[T_REFILL], 72, "Health refill\t%s", onoff[cfg.training_refill]);
        static const char *const p2_names[] = { "Full", "Last bar", "Last bar, low" };
        snprintf(b[T_P2HEALTH], 72, "Player 2 health\t< %s >", game.bars_p1 ? p2_names[cfg.training_p2] : "Full");
        snprintf(b[T_POWER], 72, "%s\t%s", game.power_name ? game.power_name : "Breaker reward", onoff[cfg.training_power]);
        snprintf(b[T_INPUTS], 72, "Input display\t%s", onoff[cfg.input_display]);
        snprintf(b[T_RECORD], 72, "Record dummy (player 2)");
        snprintf(b[T_PLAY], 72, "Play / stop dummy");
        snprintf(b[T_BACK], 72, "Back");
        foot = "START A 2-PLAYER GAME FOR A DUMMY";
        break;
    case PG_NETPLAY:
        title = "NETPLAY";
        snprintf(b[N_HOST], 72, "Host a game\tport %d", cfg.net_port);
        snprintf(b[N_JOIN], 72, "Join a game");
        if (editing == N_ADDRESS) snprintf(b[N_ADDRESS], 72, "Address\t%.40s_", edit_buf);
        else snprintf(b[N_ADDRESS], 72, "Address\t%.40s", *cfg.net_address ? cfg.net_address : "(enter)");
        if (editing == N_PORT) snprintf(b[N_PORT], 72, "Port\t%.10s_", edit_buf);
        else snprintf(b[N_PORT], 72, "Port\t%d", cfg.net_port);
        snprintf(b[N_DELAY], 72, "Input delay\t< %d frames >", cfg.net_delay);
        snprintf(b[N_DISCONNECT], 72, netplay_active() ? "Disconnect" : "(not connected)");
        snprintf(b[N_BACK], 72, "Back");
        foot = editing >= 0 ? "TYPE, THEN ENTER   ESC CANCELS" : netplay_active() ? netplay_status() : foot;
        break;
    }
    int shown = 0, sel = 0;
    for (int i = 0; i < n; i++) {
        if (row_hidden(i)) continue;
        if (i == menu_sel) sel = shown;
        items[shown++] = b[i];
    }
    osd_menu(out, title, items, shown, sel, foot);
}

/* --- actions --- */

static void menu_close(void)
{
    menu_open = false;
    menu_page = PG_MAIN;
    capturing = -1;
    if (editing >= 0) { editing = -1; SDL_StopTextInput(); }
    settle_display(true);
    if (menu_changed) {
        cfg.volume = volume;
        cfg.fullscreen = fullscreen;
        if (shader_active()) shader_params_save(cfg.shader_params, sizeof cfg.shader_params);
        if (config_save()) osd_message("Settings saved");
        menu_changed = false;
    }
}

static void cycle(int *v, int n, int dir) { *v = ((*v + dir) % n + n) % n; }

static void go(int page, int sel) { menu_page = page; menu_sel = sel; capturing = -1; }

static void start_join(void)
{
    if (!*cfg.net_address) { osd_message("Enter the host's address first"); return; }
    char h[1024];
    int port = cfg.net_port;
    snprintf(h, sizeof h, "%s", cfg.net_address);
    char *c = strrchr(h, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    if (netplay_join(h, port, cfg.net_delay)) osd_message("Connecting to %s...", cfg.net_address);
    else osd_message("%s", netplay_take_error());
}

// Left/right on the selected item (dir = -1/+1), or Enter (dir = 0).
static void menu_change(int dir)
{
    int d = dir ? dir : 1;
    switch (menu_page) {
    case PG_MAIN:
        switch (menu_sel) {
        case M_RESUME: if (!dir) menu_close(); return;
        case M_SAVE: if (!dir) { save_state(); menu_close(); } return;
        case M_LOAD:
            if (dir) return;
            if (in_netplay()) not_in_netplay(); else load_state();
            menu_close();
            return;
        case M_SLOT: { int s = slot() - 1; cycle(&s, 9, d); cfg.state_slot = s + 1; break; }
        case M_GRAPHICS: if (!dir) go(PG_GRAPHICS, 0); return;
        case M_CONTROLS: if (!dir) go(PG_CONTROLS, 0); return;
        case M_GAME: if (!dir) go(PG_GAME, 0); return;
        case M_PRACTICE: if (!dir) go(PG_PRACTICE, 0); return;
        case M_NETPLAY: if (!dir) go(PG_NETPLAY, netplay_active() ? N_DISCONNECT : N_HOST); return;
        case M_RESET:
            if (dir) return;
            if (in_netplay()) { not_in_netplay(); return; }
            hard_reset();
            menu_close();
            osd_message("Reset");
            return;
        case M_CHANGE:
            if (dir) return;
            if (in_netplay()) { not_in_netplay(); return; }
            menu_close();
            machine_restart_to_picker();
            return;
        case M_QUIT: if (!dir) quit_requested = true; return;
        }
        break;

    case PG_GRAPHICS:
        switch (menu_sel) {
        case G_FULLSCREEN: {
            int m = shown_display();
            cycle(&m, 3, d);
            cfg.fullscreen_exclusive = m == 2;
            request_display(m);
            break;
        }
        case G_MODE: {
            SDL_DisplayMode modes[64];
            int n = display_modes(modes, 64), cur = -1;
            for (int i = 0; i < n; i++) {
                char s[32];
                snprintf(s, sizeof s, "%dx%d@%d", modes[i].w, modes[i].h, modes[i].refresh_rate);
                if (!strcmp(s, cfg.fullscreen_mode)) cur = i;
            }
            cur += d;                                   // -1 means "desktop"
            if (cur < -1) cur = n - 1;
            if (cur >= n) cur = -1;
            if (cur < 0) cfg.fullscreen_mode[0] = 0;
            else snprintf(cfg.fullscreen_mode, sizeof cfg.fullscreen_mode, "%dx%d@%d", modes[cur].w, modes[cur].h, modes[cur].refresh_rate);
            if (shown_display() == 2) request_display(2);
            break;
        }
        case G_WINDOW: {
            if (shown_display()) { osd_message("Leave fullscreen to resize the window"); return; }
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            int k = (wh + 120) / 240 + d;
            if (k < 1) k = 8;
            if (k > 8) k = 1;
            SDL_SetWindowSize(win, 320 * k, 240 * k);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(win)),
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(win)));
            cfg.window_scale = k;
            break;
        }
        case G_SCALE: cycle(&cfg.scale_mode, 3, d); break;
        case G_SHADER: {
            if (nshaders < 0 || !dir) scan_shaders();
            int idx = shader_index(), total = NFILTERS + nshaders;
            idx = idx >= total ? 0 : ((idx + d) % total + total) % total;
            select_shader(idx);
            break;
        }
        case G_PARAMS: if (!dir && shader_param_count()) go(PG_PARAMS, 0); return;
        case G_PACING: cycle(&cfg.pacing, 3, d); vsync_broken = false; choose_pacing(); break;   // a choice: try vsync again
        case G_PERF: cfg.perf_overlay = !cfg.perf_overlay; break;
        case G_BACK: if (!dir) go(PG_MAIN, M_GRAPHICS); return;
        }
        break;

    case PG_PARAMS: {
        int n = shader_param_count();
        if (menu_sel == n + 1) { if (!dir) go(PG_GRAPHICS, G_PARAMS); return; }
        if (menu_sel == n) {
            if (dir) return;
            for (int i = 0; i < n; i++) shader_param(i)->value = shader_param(i)->def;
            break;
        }
        shader_param_t *p = shader_param(menu_sel);
        if (!dir) p->value = p->def;
        else {
            p->value += p->step * dir;
            if (p->value < p->min) p->value = p->min;
            if (p->value > p->max) p->value = p->max;
            if (p->value > -1e-6f && p->value < 1e-6f) p->value = 0;
        }
        break;
    }

    case PG_CONTROLS:
        switch (menu_sel) {
        case C_P1: case C_P2: case C_PAD: case C_HOTKEYS: case C_PADHOT:
            if (!dir) { bind_set = menu_sel - C_P1; go(PG_BIND, 0); }
            return;
        case C_SOCD: cfg.socd = !cfg.socd; break;
        case C_RUMBLE: cfg.rumble = !cfg.rumble; break;
        case C_DEADZONE: {
            int pct = cfg.pad_deadzone * 100 / 32767 + 5 * d;
            pct = pct < 5 ? 5 : pct > 90 ? 90 : pct;
            cfg.pad_deadzone = pct * 32767 / 100;
            break;
        }
        case C_P1LABELS: case C_P2LABELS:
            cycle(&cfg.pad_family[menu_sel == C_P1LABELS ? 0 : 1], PAD_FAMILY_COUNT, d);
            break;
        case C_BACK: if (!dir) go(PG_MAIN, M_CONTROLS); return;
        }
        break;

    case PG_BIND:
        if (menu_sel == page_count() - 1) { if (!dir) go(PG_CONTROLS, C_P1 + bind_set); return; }
        if (!dir) capturing = menu_sel;
        return;

    case PG_GAME:
        switch (menu_sel) {
        case S_VOLUME:
            if (!dir) muted = !muted;
            else { volume += 10 * dir; volume = volume < 0 ? 0 : volume > 100 ? 100 : volume; muted = false; }
            break;
        case S_MUTE_BG: cfg.mute_unfocused = !cfg.mute_unfocused; break;
        case S_PAUSE_BG: cfg.pause_unfocused = !cfg.pause_unfocused; break;
        case S_VERSION: {
            int cur = machine_version();
            if (cur < 0 || versions_there() < 2) return;
            if (in_netplay()) { not_in_netplay(); return; }
            if (version_pick < 0) version_pick = cur;
            if (dir) {                              // browse the versions there are
                int n = 0;
                while (n < 4 && game.versions[n].label) n++;
                do version_pick = (version_pick + dir + n) % n; while (!machine_version_available(version_pick));
                break;
            }
            if (version_pick == cur) return;
            char err[256];
            if (!machine_switch_version(version_pick, err, sizeof err)) { osd_message("%s", err); return; }
            snprintf(cfg.version, sizeof cfg.version, "%s", game.versions[version_pick].label);
            version_pick = -1;
            rec_unload();
            hard_reset();
            prepare_game_code();
            menu_close();
            osd_message("Version %s", cfg.version);
            return;
        }
        case S_DIFFICULTY: {
            if (in_netplay()) { not_in_netplay(); return; }
            int dl = adjust_difficulty();
            if (dl < 0) { osd_message("The disk's settings block is not readable"); return; }
            dl += d;
            dl = dl < 1 ? 5 : dl > 5 ? 1 : dl;
            adjust_set_difficulty(dl);                         // stored on the disk, like the operator menu
            break;
        }
        case S_FREEPLAY: cfg.free_play = !cfg.free_play; break;     // the game reads its switches at power-on
        case S_BLOOD: cycle(&cfg.blood, 4, d); break;
        case S_BLOODCOL: cfg.blood_colour = !cfg.blood_colour; break;
        case S_FINISH: cfg.finishing_moves = !cfg.finishing_moves; break;
        case S_DEMO: cfg.demo_sounds = !cfg.demo_sounds; break;
        case S_DISK:
            if (in_netplay()) { not_in_netplay(); return; }
            cfg.disk_speed = !cfg.disk_speed;
            hw.instant_disk = cfg.disk_speed == 1;          // the drive needs no power-cycle
            break;
        case S_AUTOCOIN: cfg.auto_coin = !cfg.auto_coin; break;
        case S_RUNAHEAD: cycle(&cfg.run_ahead, 3, d); break;
        case S_REWIND: cfg.rewind = !cfg.rewind; if (!in_netplay()) apply_rewind_setting(); break;
        case S_BACK: if (!dir) go(PG_MAIN, M_GAME); return;
        }
        break;

    case PG_PRACTICE:
        switch (menu_sel) {
        case T_TRAINING: if (in_netplay()) { not_in_netplay(); return; } cfg.training = !cfg.training; break;
        case T_REFILL: cfg.training_refill = !cfg.training_refill; break;
        case T_P2HEALTH: if (game.bars_p1) cycle(&cfg.training_p2, 3, d); break;
        case T_POWER: cfg.training_power = !cfg.training_power; break;
        case T_INPUTS: cfg.input_display = !cfg.input_display; break;
        case T_RECORD: if (!dir) { if (in_netplay()) not_in_netplay(); else { training_record_toggle(); menu_close(); } } return;
        case T_PLAY: if (!dir) { if (in_netplay()) not_in_netplay(); else { training_play_toggle(); menu_close(); } } return;
        case T_BACK: if (!dir) go(PG_MAIN, M_PRACTICE); return;
        }
        break;

    case PG_NETPLAY:
        switch (menu_sel) {
        case N_HOST:
            if (dir) return;
            if (netplay_host(cfg.net_port, cfg.net_delay)) osd_message("Hosting on port %d: waiting for a player", cfg.net_port);
            else osd_message("%s", netplay_take_error());
            menu_close();
            return;
        case N_JOIN: if (!dir) { start_join(); menu_close(); } return;
        case N_ADDRESS: case N_PORT:
            if (dir) return;
            editing = menu_sel;
            if (menu_sel == N_ADDRESS) snprintf(edit_buf, sizeof edit_buf, "%s", cfg.net_address);
            else snprintf(edit_buf, sizeof edit_buf, "%d", cfg.net_port);
            SDL_StartTextInput();
            return;
        case N_DELAY: { int v = cfg.net_delay + d; cfg.net_delay = v < 0 ? 0 : v > 4 ? 4 : v; break; }
        case N_DISCONNECT: if (!dir && netplay_active()) { netplay_stop(); osd_message("Netplay ended"); } return;
        case N_BACK: if (!dir) go(PG_MAIN, M_NETPLAY); return;
        }
        break;
    }
    menu_changed = true;
}

static void menu_toggle(void)
{
    if (menu_open) menu_close();
    else { menu_open = true; go(PG_MAIN, 0); if (!in_netplay()) stop_audio(); }
}

static void menu_back(void)
{
    switch (menu_page) {
    case PG_MAIN: menu_close(); break;
    case PG_PARAMS: go(PG_GRAPHICS, G_PARAMS); break;
    case PG_BIND: go(PG_CONTROLS, C_P1 + bind_set); break;
    case PG_GRAPHICS: go(PG_MAIN, M_GRAPHICS); break;
    case PG_CONTROLS: go(PG_MAIN, M_CONTROLS); break;
    case PG_GAME: go(PG_MAIN, M_GAME); break;
    case PG_PRACTICE: go(PG_MAIN, M_PRACTICE); break;
    case PG_NETPLAY: go(PG_MAIN, M_NETPLAY); break;
    }
}

static void menu_nav(int up_down, int left_right, bool enter, bool back)
{
    if (up_down) {
        int n = page_count();
        for (int k = 0; k < n; k++) {               // skip rows this game doesn't have
            cycle(&menu_sel, n, up_down);
            if (!row_hidden(menu_sel)) break;
        }
    }
    if (left_right) menu_change(left_right);
    if (enter) menu_change(0);
    if (back) menu_back();
}

// Typing into the address or port field.
static void edit_key(SDL_Scancode sc, const char *text)
{
    if (text) {
        for (const char *c = text; *c; c++)
            if (strlen(edit_buf) < 60 && (SDL_isalnum((unsigned char)*c) || strchr(".:-_", *c)))
                strncat(edit_buf, c, 1);
        return;
    }
    size_t n = strlen(edit_buf);
    if (sc == SDL_SCANCODE_BACKSPACE && n) edit_buf[n - 1] = 0;
    else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
        if (editing == N_ADDRESS) snprintf(cfg.net_address, sizeof cfg.net_address, "%s", edit_buf);
        else if (atoi(edit_buf) > 0 && atoi(edit_buf) < 65536) cfg.net_port = atoi(edit_buf);
        editing = -1;
        menu_changed = true;
        SDL_StopTextInput();
    } else if (sc == SDL_SCANCODE_ESCAPE) { editing = -1; SDL_StopTextInput(); }
}

// A key or pad button for the binding being captured. key = 0: pad input.
static void capture(SDL_Scancode key, int pad_button, bool clear)
{
    int row = capturing;
    bool pad_set = bind_set == 2 || bind_set == 4;
    if (!clear && (pad_set ? pad_button == PAD_NONE : !key)) return;    // wrong kind of input: keep waiting
    capturing = -1;
    switch (bind_set) {
    case 0: case 1: cfg.keys[bind_set][row] = clear ? SDL_SCANCODE_UNKNOWN : key; break;
    case 2: cfg.pad[row] = clear ? PAD_NONE : pad_button; break;
    case 3: cfg.hotkeys[row] = clear ? SDL_SCANCODE_UNKNOWN : key; break;
    case 4: *pad_hotkey(row) = clear ? PAD_NONE : pad_button; break;
    }
    menu_changed = true;
}

/* ---------------------------------------------------------------- */
/* Events                                                           */
/* ---------------------------------------------------------------- */

// Holding Back+Start quits (a tap opens the menu, as before): the pad and since when.
#define QUIT_HOLD_MS 2000
static SDL_JoystickID quit_pad = -1;
static uint32_t quit_since;

static void check_quit_hold(void)
{
    SDL_GameController *c = quit_pad >= 0 ? SDL_GameControllerFromInstanceID(quit_pad) : NULL;
    if (!c || !SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) || !SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) {
        quit_pad = -1;
        return;
    }
    uint32_t held = SDL_GetTicks() - quit_since;
    if (held >= QUIT_HOLD_MS) quit_requested = true;
    else if (held >= 600) osd_message("Keep holding to quit... %u", (QUIT_HOLD_MS - held + 999) / 1000);
}

/* returns false to quit */
static bool handle_events(void)
{
    settle_display(false);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        pad_prompt_event(&pad_prompt, &ev);
        switch (ev.type) {
        case SDL_QUIT: return false;
        case SDL_CONTROLLERDEVICEADDED: pad_added(ev.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: pad_removed(ev.cdevice.which); break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED || ev.window.event == SDL_WINDOWEVENT_MOVED) choose_pacing();
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { focused = false; if (cfg.mute_unfocused || cfg.pause_unfocused) stop_audio(); }
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (menu_open && capturing >= 0 && ev.caxis.value > 20000) {
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) capture(0, PAD_LT, false);
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) capture(0, PAD_RT, false);
            } else if (ev.caxis.value > 20000 &&
                       ((ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && cfg.pad_quit == PAD_LT) ||
                        (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && cfg.pad_quit == PAD_RT)))
                quit_requested = true;
            break;
        case SDL_CONTROLLERBUTTONDOWN: {
            int b = ev.cbutton.button;
            if (menu_open && capturing >= 0) { capture(0, b, false); break; }
            SDL_GameController *c = SDL_GameControllerFromInstanceID(ev.cbutton.which);
            bool combo = c && ((b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) ||
                               (b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)));
            if (cfg.pad_quit != PAD_NONE && b == cfg.pad_quit) { quit_requested = true; break; }
            if (combo && quit_pad == ev.cbutton.which) break;     // the pair's other button: one press, one toggle
            if (combo) { quit_pad = ev.cbutton.which; quit_since = SDL_GetTicks(); }
            if ((cfg.pad_menu != PAD_NONE && b == cfg.pad_menu) || combo) { menu_toggle(); break; }
            if (!menu_open) break;
            menu_nav(b == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 : 0,
                     b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 1 : 0,
                     b == SDL_CONTROLLER_BUTTON_A, b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START);
            break;
        }
        case SDL_TEXTINPUT:
            if (editing >= 0) edit_key(SDL_SCANCODE_UNKNOWN, ev.text.text);
            break;
        case SDL_KEYDOWN: {
            SDL_Scancode sc = ev.key.keysym.scancode;
            if (editing >= 0) { edit_key(sc, NULL); break; }
            if (menu_open && capturing >= 0) {
                if (ev.key.repeat) break;
                if (sc == SDL_SCANCODE_ESCAPE) capturing = -1;
                else if (sc == SDL_SCANCODE_BACKSPACE) capture(0, PAD_NONE, true);
                else capture(sc, PAD_NONE, false);
                break;
            }
            bool alt_enter = sc == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT);
            if (is_hotkey(sc, HK_QUIT)) { quit_requested = true; break; }
            if (sc == SDL_SCANCODE_ESCAPE && menu_open) { if (!ev.key.repeat) menu_back(); break; }
            // started by a launcher: Esc quits, as in MAME and RetroArch (F1 opens the menu)
            if (sc == SDL_SCANCODE_ESCAPE && launch_active() && !is_hotkey(sc, HK_MENU)) { quit_requested = true; break; }
            if (sc == SDL_SCANCODE_ESCAPE || is_hotkey(sc, HK_MENU)) { if (!ev.key.repeat) menu_toggle(); break; }
            if (menu_open && !alt_enter) {
                menu_nav(sc == SDL_SCANCODE_UP ? -1 : sc == SDL_SCANCODE_DOWN ? 1 : 0,
                         sc == SDL_SCANCODE_LEFT ? -1 : sc == SDL_SCANCODE_RIGHT ? 1 : 0,
                         !ev.key.repeat && (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_KP_ENTER),
                         sc == SDL_SCANCODE_BACKSPACE);
                break;
            }
            if (ev.key.repeat && !is_hotkey(sc, HK_VOL_UP) && !is_hotkey(sc, HK_VOL_DOWN) && !is_hotkey(sc, HK_STEP)) break;
            if (alt_enter || is_hotkey(sc, HK_FULLSCREEN)) {
                bool on = shown_display() == 0;
                want_display = -1;
                set_fullscreen(on);
                menu_changed = true;
            }
            else if (in_netplay() && (is_hotkey(sc, HK_PAUSE) || is_hotkey(sc, HK_STEP) || is_hotkey(sc, HK_LOAD_STATE) ||
                                      is_hotkey(sc, HK_TRAINING) || is_hotkey(sc, HK_DUMMY_RECORD) || is_hotkey(sc, HK_DUMMY_PLAY)))
                not_in_netplay();
            else if (is_hotkey(sc, HK_PAUSE)) { paused = !paused; if (paused) stop_audio(); }
            else if (is_hotkey(sc, HK_STEP)) { if (paused) step_once = true; else { paused = true; stop_audio(); } }
            else if (is_hotkey(sc, HK_SAVE_STATE)) save_state();
            else if (is_hotkey(sc, HK_LOAD_STATE)) load_state();
            else if (is_hotkey(sc, HK_NEXT_SLOT)) { cfg.state_slot = slot() % 9 + 1; osd_message("State slot %d", slot()); }
            else if (is_hotkey(sc, HK_SCREENSHOT)) screenshot();
            else if (is_hotkey(sc, HK_TRAINING)) { cfg.training = !cfg.training; osd_message("Training mode %s", cfg.training ? "on" : "off"); }
            else if (is_hotkey(sc, HK_DUMMY_RECORD)) training_record_toggle();
            else if (is_hotkey(sc, HK_DUMMY_PLAY)) training_play_toggle();
            else if (is_hotkey(sc, HK_PERF)) { cfg.perf_overlay = !cfg.perf_overlay; menu_changed = true; }
            else if (is_hotkey(sc, HK_INPUT_DISPLAY)) { cfg.input_display = !cfg.input_display; osd_message("Input display %s", cfg.input_display ? "on" : "off"); }
            else if (is_hotkey(sc, HK_VOL_UP)) set_volume(volume + 10);
            else if (is_hotkey(sc, HK_VOL_DOWN)) set_volume(volume - 10);
            else if (is_hotkey(sc, HK_MUTE)) { muted = !muted; osd_message(muted ? "Muted" : "Volume %d%%", volume); }
            break;
        }
        }
    }
    check_quit_hold();
    return !quit_requested;
}

// One real frame of the game: inputs, emulation, and a checkpoint for
// rollback/rewind. Every emulated frame must end in a checkpoint.
static void emulate_frame(void)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    uint32_t held[2];
    read_players(held);
    training_inputs(held);
    auto_coin(held);
    record_inputs(held);
    set_ports(held);
    training_apply(true);
    run_frame();
    state_checkpoint();
    frame_count++;
    rumble_check();
    perf_emu(t0);
}

// Show the frame `run_ahead` frames in the future (same inputs), then
// return to the real one: the game's own input lag disappears from view.
static void present_ahead(int n)
{
    if (n <= 0) { present(NULL); return; }
    for (int k = 0; k < n; k++) { training_apply(false); run_frame(); pump_audio(true, 1); }
    present(NULL);
    state_rollback();
}

/* ---------------------------------------------------------------- */
/* Netplay glue                                                     */
/* ---------------------------------------------------------------- */

static void np_run(const uint16_t in[2], bool resim)
{
    if (!resim) {
        uint32_t h[2] = { in[0], in[1] };
        record_inputs(h);
        frame_count++;
    }
    machine_run_inputs(in, resim);
    if (!resim) rumble_check();
}

static void np_reset(void)
{
    osd_message("Netplay: syncing...");
    present("NETPLAY");
    stop_audio();
    paused = step_once = false;
    machine_netplay_reset();
    boot_difficulty = adjust_difficulty();
    ac.active = false;
    memset(inhist, 0, sizeof inhist);
}

// Your controls (either keyboard side, either pad) drive your player.
static uint16_t netplay_input(void)
{
    if (menu_open) return 0;
    uint32_t held[2];
    read_players(held);
    int me = netplay_local_player();
    uint32_t h[2] = { 0, 0 };
    h[me] = held[0] | held[1];
    auto_coin(h);
    return (uint16_t)(h[me] & 0x0fff);
}

const char *opt_net_host, *opt_net_join;     // from the command line
bool opt_test_shaders;

static bool apply_shader(void)
{
    char path[2048], err[1024];
    if (!*cfg.shader) { video_set_shader(NULL, err, sizeof err); return true; }
    snprintf(path, sizeof path, "%s%s", base_dir(), cfg.shader);
    if (!video_set_shader(path, err, sizeof err)) {
        fprintf(stderr, "shader: %s\n", err);
        osd_message("Shader failed to load (see %s.log)", game.id);
        return false;
    }
    shader_params_apply(cfg.shader_params);
    return true;
}

// --test-shaders: load and draw every preset under shaders/, report each one.
static int test_shaders(void)
{
    static char names[512][192];
    char dir[2048];
    snprintf(dir, sizeof dir, "%sshaders", base_dir());
    int n = shader_list(dir, names, 512), ok = 0;
    for (int i = 0; i < n; i++) {
        char path[2400], err[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        uint64_t t0 = SDL_GetPerformanceCounter();
        bool good = video_set_shader(path, err, sizeof err);
        if (good) for (int k = 0; k < 3; k++) { to_rgb(px); osd_clear(ov); video_present(px, ov, cfg.filter, cfg.scale_mode); }
        double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        printf("%-4s %-60s %6.1f ms  %d params%s%s\n", good ? "OK" : "FAIL", names[i], ms, good ? shader_param_count() : 0,
               good ? "" : "\n     ", good ? "" : err);
        ok += good;
    }
    printf("%d of %d presets work\n", ok, n);
    return ok == n ? 0 : 1;
}

/* ---------------------------------------------------------------- */
/* First launch: build the game code for this PC                    */
/* ---------------------------------------------------------------- */

static void prepare_progress(const char *stage, double fraction)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) if (ev.type == SDL_CONTROLLERDEVICEADDED) pad_added(ev.cdevice.which);
    memset(px, 0, sizeof px);
    osd_clear(ov);
    osd_box(ov, 30, 70, 260, 100, 0xf0101018u);
    osd_text(ov, 160 - 21 * 3, 80, "PREPARING THE GAME", 0xffc040);
    osd_text(ov, 160 - 25 * 3, 96, "FOR THIS PC (FIRST RUN)", 0xb0b0b0);
    osd_text(ov, 160 - (int)strlen(stage) * 3, 118, stage, 0xffffff);
    osd_box(ov, 50, 136, 220, 10, 0xff303040u);
    osd_box(ov, 51, 137, (int)(218 * (fraction < 0 ? 0 : fraction > 1 ? 1 : fraction)), 8, 0xffc08020u);
    osd_text(ov, 160 - 24 * 3, 154, "THIS TAKES ABOUT A MINUTE", 0x808080);
    video_present(px, ov, FILTER_SHARP, cfg.scale_mode);
}

// Use translated code for full speed: linked in, cached from an earlier run,
// or built now. Without it the game still runs, on the interpreter.
static void prepare_game_code(void)
{
    if (rec_enabled || getenv("KI2_NOREC")) return;
    char msg[512];
    if (rec_build_load_cached(msg, sizeof msg)) { fprintf(stderr, "rec: %s\n", msg); return; }
    uint64_t t0 = SDL_GetTicks64();
    bool ok = rec_build(prepare_progress, msg, sizeof msg);
    fprintf(stderr, "rec: %s (%.0f s)\n", msg, (SDL_GetTicks64() - t0) / 1000.0);
    osd_message("%s", ok ? "Ready" : "Running without translated code (slower)");
    if (!ok) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, game.product,
        "The game code could not be prepared for full speed on this PC, so the game runs on the "
        "slower interpreter. The .log file and cache/*/build.log in the game folder say why.", win);
}

/* ---------------------------------------------------------------- */

int frontend_main(void)
{
    pad_prompt_init(&pad_prompt);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    {   // extra controller mappings (https://github.com/mdqinc/SDL_GameControllerDB), if present
        char buf[4096];
        const char *db = find_data("gamecontrollerdb.txt", buf, sizeof buf);
        FILE *f = fopen(db, "rb");
        if (f) { fclose(f); SDL_GameControllerAddMappingsFromFile(db); }
    }
    // Pads already connected. Their connect events may be gone: the game picker
    // runs first in the same process and takes them.
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) pad_added(i);
    int scale = cfg.window_scale > 0 ? cfg.window_scale : 3;
    win = video_open(game.product, 320 * scale, 240 * scale);
    if (!win) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    set_window_icon(win);
    if (cfg.window_w >= 320 && cfg.window_h >= 240) {
        // where it was last time, if that is still on a screen
        for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
            SDL_Rect r;
            SDL_Point pt = { cfg.window_x + 40, cfg.window_y + 40 };
            if (SDL_GetDisplayBounds(d, &r) == 0 && SDL_PointInRect(&pt, &r)) {
                SDL_SetWindowSize(win, cfg.window_w, cfg.window_h);
                SDL_SetWindowPosition(win, cfg.window_x, cfg.window_y);
                break;
            }
        }
    }
    fprintf(stderr, "video: %s\n", video_backend());
    if (cfg.fullscreen) set_fullscreen(true);
    choose_pacing();
    apply_rewind_setting();

    SDL_AudioSpec want = { .freq = AUDIO_HZ, .format = AUDIO_S16SYS, .channels = 1, .samples = 512 }, have;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) fprintf(stderr, "audio: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(adev, 0);
    volume = cfg.volume < 0 ? 0 : cfg.volume > 100 ? 100 : cfg.volume;
    int latency = cfg.audio_latency_ms > 10 ? cfg.audio_latency_ms : 10;

    if (opt_test_shaders) { int r = test_shaders(); video_close(); SDL_Quit(); return r; }
    prepare_game_code();
    boot_difficulty = adjust_difficulty();
    apply_shader();
    netplay_set_callbacks(np_run, np_reset);
    if (opt_net_host) {
        int port = *opt_net_host ? atoi(opt_net_host) : cfg.net_port;
        if (netplay_host(port, cfg.net_delay)) osd_message("Hosting on port %d: waiting for a player", port);
    } else if (opt_net_join) {
        snprintf(cfg.net_address, sizeof cfg.net_address, "%s", opt_net_join);
        start_join();
    }
    bool was_netplay = false;

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t next = SDL_GetPerformanceCounter();
    while (handle_events()) {
        const char *nerr = netplay_take_error();
        if (nerr) osd_message("%s", nerr);
        if (netplay_running()) {
            // the game never pauses: both sides run at the original 58.96 Hz, timed by audio
            if (!was_netplay) { was_netplay = true; vsync = false; video_set_vsync(false); osd_message("Netplay: you are player %d", netplay_local_player() + 1); }
            uint64_t t0 = SDL_GetPerformanceCounter();
            netplay_update(netplay_input());
            perf_emu(t0);
            pump_audio(false, 1);
            present(netplay_status());
            if (adev) while (queued_ms() > latency) SDL_Delay(1);
            else SDL_Delay(16);
            continue;
        }
        if (was_netplay) { was_netplay = false; choose_pacing(); apply_rewind_setting(); hw.dsw = config_dsw(); hw.instant_disk = cfg.disk_speed == 1; }
        if (netplay_active()) {                        // connecting: keep playing locally meanwhile
            netplay_update(0);
            if (netplay_running()) continue;           // the session just reset the machine: no local frame
        }
        static uint32_t last_flush;
        if (SDL_GetTicks() - last_flush > 5000) { ide_flush(); last_flush = SDL_GetTicks(); }
        bool booting = frame_count < (uint64_t)cfg.boot_skip_frames;
        bool fast = key_held(HK_FAST) || pad_held(cfg.pad_fast) || booting;
        bool rewinding = cfg.rewind && (key_held(HK_REWIND) || pad_held(cfg.pad_rewind));

        if (menu_open || (paused && !step_once) || (!focused && cfg.pause_unfocused)) {
            present(menu_open ? NULL : "PAUSED");
            if (!vsync) SDL_Delay(16);
            next = SDL_GetPerformanceCounter();
            continue;
        }
        if (step_once) {
            step_once = false;
            emulate_frame();
            pump_audio(true, 1);
            present("PAUSED");
            continue;
        }
        if (rewinding && !booting) {
            // one checkpoint back per display frame: rewinds at normal speed
            bool more = state_rewind();
            stop_audio();
            present(more ? "<< REWIND" : "REWIND: NO MORE HISTORY");
            if (!vsync) SDL_Delay((uint32_t)(1000 / FRAME_HZ));
            next = SDL_GetPerformanceCounter();
            continue;
        }

        if (fast) {
            // run unthrottled (or up to the speed limit) for one display frame, silently
            uint64_t start = SDL_GetPerformanceCounter(), budget = freq / 60;
            int limit = cfg.fast_speed > 0 ? cfg.fast_speed : 1000;   // ~frames per display frame
            for (int n = 0; n < limit && SDL_GetPerformanceCounter() - start < budget; n++) {
                emulate_frame();
                pump_audio(true, 1);
            }
            stop_audio();
            present(booting ? "" : ">> FAST");
            if (!vsync) {
                uint64_t now = SDL_GetPerformanceCounter();
                if (now - start < budget) SDL_Delay((uint32_t)((budget - (now - start)) * 1000 / freq));
            }
            next = SDL_GetPerformanceCounter();
            continue;
        }

        int ahead = cfg.run_ahead < 0 ? 0 : cfg.run_ahead > 2 ? 2 : cfg.run_ahead;
        if (vsync) {
            if (!vsync_lock) {
                // vsync on a display that isn't ~60 Hz: tear-free, but a frame
                // is emulated only when the clock says one is due
                uint64_t now = SDL_GetPerformanceCounter();
                if (now < next) { SDL_Delay(1); continue; }
                next += (uint64_t)(freq / FRAME_HZ);
                if (now > next + freq / 10) next = now;        // after a hitch: don't race to catch up
            }
            emulate_frame();
            if (vsync_lock) check_vsync();
            // nudge the resampling so the audio queue stays near its target
            // instead of slowly over- or underflowing
            double q = queued_ms();
            if (adev && q < 5) {        // (re)starting or after a hitch: pad with silence
                static int16_t silence[AUDIO_HZ / 10];
                int n = AUDIO_HZ * latency / 2000;
                SDL_QueueAudio(adev, silence, (Uint32)(n < AUDIO_HZ / 10 ? n : AUDIO_HZ / 10) * 2);
                q = queued_ms();
            }
            double err = (q - latency) / latency;
            if (err > 1) err = 1;
            if (err < -1) err = -1;
            // locked: frames come display_hz times a second, so resample to match
            pump_audio(false, (vsync_lock ? display_hz / FRAME_HZ : 1) * (1 + 0.005 * err));
            present_ahead(ahead);
        } else {
            emulate_frame();
            pump_audio(false, 1);
            present_ahead(ahead);
            if (adev) {
                // the audio device is the clock: keep `latency` ms queued, run ahead otherwise
                while (queued_ms() > latency) SDL_Delay(1);
            } else {
                next += (uint64_t)(freq / FRAME_HZ);
                uint64_t now = SDL_GetPerformanceCounter();
                if (now < next) SDL_Delay((uint32_t)((next - now) * 1000 / freq));
                else next = now;
            }
        }
    }
    if (menu_changed) menu_close();
    settle_display(true);
    if (!fullscreen) {
        SDL_GetWindowPosition(win, &cfg.window_x, &cfg.window_y);
        SDL_GetWindowSize(win, &cfg.window_w, &cfg.window_h);
    }
    cfg.fullscreen = fullscreen;
    cfg.volume = volume;
    config_save();
    video_close();
    SDL_Quit();
    return 0;
}
