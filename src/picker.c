// The game picker (picker.h): a small window with a tile per game.
#include "picker.h"
#include "version.h"
#include "controller_labels.h"
#include "icon.h"
#include "osd.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

#define W 640
#define H 480
#define ICON 128

static uint32_t canvas[W * H], ov[320 * 240], icons[2][ICON * ICON];

static int tile_x(int i) { return i == 0 ? 160 - ICON / 2 : 480 - ICON / 2; }
#define TILE_Y 110

static void blend(uint32_t *d, uint32_t s)
{
    uint32_t a = s >> 24;
    if (!a) return;
    uint32_t r = ((s >> 16 & 255) * a + (*d >> 16 & 255) * (255 - a)) / 255;
    uint32_t g = ((s >> 8 & 255) * a + (*d >> 8 & 255) * (255 - a)) / 255;
    uint32_t b = ((s & 255) * a + (*d & 255) * (255 - a)) / 255;
    *d = r << 16 | g << 8 | b;
}

static void text(int x, int y, const char *s, uint32_t rgb)   // centred on x, in canvas pixels
{
    osd_text(ov, x / 2 - (int)strlen(s) * 3, y / 2, s, rgb);
}

static void draw(const game_t *const *games, const bool *installed, int sel, const pad_prompt_t *prompt)
{
    for (int y = 0; y < H; y++)                          // dark red-to-black backdrop
        for (int x = 0; x < W; x++) {
            uint32_t r = (uint32_t)(0x28 - 0x20 * y / H);
            canvas[y * W + x] = r << 16 | 0x040406;
        }
    osd_clear(ov);
    text(320, 40, "ULTRACOMBO", 0xffc040);
    text(320, 62, "CHOOSE A GAME", 0x909090);
    for (int i = 0; i < 2; i++) {
        int x0 = tile_x(i), y0 = TILE_Y;
        if (i == sel) {                                  // gold frame around the chosen one
            for (int y = y0 - 8; y < y0 + ICON + 8; y++)
                for (int x = x0 - 8; x < x0 + ICON + 8; x++)
                    if (y < y0 - 4 || y >= y0 + ICON + 4 || x < x0 - 4 || x >= x0 + ICON + 4)
                        canvas[y * W + x] = 0xd0a030;
        }
        for (int y = 0; y < ICON; y++)
            for (int x = 0; x < ICON; x++) {
                uint32_t p = icons[i][y * ICON + x];
                if (!installed[i]) p = (p & 0xff000000u) | ((p >> 2) & 0x3f3f3fu);   // dimmed
                blend(&canvas[(y0 + y) * W + x0 + x], p);
            }
        int cx = x0 + ICON / 2;
        text(cx, y0 + ICON + 24, games[i]->title, i == sel ? 0xffffff : 0xa0a0a0);
        text(cx, y0 + ICON + 44, games[i]->product, i == sel ? 0xffc040 : 0x806020);
        char vers[64] = "";
        for (int v = 0; v < 4 && games[i]->versions[v].label; v++)
            if (picker_version_there(games[i], v)) {
                if (*vers) strncat(vers, ", ", sizeof vers - strlen(vers) - 1);
                strncat(vers, games[i]->versions[v].label, sizeof vers - strlen(vers) - 1);
            }
        char line[96];
        if (installed[i]) snprintf(line, sizeof line, "READY: VERSION%s %s", strchr(vers, ',') ? "S" : "", vers);
        else snprintf(line, sizeof line, "FILES NOT FOUND");
        for (char *c = line; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        text(cx, y0 + ICON + 64, line, installed[i] ? 0x60c060 : 0xc05050);
    }
    char buttons[64], footer[96];
    pad_prompt_footer(prompt, PAD_FAMILY_AUTO, "START", "QUIT", buttons, sizeof buttons);
    snprintf(footer, sizeof footer, "LEFT/RIGHT CHOOSE   %s", buttons);
    text(320, 400, footer, 0x808080);
    if (!installed[0] || !installed[1])
        text(320, 424, "A GAME'S FILES GO IN THE RUN FOLDER (SEE README)", 0x606060);
    char ver[64];
    snprintf(ver, sizeof ver, "PROGRAM VERSION %s", uc_version);
    for (char *c = ver; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    text(320, 452, ver, 0x505050);
    for (int y = 0; y < H; y++)                          // the text, doubled
        for (int x = 0; x < W; x++) blend(&canvas[y * W + x], ov[(y / 2) * 320 + x / 2]);
}

const game_t *picker_choose(const game_t *const *games, const bool *installed, int initial)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return NULL;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow(PRODUCT, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!r && win) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (!tex) {
        fprintf(stderr, "picker: %s\n", SDL_GetError());
        if (r) SDL_DestroyRenderer(r);
        if (win) SDL_DestroyWindow(win);
        return NULL;
    }
    SDL_RenderSetLogicalSize(r, W, H);                   // keeps the aspect; mouse coordinates follow
    {
        static uint32_t small[64 * 64];
        icon_pixels_text(small, 64, "UC");
        SDL_Surface *s = SDL_CreateRGBSurfaceFrom(small, 64, 64, 32, 64 * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (s) { SDL_SetWindowIcon(win, s); SDL_FreeSurface(s); }
    }
    for (int i = 0; i < 2; i++) icon_pixels_text(icons[i], ICON, games[i]->icon_text);

    SDL_GameController *pads[8];
    int npads = 0;
    pad_prompt_t prompt;
    pad_prompt_init(&prompt);
    int sel = initial == 1 ? 1 : 0;
    if (!installed[sel]) sel = !sel;
    const game_t *chosen = NULL;
    bool done = false, axis_held = false;
    while (!done) {
        SDL_Event ev;
        int move = 0;
        bool go = false;
        while (SDL_PollEvent(&ev)) {
            pad_prompt_event(&prompt, &ev);
            switch (ev.type) {
            case SDL_QUIT: done = true; break;
            case SDL_CONTROLLERDEVICEADDED:
                if (npads < 8 && (pads[npads] = SDL_GameControllerOpen(ev.cdevice.which))) npads++;
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_LEFT: case SDLK_a: move = -1; break;
                case SDLK_RIGHT: case SDLK_d: move = 1; break;
                case SDLK_ESCAPE: done = true; break;
                }
                break;
            case SDL_KEYUP:                      // on release: the game's window mustn't get it as held
                switch (ev.key.keysym.sym) {
                case SDLK_1: sel = 0; go = true; break;
                case SDLK_2: sel = 1; go = true; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: go = true; break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move = 1; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: go = true; break;
                case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: done = true; break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (ev.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX) break;
                if (ev.caxis.value < -16000 || ev.caxis.value > 16000) {
                    if (!axis_held) move = ev.caxis.value < 0 ? -1 : 1;
                    axis_held = true;
                } else axis_held = false;
                break;
            case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: {
                int mx = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x;
                int my = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
                for (int i = 0; i < 2; i++)
                    if (installed[i] && mx >= tile_x(i) - 8 && mx < tile_x(i) + ICON + 8 && my >= TILE_Y - 8 && my < TILE_Y + ICON + 80) {
                        sel = i;
                        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) go = true;
                    }
                break;
            }
            }
        }
        if (move && installed[0] && installed[1]) sel = !sel;     // two tiles: either way swaps
        if (go && installed[sel]) { chosen = games[sel]; done = true; }
        draw(games, installed, sel, &prompt);
        SDL_UpdateTexture(tex, NULL, canvas, W * 4);
        SDL_RenderClear(r);
        SDL_RenderCopy(r, tex, NULL, NULL);
        SDL_RenderPresent(r);
        SDL_Delay(10);
    }
    for (int i = 0; i < npads; i++) SDL_GameControllerClose(pads[i]);   // the game opens its own
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_ResetKeyboard();                     // nothing counts as held in the game's window
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    return chosen;
}

// Lower case for the message window: paths must keep their case. Same 5x7
// layout as the OSD font (osd_glyph), 'a'..'z'; g, p, q and y use an eighth
// row for their tails.
static const uint8_t lower[26][5] = {
    {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x18,0xa4,0xa4,0xa4,0x7c},{0x7f,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},
    {0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0xfc,0x24,0x24,0x24,0x18},
    {0x18,0x24,0x24,0x18,0xfc},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},
    {0x1c,0xa0,0xa0,0xa0,0x7c},{0x44,0x64,0x54,0x4c,0x44},
};

