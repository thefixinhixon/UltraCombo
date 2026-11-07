# UltraCombo

Native PC ports of the Killer Instinct arcade games, for Windows and Linux:

| | Plays | MAME set | Versions |
|---|---|---|---|
| **UltraCombo** | Killer Instinct (1994) | `kinst` | L1.5D (default), L1.4 |
| **UltraCombo 2** | Killer Instinct 2 (1996) | `kinst2` | L1.4 |

Both are one program. It runs the arcade games' own code, translated to C
for your PC, with the board around it emulated: the Midway DCS sound board
and the IDE hard disk. On top come the things a home version needs: shaders
and CRT looks, rebindable controls and gamepads, save states, rewind,
run-ahead, training mode and rollback netplay.

**No game files are included.** You need your own copies of the arcade
games' files: the ROMs and the hard disk image (CHD) of MAME's sets.

## Setup

Put the files of one game or both in `run/`, the files of MAME's sets as
they come:

**Killer Instinct** (`kinst`):

- `ki-l15d.u98`: boot ROM, version L1.5D; `ki-l14.u98` (L1.4) is optional
- `u10-l1` … `u13-l1`, `u33-l1` … `u36-l1`: sound ROMs
- `kinst.chd`: the hard disk

**Killer Instinct 2** (`kinst2`):

- `ki2-l14.u98`: boot ROM, version L1.4
- `ki2_l1.u10` … `ki2_l1.u13`, `ki2_l1.u33` … `ki2_l1.u36`: sound ROMs
- `kinst2.chd`: the hard disk

The two games' file names don't clash. A CHD can also sit in a folder named
after its set (`run/kinst/kinst.chd`, `run/kinst2/kinst2.chd`), the way MAME
keeps it. A raw disk image (`kinst.raw`, `kinst2.raw`, from `chdman
extractraw`) works too, and is used if both are there. Each game's file
paths can also be set in its settings file (`ki1.ini`, `ki2.ini`).

The disk images are only ever read. A game's settings, audits and high
scores (the first 32 KB of its disk) go to `saves/<boot ROM>.settings`, one
file per game version. A raw image that an earlier version of this program
wrote to keeps its settings: they are copied into that file on the first
launch. `ultracombo --game ki2 --write-raw kinst2.raw` writes a disk out as
a raw image, the same as `chdman extractraw`, if you ever need one.

### Checking your files

These are the files of MAME's `kinst` and `kinst2` sets (the dumps MAME
lists as good). At startup the program compares your files with them and
warns, naming the file, if one doesn't match. To check them yourself use
`md5sum` (Linux), `certutil -hashfile FILE MD5` (Windows) or any ROM
manager. Every ROM is 524,288 bytes.

**Killer Instinct:**

| File | What | MD5 | SHA1 |
|---|---|---|---|
| `ki-l15d.u98` | boot ROM, L1.5D (default) | `6a63e4bbdef8ea9f0d26f7244341ca08` | `607394d4ba1713f38c2cb5159303cace9cde991e` |
| `ki-l14.u98` | boot ROM, L1.4 (optional) | `aff07bdb5bf4763bf81e243077cf9761` | `07254f20707377f7195e64675eb6458e663c1a9a` |
| `u10-l1` | sound | `9ee2b2cdda7ede8811a57507ef61e07b` | `810d455df8f385d76143e9d7d048f2b555ff8bf0` |
| `u11-l1` | sound | `bce2d8cd20e94d4406c19fd55d798946` | `0595909cb667c38ac7c8c7bd0646b28899e27777` |
| `u12-l1` | sound | `14bb46cef4884f8df5df1fa985e6d3bf` | `7a8ee405c118fd176b66353fa7bfab888cc63cd2` |
| `u13-l1` | sound | `ca0904ee517845eed5509426cf5d8560` | `ea4d1f153eb284f1bcfc5295fbce316bba6083f4` |
| `u33-l1` | sound | `da9e7f5f70320b51e6fdc23e9337484c` | `b22e365bc8d58a80eaac226be14b4bb8d9a04844` |
| `u34-l1` | sound | `207395792342705bea307ecb321a95bf` | `ee439f5162a2b3379d3f802328017bb3c68547d2` |
| `u35-l1` | sound | `a1758b43b42b32c25c770dc3ac7badc3` | `48c4c954ac9db648f28ad64f9845e19ec432eec3` |
| `u36-l1` | sound | `57b73bec5d0e8c90d8898e102ce4066b` | `cc78070cc41701e9a91fde5cfbdc7e1e83354854` |
| `kinst.raw` | disk, freshly extracted | `6d4c2f152c9a18ab3a9b05b8804306a8` | `a37a2c5e52ea936a715210d237874dd573bb002f` |

