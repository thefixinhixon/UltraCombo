// Rollback netplay (see netplay.h).
//
// Each side schedules its input `delay` frames ahead and sends it (with every
// input the other side has not acknowledged yet, so lost packets don't
// matter). Missing remote input is predicted by repeating the last one; when
// the real input arrives and differs, the machine rewinds to that frame via
// the save-state journal and replays to the present. Both sides report how
// far ahead they are and the leader occasionally waits a frame, so rollbacks
// are shared. A hash of each confirmed second is compared to catch desyncs.
#include "netplay.h"
#include "ide.h"
#include "mem.h"
#include "net.h"
#include "state.h"
#include <stdio.h>
#include <string.h>

#define MAGIC 0x4e32494bu        // "KI2N"
#define VERSION 2
#define RING 256
#define MAX_PREDICT 8            // never run further ahead of the opponent than this
#define TIMEOUT_MS 3000
#define HASH_EVERY 60

enum { P_HELLO = 1, P_WELCOME, P_DISK, P_DISKACK, P_START, P_INPUT, P_QUIT };
enum { NP_OFF, NP_HOSTING, NP_JOINING, NP_SEND_DISK, NP_RECV_DISK, NP_STARTING, NP_RUNNING };

static int state = NP_OFF;
static bool host;
static net_addr_t peer;
static int delay;
static np_run_fn run_frame_cb;
static np_reset_fn reset_cb;
static char status[128], error[160];
static double last_heard, last_sent, rtt_ms = 0;

// frame bookkeeping (absolute frame numbers; the session starts at NP_BOOT_FRAMES)
static uint32_t frame;               // next frame to emulate
static uint32_t local_last;          // newest frame with a local input
static int64_t  remote_last;         // newest frame with the remote input (contiguous)
static uint32_t checked;             // frames < checked were emulated with the real remote input
static uint32_t remote_ack;          // remote has our inputs up to here
static uint32_t remote_frame;        // remote's frame counter, as last reported
static int      remote_adv;          // remote's frame advantage, as last reported
static int      wait_frames;         // time-sync frames still to sit out
static uint16_t in_local[RING], in_remote[RING], used_remote[RING];
static uint64_t hashes[RING];        // quick hash after each frame (only every HASH_EVERY-th kept)
static int      rollbacks, last_rollback;

static uint64_t echo_time;           // the peer's last timestamp, sent back to it
static bool     got_input;           // the peer's first input packet has arrived

static uint8_t disk_buf[IDE_TRACK_BYTES];
static uint8_t own_disk[IDE_TRACK_BYTES];   // guest: its own settings, put back after the session
static bool own_disk_saved;
static uint32_t host_dsw, host_instant_disk;

// The guest plays on the host's settings sectors; afterwards its own come back
// (they reach the disk image file on the next flush).
static void restore_own_disk(void)
{
    if (!own_disk_saved) return;
    memcpy(ide_tracked(), own_disk, IDE_TRACK_BYTES);
    ide_tracked_changed();
    for (int i = 0; i < IDE_TRACK_PAGES; i++) ide_dirty[i] = 1;
    own_disk_saved = false;
}
static uint32_t disk_have;           // guest: bitmask of 1 KB chunks received

static uint32_t end_frame = ~0u;

void netplay_set_callbacks(np_run_fn run, np_reset_fn reset) { run_frame_cb = run; reset_cb = reset; }
void netplay_set_end(uint32_t f) { end_frame = f; }
bool netplay_active(void) { return state != NP_OFF; }
bool netplay_running(void) { return state == NP_RUNNING; }
int  netplay_local_player(void) { return host ? 0 : 1; }
const char *netplay_status(void) { return status; }

const char *netplay_take_error(void)
{
    static char e[160];
    if (!error[0]) return NULL;
    snprintf(e, sizeof e, "%s", error);
    error[0] = 0;
    return e;
}

static void send_quit(int reason)
{
    uint8_t q[8]; uint32_t m = MAGIC;
    memcpy(q, &m, 4); q[4] = P_QUIT; q[5] = (uint8_t)reason;
    for (int i = 0; i < 3; i++) net_send(&peer, q, 6);
}

