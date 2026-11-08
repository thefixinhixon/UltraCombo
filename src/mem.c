// I/O space. The two games' boards put the same ports at different
// addresses (game.h).
#include "mem.h"
#include "cpu.h"
#include "game.h"
#include "ide.h"
#include "sound.h"
#include <stdio.h>
#include <string.h>

uint8_t ram[RAM_SIZE];
uint8_t ram_dirty[RAM_PAGES];
uint8_t rom[ROM_SIZE];
hw_t hw = { .p1 = 0xffffffffu, .p2 = 0xffffffffu, .volume = 0xffffffffu, .dsw = 0xffffffffu };

static const uint16_t *selected_buffer(void)
{
    return (const uint16_t *)&ram0[(hw.vram_control & 4) ? 0x58000 : 0x30000];
}

// The picture is taken when the display reaches the start of vblank, as MAME
// draws it. The games flip buffers early in vblank and may still be drawing
// into the new one, so taking it at the end of the frame showed fighters
// missing on some frames (issue #5). Not in the state: after a load the
// selected buffer stands in until the next frame is run.
static uint16_t shown[320 * 240];
static bool shown_valid;

void video_vblank(void) { memcpy(shown, selected_buffer(), sizeof shown); shown_valid = true; }
void video_invalidate(void) { shown_valid = false; }

const uint16_t *video_base(void)
{
    return shown_valid ? shown : selected_buffer();
}

static uint32_t volume_r(void)
{
    uint32_t v = hw.volume & ~2u;
    if (sound_input_empty()) v |= 2;
    return v;
}

static uint32_t io_read32(uint32_t pa)
{
    if (pa == game.io.p1) return hw.p1;
    if (pa == game.io.p2) return hw.p2;
    if (pa == game.io.volume) return volume_r();
    if (pa == game.io.dsw) return hw.dsw;
    if (pa == game.io.unused) return 0xffffffffu;
    if (pa == 0x10000170) return ide_alt_status();
    if (pa >= 0x10000100 && pa < 0x10000140) return ide_read((pa - 0x10000100) >> 3);
    // Nothing there: reads give 0, as in MAME. KI1 v1.4 reads 32 KB past the end
    // of RAM0 when it copies the screen from the second frame buffer.
    static int logged;
    if (logged < 8) fprintf(stderr, "io: unmapped read %08x (pc %08x)%s\n", pa, cpu.cur_pc, ++logged == 8 ? "; no more shown" : "");
    return 0;
}

uint32_t io_read(uint32_t pa, int size)
{
    uint32_t v = io_read32(pa & ~3u);
    int sh = (pa & 3) * 8;
    if (size == 1) return (v >> sh) & 0xff;
    if (size == 2) return (v >> sh) & 0xffff;
    return v;
}

void io_write(uint32_t pa, uint32_t val, int size)
{
    uint32_t a = pa & ~3u;
    if (size != 4) {
        int sh = (pa & 3) * 8;
        val <<= sh;
    }
    if (a == game.io.volume) {          // sound control: data is sent on a rising bit 1
        uint32_t prev = hw.sound_control;
        hw.sound_control = val;
        if (!(prev & 2) && (val & 2)) sound_data_w((uint16_t)hw.sound_data);
        return;
    }
    if (a == game.io.p2) { hw.sound_reset = val; sound_reset_w(val & 1); return; }
    if (a == game.io.p1) { hw.vram_control = val; return; }
    if (a == game.io.unused) { hw.sound_data = val; return; }
    if (a == game.io.coin) { hw.coin_control = val; return; }
    if (a == game.io.dsw || (game.io.nop && a == game.io.nop)) return;
    switch (a) {
    case 0x10000000: return;   // written 1 at boot (board control/watchdog)
    case 0x10000178: return;   // IDE cs1 reg 7 (drive address), unused
    case 0x10000170: ide_devctl(val); return;
    }
    if (a >= 0x10000100 && a < 0x10000140) { ide_write((a - 0x10000100) >> 3, val); return; }
    static int logged;
    if (logged < 8) fprintf(stderr, "io: unmapped write %08x = %08x (pc %08x)%s\n", pa, val, cpu.cur_pc, ++logged == 8 ? "; no more shown" : "");
}