**Killer Instinct 2:**

| File | What | MD5 | SHA1 |
|---|---|---|---|
| `ki2-l14.u98` | boot ROM, L1.4 | `8681c9d373394b17f3499e8902055dab` | `aa7a2a9d72a47dd0ea2ee7b2776b79288060b179` |
| `ki2_l1.u10` | sound | `fa0c7f9dc7ee0995913a4769996a7d80` | `acfc9460cd5df01403b7f00b2f68c2a8734ad6d3` |
| `ki2_l1.u11` | sound | `dc87ed16f9d95cb0aa0ea0117ef4be09` | `fe7fc78f1c60b15f2bbdc4c455f55cdf30f48ed4` |
| `ki2_l1.u12` | sound | `0b034258a8561ac4bfa339a781777c4a` | `9997a83432cb720f65b40a8af46f31a5d0d16d8e` |
| `ki2_l1.u13` | sound | `6d42f7de396c7f9eb760e51af4fcdca2` | `7ff164fc2f079d039921594be92208973d43aa03` |
| `ki2_l1.u33` | sound | `aae93e81ac881c56d0aecab2f59c8797` | `0ab6dbfb76e9fa2db072e287864ad1f9d514dd9b` |
| `ki2_l1.u34` | sound | `116cbfd189879a3a601f1f7ccb64285c` | `48456a7b6592c40bc9c664dcd2ee2cfd91942811` |
| `ki2_l1.u35` | sound | `ec3103d67a528412da9cb91a2729df65` | `24a3ff009c8a7f5a0bfcb198b8dcb5df365770d3` |
| `ki2_l1.u36` | sound | `a03144702e28a9e89e50c5a377cb47b3` | `0fca72c40067034939b984b4bf32972a5a6c26af` |
| `kinst2.raw` | disk, freshly extracted | `2563b089b316f2c8636d78af661ac656` | `ab0242233d2eaf9d907abe246a54e09a8a2561a5` |

The raw disk checksums are for an image straight from the CHD. This program
never writes to it, but earlier versions did, and so do other programs that
use raw images; an image that has been played on no longer matches. To check
a disk, check the CHD instead. A CHD file's own MD5 depends on the chdman
version that made it, so compare the SHA1 that `chdman info -i FILE` prints
as "SHA1" (the one MAME uses):

| CHD | SHA1 (from `chdman info`) | MD5 of MAME's current file |
|---|---|---|
| `kinst2.chd` | `e7c9291b4648eae0012ea0cc230731ed4987d1d5` | `5970ca942c0c7c5f62f651bf93e15dc5` |
| `kinst.chd` | `81d833236e994528d1482979261401b198d1ca53` | `a542547abdff0a28df440c3a4d1cfdde` |

### Starting

Start `ultracombo` (`UltraCombo.exe` on Windows; the AppImage on Linux).
With both games installed it asks which to play: left/right (or a click)
chooses, Enter or the controller select button starts (shown on screen).
It remembers the last choice. With one game installed it starts that one
straight away. Menu → **Change game** goes back
to the choice, and `--game ki1` or `--game ki2` skips it (for shortcuts).

The first time you play each game, and each version of it, the program shows
"PREPARING THE GAME FOR THIS PC": it boots your game files on its
interpreter until the game has loaded its code, snapshots that code,
translates it to C, along with the sound board's DSP program from the sound
ROMs, and compiles it into a native module. That takes about a
minute, once; the module is cached in `cache/` (a new one is made if the
game files change). The compiler used is the first that works of:

1. Zig, bundled in `tools/zig`, which the release packages include
2. a C compiler already installed (`cc`, `gcc` or `clang`)
3. TinyCC, bundled in `tools/tcc`: it builds in seconds, but its code runs
   at roughly half the speed (still well above full speed on most PCs)

If none works the game runs on the interpreter, with a warning; see
`build.log` in `cache/src-*/`. All three produce code that behaves
identically, so netplay works between them.

Settings (`ki1.ini`, `ki2.ini`), the games' saves, the logs, screenshots,
states and the cache are written next to the program if that folder is
writable, else into the user data folder (`%APPDATA%\UltraCombo\` on
Windows, `~/.local/share/UltraCombo/` on Linux), whose `run/` folder is also
searched for the game files. With the AppImage, make a `run/` folder beside
the `.AppImage`: the game files go in it, and settings and the cache go
beside the AppImage too.

### Starting from a launcher

Launchers (LaunchBox and Big Box, Playnite, ES-DE, Pegasus, Attract-Mode,
Steam) start the program with a game's path, and it plays the game straight
from your MAME folder:

    UltraCombo.exe "D:\MAME\roms\kinst2.zip"
    ./UltraCombo-x86_64.AppImage ~/mame/roms/kinst.zip

The path can be a MAME set's zip (merged, split or non-merged; the clone
`kinst14.zip` starts L1.4), the CHD (`roms/kinst2/kinst2.chd`) or a folder
with the files. The zip's ROMs are unpacked once into `cache/roms/`; the CHD
is read where MAME keeps it, in `roms/kinst2/` beside the zip. Started this
way the game starts at once, **Esc quits** (F1 opens the menu), and Change
game is hidden: the launcher does the choosing.

| Launcher | Set up |
|---|---|
| LaunchBox / Big Box | Add an emulator: application path `UltraCombo.exe`, no parameters (the game's path is passed for you), for the games' platform (Arcade) |
| Playnite | Configure Emulators → Add: executable `UltraCombo.exe`, arguments `"{ImagePath}"` |
| ES-DE | A command for the arcade (or mame) system in a custom `es_systems.xml`: `"C:\Games\UltraCombo\UltraCombo.exe" %ROM%` |
| Pegasus | `launch: "C:/Games/UltraCombo/UltraCombo.exe" "{file.path}"` |
| Attract-Mode | Executable `UltraCombo.exe`, arguments `"[rompath][name][romext]"` |
| Steam (and Steam Deck) | Add a Non-Steam Game, launch options `--game ki2` (or a path as above) |

From a controller, hold Back+Start for two seconds to quit, however the game
was started. A `quit` key or pad button can be set in the menu (Controls →
Hotkeys / Gamepad hotkeys) for launchers that close games with their own key.

## Controls

| Action | Player 1 | Player 2 | Pad (Xbox names) |
|---|---|---|---|
| Move | Arrow keys | I J K L | D-pad / left stick |
| Quick / Medium / Fierce punch | A S D | T Y U | X / Y / RB |
| Quick / Medium / Fierce kick | Z X C | G H B | A / B / RT |
| Start | 1 | 2 | Start |
| Coin | 5 | 6 | Back |

The games run in **free play** by default (the board's coinage DIP
switch): from the attract demo, the first Start goes to the title screen
and the second starts the game, as on a free-play cabinet. In UltraCombo 2,
with free play off, `auto_coin` makes Start insert the coins it needs. The
first pad connected is player 1, the second player 2.

| Hotkey | |
|---|---|
| Esc or F1 | Menu (pad: Guide, or tap Back+Start); Esc quits when a launcher started the game |
| Hold Back+Start (pad) | Quit, after two seconds |
| F5 / F7 | Save / load state in the current slot |
| F6 | Next state slot (1-9) |
| Backspace (hold) | Rewind |
| Tab (hold) | Fast-forward |
| P | Pause |
| . | Frame advance (pauses first) |
| F11 or Alt+Enter | Fullscreen |
| F12 | Screenshot to `screenshots/` |
| F3 | Input display |
| F4 | Training mode |
| F8 / F9 | Record / play the training dummy |
| F10 | Performance overlay |
| = / - / M | Volume up / down / mute |
| F2 | Operator test menu |
| 9 | Service credit |
| Page Up / Page Down | The cabinet's own volume buttons |

Each game keeps its own settings; changes made in the menu are saved to its
`.ini` file when the menu closes.

## The menu

**Graphics:** fullscreen (borderless, or exclusive at a chosen display
mode), window size, scaling (fit 4:3, integer, stretch), shader, shader
parameters, frame pacing and the performance overlay.

**Controls:** rebind both players' keys, the gamepad buttons and all hotkeys
(select one, press the new key or button; Backspace unbinds). Also there:
**left+right = neutral** (SOCD cleaning, on by default: left and right
together count as neither, up and down together as up, like fight sticks),
**rumble** when your character is hit, and the stick deadzone.

**Game & system:**

- **Game version** (Killer Instinct: L1.5D or L1.4, when both boot ROMs are
  there): pick it, then Enter switches. That resets the game, like swapping
  the ROM on the cabinet; each version keeps its own settings and high
  scores. L1.4 is the original release, with the bugs and infinite combos
  that L1.5D fixed.
- **CPU difficulty** (Extra easy to Extra hard): written to the disk's
  adjustments exactly as the game's operator menu does, so it applies after
  Reset.
- **Free play, blood level, finishing moves, attract-mode sound** and, in
  Killer Instinct, **blood colour** (red or white): the board's DIP
  switches, read at power-on, so they apply after Reset.
- **Disk loading** (Original drive or Instant): Original drive takes as long
  as the cabinet's hard disk did, so the VS screen holds while the fight
  loads and the announcer finishes, as in the arcade. Instant skips the wait.
  It applies straight away; in netplay the host's choice applies to both.
- Volume, mute or pause in the background, auto-coin (UltraCombo 2),
  run-ahead and rewind.

Button labels follow the last controller used; keyboard input switches menu
prompts back to keyboard names. Xbox, PlayStation and Nintendo Switch layouts
are detected through SDL. Before a controller is used, and for controllers
SDL cannot identify, labels fall back to Xbox-style A/B/X/Y names. Controls →
P1/P2 button labels overrides the detected family when an adapter hides it;
Positions remains available for South/East/West/North names. This changes
labels only, not the bindings. SDL's Nintendo letter-based and positional
mappings are both respected, including the exchanged A/B and X/Y positions.
The shared gamepad-binding page shows labels for the last pad used.

The window's size and position are remembered between runs.

## Graphics

**Shaders** are drawn by the GPU. Four are built in:

- **Sharp**: crisp pixels, evenly sized at any window size
- **Smooth**: sharpened bicubic; suits the games' pre-rendered 3D art
- **Scanlines**, **CRT**: simple, fast CRT looks

Any **libretro GLSL preset** (`.glslp`, the format of RetroArch's shader
collection) under `shaders/` works too, multi-pass presets, lookup
textures and all. Bundled: crt-lottes, crt-hyllian, crt-nobody, crt-1tap/2tap,
ScaleFX and xBR. `make shaders-extra` adds xBRZ, crt-guest-dr-venom,
crt-royale, crt-geom, crt-easymode, zfast-crt and crt-aperture (GPL
licensed, kept as separate files in `shaders/extra/`). Each preset's
`#pragma parameter` settings can be adjusted live in Shader parameters, and
are remembered. `ultracombo --test-shaders` checks that every preset
compiles.

**Lola Retro** (`lola-retro/arcade-15khz`, and `-flat` without curvature)
models a 15 kHz arcade monitor on RGB: the beam, slot mask, glow and curved
glass, built from a real tube's measurements. Its parameters set the colour,
curvature, mask strength, glow and overscan. Set **Around the tube** to 1 to
draw the monitor's casing in a room (2–11: moving scenes), and use the
*stretch* scaling so it fills the window. The tube itself stays 4:3. It is
the heaviest preset: it needs OpenGL 3.3 and a reasonably recent GPU. From
[Lola Retro](https://github.com/zoran123456/lola-retro) by Zoki (MIT); `make
shaders-lola` regenerates it.

Without OpenGL the game falls back to plain SDL drawing (sharp or smooth).

## Training mode

Turn it on with F4 or in Menu → Practice. Health refills a moment after each
combo ends (so you can still read the damage) and the round clock stops.
In Killer Instinct this works on each of a fighter's two health bars.

For a sparring partner, start a two-player game (press 2 to join), then:

1. **F8**: record. Your controls now drive player 2 (up to 10 seconds).
2. **F8**: stop recording.
3. **F9**: the dummy repeats the recording on a loop; F9 again stops it.

Directions are recorded as pressed (left/right on screen), so record from
the side the dummy will be on.

F3 shows both players' recent inputs in numpad notation (6 = forward when
facing right, 2 = down, 5 = neutral) with the number of frames each was held.

## Netplay

Two players over the internet with rollback netcode: your own inputs
respond immediately, the opponent's are predicted and corrected, and the
game stays perfectly in sync. Linux and Windows can play each other.

- **Host:** Menu → Netplay → *Host a game* (or `ultracombo --game ki2
  --host`). Your UDP port (`port` in the game's `.ini`, default 7845) must
  be reachable: forward it on your router, or both use a VPN such as
  Tailscale or ZeroTier.
- **Join:** Menu → Netplay → *Address* (type the host's IP or name), then
  *Join a game* (or `ultracombo --game ki2 --join HOST[:PORT]`).

Both players need the same game, version, ROM and disk image (checked when
connecting); the host's settings sectors and DIP switches are used. When
the other player connects, both games restart at the title screen. You
play with your usual controls whichever side you are on. The corner shows
ping, input delay and the last rollback.

**Input delay** (default 2 frames) trades a little responsiveness for
fewer corrections: 1-2 for nearby players, 3-4 across a continent.
During netplay the game cannot be paused, rewound or reloaded, and both
sides run at the arcade's 58.96 Hz.

## Save states, rewind and run-ahead

States go to `states/slot1.ki1s` … `slot9.ki1s` (Killer Instinct) and
`slot1.ki2s` … `slot9.ki2s` (Killer Instinct 2), about 6 MB each. They
include the disk's settings sectors, and only load into the same build,
game and version.

Rewind keeps about 50 seconds by default (`rewind_mb = 256`).

Killer Instinct 2 shows the result of an input one frame after it reads it.
Run-ahead 1 hides that frame by quietly emulating one frame ahead each frame;
it costs about one extra frame of CPU time. Killer Instinct has no such lag
(a press shows on the very next frame), so run-ahead is off by default for
both, as the arcade played.

Everything above can be changed in the games' `.ini` files, which are
written with comments on first run. They also have video scaling (`fit`,
`integer`, `stretch`), smoothing, frame pacing, audio latency and the boot
skip.

## Frame pacing

The games run at 58.96 Hz. With `pacing = auto` a ~60 Hz display is driven
by vsync for perfectly smooth motion (the game runs 1.7% fast to match);
other refresh rates are timed by the sound card at the original speed.
`pacing = vsync` also keeps a 120 Hz or 144 Hz display tear-free, with the
game timed by the clock at its own speed.

If vsync doesn't actually wait for the display (a graphics driver setting
can force it off), the program notices within a few seconds, says "Vsync
isn't working here", and times the game by the sound card instead, and the
menu shows Frame pacing as "not working". Choosing Frame pacing again tries
vsync again.

## Building

Needs gcc, SDL2 and python3:

    make            # ultracombo: both games; their code is built at first launch
    ./ultracombo

For developers, `make recomp` links Killer Instinct 2's translated code into
the program instead (it boots your KI2 files once, about a minute). The
generated C goes to `build/rec/`, derived from your ROM: that binary contains
game code and must not be shared. `tools/regress.sh` runs a fixed scripted
fight and prints a state hash, to check that a change doesn't alter
emulation (see `CLAUDE.md`).

### State file tests

`make test-state-files` runs state-file admission and restoration tests with synthetic RAM,
disk settings and fixed-size device serializers; no SDL or game files are needed.
It checks valid round trips, truncated cores/pages/literals, invalid run lengths,
zero-progress tokens, incompatible headers and trailing data. A rejected file must
leave live memory, devices and rewind history unchanged. The on-disk format is
unchanged. These are file-format tests, not an original-game regression run.

### Controller label tests

`make test-controller-labels` runs controller-label, last-device prompt and configuration tests
without game files. It uses SDL virtual controllers to check both Nintendo mapping
modes, physical south-button activation and its displayed label, manual overrides,
disconnection, axis noise and keyboard/controller prompt switching. Legacy action
bindings must survive configuration load/save unchanged. Physical controller checks
still need real hardware; virtual devices do not establish hardware compatibility.

### Windows

Cross-compiled from Linux with MinGW-w64 (`gcc-mingw-w64-x86-64`) and the
official SDL2 MinGW package, unpacked into `deps/sdl2-mingw/`:

    make windows    # -> dist/windows/UltraCombo.exe

On Windows each game's log goes to `ki1.log` / `ki2.log` beside the
executable. Linux and Windows builds emulate identically (same state after
the same inputs).

### Release packages

    make packages   # -> dist/UltraCombo-Windows.zip, dist/UltraCombo-Linux.tar.xz,
                    #    dist/UltraCombo-x86_64.AppImage and its .zsync

These hold the program, Zig and TinyCC, SDL2 (Linux: in `lib/`), shaders and
licences: no game files and no game code.

Pushing a version tag (`git tag v1.2.0 && git push origin v1.2.0`) builds
all of them on GitHub Actions (`.github/workflows/release.yml`) and attaches
them to a draft pre-release for that tag: write the notes there and publish
it. If a release for the tag already exists, the packages are added to it.
Running the workflow by hand (Actions > Release > Run workflow) builds the
packages without releasing.

`tools/fetch-deps.sh` fetches and builds everything below into `deps/`,
pinned by version and checksum (the workflow uses it; it lists the
system packages it needs). `make packages` expects, in `deps/`:

- Zig 0.17 for Linux and Windows (`zig-linux/`, `zig-windows/`);
- TinyCC's `libtcc.so` built with that Zig against glibc 2.31
  (`tcc-lnx231/`), and `libtcc.dll` (`tcc-win/`);
- SDL2 2.32.10 built with that Zig against glibc 2.31 into
  `deps/sdl2-lnx/build` (configure with
  `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu` and `-idirafter
  /usr/include` so it finds the X11/Wayland/audio headers);
- the SDL2 MinGW package (`sdl2-mingw/`);
- AppImage's static runtime (`dl/appimage-runtime/runtime-x86_64`, from
  github.com/AppImage/type2-runtime).

The Linux program is built with Zig against glibc 2.31, so it runs on
distributions from about 2020 on, and on the Steam Deck, whatever glibc the
build machine has. `tools/package.sh` refuses a build that needs a glibc
symbol newer than 2.29. The AppImage is the Linux package packed
with `mksquashfs` behind that runtime.

The AppImage carries update information for AppImageUpdate: the newest
GitHub release, pre-release or not, and its `UltraCombo-x86_64.AppImage.zsync`.
`zsyncmake` makes that file (package `zsync`; without root, `apt-get download
zsync` and `dpkg-deb -x` it into `deps/zsync`); upload it to every release
next to the AppImage, or updating breaks.

"Killer Instinct" is a trademark of its owners; this project is not
affiliated with them. Keep that in mind when naming anything you publish.

## Licence

UltraCombo is MIT-licensed (`LICENSE`). That covers this project's own code
only: the games, their code and their data belong to their owners, and you
need your own copies. Bundled third-party parts keep their licences:
stb_image (public domain / MIT), the shaders in `shaders/` (see
`shaders/README.txt`), libchdr, which reads CHDs, with the decompressors it
bundles (BSD, public domain, MIT; see `src/third_party/libchdr/SOURCE.txt`),
and in the release packages SDL2 (zlib), Zig (MIT) and TinyCC (LGPL 2.1).