static void fail_reason(const char *msg, int reason)
{
    snprintf(error, sizeof error, "Netplay: %s", msg);
    fprintf(stderr, "netplay: %s\n", msg);
    if (state != NP_HOSTING) send_quit(reason);
    state = NP_OFF;
    net_close();
    status[0] = 0;
    restore_own_disk();
}

static void fail(const char *msg) { fail_reason(msg, 0); }

void netplay_stop(void)
{
    if (state == NP_OFF) return;
    if (state != NP_HOSTING) send_quit(0);
    state = NP_OFF;
    restore_own_disk();
    net_close();
    status[0] = 0;
}

/* ---------------------------------------------------------------- */
/* Packets                                                          */
/* ---------------------------------------------------------------- */

typedef struct { uint8_t b[1400]; int n; } pkt_t;
static void put8(pkt_t *p, uint8_t v) { p->b[p->n++] = v; }
static void put16(pkt_t *p, uint16_t v) { memcpy(p->b + p->n, &v, 2); p->n += 2; }
static void put32(pkt_t *p, uint32_t v) { memcpy(p->b + p->n, &v, 4); p->n += 4; }
static void put64(pkt_t *p, uint64_t v) { memcpy(p->b + p->n, &v, 8); p->n += 8; }
static void begin(pkt_t *p, int type) { p->n = 0; put32(p, MAGIC); put8(p, (uint8_t)type); }
static void send_pkt(pkt_t *p) { net_send(&peer, p->b, p->n); last_sent = net_now_ms(); }

typedef struct { const uint8_t *b; int n, i; bool bad; } rd_t;
static uint8_t  get8(rd_t *r) { if (r->i + 1 > r->n) { r->bad = true; return 0; } return r->b[r->i++]; }
static uint16_t get16(rd_t *r) { uint16_t v = 0; if (r->i + 2 > r->n) r->bad = true; else memcpy(&v, r->b + r->i, 2); r->i += 2; return v; }
static uint32_t get32(rd_t *r) { uint32_t v = 0; if (r->i + 4 > r->n) r->bad = true; else memcpy(&v, r->b + r->i, 4); r->i += 4; return v; }
static uint64_t get64(rd_t *r) { uint64_t v = 0; if (r->i + 8 > r->n) r->bad = true; else memcpy(&v, r->b + r->i, 8); r->i += 8; return v; }

static uint64_t compat_id(void) { return state_compat_id(); }

/* ---------------------------------------------------------------- */
/* Session start                                                    */
/* ---------------------------------------------------------------- */

static bool open_socket(int port)
{
    if (net_open(port)) return true;
    snprintf(error, sizeof error, "Netplay: UDP port %d is not available", port);
    return false;
}

bool netplay_host(int port, int input_delay)
{
    netplay_stop();
    if (!open_socket(port)) return false;
    host = true;
    delay = input_delay;
    state = NP_HOSTING;
    snprintf(status, sizeof status, "Waiting for a player on port %d", port);
    error[0] = 0;
    return true;
}

bool netplay_join(const char *hostname, int port, int input_delay)
{
    netplay_stop();
    if (!net_resolve(hostname, port, &peer)) { snprintf(error, sizeof error, "Netplay: cannot find %s", hostname); return false; }
    if (!open_socket(0)) return false;
    host = false;
    delay = input_delay;
    state = NP_JOINING;
    snprintf(status, sizeof status, "Connecting to %s", net_addr_str(&peer));
    error[0] = 0;
    last_heard = net_now_ms();
    disk_have = 0;
    return true;
}

