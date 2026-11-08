# UltraCombo

Native ports for the Killer Instinct arcade games (C11 + SDL2 + OpenGL), for
Linux and Windows: **UltraCombo** (Killer Instinct) and **UltraCombo 2**
(Killer Instinct 2), both in one program that asks which to play. It runs the game's own MIPS code, statically translated to
C, with the hardware around it emulated. Players supply their own game files.
README.md is the user manual; this file is for working on the code.

## Hard rules: game data and game code

The game's ROMs, disk image and code are copyrighted. This repo is public.

- Never commit or ship anything made from the game files:
  - ROM or disk data, RAM snapshots (`build/snap/`), translated code
    (`build/rec/`, any `rec_*.c` / `rec_tab.c`, `cache/`);
  - audio recordings (`KI2_WAV`), screenshots or frame dumps, save states;
  - any binary built after `make recomp`, which links the game code in.
- Distributable programs contain no game code. They build it on the player's
  machine at first launch (`rec_build.c`). Release packages come only from
  `make packages`.
- Never `git add -A` / `git add .`: the working folder holds builds with game
  code and test output. Add files by name and read `git status` first.
- Commit as `thefixinhixon <132857351+thefixinhixon@users.noreply.github.com>`
  (set in the repo's git config). Never commit a personal email.
- Name the products UltraCombo and UltraCombo 2. Use "Killer Instinct" only to
  say what they play, never as a product's name. It is a trademark.

## Layout

| Area | Files |
|---|---|
| **What differs per game**: names, files, disk geometry, I/O port addresses, RAM locations, icon text | `game.h`, `game.c`, `game_ki1.c`, `game_ki2.c`; the startup choice and picker: `main.c` (`choose_game`), `picker.c` |
| CPU: MIPS R4600 interpreter, TLB, exceptions | `cpu.c/h`, `disasm.c` |
| Memory map and I/O | `mem.c/h`, `machine.h` |
| IDE disk: raw image or MAME CHD (read-only, via libchdr), settings sectors in memory and in `saves/<boot ROM>.settings` | `ide.c/h`, `third_party/libchdr/` |
| Sound: DCS board, ADSP-2105 core | `dcs.c`, `adsp.c/h` (run loop, interpreter), `adsp_core.h` (state and instruction semantics, shared with translated sound code), `sound.h` |
| The sound translator (ADSP-2105 to C), used at run time | `adsprec.c/h`; images to translate: `sound_code` in the profile |
| Translated code: dispatcher, maps | `rec.c/h` |
| The translator (MIPS to C), used at run time | `recomp.c/h` (`tools/recomp.py` is the original) |
| Contract between program and translated code | `rec_abi.h`, `rec_gen.h` |
| First-run build and cache | `rec_build.c/h`; `rec_none.c` = no linked-in code |
| Save states, rewind, run-ahead | `state.c/h` |
| Rollback netplay | `net.c/h`, `netplay.c/h` |
| Window, menu, pacing, input | `frontend.c`, `osd.c/h`, `config.c/h`, `training.c/h` |
| Video, OpenGL, libretro shader presets | `video.c/h`, `shaders.c/h`, `gl_funcs.h` |
| Game-setting tweaks, e.g. CPU difficulty | `adjust.c/h` |
| Icon (drawn in code) and PNG writer | `icon.c/h`, `png.c/h`, `ki2.rc` |
| Program entry, CLI, `data_dir()`/`base_dir()`/`find_data()` | `main.c` |
| Started by a launcher with a game's path: which game, MAME zips unpacked into `cache/roms/` (miniz from libchdr's deps), the folders `find_data()` looks in first | `launch.c/h` |

## Two games

