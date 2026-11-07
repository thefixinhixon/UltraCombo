// Training mode (see training.h). RAM locations come from the game profile
// (game.h); each value is sanity-checked before use, so other code versions
// are left alone, and a game without known locations gets no training mode.
#include "training.h"
#include "config.h"
#include "mem.h"
#include "osd.h"
#include "game.h"
#include <string.h>

#define HEALTH_P1   game.health_p1
#define HEALTH_STEP game.health_step
#define HEALTH_MAX  game.health_max
#define TIMER_TICK  game.timer_tick     // frames until the clock ticks
#define TIMER_ONES  game.timer_ones
#define TIMER_TENS  game.timer_tens
#define REFILL_DELAY 45         // frames without damage before health refills

static void poke8(uint32_t off, uint8_t v)
{
    if (ram2[off] == v) return;
    ram2[off] = v;
    ram_touch(&ram2[off]);      // keep save states and rollback exact
}

static struct { uint8_t last; int still; } hp[2];

void training_apply(bool real)
{
    if (!cfg.training || !game.health_p1) return;
    // the clock: only touch it while it shows plausible digits
    if (ram2[TIMER_ONES] <= 9 && ram2[TIMER_TENS] <= 9 && ram2[TIMER_TICK] < 0x50) poke8(TIMER_TICK, 0x40);
    for (int p = 0; p < 2; p++) {
        uint32_t off = HEALTH_P1 + (uint32_t)p * HEALTH_STEP;
        uint8_t h = ram2[off];
        if (h > HEALTH_MAX) continue;
        if (real) {
            if (h == hp[p].last) hp[p].still++;
            else { hp[p].last = h; hp[p].still = 0; }
        }
        // refill once a combo is over; a KO (0) is left to the game
        if (cfg.training_refill && h && h < HEALTH_MAX && hp[p].still >= REFILL_DELAY) {
            poke8(off, HEALTH_MAX);
            if (real) hp[p].last = HEALTH_MAX;
        }
    }
}

#define REC_MAX (60 * 10)
static uint32_t rec[REC_MAX];
static int rec_len, rec_pos;
static enum { DUMMY_OFF, DUMMY_RECORD, DUMMY_PLAY } mode;

void training_record_toggle(void)
{
    if (mode == DUMMY_RECORD) {
        mode = DUMMY_OFF;
        osd_message("Dummy recorded: %.1f s (F9 plays it)", rec_len / 60.0);
        return;
    }
    mode = DUMMY_RECORD;
    rec_len = 0;
    osd_message("Recording: you control player 2 (F8 stops)");
}

void training_play_toggle(void)
{
    if (mode == DUMMY_PLAY) { mode = DUMMY_OFF; osd_message("Dummy stopped"); return; }
    if (!rec_len) { osd_message("Nothing recorded yet (F8)"); return; }
    mode = DUMMY_PLAY;
    rec_pos = 0;
    osd_message("Dummy playing (F9 stops)");
}

void training_inputs(uint32_t held[2])
{
    const uint32_t mask = (1u << BTN_START) - 1;     // directions and attacks only
    if (mode == DUMMY_RECORD) {
        rec[rec_len++] = held[0] & mask;
        held[1] = (held[1] & ~mask) | (held[0] & mask);
        held[0] &= ~mask;
        if (rec_len == REC_MAX) training_record_toggle();
    } else if (mode == DUMMY_PLAY) {
        held[1] |= rec[rec_pos];
        rec_pos = (rec_pos + 1) % rec_len;
    }
}

const char *training_status(void)
{
    if (mode == DUMMY_RECORD) return "REC DUMMY";
    if (mode == DUMMY_PLAY) return cfg.training ? "TRAINING  DUMMY" : "DUMMY";
    return cfg.training ? "TRAINING" : NULL;
}

int game_health(int player)
{
    if (!game.health_p1) return -1;
    uint8_t h = ram2[HEALTH_P1 + (uint32_t)player * HEALTH_STEP];
    bool fight = ram2[TIMER_ONES] <= 9 && ram2[TIMER_TENS] <= 9;
    return fight && h <= HEALTH_MAX ? h : -1;
}