static void begin_session(void)
{
    // both sides: same disk settings, power-cycle, same boot frames
    if (!host) {
        memcpy(own_disk, ide_tracked(), IDE_TRACK_BYTES);
        own_disk_saved = true;
        memcpy(ide_tracked(), disk_buf, IDE_TRACK_BYTES);
        ide_tracked_changed();
        hw.dsw = host_dsw;
        hw.instant_disk = host_instant_disk;
    }
    reset_cb();
    state_set_history((size_t)64 << 20);
    frame = NP_BOOT_FRAMES;
    checked = frame;
    remote_last = (int64_t)frame + delay - 1;     // the first `delay` frames have no input on either side
    local_last = frame + (uint32_t)delay - 1;
    remote_ack = local_last;                      // ... which both sides already know
    got_input = false;
    remote_frame = frame;
    remote_adv = 0;
    wait_frames = 0;
    rollbacks = last_rollback = 0;
    memset(in_local, 0, sizeof in_local);
    memset(in_remote, 0, sizeof in_remote);
    memset(used_remote, 0, sizeof used_remote);
    memset(hashes, 0, sizeof hashes);
    hashes[frame % RING] = state_quick_hash();
    state = NP_RUNNING;
    last_heard = net_now_ms();
    fprintf(stderr, "netplay: playing as player %d, input delay %d\n", host ? 1 : 2, delay);
}

/* ---------------------------------------------------------------- */
/* Running                                                          */
/* ---------------------------------------------------------------- */

// How many frames we are ahead of the opponent, allowing for the time packets take.
static int frame_advantage(void)
{
    return (int)((int64_t)frame - (int64_t)remote_frame - (int64_t)(rtt_ms / 2 / (1000.0 / 58.9634)));
}

static void send_inputs(void)
{
    pkt_t p;
    begin(&p, P_INPUT);
    put32(&p, (uint32_t)remote_last);                       // ack: remote inputs we have
    // every input the other side has not acknowledged (it only accepts them in order)
    uint32_t first = remote_ack + 1;
    uint32_t count = local_last >= first ? local_last - first + 1 : 0;
    if (count > 200) count = 200;
    put32(&p, first);
    put8(&p, (uint8_t)count);
    for (uint32_t f = first; f < first + count; f++) put16(&p, in_local[f % RING]);
    put32(&p, frame);
    put16(&p, (uint16_t)(int16_t)frame_advantage());
    // newest confirmed hash
    uint32_t hf = checked > NP_BOOT_FRAMES ? (checked - 1) / HASH_EVERY * HASH_EVERY : NP_BOOT_FRAMES;
    if (hf < NP_BOOT_FRAMES) hf = NP_BOOT_FRAMES;
    put32(&p, hf);
    put64(&p, hashes[hf % RING]);
    put64(&p, (uint64_t)(net_now_ms() * 1000));             // echoed for round-trip time
    put64(&p, echo_time);
    send_pkt(&p);
}

static void on_input(rd_t *r)
{
    uint32_t ack = get32(r), first = get32(r);
    int count = get8(r);
    if (r->bad) return;
    if (ack > remote_ack && ack < frame + RING) remote_ack = ack;
    for (int i = 0; i < count; i++) {
        uint16_t v = get16(r);
        uint32_t f = first + (uint32_t)i;
        if (r->bad) return;
        if ((int64_t)f == remote_last + 1) { in_remote[f % RING] = v; remote_last = f; }
    }
    uint32_t rf = get32(r);
    int16_t adv = (int16_t)get16(r);
    uint32_t hf = get32(r);
    uint64_t h = get64(r);
    uint64_t sent_us = get64(r), echoed_us = get64(r);
    if (r->bad) return;
    remote_frame = rf;
    remote_adv = adv;
    got_input = true;
    if (echoed_us) rtt_ms = rtt_ms * 0.9 + (net_now_ms() - echoed_us / 1000.0) * 0.1;
    echo_time = sent_us;
    // desync check: compare a second both sides have confirmed
    if (hf >= NP_BOOT_FRAMES && hf <= checked && hf + RING > frame && hashes[hf % RING] && h) {
        if (hashes[hf % RING] != h) {
            char m[96];
            snprintf(m, sizeof m, "desync at frame %u - the games no longer match", hf);
            fail_reason(m, 1);
        }
    }
}

