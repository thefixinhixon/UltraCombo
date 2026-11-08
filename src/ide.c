// IDE/ATA hard disk backed by a disk image: a raw file or a MAME CHD, both
// only ever read. The first sectors (the game's settings, audits and high
// scores) are kept in memory so save states cover them, and saved to a small
// settings file of their own by ide_flush(); the image itself stays as it is.
#include "ide.h"
#include "cpu.h"
#include "mem.h"
#include "third_party/libchdr/include/libchdr/chd.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DSC  0x10
#define ST_DRDY 0x40
#define ST_BSY  0x80

static FILE *raw;                           // the image: a raw file...
static chd_file *chd;                       // ...or a CHD
static char chd_sha1_hex[41];
static char settings_path[4096];
static uint8_t tracked[IDE_TRACK_BYTES];   // sectors 0-63, the authoritative copy
static bool tracked_unsaved;

// Image blocks, read as needed: 16 KB of a raw file, or one CHD hunk.
#define CACHE_SLOTS 64
static uint32_t block_bytes;
static struct { uint64_t at; uint8_t *data; } cache[CACHE_SLOTS];

// Writes outside the settings sectors (no game makes any): kept for the session.
static struct { uint32_t lba; uint8_t data[512]; } *extra;
static int nextra;

static struct {
    uint8_t *img;                          // unused, kept for the save state layout
    uint64_t size;
    uint32_t cyls, heads, secs, total;

    uint8_t features, error, count, sector, cyl_lo, cyl_hi, drvhead, status, devctl;
    uint8_t cmd;
    uint16_t buf[256];
    int buf_pos;          // words transferred within buf
    bool reading, writing;
    uint32_t lba;         // current transfer LBA
    int remaining;        // sectors remaining in transfer
    int multiple;         // sectors per block for READ/WRITE MULTIPLE
    int64_t irq_delay;    // cycles until IRQ asserts (<0: none pending)
    bool irq;
    uint16_t identify[256];
    bool seeking;         // the next sector is the first of a command
    uint32_t next_lba;    // where the heads are after the last read
} d;

uint8_t ide_dirty[IDE_TRACK_PAGES];
static __typeof__(d) d_power_on;     // controller state right after ide_open

void ide_power_on(void)
{
    uint64_t size = d.size;
    d = d_power_on;
    d.size = size;
}

static const uint8_t *image_block(uint64_t blk)
{
    int slot = (int)(blk % CACHE_SLOTS);
    if (cache[slot].at == blk) return cache[slot].data;
    uint8_t *b = cache[slot].data;
    memset(b, 0, block_bytes);                  // unreadable: zeros
    if (chd) {
        static bool warned;
        if (chd_read(chd, (uint32_t)blk, b) != CHDERR_NONE && !warned) {
            fprintf(stderr, "ide: the CHD has an unreadable block (%llu)\n", (unsigned long long)blk);
            warned = true;
        }
    } else if (fseek64(raw, (long long)(blk * block_bytes), SEEK_SET) == 0 && fread(b, 1, block_bytes, raw) > 0) {}
    cache[slot].at = blk;
    return b;
}

// Bytes of the image as stored, ignoring the settings file.
static void image_read(uint64_t off, void *dst, size_t n)
{
    uint8_t *o = dst;
    while (n) {
        uint64_t blk = off / block_bytes;
        size_t at = (size_t)(off % block_bytes), k = block_bytes - at < n ? block_bytes - at : n;
        memcpy(o, image_block(blk) + at, k);
        o += k; off += k; n -= k;
    }
}

// Cheap identity of the disk image: its size plus 4 KB from every 4 MB. The
// same for a raw file and a CHD of the same disk.
uint64_t ide_image_id(void)
{
    uint64_t h = 0xcbf29ce484222325ull ^ d.size;
    static uint8_t buf[4096];
    for (uint64_t off = 0; off + sizeof buf <= d.size; off += 4u << 20) {
        if (off < IDE_TRACK_BYTES) continue;              // settings: synced separately
        image_read(off, buf, sizeof buf);
        for (size_t i = 0; i < sizeof buf; i++) { h ^= buf[i]; h *= 0x100000001b3ull; }
    }
    return h;
}

