// Save states, rewind history and rollback (see state.h).
#include "state.h"
#include "game.h"
#include "cpu.h"
#include "ide.h"
#include "machine.h"
#include "mem.h"
#include "rec.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096
#define NPAGES (RAM_PAGES + IDE_TRACK_PAGES)
#define FILE_MAGIC "KI2STATE"
#define FILE_VERSION 2      // 2: drive timing in the disk and board state
// Bumped when the emulation's timing changes without changing the state's
// layout: states still load, but netplay won't pair differing builds.
// 1: EPROM wait states, CPU slice overruns paid back, a 4 MB/s drive (issue #11)
#define TIMING_VERSION 1

static uint8_t *mirror;             // every page as of the last checkpoint
static uint8_t *core, *core_tmp;    // non-paged state at the last checkpoint
static size_t core_len;

static uint8_t *page_ptr(int g)
{
    return g < RAM_PAGES ? ram + (size_t)g * PAGE : ide_tracked() + (size_t)(g - RAM_PAGES) * PAGE;
}
static uint8_t *dirty_flag(int g)
{
    return g < RAM_PAGES ? &ram_dirty[g] : &ide_dirty[g - RAM_PAGES];
}

/* ---------------------------------------------------------------- */
/* Non-paged ("core") state: CPU, recompiler, disk controller, DCS   */
/* ---------------------------------------------------------------- */

static size_t core_io(uint8_t *buf, bool load)
{
    size_t n = 0;
    // order matters on load: RAM is already in place, rec_state rebuilds from it
    n += cpu_state(buf ? buf + n : NULL, load);
    n += rec_state(buf ? buf + n : NULL, load);
    n += ide_state(buf ? buf + n : NULL, load);
    n += sound_state(buf ? buf + n : NULL, load);
    n += machine_state(buf ? buf + n : NULL, load);
    return n;
}

/* ---------------------------------------------------------------- */
/* Page compression: XOR against a reference, then zero-run coding   */
/* ---------------------------------------------------------------- */

// Tokens of (u16 zero words, u16 literal words, literal words...). Returns bytes written
// (at most PAGE + 8 for an incompressible page).
static size_t pack_xor(uint8_t *out, const uint8_t *a, const uint8_t *b)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    uint8_t *o = out;
    int i = 0, n = PAGE / 4;
    while (i < n) {
        int z = i;
        while (i < n && x[i] == y[i]) i++;
        int l = i;
        // a literal run ends at 3+ equal words in a row (cheaper as a zero run)
        while (i < n && !(x[i] == y[i] && (i + 1 >= n || x[i + 1] == y[i + 1]) && (i + 2 >= n || x[i + 2] == y[i + 2]))) i++;
        uint16_t zc = (uint16_t)(l - z), lc = (uint16_t)(i - l);
        memcpy(o, &zc, 2); memcpy(o + 2, &lc, 2); o += 4;
        for (int k = l; k < i; k++) { uint32_t v = x[k] ^ y[k]; memcpy(o, &v, 4); o += 4; }
    }
    return (size_t)(o - out);
}

// dst ^= unpacked data. Returns bytes consumed.
static size_t unpack_xor(uint8_t *dst, const uint8_t *in)
{
    uint32_t *d = (uint32_t *)dst;
    const uint8_t *p = in;
    int i = 0, n = PAGE / 4;
    while (i < n) {
        uint16_t zc, lc;
        memcpy(&zc, p, 2); memcpy(&lc, p + 2, 2); p += 4;
        i += zc;
        for (int k = 0; k < lc && i < n; k++, i++) { uint32_t v; memcpy(&v, p, 4); p += 4; d[i] ^= v; }
    }
    return (size_t)(p - in);
}

// Admit the whole file before unpack_xor can read it or change live memory.
// Rewind entries are internal; only file input needs this bounds check.
static bool valid_pages(const uint8_t *p, const uint8_t *end)
{
    for (uint32_t g = 0; g < NPAGES; g++) {
        unsigned words = 0;
        while (words < PAGE / 4) {
            if ((size_t)(end - p) < 4) return false;
            uint16_t zc, lc;
            memcpy(&zc, p, 2); memcpy(&lc, p + 2, 2); p += 4;
            if ((!zc && !lc) || zc > PAGE / 4 - words) return false;
            words += zc;
            if (lc > PAGE / 4 - words || (size_t)(end - p) < (size_t)lc * 4) return false;
            words += lc;
            p += (size_t)lc * 4;
        }
    }
    return p == end;
}