#define MSG_COLS 150
#define MSG_ROWS 40

// n characters of s at (x, y), each font pixel `scale` pixels square, clipped to pw x ph.
static void glyphs(uint32_t *px, int pw, int ph, int x, int y, const char *s, size_t n, uint32_t rgb, int scale)
{
    for (size_t i = 0; i < n; i++, x += 6 * scale) {
        char c = s[i];
        const uint8_t *g = c >= 'a' && c <= 'z' ? lower[c - 'a'] : osd_glyph(c);
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 8; row++)
                if (g[col] >> row & 1)
                    for (int yy = y + row * scale; yy < y + (row + 1) * scale; yy++)
                        for (int xx = x + col * scale; xx < x + (col + 1) * scale; xx++)
                            if (xx >= 0 && xx < pw && yy >= 0 && yy < ph) px[yy * pw + xx] = rgb;
    }
}


bool picker_message(const char *title, const char *text)
{
    static const char footer[] = "Enter or Esc to close";
    int cols = (int)strlen(footer), rows = 0;
    for (const char *s = text; *s && rows < MSG_ROWS; rows++) {     // measure
        size_t n = strcspn(s, "\n");
        if ((int)n > cols) cols = n < MSG_COLS ? (int)n : MSG_COLS;
        s += n + (s[n] == '\n');
    }
    int pw = cols * 6 + 24, ph = rows * 10 + 44;
    static uint32_t px[(MSG_COLS * 6 + 24) * (MSG_ROWS * 10 + 44)];
    for (int i = 0; i < pw * ph; i++) px[i] = 0x140808;
    int y = 12, rows_drawn = 0;
    for (const char *s = text; *s && rows_drawn < MSG_ROWS; rows_drawn++, y += 10) {
        size_t n = strcspn(s, "\n");
        glyphs(px, pw, ph, 12, y, s, n < MSG_COLS ? n : MSG_COLS, 0xe0e0e0, 1);
        s += n + (s[n] == '\n');
    }
    glyphs(px, pw, ph, (pw - (int)strlen(footer) * 6) / 2, ph - 18, footer, strlen(footer), 0x808080, 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    int scale = pw * 2 <= 1600 ? 2 : 1;                  // doubled when it fits a laptop screen
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, pw * scale, ph * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
    if (!r && win) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, pw, ph) : NULL;
    if (!tex) {
        if (r) SDL_DestroyRenderer(r);
        if (win) SDL_DestroyWindow(win);
        return false;
    }
    SDL_RenderSetLogicalSize(r, pw, ph);
    SDL_UpdateTexture(tex, NULL, px, pw * 4);
    for (bool done = false; !done;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) done = true;
            if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_RETURN ||
                                           ev.key.keysym.sym == SDLK_KP_ENTER || ev.key.keysym.sym == SDLK_SPACE)) done = true;
            if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) done = true;
        }
        SDL_RenderClear(r);
        SDL_RenderCopy(r, tex, NULL, NULL);
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    return true;
}