// Rewind to the earliest mispredicted frame and replay up to the present.
static void rollback_if_needed(void)
{
    uint32_t limit = (uint32_t)(remote_last + 1 < frame ? remote_last + 1 : frame);
    uint32_t bad = 0;
    bool found = false;
    for (uint32_t f = checked; f < limit; f++)
        if (in_remote[f % RING] != used_remote[f % RING]) { bad = f; found = true; break; }
    if (!found) { if (limit > checked) checked = limit; return; }

    int back = (int)(frame - bad);
    for (int i = 0; i < back; i++) state_rewind();
    rollbacks++;
    last_rollback = back;
    for (uint32_t f = bad; f < frame; f++) {
        uint16_t in[2];
        uint16_t remote = (int64_t)f <= remote_last ? in_remote[f % RING] : in_remote[remote_last % RING];
        used_remote[f % RING] = remote;
        in[host ? 0 : 1] = in_local[f % RING];
        in[host ? 1 : 0] = remote;
        run_frame_cb(in, true);
        hashes[(f + 1) % RING] = (f + 1) % HASH_EVERY == 0 ? state_quick_hash() : 0;
    }
    checked = limit;
}

static int step(uint16_t local_input)
{
    // schedule this frame's input `delay` frames ahead
    if (frame + (uint32_t)delay > local_last) {
        local_last = frame + (uint32_t)delay;
        in_local[local_last % RING] = local_input;
    }
    send_inputs();
    rollback_if_needed();
    if (state != NP_RUNNING) return 0;

    // time sync: if we are further ahead than the opponent, sit a frame out now and then
    if (frame % 30 == 0 && !wait_frames) {
        int diff = (frame_advantage() - remote_adv) / 2;
        if (diff > 0) wait_frames = diff > 2 ? 2 : diff;
    }
    if (wait_frames) { wait_frames--; return 0; }
    if (frame >= end_frame) return 0;
    if ((int64_t)frame - remote_last > MAX_PREDICT) return 0;     // too far ahead: wait

    uint16_t in[2];
    uint16_t remote = (int64_t)frame <= remote_last ? in_remote[frame % RING] : in_remote[remote_last % RING];
    used_remote[frame % RING] = remote;
    in[host ? 0 : 1] = in_local[frame % RING];
    in[host ? 1 : 0] = remote;
    run_frame_cb(in, false);
    frame++;
    hashes[frame % RING] = frame % HASH_EVERY == 0 ? state_quick_hash() : 0;
    if ((int64_t)frame - 1 <= remote_last && checked == frame - 1) checked = frame;
    return 1;
}

/* ---------------------------------------------------------------- */

