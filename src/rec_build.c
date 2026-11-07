// Building the translated game code on the player's machine (see rec_build.h).
#include "rec_build.h"
#include "game.h"
#include "ide.h"
#include "machine.h"
#include "mem.h"
#include "rec.h"
#include "recomp.h"
#include "adsprec.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define make_dir(p) _mkdir(p)
#define SO_EXT ".dll"
#define EXE_EXT ".exe"
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define make_dir(p) mkdir(p, 0755)
#define SO_EXT ".so"
#define EXE_EXT ""
extern char **environ;
#endif

#define RECOMP_VERSION 3           // bump when recomp.c's or adsprec.c's output changes
#define SNAPSHOT_FRAMES game.snapshot_frames   // the game has loaded all of its code by then

// rec_abi.h, rec_gen.h and adsp_core.h, embedded at build time (build/rec_headers.c)
extern const char rec_abi_h[], rec_gen_h[], adsp_core_h[];

static void *module_handle;         // kept loaded for the rest of the run

/* ---------------------------------------------------------------- */
/* Shared libraries                                                 */
/* ---------------------------------------------------------------- */

static void *lib_open(const char *path)
{
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *lib_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static const char *lib_error(void)
{
#ifdef _WIN32
    static char buf[64];
    snprintf(buf, sizeof buf, "Windows error %lu", (unsigned long)GetLastError());
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "unknown error";
#endif
}

bool rec_build_load_file(const char *path, char *err, size_t errlen)
{
    void *lib = lib_open(path);
    if (!lib) { snprintf(err, errlen, "%s: %s", path, lib_error()); return false; }
    rec_module_init_fn init = (rec_module_init_fn)lib_sym(lib, REC_MODULE_INIT);
    if (!init || !rec_use_module(init)) {
        snprintf(err, errlen, "%s: not a usable module", path);
        return false;
    }
    module_handle = lib;
    return true;
}

/* ---------------------------------------------------------------- */
/* Files and processes                                              */
/* ---------------------------------------------------------------- */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static bool write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 0x100000001b3ull; }
    return h;
}

// Everything the translated code depends on: the boot ROM, the disk's code
// (sampled), the sound DSP's code, the generators and the module interface.
static uint64_t build_key(void)
{
    uint64_t h = fnv(0xcbf29ce484222325ull, rom, ROM_SIZE);
    uint64_t disk = ide_image_id();
    int v[2] = { RECOMP_VERSION, REC_ABI_VERSION };
    h = fnv(h, &disk, sizeof disk);
    adsprec_image_t img[6];
    int n = sound_code_images(img, 6);
    for (int i = 0; i < n; i++) {
        h = fnv(h, &img[i].base, sizeof img[i].base);
        h = fnv(h, img[i].image, img[i].words * sizeof *img[i].image);
    }
    return fnv(h, v, sizeof v);
}

static void cache_paths(char *dir, size_t n, char *module, size_t mn, char *src, size_t sn)
{
    char key[32];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)build_key());
    snprintf(dir, n, "%scache", data_dir());
    if (module) snprintf(module, mn, "%s/rec-%s" SO_EXT, dir, key);
    if (src) snprintf(src, sn, "%s/src-%s", dir, key);
}

// Written when TinyCC was the best compiler available: next time it is used
// straight away (it compiles the cached sources into memory in a moment).
static void tcc_marker(const char *src, char *out, size_t n) { snprintf(out, n, "%s/use-tinycc", src); }

#define MAX_JOBS 16

// Room for a link of every file the translator can write (MAX_FILES in
// recomp.c) plus the compiler's own arguments. A command that doesn't fit
// fails instead of quietly losing arguments.
#define CMD_ARGS 96
typedef struct { char *argv[CMD_ARGS]; int argc; bool overflow; } cmd_t;

static void cmd_add(cmd_t *c, const char *a)
{
    if (c->argc < CMD_ARGS - 1) c->argv[c->argc++] = strdup(a);
    else c->overflow = true;
    c->argv[c->argc] = NULL;
}
static void cmd_free(cmd_t *c) { for (int i = 0; i < c->argc; i++) free(c->argv[i]); c->argc = 0; }