uint8_t *ide_tracked(void) { return tracked; }
const char *ide_chd_sha1(void) { return chd ? chd_sha1_hex : NULL; }
uint64_t ide_image_size(void) { return d.size; }

static void read_sector(uint32_t lba, void *dst)
{
    uint64_t off = (uint64_t)lba * 512;
    if (off < IDE_TRACK_BYTES) { memcpy(dst, tracked + off, 512); return; }
    for (int i = 0; i < nextra; i++) if (extra[i].lba == lba) { memcpy(dst, extra[i].data, 512); return; }
    image_read(off, dst, 512);
}

static void write_sector(uint32_t lba, const void *src)
{
    uint64_t off = (uint64_t)lba * 512;
    if (off < IDE_TRACK_BYTES) {
        memcpy(tracked + off, src, 512);
        ide_dirty[off >> 12] = 1;
        tracked_unsaved = true;
        return;
    }
    static bool warned;
    if (!warned) fprintf(stderr, "ide: write to sector %u is kept only until the program closes\n", lba);
    warned = true;
    int i = 0;
    while (i < nextra && extra[i].lba != lba) i++;
    if (i == nextra) {
        extra = realloc(extra, sizeof *extra * (size_t)(nextra + 1));
        extra[nextra++].lba = lba;
    }
    memcpy(extra[i].data, src, 512);
}

// Save the settings sectors: to a new file first, so a crash can't leave half of one.
void ide_flush(void)
{
    if (!tracked_unsaved || !*settings_path) return;
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.new", settings_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "ide: can't write %s\n", tmp); return; }
    bool ok = fwrite(tracked, 1, IDE_TRACK_BYTES, f) == IDE_TRACK_BYTES;
    ok &= fclose(f) == 0;
    if (!ok) { remove(tmp); return; }
#ifdef _WIN32
    remove(settings_path);                      // rename doesn't replace on Windows
#endif
    if (rename(tmp, settings_path) == 0) tracked_unsaved = false;
}

void ide_tracked_changed(void) { tracked_unsaved = true; }

size_t ide_state(uint8_t *buf, bool load)
{
    if (buf && load) {
        uint64_t size = d.size;
        memcpy(&d, buf, sizeof d);
        d.img = NULL; d.size = size;
    } else if (buf) {
        memcpy(buf, &d, sizeof d);
        memset(buf + offsetof(__typeof__(d), img), 0, sizeof d.img);    // keep states pointer-free
    }
    return sizeof d;
}

static void set_irq(bool s)
{
    d.irq = s;
    cpu_set_irq(1, s && !(d.devctl & 2));
}

static void ident_str(int word, int nwords, const char *s)
{
    int len = (int)strlen(s);
    for (int i = 0; i < nwords; i++) {
        char a = (2 * i < len) ? s[2 * i] : ' ';
        char b = (2 * i + 1 < len) ? s[2 * i + 1] : ' ';
        d.identify[word + i] = (uint16_t)((uint8_t)a << 8 | (uint8_t)b);
    }
}

static bool open_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    char magic[8] = { 0 };
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "MComprHD", 8)) {
        fclose(f);
        chd_error e = chd_open(path, CHD_OPEN_READ, NULL, &chd);
        if (e != CHDERR_NONE) { fprintf(stderr, "%s: %s\n", path, chd_error_string(e)); chd = NULL; return false; }
        const chd_header *h = chd_get_header(chd);
        d.size = h->logicalbytes;
        block_bytes = h->hunkbytes;
        for (int i = 0; i < 20; i++) snprintf(chd_sha1_hex + 2 * i, 3, "%02x", h->sha1[i]);
    } else {
        raw = f;
        fseek64(raw, 0, SEEK_END);
        d.size = (uint64_t)ftell64(raw);
        block_bytes = 16384;
    }
    for (int i = 0; i < CACHE_SLOTS; i++) { cache[i].data = malloc(block_bytes); cache[i].at = ~0ull; }
    if (d.size < IDE_TRACK_BYTES) { fprintf(stderr, "%s: too small\n", path); return false; }
    return true;
}