Both games run on the same board (MAME's `kinst` driver), so there is one
emulator and one program. The profile (`game.h`) is chosen at the top of
`main()` (`--game`, the only game installed, or the picker) before anything
reads `game`; Menu > Change game restarts the program into the picker.
Translated code records its game, so modules never cross over. Anything
game-specific goes in the profile, never as a constant in shared code. A feature that needs a RAM location the profile
doesn't know yet (0) must turn itself off. Per-game files are named after
`game.id`: `ki1.ini` / `ki2.ini`, `.log`, `slotN.ki1s` / `.ki2s`, screenshots.
Changes to shared code must keep UltraCombo 2's regression hashes.

## Disk images

The program reads the disk as a raw file or as MAME's CHD, and never writes
to either. The only sectors the games write are the first 32 KB (settings,
audits, high scores). They live in memory, so states cover them, and are
saved to `saves/<boot ROM stem>.settings`, one file per game version. A
write outside them would only be kept for the session, with a warning; none
has been seen. `ide_image_id()` hashes the contents, so a raw image and a CHD
of the same disk share cached builds, states and netplay. The profile holds
MAME's CRC32s and CHD SHA1, and the files are checked at startup.

Reads take the time the cabinet's drive took (`ide.c`: seek, command
overhead, ~4 MB/s per sector) unless `disk_speed = instant`. The games
depend on it: the VS screen holds while the fight loads, and the announcer
finishes (issue #10). The figures are typical early-1990s drive numbers, not
measured on a cabinet. Slower isn't safer: at 2 MB/s KI1's attract mode
fell ~4 s behind its music (which runs on the sound board's own clock) and
the logo's "Killer Instinct" before Glacius was cut; MAME's near-instant
drive cuts "Fight On" instead (#11, #10). Check both when changing them. The setting is `hw.instant_disk`, kept like the DIP
switches: in states, and the host's value applies in netplay. Instant
emulates exactly as before drive timing existed.

## CPU timing

The R4600 runs one instruction per cycle at 100 MHz; Count ticks at half
that. Reads from the boot EPROM cost 128 extra cycles, as in MAME: without
them the boot is over before the sound board is ready and the attract mode
plays silently. `run_frame` gives the CPU 256-cycle slices, and whatever it
runs past a slice (translated code stops only at the end of a block or loop)
is taken off the next one (`m.cpu_over`, in the state). Without that,
translated code got ~5% more cycles per frame than the interpreter, and the
games' Count-timed waits ran fast: KI1's attract coin played at nearly twice
its speed (issue #11). Translated and interpreted runs should show the same
timing on screen; compare them when changing anything here.

The picture is taken at the start of vblank (`video_vblank()`, line 240), as
MAME draws it, not at the end of the frame: the games flip buffers early in
vblank and may still be drawing into the new one, which made fighters
vanish on single frames as they walked in (issue #5).

## Determinism (netplay, rewind and run-ahead depend on it)

- Emulation must be bit-identical across compilers (gcc, clang/Zig, MinGW,
  TinyCC modules) and across linked-in vs run-time-built code. Save states
  and netplay compare raw bytes.
- `memset` any local struct before copying it into a state blob. Padding
  bytes once made gcc and Zig builds hash differently.
- Frontend code that writes to emulated RAM must call `ram_touch()`, or
  the dirty-page journal misses the write.
- After any change to emulation, run the regression test (below); the hash
  must not change. If a change is meant to alter emulation, say so and record
  the new hash. `state_compat_id()` must differ between builds that emulate
  differently.

## Translated-code modules

- `rec_abi.h` and `rec_gen.h` are compiled into the module **without the C
  library** (`REC_MODULE`, `-nostdlib`). Keep them free of libc headers and
  calls, and make everything work under TinyCC too: give GCC builtins a
  fallback, as the existing overflow and 64-bit multiply helpers do.
- Both headers are embedded into the program (`tools/embed.py` →
  `build/rec_headers.c`), so rebuilding the program picks up changes.
- Bump `REC_ABI_VERSION` (`rec_abi.h`) when the program/module contract changes,
  and `RECOMP_VERSION` (`rec_build.c`) when the generated C changes. Both are
  in the cache key; forgetting them leaves players running stale modules.
- Compiler order at first launch: bundled Zig → system cc/gcc/clang → TinyCC
  (about half the speed) → interpreter. All four must give the regression
  hash.
- `adsp_core.h` is compiled into the module too (embedded like the other
  two): same rules. The sound DSP's translated code must stay bit-identical
  to the interpreter, so it may stop at any instruction: every instruction
  does what one pass of `adsp_run`'s loop does, with the same functions.
  Keep instruction semantics in `adsp_core.h` only, and change the
  `ADSP_GROUPS` table rather than decoding in the translator. The `adsp_t`
  layout is in save states: don't add fields to it.
- Translated sound code runs only where program memory still holds the words
  it came from (`code_ok` in `adsp.c`). The board must report every program
  word it changes (`adsp_pm_changed`, `adsp_code_reload`), including data
  writes that alias external program RAM.

## Product rules

- Enhancements are options on top of authentic defaults. Keep the boot and
  attract intros: they are part of the game.
- Graphics stay on OpenGL; no Vulkan.
- No "resume where you left off" on launch.
- Settings live in the table in `config.c` (`MAX_OPTS`), and `ki2.ini` is
  written from it with comments.
- Writable files go in `data_dir()`: beside the program if that's writable,
  beside an AppImage that has `run/` next to it, else the user data folder
  `UltraCombo2`. Bundled read-only files come from `base_dir()`.
- Bundled shaders in `shaders/` must be permissively licensed. GPL presets are
  only fetched by `make shaders-extra`.

## Workflows

Build and run (needs gcc, SDL2, python3; game files in `run/`):

    make                 # ultracombo, both games; game code is built at first launch per game
    make recomp          # developer build with the game code linked in (never share it)
    ./ultracombo

Regression test: a fixed 4400-frame boot, coin-up and two-player fight,
printing a state hash:

    tools/regress.sh [-g ki1|ki2] PROGRAM DISK_HEAD [RUN_DIR] [WORK_DIR]

The disk image is only read (by the program and by this script). The saved
head goes into a settings file in the work folder, `DISK=` picks the raw
image or the CHD (same hash), and the script fixes the DIP switches, so the user's settings don't matter. Compare the hash
against the reference for the same game and disk head. Translated and
interpreted runs give different hashes, both valid, because translated code
takes interrupts at block boundaries. What must match is every build of the
same translated code (any compiler, linked-in or built at first launch).
Useful environment variables:
- `KI2_NOREC=1`: interpreter only, which gives a different but also fixed hash.
- `KI2_NOADSP=1`: sound DSP on the interpreter, game code still translated
  (same hash either way).
- `KI2_DISK=original|instant`: drive timing, overriding the setting
  (`regress.sh` pins `original`, the default).
- `KI2_LOAD_MODULE=path.so`: use a given module.
- `KI2_DISPLAY_HZ=N`: pretend the display refreshes at N Hz, to test frame
  pacing (under Xvfb vsync never waits, so ~60 Hz falls back to audio).
- `KI2_BUILD=1 KI2_BUILD_WITH=zig|cc|tcc`: build the module now, headless,
  with that compiler.

Packages (no game files, no game code):

    make packages        # dist/UltraCombo-Windows.zip, -Linux.tar.xz, -x86_64.AppImage (+ .zsync)

Pushing a version tag builds them on GitHub Actions
(`.github/workflows/release.yml`) into a draft pre-release.
`tools/fetch-deps.sh` fills `deps/` from pinned, checksummed downloads; when
a dependency changes, change it there (its checksum is the workflow's cache
key). These need, in `deps/`:
- Zig 0.17 for Linux and Windows;
- `libtcc.so` built with Zig against glibc 2.31 (`tcc-lnx231/`), and `libtcc.dll` (`tcc-win/`);
- SDL2 2.32.10 built with Zig (`sdl2-lnx/`), and the SDL2 MinGW package (`sdl2-mingw/`);
- the AppImage type2 runtime (`dl/appimage-runtime/`).

And `zsyncmake`: package `zsync`, or that package unpacked into `deps/zsync`. Every release must carry
`UltraCombo-x86_64.AppImage.zsync` beside the AppImage: the AppImage's
update information points at it.

See README.md, "Release packages". The Linux program targets glibc 2.31.
The most it may need is GLIBC_2.29; `tools/package.sh` checks that.

Windows: `make windows` cross-compiles with MinGW. Zig won't run under Wine,
so test the Windows module path with `KI2_BUILD_WITH=tcc` there; the Zig path
needs real Windows.

Windowed testing without a display: run it under `Xvfb :99` with
`SDL_AUDIODRIVER=dummy`, and send keys with `xdotool keydown` / `keyup` held a
few frames, because input is sampled once per frame and quick `key` taps get
missed. Run a copy of the program from a scratch folder so it gets its own
`ki2.ini`.

Headless switches: see `usage()` in `main.c` (`--headless`, `--script`,
`--save-at`, `--load`, `--rewind-test`, `--runahead`, `--net-host`/`--net-join`/`--net-sim`).
Debug environment variables, all read with `getenv` in `src/` (grep them
for details): `KI2_LOG_EXC`, `KI2_LOG_IDE` and `KI2_LOG_SOUND` (logging; also the share of DSP cycles that ran translated, at exit),
`KI2_DUMP_AT` (RAM snapshot at a frame), `KI2_WAV` (record audio, output is
game-derived), `KI2_CONSOLE` (Windows: log to the console instead of
`ki2.log`). `KI2_DUMP_AT` also takes a list (`F1,F2,...` → `ram2_<F>.bin`),
`KI2_TRAINING=N` applies training mode headless from frame N,
`KI2_POKE=F:OFFSET:VALUE,...` writes a byte of ram2 (hex) before frame F, `KI2_SWITCH_TEST=N:V`
switches to game version V after N frames (must equal a fresh start).

Finding a game's RAM locations: run two deterministic headless scripts
that differ in one thing (hits or no hits), dump RAM at several frames,
and compare with numpy. Disk settings: change the value in the game's own
operator menu (`test` button in scripts) and diff the settings files.

## Style

Match the surrounding code: C11, 4-space indent, short functions with short
comments that say why, British spelling in prose ("licence", "colour"). The
`-Wextra` warnings already in the build are known. Don't add new ones.
