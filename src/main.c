// UltraCombo: native frontend for the Killer Instinct arcade games (game.h).
#include "config.h"
#include "cpu.h"
#include "ide.h"
#include "machine.h"
#include "mem.h"
#include "rec.h"
#include "sound.h"
#include "state.h"
#include "net.h"
#include "netplay.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif
#include "icon.h"
#include "recomp.h"
#include "rec_build.h"
#include "game.h"
#include "training.h"
#include "launch.h"
#include "picker.h"

#define CPU_HZ        100000000
#define LINES         261
#define VISIBLE_LINES 240
#define CYCLES_PER_LINE ((int)(CPU_HZ / FRAME_HZ / LINES))

static bool load_file(const char *path, void *dst, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    size_t n = fread(dst, 1, size, f);
    fclose(f);
    if (n != size) { fprintf(stderr, "%s: short read\n", path); return false; }
    return true;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static bool readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

const char *base_dir(void)
{
    static char *dir;
    if (!dir) dir = SDL_GetBasePath();
    if (!dir) dir = SDL_strdup("./");
    return dir;
}

// Running as an AppImage: the folder holding the .AppImage file, else NULL.
static const char *appimage_dir(void)
{
    static char dir[4096];
    const char *p = getenv("APPIMAGE");
    if (!p || !*p || strlen(p) >= sizeof dir) return NULL;
    if (!*dir) {
        strcpy(dir, p);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else strcpy(dir, "./");
    }
    return dir;
}

static bool writable_dir(const char *dir)
{
    char probe[4200];
    snprintf(probe, sizeof probe, "%s.ki2-write-test", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return false;
    fclose(f);
    remove(probe);
    return true;
}

// Where settings, the cache, save states and screenshots go: beside the
// executable when that folder is writable (portable use), beside an AppImage
// that has a run/ folder next to it, otherwise the user's own data folder.
const char *data_dir(void)
{
    static char *dir;
    if (dir) return dir;
    if (writable_dir(base_dir())) { dir = SDL_strdup(base_dir()); return dir; }
    const char *ai = appimage_dir();
    if (ai) {
        char run[4200];
        snprintf(run, sizeof run, "%srun", ai);
        struct stat st;
        if (!stat(run, &st) && S_ISDIR(st.st_mode) && writable_dir(ai)) { dir = SDL_strdup(ai); return dir; }
    }
    dir = SDL_GetPrefPath("", PRODUCT);          // one folder for both games: their files have their own names
    if (!dir) dir = SDL_strdup(base_dir());
    return dir;
}

// Use `name` if it exists, otherwise look in the folders of a game a launcher
// passed, beside the executable and in run/ there, in run/ beside an
// AppImage, then in run/ in the data folder.
const char *find_data(const char *name, char *buf, size_t n)
{
    if (readable(name)) return name;
    for (int i = 0; launch_dir(i); i++) {
        snprintf(buf, n, "%s%s", launch_dir(i), name);
        if (readable(buf)) return buf;
    }
    snprintf(buf, n, "%s%s", base_dir(), name);
    if (readable(buf)) return buf;
    snprintf(buf, n, "%srun/%s", base_dir(), name);
    if (readable(buf)) return buf;
    if (appimage_dir()) {
        snprintf(buf, n, "%srun/%s", appimage_dir(), name);
        if (readable(buf)) return buf;
    }
    snprintf(buf, n, "%srun/%s", data_dir(), name);
    return readable(buf) ? buf : name;
}

static int version = -1;               // index into game.versions, -1 = custom ROM
static char rom_dir[4096];             // folder of the boot ROM in use, with separator

int machine_version(void) { return version; }

// A version's boot ROM: beside the one in use, else where game files are looked for.
static const char *version_rom(int v, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s", rom_dir, game.versions[v].rom);
    return *rom_dir && readable(buf) ? buf : find_data(game.versions[v].rom, buf, n);
}

bool machine_version_available(int v)
{
    if (v < 0 || v >= 4 || !game.versions[v].label) return false;
    char buf[4096];
    return readable(version_rom(v, buf, sizeof buf));
}

// saves/<boot ROM name without extension>.settings: one per game version
static const char *settings_for(const char *rom_path, char *buf, size_t n)
{
    const char *base = strrchr(rom_path, '/'), *b2 = strrchr(rom_path, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : rom_path;
    snprintf(buf, n, "%ssaves", data_dir());
    make_dir(buf);
    const char *dot = strchr(base, '.');
    snprintf(buf, n, "%ssaves/%.*s.settings", data_dir(), (int)(dot ? (size_t)(dot - base) : strlen(base)), base);
    return buf;
}

bool machine_switch_version(int v, char *err, size_t n)
{
    if (!machine_version_available(v)) { snprintf(err, n, "%s is not there", game.versions[v].rom); return false; }
    char path[4096], settings[4400];
    const char *p = version_rom(v, path, sizeof path);
    static uint8_t buf[ROM_SIZE];
    if (!load_file(p, buf, ROM_SIZE)) { snprintf(err, n, "%s can't be read", p); return false; }
    memcpy(rom, buf, ROM_SIZE);
    ide_use_settings(settings_for(p, settings, sizeof settings));
    version = v;
    return true;
}

// The disk: a raw image if there is one (it may hold settings written by
// earlier versions), else the MAME CHD, also in MAME's own folder for it.
static const char *find_disk(char *buf, size_t n)
{
    const char *p = find_data(game.disk, buf, n);
    if (readable(p)) return p;
    p = find_data(game.chd, buf, n);
    if (readable(p)) return p;
    char sub[256];
    snprintf(sub, sizeof sub, "%s/%s", game.set, game.chd);
    p = find_data(sub, buf, n);
    return readable(p) ? p : game.chd;
}

static void report(bool fatal, const char *fmt, ...);

// Compare the files with MAME's good dumps; a warning, never a refusal.
static void check_files(const char *rom_path, const char *sound_dir)
{
    char msg[2048] = "", line[600];
    uint32_t crc = crc32_of(rom, ROM_SIZE);
    bool known = false;
    for (int v = 0; v < 4 && game.versions[v].label; v++) known |= crc == game.versions[v].crc;
    if (!known) {
        snprintf(line, sizeof line, "  %s (CRC32 %08x)\n", rom_path, crc);
        strncat(msg, line, sizeof msg - strlen(msg) - 1);
    }
    static uint8_t buf[0x80000];
    for (int i = 0; i < 8; i++) {
        char path[4200];
        snprintf(path, sizeof path, "%s/%s", sound_dir, game.sound_roms[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;                       // missing: reported elsewhere
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t c = crc32_of(buf, got);
        if (got != sizeof buf || c != game.sound_crc[i]) {
            snprintf(line, sizeof line, "  %s (CRC32 %08x)\n", path, c);
            strncat(msg, line, sizeof msg - strlen(msg) - 1);
        }
    }
    const char *sha1 = ide_chd_sha1();
    if (sha1 && strcmp(sha1, game.chd_sha1)) {
        snprintf(line, sizeof line, "  the CHD (SHA1 %s)\n", sha1);
        strncat(msg, line, sizeof msg - strlen(msg) - 1);
    }
    uint64_t want = (uint64_t)game.cyls * game.heads * game.secs * 512;
    if (ide_image_size() != want) {
        snprintf(line, sizeof line, "  the disk image (%llu bytes, not %llu)\n",
                 (unsigned long long)ide_image_size(), (unsigned long long)want);
        strncat(msg, line, sizeof msg - strlen(msg) - 1);
    }
    if (*msg)
        report(false, "These files don't match the known good dumps of %s:\n%s\n"
               "The game may not work correctly. README.md (\"Checking your files\") lists the right ones.",
               game.title, msg);
}

static struct {
    int sound_acc;        // CPU cycles not yet passed on to the DCS
    uint64_t frame;       // frames run since power-on
} m;
#define sound_acc m.sound_acc

uint64_t machine_frame(void) { return m.frame; }

size_t machine_state(uint8_t *buf, bool load)
{
    if (buf && load) { memcpy(&m, buf, sizeof m); memcpy(&hw, buf + sizeof m, sizeof hw); }
    else if (buf) { memcpy(buf, &m, sizeof m); memcpy(buf + sizeof m, &hw, sizeof hw); }
    return sizeof m + sizeof hw;
}

// Power-cycle: as at startup, without reloading anything from disk.
void machine_reset(void)
{
    memset(ram, 0, RAM_SIZE);
    memset(&m, 0, sizeof m);
    uint32_t dsw = hw.dsw, instant = hw.instant_disk;     // settings stay as set
    hw = (hw_t){ .p1 = 0xffffffffu, .p2 = 0xffffffffu, .volume = 0xffffffffu, .dsw = dsw, .instant_disk = instant };
    sound_power_on();
    ide_power_on();
    cpu_reset();
    rec_reset();
    state_init();
}

void run_frame(void)
{
    m.frame++;
    for (int line = 0; line < LINES; line++) {
        if (line == VISIBLE_LINES) cpu_set_irq(0, true);
        if (line == 0) cpu_set_irq(0, false);
        int remaining = CYCLES_PER_LINE;
        while (remaining > 0) {
            int slice = remaining < 256 ? remaining : 256;
            int ran = (int)cpu_run((uint64_t)slice);
            if (cpu_idle) {
                // spinning on an interrupt line: nothing changes until the next scanline
                cpu_skip((uint64_t)(remaining - ran));
                slice = remaining;
            }
            ide_tick(slice);
            // DCS runs at 10 MHz against the 100 MHz R4600
            sound_acc += slice;
            sound_run(sound_acc / 10);
            sound_acc %= 10;
            remaining -= slice;
        }
    }
}

void to_rgb(uint32_t *dst)
{
    const uint16_t *src = video_base();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            uint16_t p = src[y * 320 + x];
            uint32_t r = (p & 31), g = (p >> 5) & 31, b = (p >> 10) & 31;
            dst[y * 320 + x] = (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
        }
}

static void save_ppm(const char *path)
{
    static uint32_t px[320 * 240];
    to_rgb(px);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 240\n255\n");
    for (int i = 0; i < 320 * 240; i++) {
        uint8_t c[3] = { px[i] >> 16, px[i] >> 8, px[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

// Scripted input for headless runs: "frame:button:duration,..."
typedef struct { const char *name; uint32_t *port; uint32_t bit; } button_t;
static const button_t buttons[] = {
    { "p1hq", &hw.p1, 0x0001 }, { "p1hm", &hw.p1, 0x0002 }, { "p1hf", &hw.p1, 0x0004 },
    { "p1lq", &hw.p1, 0x0008 }, { "p1lm", &hw.p1, 0x0010 }, { "p1lf", &hw.p1, 0x0020 },
    { "p1up", &hw.p1, 0x0040 }, { "p1down", &hw.p1, 0x0080 }, { "p1left", &hw.p1, 0x0100 },
    { "p1right", &hw.p1, 0x0200 }, { "start1", &hw.p1, 0x0400 }, { "coin1", &hw.p1, 0x0800 },
    { "test", &hw.p1, 0x1000 },
    { "p2hq", &hw.p2, 0x0001 }, { "p2hm", &hw.p2, 0x0002 }, { "p2hf", &hw.p2, 0x0004 },
    { "p2lq", &hw.p2, 0x0008 }, { "p2lm", &hw.p2, 0x0010 }, { "p2lf", &hw.p2, 0x0020 },
    { "p2up", &hw.p2, 0x0040 }, { "p2down", &hw.p2, 0x0080 }, { "p2left", &hw.p2, 0x0100 },
    { "p2right", &hw.p2, 0x0200 }, { "start2", &hw.p2, 0x0400 }, { "coin2", &hw.p2, 0x0800 },
};
typedef struct { int frame, len; const button_t *b; } script_ev_t;
static script_ev_t script[256];
static int nscript;

static void parse_script(const char *spec)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok && nscript < 256; tok = strtok(NULL, ",")) {
        int frame, len = 6;
        char name[32];
        if (sscanf(tok, "%d:%31[a-z0-9]:%d", &frame, name, &len) < 2) continue;
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if (!strcmp(buttons[i].name, name)) { script[nscript++] = (script_ev_t){ frame, len, &buttons[i] }; break; }
    }
}

static void apply_script(int frame)
{
    hw.p1 = hw.p2 = 0xffffffffu;
    for (int i = 0; i < nscript; i++)
        if (frame >= script[i].frame && frame < script[i].frame + script[i].len)
            *script[i].b->port &= ~script[i].b->bit;
}

/* Netplay callbacks shared with the frontend: emulate with explicit inputs. */
void machine_run_inputs(const uint16_t in[2], bool resim)
{
    hw.p1 = 0xffffffffu & ~(uint32_t)(in[0] & 0x0fff);
    hw.p2 = 0xffffffffu & ~(uint32_t)(in[1] & 0x0fff);
    run_frame();
    static int desync_at = -1;                      // testing: KI2_DESYNC_AT=frame breaks this side
    if (desync_at < 0) desync_at = getenv("KI2_DESYNC_AT") ? atoi(getenv("KI2_DESYNC_AT")) : 0;
    if (desync_at && machine_frame() == (uint64_t)desync_at && !resim) { ram2[game.vars + 0x10] ^= 0x55; ram_touch(&ram2[game.vars + 0x10]); }
    state_checkpoint();
    if (resim) { int16_t junk[4096]; while (sound_fetch(junk, 4096) > 0) {} }
}

void machine_netplay_reset(void)
{
    machine_reset();
    for (int i = 0; i < NP_BOOT_FRAMES; i++) {
        hw.p1 = hw.p2 = 0xffffffffu;
        run_frame();
    }
    state_init();
}

int net_warmup;     // testing: play locally (with rewind history) until the session starts

static int headless_netplay(int end, const char *host_port, const char *join, int delay, int fps,
                            int lat, int jit, int loss)
{
    netplay_set_callbacks(machine_run_inputs, machine_netplay_reset);
    net_simulate(lat, jit, loss);
    bool ok;
    if (host_port) ok = netplay_host(atoi(host_port), delay);
    else {
        char h[256]; int port = NP_DEFAULT_PORT;
        snprintf(h, sizeof h, "%s", join);
        char *c = strrchr(h, ':');
        if (c) { *c = 0; port = atoi(c + 1); }
        ok = netplay_join(h, port, delay);
    }
    if (!ok) { fprintf(stderr, "%s\n", netplay_take_error()); return 1; }
    netplay_set_end((uint32_t)end);
    int me = host_port ? 0 : 1;
    double next = net_now_ms();
    uint32_t last_report = 0;
    if (net_warmup) state_set_history((size_t)256 << 20);
    for (;;) {
        if (net_warmup && !netplay_running()) { hw.p1 = hw.p2 = 0xffffffffu; run_frame(); state_checkpoint(); }
        // this side's buttons from the script, for the frame the input will be used in
        uint32_t f = netplay_input_frame();
        apply_script((int)f + 1);
        uint16_t local = (uint16_t)(~(me ? hw.p2 : hw.p1) & 0x0fff);
        netplay_update(local);
        const char *err = netplay_take_error();
        if (err) { fprintf(stderr, "%s\n", err); return 2; }
        if (netplay_running() && netplay_confirmed() >= (uint32_t)end) {
            fprintf(stderr, "state hash %016llx at frame %llu\n", (unsigned long long)state_hash(), (unsigned long long)machine_frame());
            for (int i = 0; i < 30; i++) { netplay_update(0); SDL_Delay(10); }   // let the other side confirm too
            netplay_stop();
            return 0;
        }
        if (netplay_running() && machine_frame() / 600 != last_report) {
            last_report = (uint32_t)(machine_frame() / 600);
            fprintf(stderr, "%s\n", netplay_status());
        }
        next += 1000.0 / fps;
        double now = net_now_ms();
        if (next > now) SDL_Delay((uint32_t)(next - now));
        else if (now - next > 100) next = now;
    }
}

// Missing game files: a dialog in windowed mode (Windows has no console), stderr otherwise.
static int headless_mode;

static void report(bool fatal, const char *fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
    if (!headless_mode && SDL_ShowSimpleMessageBox(fatal ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_WARNING, game.product, msg, NULL) != 0)
        picker_message(game.product, msg);      // no zenity and no X core fonts: our own window
}

static const char *files_help(void)
{
    static char buf[8400], where[8200];
    if (appimage_dir() && strcmp(appimage_dir(), data_dir()))
        snprintf(where, sizeof where, "a folder named run beside the AppImage:\n    %srun\nor:\n    %srun", appimage_dir(), data_dir());
    else
        snprintf(where, sizeof where, "\n    %srun", data_dir());
    snprintf(buf, sizeof buf,
             "Put the game files in %s\n(or set their paths in %s.ini).\n\n"
             "Needed (%s):\n"
             "  %s  - boot ROM\n"
             "  %s  - hard disk: MAME's CHD as it is (or a raw image, %s)\n"
             "  %s - %s, %s - %s  - sound ROMs", where, game.id, game.title,
             game.versions[0].rom, game.chd, game.disk,
             game.sound_roms[0], game.sound_roms[3], game.sound_roms[4], game.sound_roms[7]);
    return buf;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ultracombo [options]\n"
        "  --game ki1|ki2  Killer Instinct or Killer Instinct 2 (default: a picker\n"
        "                  when both are installed); the options below are for %s\n"
        "  --rom PATH      boot ROM (%s)\n"
        "  --disk PATH     disk image: MAME's %s, or a raw image extracted from it\n"
        "  --settings PATH file for the game's settings and high scores (default: saves/)\n"
        "  --game-version V  which version to run (as in the menu, e.g. 1.4)\n"
        "  --write-raw PATH  write the disk image out as a raw file and exit\n"
        "  --sound DIR     directory holding the 8 sound ROMs (%s...)\n"
        "  --headless N    run N frames without a window\n"
        "  --shot-every N  in headless mode, save a PPM every N frames\n"
        "  --script S      headless input, e.g. \"3600:coin1:6,3700:start1:6\"\n"
        "  --trace         log every executed instruction\n"
        "  --save-at F P   in headless mode, save a state file P after frame F\n"
        "  --load P        start from state file P\n"
        "  --rewind-test N headless: every N frames rewind N, replay, and check the state matches\n"
        "  --runahead N    headless: run N frames ahead and roll back after each frame\n"
        "  PATH            play the game in PATH (a MAME set's zip, its CHD or a folder), as\n"
        "                  launchers start it: Esc quits instead of opening the menu (F1 opens it)\n"
        "  --host [PORT]   host a netplay game (default port from %s.ini, 7845)\n"
        "  --join HOST[:PORT]  join a netplay game\n"
        "  --net-host PORT / --net-join HOST:PORT\n"
        "                  headless netplay test: play this side of --script until frame N\n"
        "  --net-delay N, --net-fps F, --net-sim LATENCY:JITTER:LOSS%%  (testing)\n"
        "Settings, key bindings and hotkeys are in %s.ini beside the executable\n"
        "(written with defaults on first run). See README.md.\n",
        game.title, game.versions[0].rom, game.chd, game.sound_roms[0], game.id, game.id);
}

static int main_argc;
static char **main_argv;

// A game counts as installed when a boot ROM of one of its versions is there.
static bool installed(const game_t *g)
{
    char buf[4096];
    for (int v = 0; v < 4 && g->versions[v].label; v++)
        if (readable(find_data(g->versions[v].rom, buf, sizeof buf))) return true;
    return false;
}

static bool any_installed(void) { return installed(&game_ki1) || installed(&game_ki2); }

bool picker_version_there(const game_t *g, int v)
{
    char buf[4096];
    return readable(find_data(g->versions[v].rom, buf, sizeof buf));
}

bool machine_both_installed(void)
{
    static int both = -1;                // files don't come and go while it runs
    if (both < 0) both = installed(&game_ki1) && installed(&game_ki2);
    return both;
}

// Options followed by values (main's loop): the first scan skips the values,
// and an argument that is neither is a game's path from a launcher.
static int option_values(const char *a, const char *next)
{
    static const char *const one[] = {
        "--rom", "--game-version", "--game", "--disk", "--settings", "--write-raw", "--sound", "--headless",
        "--shot-every", "--script", "--trace-from", "--load", "--rewind-test", "--runahead", "--join",
        "--net-host", "--net-join", "--net-delay", "--net-fps", "--net-sim", NULL,
    };
    for (int i = 0; one[i]; i++) if (!strcmp(a, one[i])) return 1;
    if (!strcmp(a, "--save-at") || !strcmp(a, "--dis")) return 2;
    if (!strcmp(a, "--recomp")) return 3;
    if (!strcmp(a, "--host")) return next && next[0] != '-';     // the port is optional
    return 0;
}

// Which game to run: --game, else the only one installed, else the picker
// (headless: the one used last). False: quit.
static bool choose_game(const char *want, bool headless)
{
    char last_path[4200], last[16] = "";
    snprintf(last_path, sizeof last_path, "%slast_game.txt", data_dir());
    FILE *f = fopen(last_path, "r");
    if (f) { if (!fgets(last, sizeof last, f)) last[0] = 0; fclose(f); last[strcspn(last, "\r\n")] = 0; }
    const game_t *g = NULL;
    if (want) {
        g = game_find(want);
        if (!g) { fprintf(stderr, "--game %s: unknown (ki1 or ki2)\n", want); return false; }
    } else {
        bool have[2] = { installed(&game_ki1), installed(&game_ki2) };
        const game_t *const both[2] = { &game_ki1, &game_ki2 };
        int lasti = game_find(last) == &game_ki1 ? 0 : 1;
        if (!have[0] && !have[1] && !headless) {         // neither: the welcome screen, until they're there
            char folder[4200], alt[4200] = "";
            if (appimage_dir() && strcmp(appimage_dir(), data_dir())) {
                snprintf(folder, sizeof folder, "%srun", appimage_dir());
                snprintf(alt, sizeof alt, "%srun", data_dir());
            } else
                snprintf(folder, sizeof folder, "%srun", data_dir());
            fprintf(stderr, "No game files found. Put them in %s%s%s (see README.md).\n", folder, *alt ? " or " : "", alt);
            int w = picker_welcome(both, folder, alt, any_installed);
            if (w == 0) return false;                    // closed
            have[0] = installed(&game_ki1);              // no window: the missing-file message follows
            have[1] = installed(&game_ki2);
        }
        if (have[0] != have[1]) g = have[0] ? &game_ki1 : &game_ki2;
        else if (!have[0] || headless) g = both[lasti];       // none: its error explains what's missing
        else if (!(g = picker_choose(both, have, lasti))) return false;
    }
    game_select(g);
    if (!headless && (f = fopen(last_path, "w"))) { fprintf(f, "%s\n", g->id); fclose(f); }
    return true;
}

// Back to the picker: save, then start the program again without --game.
void machine_restart_to_picker(void)
{
    config_save();
    ide_flush();
    char *args[64];
    int n = 0;
    for (int i = 0; i < main_argc && n < 63; i++) {
        if (i > 0 && !strcmp(main_argv[i], "--game") && i + 1 < main_argc) { i++; continue; }
        args[n++] = main_argv[i];
    }
    args[n] = NULL;
#ifdef _WIN32
    static char exe[4096];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    _execv(exe, (const char *const *)args);
#else
    static char exe[4096];                  // the real path, so the process keeps its name
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[len > 0 ? len : 0] = 0;
    execv(len > 0 ? exe : "/proc/self/exe", args);
#endif
    fprintf(stderr, "could not restart: %s\n", strerror(errno));
}

// --recomp RAM0 RAM2 OUTDIR: translate a snapshot, and the sound DSP's code
// from the sound ROMs, to C (what `make recomp` uses).
static char **recomp_args;

static int recomp_snapshot(const char *snap0, const char *snap2, const char *outdir)
{
    static uint8_t r0b[RAM0_SIZE], r2b[RAM2_SIZE];
    FILE *a = fopen(snap0, "rb"), *b = fopen(snap2, "rb");
    bool ok = a && b && fread(r0b, 1, RAM0_SIZE, a) == RAM0_SIZE && fread(r2b, 1, RAM2_SIZE, b) == RAM2_SIZE;
    if (a) fclose(a);
    if (b) fclose(b);
    char err[512] = "cannot read the snapshot";
    recomp_info_t info;
    adsprec_image_t img[6];
    int nimg = sound_code_images(img, 6);
    if (ok) ok = recomp_generate(r0b, r2b, outdir, &info, err, sizeof err) && adsprec_generate(img, nimg, outdir, err, sizeof err);
    if (!ok) fprintf(stderr, "recomp: %s\n", err);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    main_argc = argc;
    main_argv = argv;
    game_select(&game_ki1);              // until one is chosen (the program's icon is UltraCombo's)
    // packaging helper: before anything writes a log or settings
    if (argc == 3 && !strcmp(argv[1], "--write-icon")) {
        size_t n = strlen(argv[2]);                     // .png: 256 pixels, else .ico
        bool png = n > 4 && !strcmp(argv[2] + n - 4, ".png");
        return (png ? icon_write_png(argv[2], 256) : icon_write_ico(argv[2])) ? 0 : 1;
    }
    const char *launch_path = NULL;
    {
        const char *want = NULL;
        bool tool = false;                   // headless runs and tools never show the picker
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--game") && i + 1 < argc) want = argv[i + 1];
            if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--recomp") || !strcmp(argv[i], "--dis") ||
                !strcmp(argv[i], "--write-raw") || !strcmp(argv[i], "--test-shaders")) tool = true;
            if (argv[i][0] != '-' && !launch_path) launch_path = argv[i];
            i += option_values(argv[i], i + 1 < argc ? argv[i + 1] : NULL);
        }
        if (launch_path) {                   // from a launcher: that game, from its files
            char err[4400];
            const game_t *g = launch_open(launch_path, err, sizeof err);
            if (!g) {
                fprintf(stderr, "%s\n", err);
                if (!tool) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, PRODUCT, err, NULL);
                return 1;
            }
            if (!want) want = g->id;
        }
        if (!choose_game(want, tool)) return 0;
    }
#ifdef _WIN32
    {   // a GUI program has no console: keep the log beside the executable
        static char log[4096];
        snprintf(log, sizeof log, "%s%s.log", data_dir(), game.id);
        if (!getenv("KI2_CONSOLE")) { freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0); }
    }
#endif
    static char ini[4096];
    snprintf(ini, sizeof ini, "%s%s.ini", data_dir(), game.id);
    config_load(ini);
    const char *rom_path = *cfg.rom ? cfg.rom : NULL;       // NULL: the configured version
    const char *want_version = launch_version() ? launch_version() : cfg.version;   // a clone's zip: its version
    const char *disk_path = *cfg.disk ? cfg.disk : NULL;     // NULL: look for one
    const char *settings_path = NULL, *write_raw = NULL;
    const char *sound_dir = *cfg.sound ? cfg.sound : ".";
    int headless = 0, shot_every = 0, dis_count = 0, trace_from = 0;
    int save_at = 0, rewind_test = 0, runahead = 0;
    const char *net_host = NULL, *net_join = NULL;
    int net_delay = 2, net_fps = 60, net_lat = 0, net_jit = 0, net_loss = 0;
    extern int net_warmup;
    const char *save_path = NULL, *load_path = NULL;
    uint32_t dis_addr = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--game-version") && i + 1 < argc) want_version = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) i++;            // chosen before the settings
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settings_path = argv[++i];
        else if (!strcmp(argv[i], "--write-raw") && i + 1 < argc) write_raw = argv[++i];
        else if (!strcmp(argv[i], "--sound") && i + 1 < argc) sound_dir = argv[++i];
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) headless = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")) cpu_trace = true;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) parse_script(argv[++i]);
        else if (!strcmp(argv[i], "--trace-from") && i + 1 < argc) trace_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save-at") && i + 2 < argc) { save_at = atoi(argv[++i]); save_path = argv[++i]; }
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "--rewind-test") && i + 1 < argc) rewind_test = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--runahead") && i + 1 < argc) runahead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host")) { extern const char *opt_net_host; opt_net_host = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ""; }
        else if (!strcmp(argv[i], "--recomp") && i + 3 < argc) { recomp_args = &argv[i + 1]; i += 3; }
        else if (!strcmp(argv[i], "--test-shaders")) { extern bool opt_test_shaders; opt_test_shaders = true; }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) { extern const char *opt_net_join; opt_net_join = argv[++i]; }
        else if (!strcmp(argv[i], "--net-host") && i + 1 < argc) net_host = argv[++i];
        else if (!strcmp(argv[i], "--net-join") && i + 1 < argc) net_join = argv[++i];
        else if (!strcmp(argv[i], "--net-delay") && i + 1 < argc) net_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--net-fps") && i + 1 < argc) net_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--net-warmup")) net_warmup = 1;
        else if (!strcmp(argv[i], "--net-sim") && i + 1 < argc) sscanf(argv[++i], "%d:%d:%d", &net_lat, &net_jit, &net_loss);
        else if (!strcmp(argv[i], "--dis") && i + 2 < argc) { dis_addr = (uint32_t)strtoul(argv[++i], 0, 16); dis_count = atoi(argv[++i]); }
        else if (argv[i] != launch_path) { usage(); return 1; }
    }

    headless_mode = headless || dis_count;
    static char rom_buf[4096], disk_buf[4096];
    if (!rom_path) {
        // the configured version, else the first one there is
        for (int v = 0; v < 4 && game.versions[v].label; v++)
            if (!strcmp(want_version, game.versions[v].label)) version = v;
        if (version < 0 && *want_version) fprintf(stderr, "version %s: unknown, using %s\n", want_version, game.versions[0].label);
        if (version < 0) version = 0;
        if (!machine_version_available(version))
            for (int v = 0; v < 4 && game.versions[v].label; v++)
                if (machine_version_available(v)) {
                    fprintf(stderr, "version %s: %s not found, using %s\n", game.versions[version].label,
                            game.versions[version].rom, game.versions[v].label);
                    version = v;
                    break;
                }
        rom_path = game.versions[version].rom;
    }
    rom_path = find_data(rom_path, rom_buf, sizeof rom_buf);
    {
        const char *base = strrchr(rom_path, '/'), *b2 = strrchr(rom_path, '\\');
        if (b2 > base) base = b2;
        base = base ? base + 1 : rom_path;
        snprintf(rom_dir, sizeof rom_dir, "%.*s", (int)(base - rom_path), rom_path);
        for (int v = 0; version < 0 && v < 4 && game.versions[v].label; v++)   // a ROM given by path
            if (!strcmp(base, game.versions[v].rom)) version = v;
    }
    disk_path = disk_path ? find_data(disk_path, disk_buf, sizeof disk_buf) : find_disk(disk_buf, sizeof disk_buf);
    if (write_raw) {                        // a tool: needs only the disk
        bool ok = ide_open(disk_path, NULL, game.cyls, game.heads, game.secs, game.serial_offset) && ide_write_raw(write_raw);
        fprintf(stderr, "%s %s\n", ok ? "wrote" : "could not write", write_raw);
        return ok ? 0 : 1;
    }
    if (!load_file(rom_path, rom, ROM_SIZE)) {
        report(true, "%s can't start: the boot ROM %s was not found or is incomplete.\n\n%s", game.product, rom_path, files_help());
        return 1;
    }
    if (dis_count) {
        for (int i = 0; i < dis_count; i++) {
            uint32_t a = dis_addr + 4 * i, op;
            uint8_t *p = mem_ptr(a & 0x1fffffffu);
            if (getenv("KI2_DIS_RAM")) { static bool loaded; if (!loaded) { loaded = true;
                FILE *f = fopen("ram0.bin", "rb"); if (f) { fread(ram0, 1, RAM0_SIZE, f); fclose(f); }
                f = fopen("ram2.bin", "rb"); if (f) { fread(ram2, 1, RAM2_SIZE, f); fclose(f); } } }
            if (!p) break;
            memcpy(&op, p, 4);
            char buf[96];
            printf("%08x: %08x  %s\n", a, op, cpu_disasm(a, op, buf));
        }
        return 0;
    }
    // the game's settings and high scores: one file per boot ROM (game version)
    static char settings_buf[4400];
    if (!settings_path) settings_path = settings_for(rom_path, settings_buf, sizeof settings_buf);
    if (!ide_open(disk_path, settings_path, game.cyls, game.heads, game.secs, game.serial_offset)) {
        report(true, "%s can't start: the disk image %s was not found or can't be read.\n\n%s", game.product, disk_path, files_help());
        return 1;
    }

    static char snd_buf[4096];
    if (!strcmp(sound_dir, ".")) {
        // locate the sound ROMs the same way as the boot ROM
        const char *u10 = find_data(game.sound_roms[0], snd_buf, sizeof snd_buf);
        const char *slash = strrchr(u10, '/');
        if (!slash) slash = strrchr(u10, '\\');
        if (slash) { memmove(snd_buf, u10, (size_t)(slash - u10)); snd_buf[slash - u10] = 0; sound_dir = snd_buf; }
    }
    if (!sound_init(sound_dir))
        report(false, "Some sound ROMs were not found in %s, so the game will run without sound.\n\n%s", sound_dir, files_help());
    if (recomp_args) return recomp_snapshot(recomp_args[0], recomp_args[1], recomp_args[2]);
    check_files(rom_path, sound_dir);
    hw.dsw = getenv("KI2_DSW") ? (uint32_t)strtoul(getenv("KI2_DSW"), 0, 16) : config_dsw();
    hw.instant_disk = getenv("KI2_DISK") ? !strcmp(getenv("KI2_DISK"), "instant") : cfg.disk_speed == 1;
    cpu_reset();
    rec_init();
    if (getenv("KI2_LOAD_MODULE")) {        // testing: a module built by hand
        char err[512];
        if (!rec_build_load_file(getenv("KI2_LOAD_MODULE"), err, sizeof err)) fprintf(stderr, "%s\n", err);
    }
    if (headless && !rec_enabled && !getenv("KI2_NOREC")) {
        // no linked-in code: use the code built on this machine, building it if asked (KI2_BUILD=1)
        char msg[512];
        if (rec_build_load_cached(msg, sizeof msg) || (getenv("KI2_BUILD") && rec_build(NULL, msg, sizeof msg)))
            fprintf(stderr, "rec: %s\n", msg);
        else if (getenv("KI2_BUILD")) fprintf(stderr, "rec: %s\n", msg);
    }
    // KI2_INTERP_HIST=file: where the interpreter still runs (missing entry points)
    if (headless && getenv("KI2_INTERP_HIST")) cpu_interp_hist = calloc(1 << 16, sizeof *cpu_interp_hist);

    state_init();
    if (load_path) {
        if (!state_load_file(load_path)) { fprintf(stderr, "%s: cannot load state\n", load_path); return 1; }
        fprintf(stderr, "loaded %s at frame %llu\n", load_path, (unsigned long long)machine_frame());
    }

    if (getenv("KI2_RESET_TEST")) {         // testing: does anything survive a power-on reset?
        int n = atoi(getenv("KI2_RESET_TEST"));
        for (int i = 0; i < n; i++) run_frame();
        machine_netplay_reset();
        fprintf(stderr, "after reset: hash %016llx quick %016llx\n", (unsigned long long)state_hash(), (unsigned long long)state_quick_hash());
        FILE *o = fopen("reset_ram.bin", "wb"); fwrite(ram, 1, RAM_SIZE, o); fclose(o);
        static uint8_t core[1 << 20];
        size_t n0 = cpu_state(core, false), n1 = rec_state(core + n0, false), n2 = ide_state(core + n0 + n1, false);
        size_t n3 = sound_state(core + n0 + n1 + n2, false), n4 = machine_state(core + n0 + n1 + n2 + n3, false);
        o = fopen("reset_core.bin", "wb"); fwrite(core, 1, n0 + n1 + n2 + n3 + n4, o); fclose(o);
        fprintf(stderr, "core parts: cpu %zu rec %zu ide %zu sound %zu machine %zu\n", n0, n1, n2, n3, n4);
        return 0;
    }
    if (getenv("KI2_SWITCH_TEST")) {        // testing: N:V runs N frames, then switches to version V
        int n = 0, v = 0;
        sscanf(getenv("KI2_SWITCH_TEST"), "%d:%d", &n, &v);
        for (int i = 0; i < n; i++) run_frame();
        char err[256], msg[512];
        if (!machine_switch_version(v, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        rec_unload();
        machine_reset();
        if (!getenv("KI2_NOREC") && rec_build_load_cached(msg, sizeof msg)) fprintf(stderr, "rec: %s\n", msg);
    }
    if (headless && (net_host || net_join)) return headless_netplay(headless, net_host, net_join, net_delay, net_fps, net_lat, net_jit, net_loss);

    if (headless) {
        if (rewind_test || getenv("KI2_HISTORY")) state_set_history((size_t)1 << 30);
        int rewind_fail = 0;
        FILE *wav = getenv("KI2_WAV") ? fopen(getenv("KI2_WAV"), "wb") : NULL;
        uint32_t wav_samples = 0;
        if (wav) fseek(wav, 44, SEEK_SET);
        for (int f = (int)machine_frame() + 1; f <= headless; f++) {
            if (trace_from && f == trace_from) cpu_trace = true;
            apply_script(f);
            if (getenv("KI2_TRAINING") && f >= atoi(getenv("KI2_TRAINING"))) { cfg.training = true; training_apply(true); }   // testing: from frame N
            uint64_t t0 = SDL_GetPerformanceCounter();
            run_frame();
            state_checkpoint();
            if (runahead) {
                for (int k = 0; k < runahead; k++) run_frame();
                state_rollback();
            }
            if (save_at && f == save_at) {
                if (state_save_file(save_path)) fprintf(stderr, "saved %s at frame %d\n", save_path, f);
                else fprintf(stderr, "%s: save failed\n", save_path);
            }
            if (rewind_test && f % rewind_test == 0 && f > rewind_test) {
                // go back N frames, replay them with the same input, compare
                uint64_t want = state_hash();
                int n = 0;
                while (n < rewind_test && state_rewind()) n++;
                for (int k = f - n + 1; k <= f; k++) { apply_script(k); run_frame(); state_checkpoint(); }
                uint64_t got = state_hash();
                if (got != want) rewind_fail++;
                fprintf(stderr, "rewind test frame %d: back %d, %s (%.1f MB history)\n", f, n,
                        got == want ? "match" : "MISMATCH", state_history_bytes() / 1048576.0);
            }
            static double acc_ms, max_ms;
            double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            acc_ms += ms;
            if (ms > max_ms) max_ms = ms;
            if (f % 60 == 0) {
                if (getenv("KI2_SPEED")) {
                    static uint64_t last_cyc, last_int;
                    fprintf(stderr, "frames %d-%d: %.1f ms/frame (%.0f%% speed) worst %.1f ms, %.0f%% interpreted, code gen %u\n", f - 59, f, acc_ms / 60, 100.0 * (1000.0 / FRAME_HZ) / (acc_ms / 60), max_ms,
                            100.0 * (cpu_interp_count - last_int) / (double)(cpu.cycles - last_cyc + 1), rec_code_gen);
                    last_cyc = cpu.cycles; last_int = cpu_interp_count;
                }
                acc_ms = max_ms = 0;
            }
            if (getenv("KI2_DUMP_AT")) {        // F: ram0.bin/ram2.bin at frame F; F1,F2,...: ram2_<F>.bin
                const char *l = getenv("KI2_DUMP_AT");
                bool list = strchr(l, ',') != NULL;
                for (const char *q = l; *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
                    if (atoi(q) != f) continue;
                    char n0[32] = "ram0.bin", n2[32] = "ram2.bin";
                    if (list) { snprintf(n0, sizeof n0, "ram0_%05d.bin", f); snprintf(n2, sizeof n2, "ram2_%05d.bin", f); }
                    FILE *o = fopen(n0, "wb"); fwrite(ram0, 1, RAM0_SIZE, o); fclose(o);
                    o = fopen(n2, "wb"); fwrite(ram2, 1, RAM2_SIZE, o); fclose(o);
                }
            }
            if (wav) {
                int16_t buf[4096]; int n;
                while ((n = sound_fetch(buf, 4096)) > 0) { fwrite(buf, 2, (size_t)n, wav); wav_samples += (uint32_t)n; }
            }
            if (shot_every && f % shot_every == 0) {
                char name[64];
                snprintf(name, sizeof name, "frame_%05d.ppm", f);
                save_ppm(name);
                fprintf(stderr, "frame %d pc=%08x\n", f, cpu.pc);
            }
        }
        save_ppm("frame_last.ppm");
        fprintf(stderr, "state hash %016llx at frame %llu\n", (unsigned long long)state_hash(), (unsigned long long)machine_frame());
        if (rewind_fail) { fprintf(stderr, "%d rewind mismatches\n", rewind_fail); return 3; }
        if (cpu_interp_hist) {
            FILE *h = fopen(getenv("KI2_INTERP_HIST"), "w");
            for (int i = 0; i < 1 << 16; i++) if (cpu_interp_hist[i] > 1000) fprintf(h, "%08x %u\n", 0x88000000u + i * 4, cpu_interp_hist[i]);
            fclose(h);
        }
        if (wav) {
            uint32_t rate = (uint32_t)sound_rate(), bytes = wav_samples * 2;
            uint8_t h[44] = "RIFF____WAVEfmt ";
            uint32_t v;
            v = 36 + bytes; memcpy(h + 4, &v, 4);
            v = 16; memcpy(h + 16, &v, 4);
            uint16_t w = 1; memcpy(h + 20, &w, 2); memcpy(h + 22, &w, 2);
            memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4);
            w = 2; memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2);
            memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
            fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
            fprintf(stderr, "wrote %u samples at %u Hz\n", wav_samples, rate);
        }
        return 0;
    }

    return frontend_main();
}
