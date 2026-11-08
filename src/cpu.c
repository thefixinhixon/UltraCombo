// R4600 interpreter: MIPS III integer core, COP0 (with TLB), COP1 FPU.
// Little-endian, 32-bit kernel addressing (KSEG0/KSEG1 direct mapped).
#include "cpu.h"
#include "mem.h"
#include "rec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cpu_t cpu;
bool cpu_trace;
bool cpu_idle;

#define RS ((op >> 21) & 31)
#define RT ((op >> 16) & 31)
#define RD ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define IMM ((int64_t)(int16_t)op)
#define UIMM ((uint64_t)(uint16_t)op)
#define SX32(x) ((uint64_t)(int64_t)(int32_t)(x))

#define R(n) cpu.r[n]
#define WR(n, v) do { uint64_t _v = (v); if (n) cpu.r[n] = _v; } while (0)

// Thrown-exception mechanism: memory helpers set this and the step aborts.
static bool exc_raised;
bool irq_dirty = true;          // recheck pending interrupts before the next instruction
static bool tlb_refill;

// Coverage (KI2_COVER=file): one bit per executed RAM word, plus a count of
// stores that hit words already executed (self-modifying code / overlays).
static uint8_t *cover, *cover_lead;   // executed words; words reached by a jump
static uint64_t cover_smc;
#define COVER_IDX(pa) ((pa) < 0x08000000u ? (pa) >> 2 : (((pa) - 0x08000000u) + RAM0_SIZE) >> 2)
static inline bool cover_hit(uint32_t pa) { uint32_t i = COVER_IDX(pa); return cover[i >> 3] >> (i & 7) & 1; }

static void cover_save(void)
{
    FILE *f = fopen(getenv("KI2_COVER"), "wb");
    if (!f) return;
    fwrite(cover, 1, (RAM0_SIZE + RAM2_SIZE) / 32, f);
    fwrite(cover_lead, 1, (RAM0_SIZE + RAM2_SIZE) / 32, f);
    fclose(f);
    fprintf(stderr, "cover: %llu stores into executed code\n", (unsigned long long)cover_smc);
}


// Recognise "1: mfc0 rX,Cause; nop; andi rX,rX,IPmask; beq/bne rX,zero,1b; <slot>"
// i.e. polling an external interrupt line without doing anything else. The
// delay slot may be a nop or "addiu rY,rY,imm" (an idle counter), which
// cpu_skip keeps up to date.
typedef struct { uint32_t pc; int8_t cnt_reg; int16_t cnt_imm; } poll_loop_t;
static poll_loop_t idle_loop;   // loop that set cpu_idle

static poll_loop_t poll_yes[16];
static uint32_t poll_no[64];
static int poll_nyes, poll_nno;     // cleared at reset: decisions depend on RAM contents

static bool is_irq_poll_loop(uint32_t pc, uint32_t op)
{
    poll_loop_t *yes = poll_yes;
    uint32_t *no = poll_no;
    int nyes = poll_nyes, nno = poll_nno;
    for (int i = 0; i < nyes; i++) if (yes[i].pc == pc) { idle_loop = yes[i]; return true; }
    for (int i = 0; i < nno && i < 64; i++) if (no[i] == pc) return false;

    bool match = false;
    poll_loop_t l = { pc, -1, 0 };
    uint8_t *p = mem_ptr(pc & 0x1fffffffu);
    if (p && (pc & 0xe0000000u) == 0x80000000u) {
        uint32_t w[5];
        memcpy(w, p, sizeof w);
        int rt = (op >> 16) & 31;
        match = w[1] == 0 &&
                (w[2] >> 26) == 0x0c && ((w[2] >> 21) & 31) == rt && ((w[2] >> 16) & 31) == rt &&
                (w[2] & 0xffff) && !(w[2] & 0x80ff) &&                       // IP2..IP6 only
                ((w[3] >> 26) == 0x04 || (w[3] >> 26) == 0x05) &&
                ((w[3] >> 21) & 31) == rt && ((w[3] >> 16) & 31) == 0 &&
                (int16_t)w[3] == -4;
        if (match && w[4] != 0) {
            int ry = (w[4] >> 16) & 31;
            if ((w[4] >> 26) == 0x09 && ((w[4] >> 21) & 31) == ry && ry != rt && ry != 0) {
                l.cnt_reg = (int8_t)ry;
                l.cnt_imm = (int16_t)w[4];
            } else match = false;
        }
    }
    if (match) { if (nyes < 16) yes[nyes++] = l; idle_loop = l; }
    else no[nno++ % 64] = pc;
    poll_nyes = nyes; poll_nno = nno;
    return match;
}

/* ---------------------------------------------------------------- */
/* Exceptions and interrupts                                        */
/* ---------------------------------------------------------------- */

void cpu_exception(int code, uint32_t badvaddr, bool has_badvaddr)
{
    uint32_t sr = (uint32_t)cpu.cp0[CP0_STATUS];
    uint32_t cause = (uint32_t)cpu.cp0[CP0_CAUSE];
    uint32_t vec_off = 0x180;

    if (has_badvaddr) {
        cpu.cp0[CP0_BADVADDR] = SX32(badvaddr);
        if (code == EXC_TLBL || code == EXC_TLBS || code == EXC_MOD) {
            cpu.cp0[CP0_CONTEXT] = (cpu.cp0[CP0_CONTEXT] & ~0x7ffff0ull) | ((badvaddr >> 9) & 0x7ffff0);
            cpu.cp0[CP0_ENTRYHI] = (cpu.cp0[CP0_ENTRYHI] & 0xff) | SX32(badvaddr & 0xffffe000u);
        }
    }

    if (!(sr & SR_EXL)) {
        uint32_t epc = cpu.cur_pc;
        if (cpu.in_delay) { epc -= 4; cause |= 0x80000000u; }
        else cause &= ~0x80000000u;
        cpu.cp0[CP0_EPC] = SX32(epc);
        // TLB refill uses offset 0 only when EXL was clear
        if (tlb_refill) vec_off = 0x000;
    }
    tlb_refill = false;
    cause = (cause & ~0x7cu) | (code << 2);
    cpu.cp0[CP0_CAUSE] = cause;
    cpu.cp0[CP0_STATUS] = sr | SR_EXL;

    static int log_exc = -1;
    if (log_exc < 0) log_exc = getenv("KI2_LOG_EXC") != NULL;
    if (log_exc && code != EXC_INT)
        fprintf(stderr, "exc %d at %08x%s badva=%08x sr=%08x\n", code, cpu.cur_pc, cpu.in_delay ? " (delay)" : "", badvaddr, sr);

    if (code >= EXC_MOD && code <= EXC_DBE && getenv("KI2_DUMP_ON_EXC")) {
        FILE *f = fopen("ram0.bin", "wb"); fwrite(ram0, 1, RAM0_SIZE, f); fclose(f);
        f = fopen("ram2.bin", "wb"); fwrite(ram2, 1, RAM2_SIZE, f); fclose(f);
        for (int i = 0; i < 32; i++) fprintf(stderr, "r%-2d=%016llx%s", i, (unsigned long long)cpu.r[i], (i & 3) == 3 ? "\n" : " ");
        fprintf(stderr, "epc=%08x ra=%08x\n", cpu.cur_pc, (uint32_t)cpu.r[31]);
        exit(2);
    }

    uint32_t base = (sr & SR_BEV) ? 0xbfc00200u : 0x80000000u;
    cpu.pc = base + vec_off;
    cpu.npc = cpu.pc + 4;
    cpu.in_delay = cpu.next_delay = false;
    exc_raised = true;
}

