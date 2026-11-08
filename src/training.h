// Training mode: health refill, timer freeze, and a recordable player 2 dummy.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Rewrite this frame's inputs (bits per BTN_*) for dummy recording/playback.
void training_inputs(uint32_t held[2]);
// Apply health/timer changes to game RAM; call before every emulated frame.
// `real` is false for run-ahead frames, which must not advance any counters.
void training_apply(bool real);

void training_record_toggle(void);   // start/stop recording the dummy
void training_play_toggle(void);
const char *training_status(void);   // short label for the screen, or NULL
int  game_health(int player);        // current health bar, 0-120, or -1 outside a fight
bool game_player_is_human(int player); // a person controls this fighter in a fight
