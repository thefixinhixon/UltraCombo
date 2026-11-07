// UltraCombo: Killer Instinct (MAME kinst). RAM locations not found yet are
// 0, which turns the features using them off.
#include "game.h"

const game_t game_ki1 = {
    .id = "ki1",
    .title = "Killer Instinct",
    .product = "UltraCombo",
    .icon_text = "UC",

    .versions = { { "1.5d", "ki-l15d.u98", 0x7b65ca3d }, { "1.4", "ki-l14.u98", 0xafedb75f } },
    .sound_roms = { "u10-l1", "u11-l1", "u12-l1", "u13-l1",
                    "u33-l1", "u34-l1", "u35-l1", "u36-l1" },
    .sound_crc = { 0xb6cc155f, 0x0b5e05df, 0xd05ce6ad, 0x7d0954ea,
                   0x8bbe4f0c, 0xb2e73603, 0x0aaef4fc, 0x0577bb60 },
    // the boot loader page, the sound OS it reboots into (bank 1), and the
    // two programs the OS copies into external RAM
    .sound_code = { { 0x0000, 0x0000, 0, true }, { 0x1000, 0x0000, 0, true },
                    { 0x0800, 0x0800, 0x300 }, { 0x2000, 0x0800, 0x400 } },
    .set = "kinst",
    .disk = "kinst.raw",
    .chd = "kinst.chd",
    .chd_sha1 = "81d833236e994528d1482979261401b198d1ca53",
    .cyls = 419, .heads = 13, .secs = 47,
    .serial_offset = 27,
    .io = { .p1 = 0x10000080, .p2 = 0x10000088, .volume = 0x10000090,
            .unused = 0x10000098, .dsw = 0x100000a0, .coin = 0x100000b0 },

    .snapshot_frames = 900,
    .input_lag = 0,             // measured: the next frame already shows a press
    .has_blood_colour = true,

    .health_p1 = 0x8bc55, .health_step = 0x100, .health_max = 120,   // each of the two bars
    .bars_p1 = 0x8bc8f,         // found with KI2_POKE: preset to 1, emptying the bar ends the match (v1.5d, v1.4)
    .timer_tick = 0x884f8, .timer_ones = 0x884f9, .timer_tens = 0x884fa,
    .vars = 0x880000, .vars_len = 0x10000,

    // found by changing the difficulty in the game's operator menu (both versions)
    .adj_data = 0x1a00, .adj_len = 512, .adj_difficulty = 0x1a18,
    .adj_headers = { 0x1800, 0x2400 },
};