/* ---------------------------------------------------------------- */
/* Rewind history                                                    */
/* ---------------------------------------------------------------- */

// Entry k takes the machine from checkpoint k+1 back to checkpoint k:
// the core at k, plus each page that changed in between, XOR its value at k+1.
typedef struct {
    size_t size;
    uint32_t npages;
    // followed by core_len bytes of core, then npages x (u32 page, packed xor)
} entry_t;

#define MAX_ENTRIES 16384
static entry_t *hist[MAX_ENTRIES];
static int hist_head, hist_count;   // ring of the newest hist_count entries ending at hist_head-1
static size_t hist_bytes, hist_budget;
static uint8_t *scratch;            // building one entry

static void hist_drop_oldest(void)
{
    int i = (hist_head - hist_count + MAX_ENTRIES) % MAX_ENTRIES;
    hist_bytes -= hist[i]->size;
    free(hist[i]);
    hist[i] = NULL;
    hist_count--;
}

void state_set_history(size_t bytes)
{
    hist_budget = bytes;
    while (hist_count && (hist_bytes > hist_budget || !bytes)) hist_drop_oldest();
}

double state_history_seconds(void) { return hist_count / FRAME_HZ; }
size_t state_history_bytes(void) { return hist_bytes; }

void state_init(void)
{
    core_len = core_io(NULL, false);
    if (!mirror) {
        mirror = malloc((size_t)NPAGES * PAGE);
        core = malloc(core_len);
        core_tmp = malloc(core_len);
        scratch = malloc(sizeof(entry_t) + core_len + (size_t)NPAGES * (PAGE + 16));
    }
    for (int g = 0; g < NPAGES; g++) { memcpy(mirror + (size_t)g * PAGE, page_ptr(g), PAGE); *dirty_flag(g) = 0; }
    core_io(core, false);
    while (hist_count) hist_drop_oldest();
}

void state_checkpoint(void)
{
    uint8_t *p = NULL;
    entry_t *e = NULL;
    if (hist_budget) {
        e = (entry_t *)scratch;
        e->npages = 0;
        p = scratch + sizeof *e;
        memcpy(p, core, core_len);
        p += core_len;
    }
    for (int g = 0; g < NPAGES; g++) {
        uint8_t *df = dirty_flag(g);
        if (!*df) continue;
        *df = 0;
        uint8_t *cur = page_ptr(g), *mir = mirror + (size_t)g * PAGE;
        if (!memcmp(cur, mir, PAGE)) continue;          // rewritten with the same data
        if (e) {
            uint32_t gi = (uint32_t)g;
            memcpy(p, &gi, 4);
            p += 4;
            p += pack_xor(p, cur, mir);
            e->npages++;
        }
        memcpy(mir, cur, PAGE);
    }
    core_io(core, false);
    if (!e) return;

    e->size = (size_t)(p - scratch);
    entry_t *copy = malloc(e->size);
    if (!copy) return;
    memcpy(copy, scratch, e->size);
    if (hist_count == MAX_ENTRIES) hist_drop_oldest();
    hist[hist_head] = copy;
    hist_head = (hist_head + 1) % MAX_ENTRIES;
    hist_count++;
    hist_bytes += copy->size;
    while (hist_bytes > hist_budget && hist_count > 1) hist_drop_oldest();
}

void state_rollback(void)
{
    for (int g = 0; g < NPAGES; g++) {
        uint8_t *df = dirty_flag(g);
        if (!*df) continue;
        *df = 0;
        memcpy(page_ptr(g), mirror + (size_t)g * PAGE, PAGE);
        if (g >= RAM_PAGES) ide_tracked_changed();
    }
    core_io(core, true);
}