static inline void update_irq_cause(void)
{
    uint32_t cause = (uint32_t)cpu.cp0[CP0_CAUSE];
    cause = (cause & ~0x7c00u) | ((cpu.ext_irq & 0x1f) << 10);
    cpu.cp0[CP0_CAUSE] = cause;
    irq_dirty = true;
}

void cpu_set_irq(int line, bool state)
{
    if (state) cpu.ext_irq |= 1u << line;
    else cpu.ext_irq &= ~(1u << line);
    update_irq_cause();
}

static inline bool irq_pending(void)
{
    uint32_t sr = (uint32_t)cpu.cp0[CP0_STATUS];
    if ((sr & (SR_IE | SR_EXL | SR_ERL)) != SR_IE) return false;
    return (sr & (uint32_t)cpu.cp0[CP0_CAUSE] & 0xff00u) != 0;
}

/* ---------------------------------------------------------------- */
/* Address translation                                              */
/* ---------------------------------------------------------------- */

// Translations of recently used mapped pages, refilled from the TLB on a
// miss and flushed whenever the TLB or the current ASID changes.
tlbc_t tlbc[TLBC_SIZE];

static void tlbc_flush(void) { memset(tlbc, 0, sizeof tlbc); }

static void tlbc_fill(uint32_t va, uint32_t pa, bool dirty)
{
    tlbc_t *e = &tlbc[(va >> 12) & (TLBC_SIZE - 1)];
    uint32_t page = pa & ~0xfffu;
    e->tag = (va >> 12) + 1;
    e->pa = page;
    e->dirty = dirty;
    e->rd = mem_ptr(page);
    bool ram = e->rd && page < 0x1fc00000u;
    bool code = page < rec_code_lo + rec_code_size && page + 0x1000 > rec_code_lo;
    e->wr = (dirty && ram && !code) ? e->rd : 0;
}

static bool tlb_lookup(uint32_t va, bool write, uint32_t *pa)
{
    uint8_t asid = cpu.cp0[CP0_ENTRYHI] & 0xff;
    for (int i = 0; i < 48; i++) {
        tlb_entry_t *e = &cpu.tlb[i];
        uint32_t mask = (uint32_t)(e->mask | 0x1fff);
        uint32_t vpn2 = (uint32_t)e->hi & ~mask;
        if ((va & ~mask) != vpn2) continue;
        bool global = (e->lo0 & e->lo1 & 1);
        if (!global && (e->hi & 0xff) != asid) continue;
        uint32_t half = (mask + 1) >> 1;
        uint64_t lo = (va & half) ? e->lo1 : e->lo0;
        if (!(lo & 2)) { // invalid
            cpu_exception(write ? EXC_TLBS : EXC_TLBL, va, true);
            return false;
        }
        if (write && !(lo & 4)) {
            cpu_exception(EXC_MOD, va, true);
            return false;
        }
        uint32_t pfn = (uint32_t)((lo >> 6) & 0xffffff) << 12;
        *pa = pfn | (va & (half - 1));
        tlbc_fill(va, *pa, (lo & 4) != 0);
        return true;
    }
    tlb_refill = true;
    cpu_exception(write ? EXC_TLBS : EXC_TLBL, va, true);
    return false;
}

static inline bool translate(uint32_t va, bool write, uint32_t *pa)
{
    if ((va & 0xc0000000u) == 0x80000000u) { *pa = va & 0x1fffffffu; return true; }
    tlbc_t *e = &tlbc[(va >> 12) & (TLBC_SIZE - 1)];
    if (e->tag == (va >> 12) + 1 && (!write || e->dirty)) { *pa = e->pa | (va & 0xfff); return true; }
    return tlb_lookup(va, write, pa);
}

/* ---------------------------------------------------------------- */
/* Memory access                                                    */
/* ---------------------------------------------------------------- */

static uint32_t last_unmapped_report;

static void cover_note_smc(uint32_t pa)
{
    if (cover_smc++ < 20) fprintf(stderr, "cover: store to executed code pa=%08x pc=%08x\n", pa, cpu.cur_pc);
    uint32_t i = COVER_IDX(pa);
    cover[i >> 3] &= ~(1u << (i & 7));
}

// The boot EPROM is slow: each read waits for its RdRdy (MAME's figure,
// kinst.cpp). Without the wait the blue boot screen is over too soon, before
// the sound board is ready, and the attract mode plays without its music
// (issue #11).
#define ROM_WAIT_CYCLES 128

#define DEF_LOAD(name, type, size)                                          \
    static inline type name(uint32_t va)                                    \
    {                                                                       \
        uint32_t pa;                                                        \
        if (va & (size - 1)) { cpu_exception(EXC_ADEL, va, true); return 0; } \
        if (!translate(va, false, &pa)) return 0;                           \
        uint8_t *p = mem_ptr(pa);                                           \
        if (p) {                                                            \
            if (pa >= 0x1fc00000u) cpu.cycles += ROM_WAIT_CYCLES;           \
            type v; memcpy(&v, p, size); return v;                          \
        }                                                                   \
        if (size == 8) {                                                    \
            uint64_t lo = io_read(pa, 4), hi = io_read(pa + 4, 4);          \
            return (type)(lo | (hi << 32));                                 \
        }                                                                   \
        return (type)io_read(pa, size);                                     \
    }