// Run the commands, `parallel` at a time, output appended to `log`. True if all succeeded.
static bool run_all(cmd_t *cmds, int n, int parallel, const char *log, void (*tick)(int done, int total))
{
    if (parallel < 1) parallel = 1;
    if (parallel > MAX_JOBS) parallel = MAX_JOBS;
    for (int i = 0; i < n; i++) if (cmds[i].overflow) return false;
    bool ok = true;
    int next = 0, done = 0;
#ifdef _WIN32
    HANDLE running[MAX_JOBS];
    int nrun = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    // a compiler that can't start or crashes must fail quietly, not stop the
    // build with a Windows message box (children inherit this)
    UINT error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HANDLE out = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    while (done < n) {
        while (nrun < parallel && next < n && ok) {
            static char line[32768];                    // Windows' limit for a command line
            size_t len = 0;
            for (int i = 0; i < cmds[next].argc; i++) {
                size_t al = strlen(cmds[next].argv[i]);
                if (len + al + 4 > sizeof line) { ok = false; break; }   // too long: fail, don't truncate
                line[len++] = '"';
                memcpy(line + len, cmds[next].argv[i], al); len += al;
                line[len++] = '"'; line[len++] = ' ';
            }
            line[len] = 0;
            if (!ok) { next = n; break; }
            STARTUPINFOA si = { sizeof si };
            PROCESS_INFORMATION pi;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = out;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { ok = false; next = n; break; }
            CloseHandle(pi.hThread);
            running[nrun++] = pi.hProcess;
            next++;
        }
        if (!nrun) break;
        DWORD r = WaitForMultipleObjects((DWORD)nrun, running, FALSE, 50);
        if (r == WAIT_TIMEOUT) { if (tick) tick(done, n); continue; }
        int i = (int)(r - WAIT_OBJECT_0);
        if (i < 0 || i >= nrun) { ok = false; break; }
        DWORD code = 1;
        GetExitCodeProcess(running[i], &code);
        CloseHandle(running[i]);
        running[i] = running[--nrun];
        if (code) ok = false;
        done++;
        if (tick) tick(done, n);
        if (!ok) next = n;
    }
    for (int i = 0; i < nrun; i++) { WaitForSingleObject(running[i], INFINITE); CloseHandle(running[i]); }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    SetErrorMode(error_mode);
#else
    pid_t running[MAX_JOBS];
    int nrun = 0;
    int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    while (done < n) {
        while (nrun < parallel && next < n && ok) {
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            if (fd >= 0) { posix_spawn_file_actions_adddup2(&fa, fd, 1); posix_spawn_file_actions_adddup2(&fa, fd, 2); }
            pid_t pid;
            int e = posix_spawnp(&pid, cmds[next].argv[0], &fa, NULL, cmds[next].argv, environ);
            posix_spawn_file_actions_destroy(&fa);
            if (e) { ok = false; next = n; break; }
            running[nrun++] = pid;
            next++;
        }
        if (!nrun) break;
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0) { if (tick) tick(done, n); usleep(50000); continue; }
        int i = 0;
        while (i < nrun && running[i] != pid) i++;
        if (i == nrun) continue;
        running[i] = running[--nrun];
        if (!WIFEXITED(status) || WEXITSTATUS(status)) ok = false;
        done++;
        if (tick) tick(done, n);
        if (!ok) next = n;
    }
    while (nrun) { int st; waitpid(running[--nrun], &st, 0); }
    if (fd >= 0) close(fd);
#endif
    return ok;
}

static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 2;
#endif
}