bool state_rewind(void)
{
    if (!hist_count) return false;
    state_rollback();
    hist_head = (hist_head - 1 + MAX_ENTRIES) % MAX_ENTRIES;
    entry_t *e = hist[hist_head];
    hist[hist_head] = NULL;
    hist_count--;
    hist_bytes -= e->size;

    const uint8_t *p = (const uint8_t *)(e + 1);
    memcpy(core, p, core_len);
    p += core_len;
    for (uint32_t i = 0; i < e->npages; i++) {
        uint32_t g;
        memcpy(&g, p, 4);
        p += 4;
        uint8_t *mir = mirror + (size_t)g * PAGE;
        p += unpack_xor(mir, p);                        // mirror now holds the older page
        memcpy(page_ptr((int)g), mir, PAGE);
        if (g >= RAM_PAGES) ide_tracked_changed();
    }
    free(e);
    for (int g = 0; g < NPAGES; g++) *dirty_flag(g) = 0;
    core_io(core, true);
    return true;
}

/* ---------------------------------------------------------------- */
/* Files                                                             */
/* ---------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static uint64_t rom_id(void) { return fnv(0xcbf29ce484222325ull, rom, ROM_SIZE); }

// Builds that can play together: same ROM, same recompiled code, same state layout.
uint64_t state_compat_id(void)
{
    uint64_t h = rom_id() ^ ((uint64_t)core_len << 32) ^ FILE_VERSION ^ ((uint64_t)TIMING_VERSION << 8);
    if (rec_enabled && rec_mod) {
        h = fnv(h, &rec_mod->nfuncs, sizeof rec_mod->nfuncs);
        h = fnv(h, &rec_mod->nlabels, sizeof rec_mod->nlabels);
        h = fnv(h, rec_mod->labels, rec_mod->nlabels * sizeof *rec_mod->labels);
        h = fnv(h, rec_mod->image, rec_mod->image_words * 4);
    }
    h = fnv(h, &rec_enabled, sizeof rec_enabled);
    return h;
}

// Fast check for netplay: CPU/DSP/disk state plus the game's variables page range.
uint64_t state_quick_hash(void)
{
    core_io(core_tmp, false);
    uint64_t h = fnv(0xcbf29ce484222325ull, core_tmp, core_len);
    return game.vars_len ? fnv(h, ram2 + game.vars, game.vars_len) : h;
}

uint64_t state_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int g = 0; g < NPAGES; g++) h = fnv(h, page_ptr(g), PAGE);
    core_io(core_tmp, false);
    return fnv(h, core_tmp, core_len);
}

typedef struct {
    char magic[8];
    uint32_t version, core_len, npages, reserved;
    uint64_t rom_id, frame;
} file_header_t;

bool state_save_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    file_header_t h = { FILE_MAGIC, FILE_VERSION, (uint32_t)core_len, NPAGES, 0, rom_id(), machine_frame() };
    fwrite(&h, sizeof h, 1, f);
    core_io(core_tmp, false);
    fwrite(core_tmp, 1, core_len, f);
    static const uint8_t zero[PAGE];
    static uint8_t buf[PAGE + 16];
    for (int g = 0; g < NPAGES; g++) {
        size_t n = pack_xor(buf, page_ptr(g), zero);
        fwrite(buf, 1, n, f);
    }
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool state_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = len > 0 ? malloc((size_t)len) : NULL;
    bool ok = data && fread(data, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    file_header_t h;
    if (ok && (size_t)len >= sizeof h) memcpy(&h, data, sizeof h);
    else ok = false;
    if (ok && (memcmp(h.magic, FILE_MAGIC, 8) || h.version != FILE_VERSION || h.core_len != core_len ||
               h.npages != NPAGES || h.rom_id != rom_id())) {
        fprintf(stderr, "state: %s is from a different build or ROM\n", path);
        ok = false;
    }
    if (!ok) { free(data); return false; }

    if (core_len > (size_t)len - sizeof h ||
        !valid_pages(data + sizeof h + core_len, data + len)) {
        fprintf(stderr, "state: %s is invalid or truncated\n", path);
        free(data);
        return false;
    }

    const uint8_t *p = data + sizeof h, *end = data + len;
    const uint8_t *core_in = p;
    p += core_len;
    for (int g = 0; g < NPAGES && p < end; g++) {
        uint8_t *dst = page_ptr(g);
        memset(dst, 0, PAGE);
        p += unpack_xor(dst, p);
    }
    rec_forget();
    ide_tracked_changed();
    memcpy(core, core_in, core_len);
    core_io(core, true);
    free(data);
    state_init();      // new mirror and checkpoint; history no longer applies
    return true;
}