#define DEF_STORE(name, type, size)                                         \
    static inline void name(uint32_t va, type v)                            \
    {                                                                       \
        uint32_t pa;                                                        \
        if (va & (size - 1)) { cpu_exception(EXC_ADES, va, true); return; } \
        if (!translate(va, true, &pa)) return;                              \
        uint8_t *p = mem_ptr(pa);                                           \
        if (p) {                                                            \
            if (pa >= 0x1fc00000u) return; /* ROM */                        \
            if (cover && cover_hit(pa)) cover_note_smc(pa);                 \
            if (pa - rec_code_lo < rec_code_size) rec_code_gen++;           \
            memcpy(p, &v, size); ram_touch(p); return;                      \
        }                                                                   \
        if (size == 8) {                                                    \
            io_write(pa, (uint32_t)v, 4); io_write(pa + 4, (uint32_t)((uint64_t)v >> 32), 4); \
            return;                                                         \
        }                                                                   \
        io_write(pa, (uint32_t)v, size);                                    \
    }

DEF_LOAD(rd8, uint8_t, 1)
DEF_LOAD(rd16, uint16_t, 2)
DEF_LOAD(rd32, uint32_t, 4)
DEF_LOAD(rd64, uint64_t, 8)
DEF_STORE(wr8, uint8_t, 1)
DEF_STORE(wr16, uint16_t, 2)
DEF_STORE(wr32, uint32_t, 4)
DEF_STORE(wr64, uint64_t, 8)

static inline uint32_t fetch(uint32_t va)
{
    uint32_t pa;
    if (va & 3) { cpu_exception(EXC_ADEL, va, true); return 0; }
    if (!translate(va, false, &pa)) return 0;
    uint8_t *p = mem_ptr(pa);
    if (!p) {
        if (pa != last_unmapped_report) {
            fprintf(stderr, "fetch from unmapped pa %08x (va %08x)\n", pa, va);
            last_unmapped_report = pa;
        }
        cpu_exception(EXC_IBE, va, false);
        return 0;
    }
    uint32_t v; memcpy(&v, p, 4); return v;
}

/* ---------------------------------------------------------------- */
/* COP0                                                             */
/* ---------------------------------------------------------------- */

static inline uint32_t count_now(void)
{
    return cpu.count_base + (uint32_t)((cpu.cycles - cpu.count_cyc) / 2);
}

static void schedule_compare(void)
{
    uint32_t delta = (uint32_t)cpu.cp0[CP0_COMPARE] - cpu.count_base;
    uint64_t at = cpu.count_cyc + 2 * (uint64_t)delta;
    while (at <= cpu.cycles) at += 2ull << 32;
    cpu.next_compare = at;
}

static inline void compare_check(void)
{
    if (cpu.cycles >= cpu.next_compare) {
        cpu.cp0[CP0_CAUSE] |= 0x8000;  // IP7 timer
        cpu.next_compare += 2ull << 32;
        irq_dirty = true;
    }
}

static uint64_t cp0_read(int reg)
{
    switch (reg) {
    case CP0_RANDOM: {
        uint32_t wired = cpu.cp0[CP0_WIRED] & 63;
        uint32_t range = 48 - wired;
        return wired + (uint32_t)(cpu.cycles % (range ? range : 1));
    }
    case CP0_COUNT: return count_now();
    default: return cpu.cp0[reg];
    }
}

static void cp0_write(int reg, uint64_t v)
{
    switch (reg) {
    case CP0_INDEX: cpu.cp0[reg] = (cpu.cp0[reg] & 0x80000000u) | (v & 63); break;
    case CP0_RANDOM: case CP0_PRID: case CP0_BADVADDR: break;
    case CP0_ENTRYLO0: case CP0_ENTRYLO1: cpu.cp0[reg] = v & 0x3fffffff; break;
    case CP0_PAGEMASK: cpu.cp0[reg] = v & 0x01ffe000; break;
    case CP0_WIRED: cpu.cp0[reg] = v & 63; break;
    case CP0_COUNT: cpu.count_base = (uint32_t)v; cpu.count_cyc = cpu.cycles; schedule_compare(); break;
    case CP0_COMPARE:
        cpu.count_base = count_now(); cpu.count_cyc = cpu.cycles;
        cpu.cp0[reg] = (uint32_t)v;
        cpu.cp0[CP0_CAUSE] &= ~0x8000ull;
        schedule_compare();
        break;
    case CP0_CAUSE:
        cpu.cp0[reg] = (cpu.cp0[reg] & ~0x300ull) | (v & 0x300);
        irq_dirty = true;
        break;
    case CP0_STATUS: cpu.cp0[reg] = (uint32_t)v; irq_dirty = true; break;
    case CP0_ENTRYHI:
        if ((cpu.cp0[reg] ^ v) & 0xff) tlbc_flush();
        cpu.cp0[reg] = v;
        break;
    case CP0_CONFIG: cpu.cp0[reg] = (cpu.cp0[reg] & ~7ull) | (v & 7); break;
    default: cpu.cp0[reg] = v; break;
    }
}

static void tlb_write(int idx)
{
    tlbc_flush();
    tlb_entry_t *e = &cpu.tlb[idx % 48];
    e->mask = cpu.cp0[CP0_PAGEMASK];
    e->hi = cpu.cp0[CP0_ENTRYHI] & ~(e->mask | 0x1fffull) | (cpu.cp0[CP0_ENTRYHI] & 0xff);
    e->lo0 = cpu.cp0[CP0_ENTRYLO0];
    e->lo1 = cpu.cp0[CP0_ENTRYLO1];
}