// The welcome screen: no game's files were found.
#define WW 800
#define WH 600

static uint32_t wpx[WW * WH];

static int centred(int y, const char *s, uint32_t rgb, int scale)
{
    glyphs(wpx, WW, WH, (WW - (int)strlen(s) * 6 * scale) / 2, y, s, strlen(s), rgb, scale);
    return y + 11 * scale;
}

// A path, broken after a slash where it doesn't fit; returns the next line's y.
static int path_lines(int x, int y, const char *s, uint32_t rgb)
{
    size_t cols = (size_t)(WW - x - 24) / 12;
    while (*s) {
        size_t n = strlen(s);
        if (n > cols) {
            n = cols;
            for (size_t i = cols; i > cols / 2; i--)
                if (s[i - 1] == '/' || s[i - 1] == '\\') { n = i; break; }
        }
        glyphs(wpx, WW, WH, x, y, s, n, rgb, 2);
        s += n;
        y += 22;
    }
    return y;
}

static void welcome_draw(const game_t *const *games, const char *folder, const char *alt, const char *status, uint32_t status_rgb)
{
    for (int y = 0; y < WH; y++)                         // the picker's backdrop
        for (int x = 0; x < WW; x++) wpx[y * WW + x] = (uint32_t)(0x28 - 0x20 * y / WH) << 16 | 0x040406;
    for (int i = 0; i < 2; i++) {
        int x0 = WW / 2 - ICON - 12 + i * (ICON + 24), y0 = 28;
        for (int y = 0; y < ICON; y++)
            for (int x = 0; x < ICON; x++) blend(&wpx[(y0 + y) * WW + x0 + x], icons[i][y * ICON + x]);
    }
    int y = centred(180, "Welcome to UltraCombo", 0xffc040, 3) + 4;
    y = centred(y, "Native ports of the Killer Instinct arcade games", 0x909090, 2) + 18;
    char line[160];
    snprintf(line, sizeof line, "To play, add your own copies of the game files:");
    glyphs(wpx, WW, WH, 40, y, line, strlen(line), 0xffffff, 2);
    y += 30;
    for (int i = 0; i < 2; i++, y += 22) {
        snprintf(line, sizeof line, "  %s: MAME's %s set (ROMs and %s)", games[i]->title, games[i]->set, games[i]->chd);
        glyphs(wpx, WW, WH, 40, y, line, strlen(line), 0xb0b0b0, 2);
    }
    y += 16;
    snprintf(line, sizeof line, "Put them in this folder:");
    glyphs(wpx, WW, WH, 40, y, line, strlen(line), 0xffffff, 2);
    y = path_lines(64, y + 26, folder, 0x80c0ff);
    if (*alt) {
        glyphs(wpx, WW, WH, 40, y, "or:", 3, 0xffffff, 2);
        path_lines(64, y + 22, alt, 0x80c0ff);
    }
    if (*status) centred(WH - 96, status, status_rgb, 2);
    centred(WH - 60, "O  open the folder     Enter  check again     Esc  quit", 0x909090, 2);
    centred(WH - 34, "File names: README.md, or github.com/thefixinhixon/UltraCombo", 0x606060, 2);
}

