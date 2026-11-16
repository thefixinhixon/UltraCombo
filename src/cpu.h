// R4600 (MIPS III, little-endian) CPU state and interpreter interface.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rec_abi.h"     // cpu_t, tlb_entry_t

enum {
    CP0_INDEX = 0, CP0_RANDOM, CP0_ENTRYLO0, CP0_ENTRYLO1, CP0_CONTEXT,
    CP0_PAGEMASK, CP0_WIRED, CP0_BADVADDR = 8, CP0_COUNT, CP0_ENTRYHI,
    CP0_COMPARE, CP0_STATUS, CP0_CAUSE, CP0_EPC, CP0_PRID, CP0_CONFIG,
    CP0_LLADDR, CP0_WATCHLO, CP0_WATCHHI, CP0_XCONTEXT = 20,
    CP0_ECC = 26, CP0_CACHEERR, CP0_TAGLO, CP0_TAGHI, CP0_ERROREPC,
};


enum {
    EXC_INT = 0, EXC_MOD, EXC_TLBL, EXC_TLBS, EXC_ADEL, EXC_ADES, EXC_IBE,
    EXC_DBE, EXC_SYS, EXC_BP, EXC_RI, EXC_CPU, EXC_OV, EXC_TR, EXC_FPE = 15,
};

extern cpu_t cpu;

void cpu_reset(void);
// Run until at least `n` instructions have executed. Returns instructions run.
uint64_t cpu_run(uint64_t n);
void cpu_set_irq(int line, bool state);
// Advance time without executing (used to skip recognised idle loops).
void cpu_skip(uint64_t n);
// Set when the CPU is spinning on an interrupt line that only changes on a
// scanline boundary; cpu_run returns early and the caller may skip ahead.
// cpu_idle_slice: waiting on the disk, which changes between slices, so
// skip only to the end of the current slice.
extern bool cpu_idle, cpu_idle_slice;
void cpu_exception(int code, uint32_t badvaddr, bool has_badvaddr);
size_t cpu_state(uint8_t *buf, bool load);     // save state; NULL buf: size only

// Debug hooks
extern bool cpu_trace;
extern uint64_t cpu_interp_count;    // instructions run by the interpreter, not recompiled code
extern uint32_t *cpu_interp_hist;    // optional: per-word counts for 0x88000000-0x8803ffff
const char *cpu_disasm(uint32_t pc, uint32_t op, char *buf);