static void exec_cop0(uint32_t op)
{
    if ((cpu.cp0[CP0_STATUS] & SR_KSU) && !(cpu.cp0[CP0_STATUS] & (SR_EXL | SR_ERL)) && !(cpu.cp0[CP0_STATUS] & 0x10000000u)) {
        cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull);
        cpu_exception(EXC_CPU, 0, false);
        return;
    }
    switch (RS) {
    case 0x00:                                                // MFC0
        if (RD == CP0_CAUSE && !cpu.in_delay && is_irq_poll_loop(cpu.cur_pc, op)) cpu_idle = true;
        WR(RT, SX32(cp0_read(RD)));
        break;
    case 0x01: WR(RT, cp0_read(RD)); break;                   // DMFC0
    case 0x04: cp0_write(RD, SX32(R(RT))); break;             // MTC0
    case 0x05: cp0_write(RD, R(RT)); break;                   // DMTC0
    case 0x10 ... 0x1f:
        switch (op & 63) {
        case 0x01: { // TLBR
            tlb_entry_t *e = &cpu.tlb[cpu.cp0[CP0_INDEX] % 48];
            cpu.cp0[CP0_PAGEMASK] = e->mask;
            cpu.cp0[CP0_ENTRYHI] = e->hi;
            cpu.cp0[CP0_ENTRYLO0] = e->lo0 & ~1ull;
            cpu.cp0[CP0_ENTRYLO1] = e->lo1 & ~1ull;
            if (e->lo0 & e->lo1 & 1) { cpu.cp0[CP0_ENTRYLO0] |= 1; cpu.cp0[CP0_ENTRYLO1] |= 1; }
            break;
        }
        case 0x02: tlb_write((int)(cpu.cp0[CP0_INDEX] & 63)); break;   // TLBWI
        case 0x06: tlb_write((int)cp0_read(CP0_RANDOM)); break;        // TLBWR
        case 0x08: { // TLBP
            uint64_t hi = cpu.cp0[CP0_ENTRYHI];
            cpu.cp0[CP0_INDEX] = 0x80000000u;
            for (int i = 0; i < 48; i++) {
                tlb_entry_t *e = &cpu.tlb[i];
                uint64_t mask = ~(e->mask | 0x1fffull) & 0xffffffffull;
                if (((e->hi ^ hi) & mask) == 0 && ((e->lo0 & e->lo1 & 1) || ((e->hi ^ hi) & 0xff) == 0)) {
                    cpu.cp0[CP0_INDEX] = i;
                    break;
                }
            }
            break;
        }
        case 0x18: { // ERET
            uint32_t sr = (uint32_t)cpu.cp0[CP0_STATUS];
            if (sr & SR_ERL) { cpu.pc = (uint32_t)cpu.cp0[CP0_ERROREPC]; sr &= ~SR_ERL; }
            else { cpu.pc = (uint32_t)cpu.cp0[CP0_EPC]; sr &= ~SR_EXL; }
            cpu.cp0[CP0_STATUS] = sr;
            irq_dirty = true;
            cpu.npc = cpu.pc + 4;
            cpu.next_delay = false;
            cpu.llbit = false;
            break;
        }
        case 0x20: break; // WAIT (not on R4600, treat as nop)
        default: cpu_exception(EXC_RI, 0, false); break;
        }
        break;
    case 0x08: { // BC0x: treat CpCond as false
        bool cond = false;
        bool tf = (op >> 16) & 1, likely = (op >> 17) & 1;
        if (cond == tf) { cpu.npc = cpu.pc + ((uint32_t)IMM << 2); cpu.next_delay = true; }
        else if (likely) { cpu.pc = cpu.npc; cpu.npc += 4; }
        break;
    }
    default: cpu_exception(EXC_RI, 0, false); break;
    }
}

/* ---------------------------------------------------------------- */
/* COP1 (FPU)                                                       */
/* ---------------------------------------------------------------- */

static inline bool fr64(void) { return (cpu.cp0[CP0_STATUS] & SR_FR) != 0; }

static inline uint32_t fget32(int n)
{
    if (fr64()) return (uint32_t)cpu.fpr[n];
    return (n & 1) ? (uint32_t)(cpu.fpr[n & ~1] >> 32) : (uint32_t)cpu.fpr[n];
}
static inline void fset32(int n, uint32_t v)
{
    if (fr64()) { cpu.fpr[n] = (cpu.fpr[n] & 0xffffffff00000000ull) | v; return; }
    uint64_t *p = &cpu.fpr[n & ~1];
    if (n & 1) *p = (*p & 0xffffffffull) | ((uint64_t)v << 32);
    else *p = (*p & 0xffffffff00000000ull) | v;
}
static inline uint64_t fget64(int n) { return cpu.fpr[fr64() ? n : (n & ~1)]; }
static inline void fset64(int n, uint64_t v) { cpu.fpr[fr64() ? n : (n & ~1)] = v; }

static inline float fgets_(int n) { uint32_t v = fget32(n); float f; memcpy(&f, &v, 4); return f; }
static inline void fsets(int n, float f) { uint32_t v; memcpy(&v, &f, 4); fset32(n, v); }
static inline double fgetd(int n) { uint64_t v = fget64(n); double d; memcpy(&d, &v, 8); return d; }
static inline void fsetd(int n, double d) { uint64_t v; memcpy(&v, &d, 8); fset64(n, v); }

static inline double fround_mode(double x, int mode)
{
    switch (mode) {
    case 0: return nearbyint(x);  // host default is round-to-nearest-even
    case 1: return trunc(x);
    case 2: return ceil(x);
    default: return floor(x);
    }
}

static inline void set_fcc(bool c)
{
    if (c) cpu.fcr31 |= 0x00800000u; else cpu.fcr31 &= ~0x00800000u;
}