// The settings sectors: from the settings file once there is one, else from
// the image (a raw file keeps what earlier versions wrote into it).
static void load_settings(const char *settings)
{
    snprintf(settings_path, sizeof settings_path, "%s", settings ? settings : "");
    FILE *s = settings ? fopen(settings, "rb") : NULL;
    if (!s || fread(tracked, 1, IDE_TRACK_BYTES, s) != IDE_TRACK_BYTES) image_read(0, tracked, IDE_TRACK_BYTES);
    if (s) fclose(s);
    tracked_unsaved = false;
    memset(ide_dirty, 1, sizeof ide_dirty);     // save states and rewind see the change
}

void ide_use_settings(const char *path)
{
    ide_flush();
    load_settings(path);
}

bool ide_open(const char *image, const char *settings, uint32_t cyls, uint32_t heads, uint32_t secs, int serial_offset)
{
    if (!open_image(image)) return false;
    load_settings(settings);
    atexit(ide_flush);

    d.cyls = cyls; d.heads = heads; d.secs = secs;
    d.total = (uint32_t)(d.size / 512);

    memset(d.identify, 0, sizeof(d.identify));
    d.identify[0] = 0x045a;
    d.identify[1] = (uint16_t)cyls;
    d.identify[3] = (uint16_t)heads;
    d.identify[4] = (uint16_t)(512 * secs);
    d.identify[5] = 512;
    d.identify[6] = (uint16_t)secs;
    ident_str(10, 10, "");
    ident_str(23, 4, "1.0");
    ident_str(27, 20, "");
    d.identify[47] = 0x8010;
    d.identify[49] = 0x0200;
    d.identify[51] = 0x0200;
    d.identify[53] = 0x0001;
    d.identify[54] = (uint16_t)cyls;
    d.identify[55] = (uint16_t)heads;
    d.identify[56] = (uint16_t)secs;
    uint32_t chs = cyls * heads * secs;
    d.identify[57] = (uint16_t)chs;
    d.identify[58] = (uint16_t)(chs >> 16);
    d.identify[60] = (uint16_t)d.total;
    d.identify[61] = (uint16_t)(d.total >> 16);

    // The game checks the drive model; mirror what the original hardware reports.
    for (int i = 10; i < 20; i++) d.identify[i] = ('0' << 8) | '0';
    for (int i = 27; i < 47; i++) d.identify[i] = (' ' << 8) | ' ';
    ident_str(serial_offset, 4, "ST9150AG");

    d.status = ST_DRDY | ST_DSC;
    d.count = 1; d.sector = 1;
    d.irq_delay = -1;
    d_power_on = d;
    return true;
}

// Write the image out as a raw file (what chdman extractraw makes).
bool ide_write_raw(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return false; }
    static uint8_t buf[1 << 16];
    bool ok = true;
    for (uint64_t off = 0; off < d.size && ok; off += sizeof buf) {
        size_t n = d.size - off < sizeof buf ? (size_t)(d.size - off) : sizeof buf;
        image_read(off, buf, n);
        ok = fwrite(buf, 1, n, f) == n;
    }
    ok &= fclose(f) == 0;
    return ok;
}

static uint32_t cur_lba(void)
{
    if (d.drvhead & 0x40)
        return ((uint32_t)(d.drvhead & 0x0f) << 24) | ((uint32_t)d.cyl_hi << 16) | ((uint32_t)d.cyl_lo << 8) | d.sector;
    uint32_t cyl = ((uint32_t)d.cyl_hi << 8) | d.cyl_lo;
    uint32_t head = d.drvhead & 0x0f;
    return (cyl * d.heads + head) * d.secs + (d.sector - 1);
}