// Make the folder (dir has no trailing slash) and show it in the file manager.
static bool open_folder(const char *dir)
{
    make_dir(dir);
    struct stat st;
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return false;
    char url[4200];
    snprintf(url, sizeof url, "file://%s%s", dir[0] == '/' ? "" : "/", dir);
    return SDL_OpenURL(url) == 0;
}

int picker_welcome(const game_t *const *games, const char *folder, const char *alt, bool (*found)(void))
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return -1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow(PRODUCT, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WW, WH, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
    if (!r && win) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, WW, WH) : NULL;
    if (!tex) {
        fprintf(stderr, "welcome: %s\n", SDL_GetError());
        if (r) SDL_DestroyRenderer(r);
        if (win) SDL_DestroyWindow(win);
        return -1;
    }
    SDL_RenderSetLogicalSize(r, WW, WH);
    {
        static uint32_t small[64 * 64];
        icon_pixels_text(small, 64, "UC");
        SDL_Surface *s = SDL_CreateRGBSurfaceFrom(small, 64, 64, 32, 64 * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (s) { SDL_SetWindowIcon(win, s); SDL_FreeSurface(s); }
    }
    for (int i = 0; i < 2; i++) icon_pixels_text(icons[i], ICON, games[i]->icon_text);

    const char *status = "";
    uint32_t status_rgb = 0;
    bool done = false, ok = false;
    while (!done) {
        SDL_Event ev;
        bool check = false;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: done = true; break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(ev.cdevice.which); break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) done = true;
                if (ev.key.keysym.sym == SDLK_o) {
                    bool opened = open_folder(folder) || (*alt && open_folder(alt));
                    status = opened ? "Opening the folder: copy the files in, then press Enter"
                                    : "Couldn't open it; the folder's path is shown above";
                    status_rgb = opened ? 0x60c060 : 0xc05050;
                }
                break;
            case SDL_KEYUP:                      // on release: the game's window mustn't get it as held
                if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER || ev.key.keysym.sym == SDLK_SPACE) check = true;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A || ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) check = true;
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B || ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) done = true;
                break;
            }
        }
        if (check && found()) ok = done = true;
        else if (check) { status = "Still no game files there: check the folder and the file names"; status_rgb = 0xc05050; }
        welcome_draw(games, folder, alt, status, status_rgb);
        SDL_UpdateTexture(tex, NULL, wpx, WW * 4);
        SDL_RenderClear(r);
        SDL_RenderCopy(r, tex, NULL, NULL);
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_ResetKeyboard();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    return ok ? 1 : 0;
}