// A program on PATH (or an absolute path) that exists.
static bool find_program(const char *name, char *out, size_t n)
{
    if (strchr(name, '/') || strchr(name, '\\')) { snprintf(out, n, "%s", name); return file_exists(out); }
    const char *path = getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *dir = buf; dir && *dir;) {
        char *e = strchr(dir, sep);
        if (e) *e = 0;
        snprintf(out, n, "%s/%s" EXE_EXT, dir, name);
        if (file_exists(out)) return true;
        dir = e ? e + 1 : NULL;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* Compilers                                                        */
/* ---------------------------------------------------------------- */

static rec_progress_fn progress_cb;
static double compile_from, compile_to;

static void compile_tick(int done, int total)
{
    if (progress_cb) progress_cb("Compiling the game code", compile_from + (compile_to - compile_from) * done / total);
}

static int nsources;

// The generated files: rec_000.c ... (nsources of them), then rec_tab.c,
// rec_dsp.c and the sound images rec_dsp_0.c ...
#define ALL_SOURCES (nsources + 2 + ADSPREC_FILES)

static void source_name(char *out, size_t n, const char *src, int i)
{
    if (i < nsources) snprintf(out, n, "%s/rec_%03d.c", src, i);
    else if (i == nsources) snprintf(out, n, "%s/rec_tab.c", src);
    else if (i == nsources + 1) snprintf(out, n, "%s/rec_dsp.c", src);
    else snprintf(out, n, "%s/rec_dsp_%d.c", src, i - nsources - 2);
}

// zig = true: "zig cc" (bundled), else a gcc/clang driver.
static bool build_with(const char *cc, bool zig, const char *src, const char *module, const char *log)
{
    cmd_t jobs[80];
    int n = 0;
    char obj[2200], file[2200];
    for (int i = 0; i < ALL_SOURCES; i++) {
        cmd_t *c = &jobs[n++];
        c->argc = 0;
        cmd_add(c, cc);
        if (zig) cmd_add(c, "cc");
#ifdef _WIN32
        if (zig) { cmd_add(c, "-target"); cmd_add(c, "x86_64-windows-gnu"); }
#else
        cmd_add(c, "-fPIC");
#endif
        cmd_add(c, "-O1");
        cmd_add(c, "-fvisibility=hidden");
        cmd_add(c, "-fno-stack-protector");
        cmd_add(c, "-DREC_MODULE");
        cmd_add(c, "-w");
        cmd_add(c, "-I");
        cmd_add(c, src);
        cmd_add(c, "-c");
        source_name(file, sizeof file, src, i);
        cmd_add(c, file);
        cmd_add(c, "-o");
        snprintf(obj, sizeof obj, "%s.o", file);
        cmd_add(c, obj);
    }
    compile_from = 0.35; compile_to = 0.9;
    bool ok = run_all(jobs, n, cpu_count(), log, compile_tick);
    for (int i = 0; i < n; i++) cmd_free(&jobs[i]);
    if (!ok) return false;

    cmd_t link = { .argc = 0 };
    cmd_add(&link, cc);
    if (zig) cmd_add(&link, "cc");
#ifdef _WIN32
    if (zig) { cmd_add(&link, "-target"); cmd_add(&link, "x86_64-windows-gnu"); }
#endif
    cmd_add(&link, "-shared");
#ifndef _WIN32
    cmd_add(&link, "-nostdlib");                 // the module needs nothing from the C library
#endif
    cmd_add(&link, "-o");
    cmd_add(&link, module);
    for (int i = 0; i < ALL_SOURCES; i++) {
        source_name(file, sizeof file, src, i);
        snprintf(obj, sizeof obj, "%s.o", file);
        cmd_add(&link, obj);
    }
    if (progress_cb) progress_cb("Linking", 0.92);
    ok = run_all(&link, 1, 1, log, NULL);
    cmd_free(&link);
    return ok && file_exists(module);
}

// TinyCC, loaded at run time (LGPL library shipped beside the program):
// compiles straight into memory, so nothing is cached but the C sources.
typedef struct TCCState TCCState;
typedef struct {
    TCCState *(*new_)(void);
    void (*delete_)(TCCState *);
    int (*set_output_type)(TCCState *, int);
    int (*add_include_path)(TCCState *, const char *);
    void (*define_symbol)(TCCState *, const char *, const char *);
    int (*add_file)(TCCState *, const char *);
    void (*set_options)(TCCState *, const char *);
    void (*set_lib_path)(TCCState *, const char *);
    int (*relocate)(TCCState *);
    void *(*get_symbol)(TCCState *, const char *);
} tcc_api_t;

static bool load_tcc(tcc_api_t *t, char *libdir, size_t n)
{
    const char *names[] = {
#ifdef _WIN32
        "tools\\tcc\\libtcc.dll", "libtcc.dll",
#else
        "tools/tcc/libtcc.so", "libtcc.so",
#endif
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s%s", i == 0 ? base_dir() : "", names[i]);
        void *lib = lib_open(path);
        if (!lib) continue;
        t->new_ = lib_sym(lib, "tcc_new");
        t->delete_ = lib_sym(lib, "tcc_delete");
        t->set_output_type = lib_sym(lib, "tcc_set_output_type");
        t->add_include_path = lib_sym(lib, "tcc_add_include_path");
        t->define_symbol = lib_sym(lib, "tcc_define_symbol");
        t->add_file = lib_sym(lib, "tcc_add_file");
        t->set_options = lib_sym(lib, "tcc_set_options");
        t->set_lib_path = lib_sym(lib, "tcc_set_lib_path");
        t->relocate = lib_sym(lib, "tcc_relocate");
        t->get_symbol = lib_sym(lib, "tcc_get_symbol");
        if (t->new_ && t->set_output_type && t->add_file && t->relocate && t->get_symbol) {
            snprintf(libdir, n, "%stools/tcc", base_dir());
            return true;
        }
    }
    return false;
}

static bool load_with_tcc(const char *src, char *err, size_t errlen)
{
    tcc_api_t t;
    char libdir[2048];
    if (!load_tcc(&t, libdir, sizeof libdir)) { snprintf(err, errlen, "TinyCC (libtcc) not found"); return false; }
    TCCState *s = t.new_();
    if (!s) { snprintf(err, errlen, "TinyCC failed to start"); return false; }
    if (t.set_lib_path) t.set_lib_path(s, libdir);
    if (t.set_options) t.set_options(s, "-w -nostdlib");
    t.set_output_type(s, 1 /* TCC_OUTPUT_MEMORY */);
    t.add_include_path(s, src);
    t.define_symbol(s, "REC_MODULE", "1");
    char file[2200];
#ifdef _WIN32
    // TinyCC's prologue calls its own stack probe for big frames (from its runtime library)
    snprintf(file, sizeof file, "%s/chkstk.S", libdir);
    if (t.add_file(s, file) < 0) { snprintf(err, errlen, "TinyCC could not assemble %s", file); return false; }
#endif
    for (int i = 0; i < ALL_SOURCES; i++) {
        source_name(file, sizeof file, src, i);
        if (t.add_file(s, file) < 0) { snprintf(err, errlen, "TinyCC could not compile %s", file); return false; }
        if (progress_cb) progress_cb("Compiling the game code (TinyCC)", 0.35 + 0.55 * (i + 1) / ALL_SOURCES);
    }
    if (t.relocate(s) < 0) { snprintf(err, errlen, "TinyCC could not link the game code"); return false; }
    rec_module_init_fn init = (rec_module_init_fn)t.get_symbol(s, REC_MODULE_INIT);
    if (!init || !rec_use_module(init)) { snprintf(err, errlen, "TinyCC build is not usable"); return false; }
    return true;    // the TCCState stays alive: it holds the code
}

/* ---------------------------------------------------------------- */

static int count_sources(const char *src)
{
    int n = 0;
    char file[2200];
    for (;;) {
        snprintf(file, sizeof file, "%s/rec_%03d.c", src, n);
        if (!file_exists(file)) return n;
        n++;
    }
}

bool rec_build_load_cached(char *msg, size_t msglen)
{
    char dir[2048], module[2300], src[2300], err[512];
    cache_paths(dir, sizeof dir, module, sizeof module, src, sizeof src);
    if (file_exists(module) && rec_build_load_file(module, err, sizeof err)) {
        snprintf(msg, msglen, "translated code loaded");
        return true;
    }
    char marker[2400];
    tcc_marker(src, marker, sizeof marker);
    nsources = count_sources(src);
    if (nsources && file_exists(marker) && load_with_tcc(src, err, sizeof err)) {
        snprintf(msg, msglen, "translated code built with TinyCC");
        return true;
    }
    return false;
}

// The bundled Zig (0.17) needs Windows 10 or later: on Windows 7 or 8 it
// can't start, so it isn't tried there.
static bool zig_runs_here(void)
{
#ifdef _WIN32
    typedef LONG (WINAPI *get_version_fn)(OSVERSIONINFOW *);
    get_version_fn get = (get_version_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = { sizeof v };        // (GetVersionEx would say 8 on any later Windows)
    if (get && get(&v) == 0 && v.dwMajorVersion < 10) {
        fprintf(stderr, "rec: Windows %lu.%lu: the bundled Zig needs Windows 10, trying the other compilers\n",
                (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion);
        return false;
    }
#endif
    return true;
}

static bool compile_and_load(const char *src, const char *module, const char *log, char *msg, size_t msglen)
{
    char cc[2048], err[512] = "";
    const char *only = getenv("KI2_BUILD_WITH");         // testing: zig, cc or tcc
    // 1. a bundled Zig
    snprintf(cc, sizeof cc, "%stools/zig/zig" EXE_EXT, base_dir());
    if (file_exists(cc) && (!only || !strcmp(only, "zig")) && zig_runs_here()) {
        char zc[2200];
        snprintf(zc, sizeof zc, "%scache/zig", data_dir());       // kept across builds
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc); _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1); setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
        if (progress_cb) progress_cb("Compiling the game code (Zig)", 0.35);
        if (build_with(cc, true, src, module, log) && rec_build_load_file(module, err, sizeof err)) {
            snprintf(msg, msglen, "translated code built with Zig");
            return true;
        }
    }
    // 2. a C compiler already on the system
    const char *names[] = { "cc", "gcc", "clang" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && (!only || !strcmp(only, "cc")); i++) {
        if (!find_program(names[i], cc, sizeof cc)) continue;
        if (progress_cb) progress_cb("Compiling the game code", 0.35);
        if (build_with(cc, false, src, module, log) && rec_build_load_file(module, err, sizeof err)) {
            snprintf(msg, msglen, "translated code built with %s", names[i]);
            return true;
        }
    }
    // 3. TinyCC
    if ((!only || !strcmp(only, "tcc")) && load_with_tcc(src, err, sizeof err)) {
        char marker[2400];
        tcc_marker(src, marker, sizeof marker);
        write_file(marker, "TinyCC was the only compiler that worked\n");
        snprintf(msg, msglen, "translated code built with TinyCC (slower; see README)");
        return true;
    }
    snprintf(msg, msglen, "no compiler worked (%s); running on the interpreter", err);
    return false;
}

bool rec_build(rec_progress_fn progress, char *msg, size_t msglen)
{
    progress_cb = progress;
    char dir[2048], module[2300], src[2300], log[2400], err[512];
    cache_paths(dir, sizeof dir, module, sizeof module, src, sizeof src);
    make_dir(dir);
    make_dir(src);
    snprintf(log, sizeof log, "%s/build.log", src);
    remove(log);

    // 1. boot the game on the interpreter until it has loaded its code. This
    // hidden boot must leave no trace: games write their settings sectors
    // while booting (KI1 counts power-ons), so those are put back after it.
    uint32_t dsw = hw.dsw;
    uint8_t *disk_head = malloc(IDE_TRACK_BYTES);
    memcpy(disk_head, ide_tracked(), IDE_TRACK_BYTES);
    machine_reset();
    for (int f = 0; f < SNAPSHOT_FRAMES; f++) {
        hw.p1 = hw.p2 = 0xffffffffu;
        run_frame();
        if (progress && f % 15 == 0) progress("Reading the game's code", 0.3 * f / SNAPSHOT_FRAMES);
    }
    uint8_t *snap = malloc(RAM_SIZE);
    memcpy(snap, ram, RAM_SIZE);
    if (memcmp(disk_head, ide_tracked(), IDE_TRACK_BYTES)) {
        memcpy(ide_tracked(), disk_head, IDE_TRACK_BYTES);
        ide_tracked_changed();
    }
    free(disk_head);
    hw.dsw = dsw;
    machine_reset();

    // 2. translate it
    if (progress) progress("Translating the game's code", 0.3);
    recomp_info_t info;
    char abi[2300], gen[2300], core[2300];
    snprintf(abi, sizeof abi, "%s/rec_abi.h", src);
    snprintf(gen, sizeof gen, "%s/rec_gen.h", src);
    snprintf(core, sizeof core, "%s/adsp_core.h", src);
    adsprec_image_t img[6];
    int nimg = sound_code_images(img, 6);
    bool ok = recomp_generate(snap, snap + RAM0_SIZE, src, &info, err, sizeof err) &&
              adsprec_generate(img, nimg, src, err, sizeof err) &&
              write_file(abi, rec_abi_h) && write_file(gen, rec_gen_h) && write_file(core, adsp_core_h);
    free(snap);
    if (!ok) { snprintf(msg, msglen, "translation failed: %s", err); return false; }
    nsources = info.files;

    // 3. compile and load
    ok = compile_and_load(src, module, log, msg, msglen);
    if (ok && rec_mod->image_hash != info.image_hash) {
        snprintf(msg, msglen, "the built code does not match the game; running on the interpreter");
        ok = false;
    }
    if (progress) progress("Done", 1.0);
    return ok;
}
