// UltraCombo 2: Killer Instinct 2 (MAME kinst2). RAM locations are for the
// L1.4 code and were found by diffing deterministic runs.
#include "game.h"

const game_t game_ki2 = {
    .id = "ki2",
    .title = "Killer Instinct 2",
    .product = "UltraCombo 2",
    .icon_text = "UC2",

    .versions = { { "1.4", "ki2-l14.u98", 0x27d0285e } },
    .sound_roms = { "ki2_l1.u10", "ki2_l1.u11", "ki2_l1.u12", "ki2_l1.u13",
                    "ki2_l1.u33", "ki2_l1.u34", "ki2_l1.u35", "ki2_l1.u36" },
    .sound_crc = { 0xfdf6ed51, 0xf9e70024, 0x2994c199, 0x3fe6327b,
                   0x6f4dcdcf, 0x5db48206, 0x7245ce69, 0x8920acbb },
    // the boot loader page, the sound OS it reboots into (bank 1), and the
    // two programs the OS copies into external RAM
    .sound_code = { { 0x0000, 0x0000, 0, true }, { 0x1000, 0x0000, 0, true },
                    { 0x0800, 0x0800, 0x300 }, { 0x2000, 0x0800, 0x400 } },
    .set = "kinst2",
    .disk = "kinst2.raw",
    .chd = "kinst2.chd",
    .chd_sha1 = "e7c9291b4648eae0012ea0cc230731ed4987d1d5",
    .cyls = 1463, .heads = 13, .secs = 47,
    .serial_offset = 11,
    .io = { .p1 = 0x10000098, .p2 = 0x10000090, .volume = 0x10000080,
            .unused = 0x100000a0, .dsw = 0x10000088, .coin = 0x100000b8, .nop = 0x100000b0 },

    .snapshot_frames = 900,
    .input_lag = 1,

    .credits = 0x7ff124, .credits_needed = 2,
    .health_p1 = 0x7fc105, .health_step = 0x100, .health_max = 120,
    .fighter_p1 = 0x7fc0b0,
    .bars_p1 = 0x7fc13f,        // found with KI2_POKE: preset to 1, emptying the bar ends the match
    .power_p1 = 0x7fbeb5, .power_full = 192, .power_name = "Super meter full",   // the game caps it at 192; 0x7fbeb4 is the bar shown, catching up
    .timer_tick = 0x7f7a10, .timer_ones = 0x7f7a11, .timer_tens = 0x7f7a12,
    .vars = 0x7f0000, .vars_len = 0x10000,

    // checksum of the 88 bytes after it, computed by the game at 0x8802d400;
    // +4 is set once the adjustments were edited
    .adj_sum = 0xa00, .adj_data = 0xa04, .adj_len = 88, .adj_difficulty = 0xa1c,
};
