// What differs between the games UltraCombo plays. Both run on the same
// board (MAME's kinst driver). The program holds both profiles (game_ki1.c,
// game_ki2.c) and picks one at startup; `game` is the one in use.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *label;          // as shown to the player, e.g. "1.5d"
    const char *rom;            // boot ROM file name
    uint32_t crc;               // CRC32 of the good dump (MAME's)
} game_version_t;

typedef struct {
    const char *id;             // "ki1" / "ki2"
    const char *title;          // the game it plays, e.g. "Killer Instinct 2"
    const char *product;        // window and dialog titles, e.g. "UltraCombo 2"
    const char *icon_text;      // up to 3 characters; a final digit is drawn glowing

    game_version_t versions[4]; // [0] is the default; label NULL ends the list
    const char *sound_roms[8];  // U10-U13, U33-U36
    uint32_t sound_crc[8];
    // Sound DSP program images to translate (adsprec.c): where U10 holds them
    // (byte offset, 4 bytes per word), the program address they run at and
    // their length; boot: a boot page, its length in its header. Found by
    // logging what the DSP runs; anything else it runs is interpreted.
    struct { uint32_t rom; uint16_t pm, words; bool boot; } sound_code[6];   // words 0, !boot: end
    const char *set;            // MAME's name for the game (its CHD sits in a folder of that name)
    const char *disk;           // raw disk image file name
    const char *chd;            // or the MAME CHD itself
    const char *chd_sha1;       // the CHD's SHA1 as chdman shows it (MAME's)

    // drive geometry (from the CHD metadata) and the identify word where the
    // game checks the drive model
    uint32_t cyls, heads, secs;
    int serial_offset;

    // I/O ports (physical addresses). Each write port is paired with a read
    // port the same way on both boards: P1/video control, P2/sound reset,
    // volume/sound control, "unused"/sound data.
    struct { uint32_t p1, p2, volume, unused, dsw, coin, nop; } io;   // nop: written, ignored

    int snapshot_frames;        // the game has loaded all of its code by then
    int input_lag;              // frames the game itself waits before acting on input
    bool has_blood_colour;      // DIP switch 6: red or white blood (KI1)

    // Game RAM locations, as offsets into ram2; 0 = not known for this game,
    // and the features using them are off.
    uint32_t credits;           // credit count (32-bit)
    int credits_needed;         // credits to start with the default coinage
    uint32_t health_p1;         // current health bar, one byte per player: the whole part of
                                // an 8.8 value, its fraction in the byte before...
    uint32_t health_step;       // ...this far apart
    int health_max;
    uint32_t bars_p1;           // bars a fighter has lost (0, or 1 on the last), same step as health
    uint32_t timer_tick, timer_ones, timer_tens;   // round clock
    // What a combo breaker earns (KI1: the spark on the life bar, KI2: the
    // super meter), same step as health: training mode keeps it at least at
    // power_full. power_name names the menu row.
    uint32_t power_p1;
    int power_full;
    const char *power_name;
    uint32_t vars, vars_len;    // range of game variables for netplay's quick check

    // Operator adjustments on the disk: adj_len bytes at adj_data, the first
    // set once they were edited, with the game's checksum of them at adj_sum.
    // adj_len 0 = not known. KI1 keeps that checksum in two header sectors
    // instead (at +4), each with a generation count (+0, +0x1fc) and a
    // checksum of its own first 0x1f8 bytes (+0x1f8).
    uint32_t adj_sum, adj_data, adj_len, adj_difficulty;
    uint32_t adj_headers[2];
} game_t;

// The program as a whole: its name and its data folder.
#define PRODUCT "UltraCombo"

extern game_t game;                          // the profile in use (game_select)
extern const game_t game_ki1, game_ki2;
extern const game_t *const game_profiles[]; // NULL-terminated
void game_select(const game_t *g);
const game_t *game_find(const char *id);    // by id ("ki1", "ki2"), NULL if unknown