static void exec_cop1(uint32_t op)
{
    if (!(cpu.cp0[CP0_STATUS] & SR_CU1)) {
        cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | (1ull << 28);
        cpu_exception(EXC_CPU, 0, false);
        return;
    }
    int fmt = RS;
    int ft = RT, fs = RD, fd = SA;
    switch (fmt) {
    case 0x00: WR(RT, SX32(fget32(fs))); return;                 // MFC1
    case 0x01: WR(RT, fget64(fs)); return;                       // DMFC1
    case 0x02: WR(RT, SX32(fs == 31 ? cpu.fcr31 : (fs == 0 ? 0x00000a00u : 0))); return; // CFC1
    case 0x04: fset32(fs, (uint32_t)R(RT)); return;              // MTC1
    case 0x05: fset64(fs, R(RT)); return;                        // DMTC1
    case 0x06: if (fs == 31) cpu.fcr31 = (uint32_t)R(RT) & 0x0183ffffu; return; // CTC1
    case 0x08: {                                                 // BC1
        bool cond = (cpu.fcr31 >> 23) & 1;
        bool tf = (op >> 16) & 1, likely = (op >> 17) & 1;
        if (cond == tf) { cpu.npc = cpu.pc + ((uint32_t)IMM << 2); cpu.next_delay = true; }
        else if (likely) { cpu.pc = cpu.npc; cpu.npc += 4; }
        return;
    }
    }

    int rm = cpu.fcr31 & 3;
    int fn = op & 63;
    if (fmt == 0x10) { // single
        float a = fgets_(fs), b = fgets_(ft);
        switch (fn) {
        case 0x00: fsets(fd, a + b); return;
        case 0x01: fsets(fd, a - b); return;
        case 0x02: fsets(fd, a * b); return;
        case 0x03: fsets(fd, a / b); return;
        case 0x04: fsets(fd, sqrtf(a)); return;
        case 0x05: fsets(fd, fabsf(a)); return;
        case 0x06: fset32(fd, fget32(fs)); return;
        case 0x07: fsets(fd, -a); return;
        case 0x08: fset64(fd, (uint64_t)(int64_t)fround_mode(a, 0)); return;
        case 0x09: fset64(fd, (uint64_t)(int64_t)truncf(a)); return;
        case 0x0a: fset64(fd, (uint64_t)(int64_t)ceilf(a)); return;
        case 0x0b: fset64(fd, (uint64_t)(int64_t)floorf(a)); return;
        case 0x0c: fset32(fd, (uint32_t)(int32_t)fround_mode(a, 0)); return;
        case 0x0d: fset32(fd, (uint32_t)(int32_t)truncf(a)); return;
        case 0x0e: fset32(fd, (uint32_t)(int32_t)ceilf(a)); return;
        case 0x0f: fset32(fd, (uint32_t)(int32_t)floorf(a)); return;
        case 0x21: fsetd(fd, (double)a); return;
        case 0x24: fset32(fd, (uint32_t)(int32_t)fround_mode(a, rm)); return;
        case 0x25: fset64(fd, (uint64_t)(int64_t)fround_mode(a, rm)); return;
        }
        if (fn >= 0x30) {
            bool un = isnan(a) || isnan(b);
            int c = fn & 15;
            bool r = ((c & 4) && !un && a < b) || ((c & 2) && !un && a == b) || ((c & 1) && un);
            set_fcc(r);
            return;
        }
    } else if (fmt == 0x11) { // double
        double a = fgetd(fs), b = fgetd(ft);
        switch (fn) {
        case 0x00: fsetd(fd, a + b); return;
        case 0x01: fsetd(fd, a - b); return;
        case 0x02: fsetd(fd, a * b); return;
        case 0x03: fsetd(fd, a / b); return;
        case 0x04: fsetd(fd, sqrt(a)); return;
        case 0x05: fsetd(fd, fabs(a)); return;
        case 0x06: fset64(fd, fget64(fs)); return;
        case 0x07: fsetd(fd, -a); return;
        case 0x08: fset64(fd, (uint64_t)(int64_t)fround_mode(a, 0)); return;
        case 0x09: fset64(fd, (uint64_t)(int64_t)trunc(a)); return;
        case 0x0a: fset64(fd, (uint64_t)(int64_t)ceil(a)); return;
        case 0x0b: fset64(fd, (uint64_t)(int64_t)floor(a)); return;
        case 0x0c: fset32(fd, (uint32_t)(int32_t)fround_mode(a, 0)); return;
        case 0x0d: fset32(fd, (uint32_t)(int32_t)trunc(a)); return;
        case 0x0e: fset32(fd, (uint32_t)(int32_t)ceil(a)); return;
        case 0x0f: fset32(fd, (uint32_t)(int32_t)floor(a)); return;
        case 0x20: fsets(fd, (float)a); return;
        case 0x24: fset32(fd, (uint32_t)(int32_t)fround_mode(a, rm)); return;
        case 0x25: fset64(fd, (uint64_t)(int64_t)fround_mode(a, rm)); return;
        }
        if (fn >= 0x30) {
            bool un = isnan(a) || isnan(b);
            int c = fn & 15;
            bool r = ((c & 4) && !un && a < b) || ((c & 2) && !un && a == b) || ((c & 1) && un);
            set_fcc(r);
            return;
        }
    } else if (fmt == 0x14) { // word
        int32_t w = (int32_t)fget32(fs);
        if (fn == 0x20) { fsets(fd, (float)w); return; }
        if (fn == 0x21) { fsetd(fd, (double)w); return; }
    } else if (fmt == 0x15) { // long
        int64_t l = (int64_t)fget64(fs);
        if (fn == 0x20) { fsets(fd, (float)l); return; }
        if (fn == 0x21) { fsetd(fd, (double)l); return; }
    }
    fprintf(stderr, "unimplemented COP1 op %08x at %08x\n", op, cpu.pc - 4);
    cpu_exception(EXC_RI, 0, false);
}

/* ---------------------------------------------------------------- */
/* Main execution                                                   */
/* ---------------------------------------------------------------- */

static inline void branch(bool cond, uint32_t op)
{
    if (cond) { cpu.npc = cpu.pc + ((uint32_t)IMM << 2); cpu.next_delay = true; }
}
static inline void branch_likely(bool cond, uint32_t op)
{
    if (cond) { cpu.npc = cpu.pc + ((uint32_t)IMM << 2); cpu.next_delay = true; }
    else { cpu.pc = cpu.npc; cpu.npc += 4; }
}

static inline void trap_if(bool c) { if (c) cpu_exception(EXC_TR, 0, false); }