static void handle_packet(const uint8_t *b, int n, const net_addr_t *from)
{
    rd_t r = { b, n, 0, false };
    if (get32(&r) != MAGIC) return;
    int type = get8(&r);
    if (state == NP_HOSTING && type == P_HELLO) {
        uint32_t ver = get32(&r);
        uint64_t cid = get64(&r), did = get64(&r);
        int their_delay = get8(&r);
        if (r.bad) return;
        peer = *from;
        pkt_t p;
        begin(&p, P_WELCOME);
        int reason = ver != VERSION ? 1 : cid != compat_id() ? 2 : did != ide_image_id() ? 3 : 0;
        if (their_delay > delay) delay = their_delay;
        put8(&p, (uint8_t)reason);
        put8(&p, (uint8_t)delay);
        put32(&p, hw.dsw);                      // the host's DIP switches apply to both
        put8(&p, (uint8_t)hw.instant_disk);     // ...and its disk timing
        send_pkt(&p);
        if (reason) {
            static const char *const why[] = { "", "different program version", "different ROM or build", "different disk image" };
            fprintf(stderr, "netplay: rejected %s: %s\n", net_addr_str(from), why[reason]);
            return;
        }
        memcpy(disk_buf, ide_tracked(), IDE_TRACK_BYTES);
        disk_have = 0;
        state = NP_SEND_DISK;
        last_heard = net_now_ms();
        snprintf(status, sizeof status, "Player found at %s, syncing", net_addr_str(from));
        return;
    }
    if (!net_addr_eq(from, &peer)) return;
    last_heard = net_now_ms();
    switch (type) {
    case P_WELCOME:
        if (state != NP_JOINING) return;
        {
            int reason = get8(&r), d = get8(&r);
            uint32_t dsw = get32(&r);
            static const char *const why[] = { "", "the host runs a different program version",
                                               "the host has a different ROM or build", "the host has a different disk image" };
            if (!r.bad && reason) { fail(why[reason > 3 ? 3 : reason]); return; }   // before fields an older host lacks
            int instant = get8(&r);
            if (r.bad) return;
            host_dsw = dsw;
            host_instant_disk = instant != 0;
            delay = d;
            state = NP_RECV_DISK;
            snprintf(status, sizeof status, "Connected, syncing");
        }
        return;
    case P_DISK:
        if (state != NP_RECV_DISK && state != NP_STARTING) return;
        {
            int idx = get8(&r);
            if (r.bad || idx >= IDE_TRACK_BYTES / 1024 || r.i + 1024 > n) return;
            memcpy(disk_buf + idx * 1024, b + r.i, 1024);
            disk_have |= 1u << idx;
            pkt_t p;
            begin(&p, P_DISKACK);
            put32(&p, disk_have);
            send_pkt(&p);
        }
        return;
    case P_DISKACK:
        if (state == NP_SEND_DISK) { disk_have = get32(&r); if (r.bad) disk_have = 0; }
        return;
    case P_START:
        if (state == NP_RECV_DISK && disk_have == (1ull << (IDE_TRACK_BYTES / 1024)) - 1) {
            snprintf(status, sizeof status, "Starting");
            begin_session();
        }
        return;
    case P_INPUT:
        if (state == NP_STARTING) begin_session();
        if (state == NP_RUNNING) on_input(&r);
        return;
    case P_QUIT: {
        int reason = get8(&r);
        if (state == NP_OFF) return;
        if (!r.bad && reason == 1) fail_reason("desync - the games no longer match", 1);
        else fail("the other player left");
        return;
    }
    }
}

int netplay_update(uint16_t local_input)
{
    if (state == NP_OFF) return 0;
    uint8_t buf[1500];
    net_addr_t from;
    int n;
    while ((n = net_recv(&from, buf, sizeof buf)) >= 0) {
        handle_packet(buf, n, &from);
        if (state == NP_OFF) return 0;
    }
    double now = net_now_ms();
    // generous until the other side has finished powering on and sends input
    double timeout = state == NP_RUNNING && got_input ? TIMEOUT_MS : 30000;
    if (state != NP_HOSTING && now - last_heard > timeout) {
        fail(state == NP_RUNNING ? "connection lost" : "no answer from the host");
        return 0;
    }
    pkt_t p;
    switch (state) {
    case NP_JOINING:
        if (now - last_sent > 250) {
            begin(&p, P_HELLO);
            put32(&p, VERSION);
            put64(&p, compat_id());
            put64(&p, ide_image_id());
            put8(&p, (uint8_t)delay);
            send_pkt(&p);
        }
        return 0;
    case NP_SEND_DISK: {
        uint32_t all = (uint32_t)((1ull << (IDE_TRACK_BYTES / 1024)) - 1);
        if (disk_have == all) {
            begin(&p, P_START);
            send_pkt(&p);
            state = NP_STARTING;
            return 0;
        }
        if (now - last_sent > 30)
            for (int i = 0; i < IDE_TRACK_BYTES / 1024; i++) {
                if (disk_have >> i & 1) continue;
                begin(&p, P_DISK);
                put8(&p, (uint8_t)i);
                memcpy(p.b + p.n, disk_buf + i * 1024, 1024);
                p.n += 1024;
                send_pkt(&p);
            }
        return 0;
    }
    case NP_STARTING:
        if (now - last_sent > 100) { begin(&p, P_START); send_pkt(&p); }
        return 0;
    case NP_RUNNING: {
        int ran = step(local_input);
        if (state == NP_RUNNING)
            snprintf(status, sizeof status, "P%d  ping %.0f ms  delay %d  rollback %d", host ? 1 : 2, rtt_ms, delay, last_rollback);
        return ran;
    }
    }
    return 0;
}

uint32_t netplay_confirmed(void) { return checked; }
uint32_t netplay_input_frame(void) { return frame + (uint32_t)delay; }