static void set_lba_regs(uint32_t lba)
{
    if (d.drvhead & 0x40) {
        d.sector = (uint8_t)lba; d.cyl_lo = (uint8_t)(lba >> 8); d.cyl_hi = (uint8_t)(lba >> 16);
        d.drvhead = (d.drvhead & 0xf0) | ((lba >> 24) & 0x0f);
    } else {
        uint32_t cyl = lba / (d.heads * d.secs);
        uint32_t rem = lba % (d.heads * d.secs);
        d.cyl_lo = (uint8_t)cyl; d.cyl_hi = (uint8_t)(cyl >> 8);
        d.drvhead = (d.drvhead & 0xf0) | (uint8_t)(rem / d.secs);
        d.sector = (uint8_t)(rem % d.secs + 1);
    }
}

static void schedule_irq(void) { d.irq_delay = 200; }

// The cabinet's drive took time to find and read data, and the games rely on
// it: the VS screen waits for the fight's data while the announcer finishes.
// Typical figures for an early-1990s IDE drive, in CPU cycles (100 MHz).
#define SEEK_CYCLES   1700000     // average seek plus half a turn (~17 ms)
#define CMD_CYCLES     100000     // command overhead when the heads are already there (1 ms)
#define SECTOR_CYCLES   12800     // 512 bytes at ~4 MB/s: at 2 MB/s KI1's attract mode falls behind
                                  // its music, which cuts the logo's voice before Glacius (issue #11)

// Sector data is ready: after the drive's time, or at once with an instant disk.
static void sector_ready(void)
{
    if (hw.instant_disk) {
        d.status = ST_DRDY | ST_DSC | ST_DRQ;
        schedule_irq();
        return;
    }
    int64_t t = SECTOR_CYCLES;
    if (d.seeking) t += d.lba == d.next_lba ? CMD_CYCLES : SEEK_CYCLES;
    d.seeking = false;
    d.next_lba = d.lba + 1;
    d.status = ST_BSY;          // ide_tick sets DRQ when the time is up
    d.irq_delay = t;
}

static void fill_read_buf(void)
{
    if (d.lba >= d.total) {
        d.status = ST_DRDY | ST_ERR; d.error = 0x10; // IDNF
        d.reading = false;
        schedule_irq();
        return;
    }
    read_sector(d.lba, d.buf);
    d.buf_pos = 0;
    sector_ready();
}

static void command(uint8_t c)
{
    static int log = -1;
    if (log < 0) log = getenv("KI2_LOG_IDE") != NULL;
    if (log) fprintf(stderr, "ide: cmd %02x count=%d sec=%d cyl=%d dh=%02x lba=%u\n", c, d.count, d.sector,
                     d.cyl_hi << 8 | d.cyl_lo, d.drvhead, cur_lba());
    d.cmd = c;
    d.error = 0;
    set_irq(false);
    d.reading = d.writing = false;
    // only the master drive exists
    if (d.drvhead & 0x10) { d.status = 0; return; }
    switch (c) {
    case 0xec: // IDENTIFY DEVICE
        memcpy(d.buf, d.identify, 512);
        d.buf_pos = 0; d.remaining = 1; d.reading = true; d.lba = ~0u;
        d.status = ST_DRDY | ST_DSC | ST_DRQ;
        schedule_irq();
        return;
    case 0x20: case 0x21: case 0xc4: // READ SECTORS / READ MULTIPLE
        d.lba = cur_lba();
        d.remaining = d.count ? d.count : 256;
        d.reading = true;
        d.seeking = true;
        fill_read_buf();
        return;
    case 0x30: case 0x31: case 0xc5: // WRITE SECTORS / WRITE MULTIPLE
        d.lba = cur_lba();
        d.remaining = d.count ? d.count : 256;
        d.writing = true;
        d.buf_pos = 0;
        d.status = ST_DRDY | ST_DSC | ST_DRQ;
        return;
    case 0xc6: d.multiple = d.count; break;          // SET MULTIPLE MODE
    case 0x91:                                       // INITIALIZE DEVICE PARAMETERS
        if (d.count) { d.secs = d.count; d.heads = (d.drvhead & 0x0f) + 1; }
        break;
    case 0xef: break;                                // SET FEATURES
    case 0x40: case 0x41: break;                     // READ VERIFY
    case 0x70: break;                                // SEEK
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe5: case 0xe6: break; // power mgmt
    default:
        if ((c & 0xf0) == 0x10) break;               // RECALIBRATE
        fprintf(stderr, "ide: unknown command %02x\n", c);
        d.status = ST_DRDY | ST_ERR; d.error = 0x04;
        schedule_irq();
        return;
    }
    d.status = ST_DRDY | ST_DSC;
    schedule_irq();
}