static void exec_special(uint32_t op)
{
    uint64_t rs = R(RS), rt = R(RT);
    switch (op & 63) {
    case 0x00: WR(RD, SX32((uint32_t)rt << SA)); break;                        // SLL
    case 0x02: WR(RD, SX32((uint32_t)rt >> SA)); break;                        // SRL
    case 0x03: WR(RD, SX32((int32_t)rt >> SA)); break;                         // SRA
    case 0x04: WR(RD, SX32((uint32_t)rt << (rs & 31))); break;                 // SLLV
    case 0x06: WR(RD, SX32((uint32_t)rt >> (rs & 31))); break;                 // SRLV
    case 0x07: WR(RD, SX32((int32_t)rt >> (rs & 31))); break;                  // SRAV
    case 0x08: cpu.npc = (uint32_t)rs; cpu.next_delay = true; break;           // JR
    case 0x09: WR(RD, SX32(cpu.pc + 4)); cpu.npc = (uint32_t)rs; cpu.next_delay = true; break; // JALR
    case 0x0c: cpu_exception(EXC_SYS, 0, false); break;                        // SYSCALL
    case 0x0d: cpu_exception(EXC_BP, 0, false); break;                         // BREAK
    case 0x0f: break;                                                          // SYNC
    case 0x10: WR(RD, cpu.hi); break;
    case 0x11: cpu.hi = rs; break;
    case 0x12: WR(RD, cpu.lo); break;
    case 0x13: cpu.lo = rs; break;
    case 0x14: WR(RD, rt << (rs & 63)); break;                                 // DSLLV
    case 0x16: WR(RD, rt >> (rs & 63)); break;                                 // DSRLV
    case 0x17: WR(RD, (uint64_t)((int64_t)rt >> (rs & 63))); break;            // DSRAV
    case 0x18: { int64_t p = (int64_t)(int32_t)rs * (int32_t)rt;               // MULT
        cpu.lo = SX32((uint32_t)p); cpu.hi = SX32((uint32_t)(p >> 32)); break; }
    case 0x19: { uint64_t p = (uint64_t)(uint32_t)rs * (uint32_t)rt;           // MULTU
        cpu.lo = SX32((uint32_t)p); cpu.hi = SX32((uint32_t)(p >> 32)); break; }
    case 0x1a: { int32_t a = (int32_t)rs, b = (int32_t)rt;                     // DIV
        if (b == 0) { cpu.lo = SX32(a < 0 ? 1 : 0xffffffffu); cpu.hi = SX32(a); }
        else if (a == INT32_MIN && b == -1) { cpu.lo = SX32(a); cpu.hi = 0; }
        else { cpu.lo = SX32(a / b); cpu.hi = SX32(a % b); }
        break; }
    case 0x1b: { uint32_t a = (uint32_t)rs, b = (uint32_t)rt;                  // DIVU
        if (b == 0) { cpu.lo = SX32(0xffffffffu); cpu.hi = SX32(a); }
        else { cpu.lo = SX32(a / b); cpu.hi = SX32(a % b); }
        break; }
    case 0x1c: { __int128 p = (__int128)(int64_t)rs * (int64_t)rt;             // DMULT
        cpu.lo = (uint64_t)p; cpu.hi = (uint64_t)(p >> 64); break; }
    case 0x1d: { unsigned __int128 p = (unsigned __int128)rs * rt;             // DMULTU
        cpu.lo = (uint64_t)p; cpu.hi = (uint64_t)(p >> 64); break; }
    case 0x1e: { int64_t a = (int64_t)rs, b = (int64_t)rt;                     // DDIV
        if (b == 0) { cpu.lo = a < 0 ? 1 : ~0ull; cpu.hi = a; }
        else if (a == INT64_MIN && b == -1) { cpu.lo = a; cpu.hi = 0; }
        else { cpu.lo = a / b; cpu.hi = a % b; }
        break; }
    case 0x1f:                                                                 // DDIVU
        if (rt == 0) { cpu.lo = ~0ull; cpu.hi = rs; }
        else { cpu.lo = rs / rt; cpu.hi = rs % rt; }
        break;
    case 0x20: { int32_t r;                                                    // ADD
        if (__builtin_add_overflow((int32_t)rs, (int32_t)rt, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RD, SX32(r)); break; }
    case 0x21: WR(RD, SX32((uint32_t)rs + (uint32_t)rt)); break;               // ADDU
    case 0x22: { int32_t r;                                                    // SUB
        if (__builtin_sub_overflow((int32_t)rs, (int32_t)rt, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RD, SX32(r)); break; }
    case 0x23: WR(RD, SX32((uint32_t)rs - (uint32_t)rt)); break;               // SUBU
    case 0x24: WR(RD, rs & rt); break;
    case 0x25: WR(RD, rs | rt); break;
    case 0x26: WR(RD, rs ^ rt); break;
    case 0x27: WR(RD, ~(rs | rt)); break;
    case 0x2a: WR(RD, (int64_t)rs < (int64_t)rt); break;
    case 0x2b: WR(RD, rs < rt); break;
    case 0x2c: { int64_t r;                                                    // DADD
        if (__builtin_add_overflow((int64_t)rs, (int64_t)rt, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RD, r); break; }
    case 0x2d: WR(RD, rs + rt); break;
    case 0x2e: { int64_t r;                                                    // DSUB
        if (__builtin_sub_overflow((int64_t)rs, (int64_t)rt, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RD, r); break; }
    case 0x2f: WR(RD, rs - rt); break;
    case 0x30: trap_if((int64_t)rs >= (int64_t)rt); break;
    case 0x31: trap_if(rs >= rt); break;
    case 0x32: trap_if((int64_t)rs < (int64_t)rt); break;
    case 0x33: trap_if(rs < rt); break;
    case 0x34: trap_if(rs == rt); break;
    case 0x36: trap_if(rs != rt); break;
    case 0x38: WR(RD, rt << SA); break;                                        // DSLL
    case 0x3a: WR(RD, rt >> SA); break;                                        // DSRL
    case 0x3b: WR(RD, (uint64_t)((int64_t)rt >> SA)); break;                   // DSRA
    case 0x3c: WR(RD, rt << (SA + 32)); break;                                 // DSLL32
    case 0x3e: WR(RD, rt >> (SA + 32)); break;                                 // DSRL32
    case 0x3f: WR(RD, (uint64_t)((int64_t)rt >> (SA + 32))); break;            // DSRA32
    default: cpu_exception(EXC_RI, 0, false); break;
    }
}

static void exec_regimm(uint32_t op)
{
    int64_t rs = (int64_t)R(RS);
    switch (RT) {
    case 0x00: branch(rs < 0, op); break;
    case 0x01: branch(rs >= 0, op); break;
    case 0x02: branch_likely(rs < 0, op); break;
    case 0x03: branch_likely(rs >= 0, op); break;
    case 0x08: trap_if(rs >= IMM); break;
    case 0x09: trap_if((uint64_t)rs >= (uint64_t)IMM); break;
    case 0x0a: trap_if(rs < IMM); break;
    case 0x0b: trap_if((uint64_t)rs < (uint64_t)IMM); break;
    case 0x0c: trap_if(rs == IMM); break;
    case 0x0e: trap_if(rs != IMM); break;
    case 0x10: cpu.r[31] = SX32(cpu.pc + 4); branch(rs < 0, op); break;
    case 0x11: cpu.r[31] = SX32(cpu.pc + 4); branch(rs >= 0, op); break;
    case 0x12: cpu.r[31] = SX32(cpu.pc + 4); branch_likely(rs < 0, op); break;
    case 0x13: cpu.r[31] = SX32(cpu.pc + 4); branch_likely(rs >= 0, op); break;
    default: cpu_exception(EXC_RI, 0, false); break;
    }
}

static void exec(uint32_t op)
{
    uint32_t ea;
    switch (op >> 26) {
    case 0x00: exec_special(op); break;
    case 0x01: exec_regimm(op); break;
    case 0x02: cpu.npc = (cpu.pc & 0xf0000000u) | ((op & 0x3ffffff) << 2); cpu.next_delay = true; break;
    case 0x03: cpu.r[31] = SX32(cpu.pc + 4);
               cpu.npc = (cpu.pc & 0xf0000000u) | ((op & 0x3ffffff) << 2); cpu.next_delay = true; break;
    case 0x04: branch(R(RS) == R(RT), op); break;
    case 0x05: branch(R(RS) != R(RT), op); break;
    case 0x06: branch((int64_t)R(RS) <= 0, op); break;
    case 0x07: branch((int64_t)R(RS) > 0, op); break;
    case 0x08: { int32_t r;                                                    // ADDI
        if (__builtin_add_overflow((int32_t)R(RS), (int32_t)IMM, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RT, SX32(r)); break; }
    case 0x09: WR(RT, SX32((uint32_t)R(RS) + (uint32_t)IMM)); break;
    case 0x0a: WR(RT, (int64_t)R(RS) < IMM); break;
    case 0x0b: WR(RT, R(RS) < (uint64_t)IMM); break;
    case 0x0c: WR(RT, R(RS) & UIMM); break;
    case 0x0d: WR(RT, R(RS) | UIMM); break;
    case 0x0e: WR(RT, R(RS) ^ UIMM); break;
    case 0x0f: WR(RT, SX32((uint32_t)(op << 16))); break;
    case 0x10: exec_cop0(op); break;
    case 0x11: exec_cop1(op); break;
    case 0x12: case 0x13:
        cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | ((uint64_t)((op >> 26) & 3) << 28);
        cpu_exception(EXC_CPU, 0, false); break;
    case 0x14: branch_likely(R(RS) == R(RT), op); break;
    case 0x15: branch_likely(R(RS) != R(RT), op); break;
    case 0x16: branch_likely((int64_t)R(RS) <= 0, op); break;
    case 0x17: branch_likely((int64_t)R(RS) > 0, op); break;
    case 0x18: { int64_t r;                                                    // DADDI
        if (__builtin_add_overflow((int64_t)R(RS), IMM, &r)) cpu_exception(EXC_OV, 0, false);
        else WR(RT, r); break; }
    case 0x19: WR(RT, R(RS) + IMM); break;
    case 0x1a: case 0x1b: { // LDL / LDR
        ea = (uint32_t)(R(RS) + IMM);
        uint64_t w = rd64(ea & ~7u); if (exc_raised) break;
        int b = ea & 7;
        uint64_t rt = R(RT);
        if ((op >> 26) == 0x1a) { // LDL
            int sh = 8 * (7 - b);
            uint64_t keep = sh ? ((1ull << sh) - 1) : 0;
            WR(RT, (rt & keep) | (w << sh));
        } else {                  // LDR
            int sh = 8 * b;
            uint64_t keep = sh ? ~(~0ull >> sh) : 0;
            WR(RT, (rt & keep) | (w >> sh));
        }
        break;
    }
    case 0x20: ea = (uint32_t)(R(RS) + IMM); { uint8_t v = rd8(ea); if (!exc_raised) WR(RT, (uint64_t)(int8_t)v); } break;
    case 0x21: ea = (uint32_t)(R(RS) + IMM); { uint16_t v = rd16(ea); if (!exc_raised) WR(RT, (uint64_t)(int16_t)v); } break;
    case 0x22: { // LWL
        ea = (uint32_t)(R(RS) + IMM);
        uint32_t w = rd32(ea & ~3u); if (exc_raised) break;
        int sh = 8 * (3 - (ea & 3));
        uint32_t keep = sh ? ((1u << sh) - 1) : 0;
        WR(RT, SX32(((uint32_t)R(RT) & keep) | (w << sh)));
        break;
    }
    case 0x23: ea = (uint32_t)(R(RS) + IMM); { uint32_t v = rd32(ea); if (!exc_raised) WR(RT, SX32(v)); } break;
    case 0x24: ea = (uint32_t)(R(RS) + IMM); { uint8_t v = rd8(ea); if (!exc_raised) WR(RT, v); } break;
    case 0x25: ea = (uint32_t)(R(RS) + IMM); { uint16_t v = rd16(ea); if (!exc_raised) WR(RT, v); } break;
    case 0x26: { // LWR
        ea = (uint32_t)(R(RS) + IMM);
        uint32_t w = rd32(ea & ~3u); if (exc_raised) break;
        int sh = 8 * (ea & 3);
        uint32_t keep = sh ? ~(0xffffffffu >> sh) : 0;
        uint32_t v = ((uint32_t)R(RT) & keep) | (w >> sh);
        WR(RT, sh ? ((R(RT) & 0xffffffff00000000ull) | v) : SX32(v));
        break;
    }
    case 0x27: ea = (uint32_t)(R(RS) + IMM); { uint32_t v = rd32(ea); if (!exc_raised) WR(RT, v); } break;
    case 0x28: wr8((uint32_t)(R(RS) + IMM), (uint8_t)R(RT)); break;
    case 0x29: wr16((uint32_t)(R(RS) + IMM), (uint16_t)R(RT)); break;
    case 0x2a: { // SWL
        ea = (uint32_t)(R(RS) + IMM);
        uint32_t w = rd32(ea & ~3u); if (exc_raised) break;
        int sh = 8 * (3 - (ea & 3));
        uint32_t mask = 0xffffffffu >> sh;
        w = (w & ~mask) | ((uint32_t)R(RT) >> sh);
        wr32(ea & ~3u, w);
        break;
    }
    case 0x2b: wr32((uint32_t)(R(RS) + IMM), (uint32_t)R(RT)); break;
    case 0x2c: case 0x2d: { // SDL / SDR
        ea = (uint32_t)(R(RS) + IMM);
        uint64_t w = rd64(ea & ~7u); if (exc_raised) break;
        int b = ea & 7;
        if ((op >> 26) == 0x2c) { int sh = 8 * (7 - b); uint64_t mask = ~0ull >> sh; w = (w & ~mask) | (R(RT) >> sh); }
        else { int sh = 8 * b; uint64_t mask = ~0ull << sh; w = (w & ~mask) | (R(RT) << sh); }
        wr64(ea & ~7u, w);
        break;
    }
    case 0x2e: { // SWR
        ea = (uint32_t)(R(RS) + IMM);
        uint32_t w = rd32(ea & ~3u); if (exc_raised) break;
        int sh = 8 * (ea & 3);
        uint32_t mask = 0xffffffffu << sh;
        w = (w & ~mask) | ((uint32_t)R(RT) << sh);
        wr32(ea & ~3u, w);
        break;
    }
    case 0x2f: break; // CACHE
    case 0x30: ea = (uint32_t)(R(RS) + IMM); { uint32_t v = rd32(ea); if (!exc_raised) { WR(RT, SX32(v)); cpu.llbit = true; } } break; // LL
    case 0x31: ea = (uint32_t)(R(RS) + IMM); { uint32_t v; // LWC1
        if (!(cpu.cp0[CP0_STATUS] & SR_CU1)) { cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | (1ull << 28); cpu_exception(EXC_CPU, 0, false); break; }
        v = rd32(ea); if (!exc_raised) fset32(RT, v); } break;
    case 0x34: ea = (uint32_t)(R(RS) + IMM); { uint64_t v = rd64(ea); if (!exc_raised) { WR(RT, v); cpu.llbit = true; } } break; // LLD
    case 0x35: ea = (uint32_t)(R(RS) + IMM); { uint64_t v; // LDC1
        if (!(cpu.cp0[CP0_STATUS] & SR_CU1)) { cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | (1ull << 28); cpu_exception(EXC_CPU, 0, false); break; }
        v = rd64(ea); if (!exc_raised) fset64(RT, v); } break;
    case 0x37: ea = (uint32_t)(R(RS) + IMM); { uint64_t v = rd64(ea); if (!exc_raised) WR(RT, v); } break;
    case 0x38: ea = (uint32_t)(R(RS) + IMM); // SC
        if (cpu.llbit) { wr32(ea, (uint32_t)R(RT)); if (!exc_raised) WR(RT, 1); }
        else WR(RT, 0);
        break;
    case 0x39: // SWC1
        if (!(cpu.cp0[CP0_STATUS] & SR_CU1)) { cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | (1ull << 28); cpu_exception(EXC_CPU, 0, false); break; }
        wr32((uint32_t)(R(RS) + IMM), fget32(RT)); break;
    case 0x3c: ea = (uint32_t)(R(RS) + IMM); // SCD
        if (cpu.llbit) { wr64(ea, R(RT)); if (!exc_raised) WR(RT, 1); }
        else WR(RT, 0);
        break;
    case 0x3d: // SDC1
        if (!(cpu.cp0[CP0_STATUS] & SR_CU1)) { cpu.cp0[CP0_CAUSE] = (cpu.cp0[CP0_CAUSE] & ~0x30000000ull) | (1ull << 28); cpu_exception(EXC_CPU, 0, false); break; }
        wr64((uint32_t)(R(RS) + IMM), fget64(RT)); break;
    case 0x3f: wr64((uint32_t)(R(RS) + IMM), R(RT)); break;
    default: cpu_exception(EXC_RI, 0, false); break;
    }
}

void cpu_reset(void)
{
    memset(&cpu, 0, sizeof(cpu));
    poll_nyes = poll_nno = 0;
    cpu.pc = 0xbfc00000u;
    cpu.npc = cpu.pc + 4;
    cpu.cp0[CP0_STATUS] = SR_BEV | SR_ERL;
    cpu.cp0[CP0_PRID] = 0x2020;   // R4600
    // IC=DC=16KB, 32-byte lines, little-endian, KSEG0 cacheable
    cpu.cp0[CP0_CONFIG] = (2u << 9) | (2u << 6) | (1u << 5) | (1u << 4) | 3;
    cpu.cp0[CP0_WIRED] = 0;
    schedule_compare();
    irq_dirty = true;
    tlbc_flush();
    if (getenv("KI2_COVER") && !cover) {
        cover = calloc(1, (RAM0_SIZE + RAM2_SIZE) / 32);
        cover_lead = calloc(1, (RAM0_SIZE + RAM2_SIZE) / 32);
        atexit(cover_save);
    }
}

size_t cpu_state(uint8_t *buf, bool load)
{
    if (buf && load) {
        memcpy(&cpu, buf, sizeof cpu);
        irq_dirty = true;       // safe: a check with nothing pending changes nothing
        tlbc_flush();
    } else if (buf) memcpy(buf, &cpu, sizeof cpu);
    return sizeof cpu;
}

void cpu_skip(uint64_t n)
{
    if (cpu_idle && idle_loop.cnt_reg > 0) {
        int r = idle_loop.cnt_reg;
        cpu.r[r] = SX32((uint32_t)cpu.r[r] + (uint32_t)(n / 5) * (uint32_t)(int32_t)idle_loop.cnt_imm);
    }
    cpu.cycles += n;
    compare_check();
}

/* ---------------------------------------------------------------- */
/* Entry points for recompiled code (see rec.h)                     */
/* ---------------------------------------------------------------- */

bool cpu_exc;
uint64_t rec_deadline;
uint64_t cpu_interp_count;
uint32_t *cpu_interp_hist;

uint64_t rec_load(uint32_t va, int size, uint32_t pc, bool delay)
{
    cpu.cur_pc = pc; cpu.in_delay = delay; exc_raised = false;
    uint64_t v;
    switch (size) {
    case 1: v = rd8(va); break;
    case 2: v = rd16(va); break;
    case 4: v = rd32(va); break;
    default: v = rd64(va); break;
    }
    cpu_exc = exc_raised;
    return v;
}

void rec_store(uint32_t va, uint64_t v, int size, uint32_t pc, bool delay)
{
    cpu.cur_pc = pc; cpu.in_delay = delay; exc_raised = false;
    switch (size) {
    case 1: wr8(va, (uint8_t)v); break;
    case 2: wr16(va, (uint16_t)v); break;
    case 4: wr32(va, (uint32_t)v); break;
    default: wr64(va, v); break;
    }
    cpu_exc = exc_raised;
}

bool rec_op(uint32_t op, uint32_t pc, bool delay)
{
    cpu.cur_pc = pc; cpu.in_delay = delay; exc_raised = false;
    cpu.pc = pc + 4; cpu.npc = pc + 8; cpu.next_delay = false;
    exec(op);
    if (exc_raised) return true;
    if (delay) return false;
    return irq_dirty || cpu_idle || cpu.pc != pc + 4;
}

uint64_t cpu_run(uint64_t n)
{
    uint64_t start = cpu.cycles;
    uint64_t end = start + n;
    cpu_idle = false;
    while (cpu.cycles < end && !cpu_idle) {
        cpu.cur_pc = cpu.pc;
        cpu.in_delay = cpu.next_delay;
        exc_raised = false;
        if (irq_dirty && (irq_dirty = false, irq_pending())) {
            cpu_exception(EXC_INT, 0, false);
            cpu.cur_pc = cpu.pc;
            cpu.in_delay = false;
            exc_raised = false;
        }
        if (!cpu.in_delay && rec_enabled) {
            rec_fn_t f = rec_lookup(cpu.pc);
            if (f) {
                rec_deadline = end < cpu.next_compare ? end : cpu.next_compare;
                f();
                compare_check();
                continue;
            }
        }
        cpu.next_delay = false;
        uint32_t pc = cpu.pc;
        uint32_t op = fetch(pc);
        if (!exc_raised) {
            if (cover && (pc & 0xc0000000u) == 0x80000000u && (pc & 0x1fffffffu) < 0x08800000u) {
                static uint32_t prev;
                uint32_t i = COVER_IDX(pc & 0x1fffffffu);
                cover[i >> 3] |= 1u << (i & 7);
                if (pc != prev + 4) cover_lead[i >> 3] |= 1u << (i & 7);
                prev = pc;
            }
            if (cpu_trace) {
                char buf[96];
                fprintf(stderr, "%08x: %08x  %s\n", pc, op, cpu_disasm(pc, op, buf));
            }
            cpu.pc = cpu.npc;
            cpu.npc += 4;
            exec(op);
        }
        cpu.cycles++;
        cpu_interp_count++;
        if (cpu_interp_hist && (pc & 0xfff00000u) == 0x88000000u) cpu_interp_hist[(pc >> 2) & 0xffff]++;
        compare_check();
    }
    return cpu.cycles - start;
}