static uint16_t data_read(void)
{
    if (!d.reading || !(d.status & ST_DRQ)) return 0xffff;
    uint16_t v = d.buf[d.buf_pos++];
    if (d.buf_pos >= 256) {
        d.status &= ~ST_DRQ;
        if (d.lba != ~0u) {
            d.lba++;
            set_lba_regs(d.lba);
            d.count--;
        }
        if (--d.remaining > 0) fill_read_buf();
        else { d.reading = false; d.status = ST_DRDY | ST_DSC; }
    }
    return v;
}

static void data_write(uint16_t v)
{
    if (!d.writing || !(d.status & ST_DRQ)) return;
    d.buf[d.buf_pos++] = v;
    if (d.buf_pos >= 256) {
        if (d.lba < d.total) write_sector(d.lba, d.buf);
        d.lba++;
        set_lba_regs(d.lba);
        d.count--;
        d.buf_pos = 0;
        if (--d.remaining > 0) d.status = ST_DRDY | ST_DSC | ST_DRQ;
        else { d.writing = false; d.status = ST_DRDY | ST_DSC; }
        schedule_irq();
    }
}

uint32_t ide_read(int reg)
{
    switch (reg) {
    case 0: return data_read();
    case 1: return d.error;
    case 2: return d.count;
    case 3: return d.sector;
    case 4: return d.cyl_lo;
    case 5: return d.cyl_hi;
    case 6: return d.drvhead | 0xa0;
    case 7:
        set_irq(false);
        if (d.drvhead & 0x10) return 0;
        return d.status;
    }
    return 0xff;
}

void ide_write(int reg, uint32_t v)
{
    switch (reg) {
    case 0: data_write((uint16_t)v); break;
    case 1: d.features = (uint8_t)v; break;
    case 2: d.count = (uint8_t)v; break;
    case 3: d.sector = (uint8_t)v; break;
    case 4: d.cyl_lo = (uint8_t)v; break;
    case 5: d.cyl_hi = (uint8_t)v; break;
    case 6: d.drvhead = (uint8_t)v; break;
    case 7: command((uint8_t)v); break;
    }
}

uint32_t ide_alt_status(void) { return (d.drvhead & 0x10) ? 0 : d.status; }

void ide_devctl(uint32_t v)
{
    uint8_t prev = d.devctl;
    d.devctl = (uint8_t)v;
    if ((v & 4) && !(prev & 4)) { // software reset
        d.reading = d.writing = false;
        d.status = ST_DRDY | ST_DSC;
        d.error = 1; d.count = 1; d.sector = 1; d.cyl_lo = d.cyl_hi = 0; d.drvhead = 0;
    }
    set_irq(d.irq);
}

void ide_tick(int cycles)
{
    if (d.irq_delay >= 0) {
        d.irq_delay -= cycles;
        if (d.irq_delay < 0) {
            if (d.reading && (d.status & ST_BSY)) d.status = ST_DRDY | ST_DSC | ST_DRQ;
            set_irq(true);
        }
    }
}
