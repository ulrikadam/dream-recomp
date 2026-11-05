# dream-recomp

dream-recomp takes a Sega Dreamcast disc image and produces a native executable for macOS, Windows
or Linux. It reads the game's SH-4 machine code once, ahead of time, translates it into C++, and
hands that to your compiler. The program you end up with contains no SH-4 instructions.

This is static recompilation, the approach taken by
[N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) and
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), aimed at the Dreamcast's SH-4 and at the
Katana SDK that most Dreamcast games were built with.

You supply the game. Nothing here contains Sega code or data, and none is ever committed. You need
to dump your own game legally.

## About this fork

This is a fork of [phobos665/dream-recomp](https://github.com/phobos665/dream-recomp), the original
project by phobos665. The fork adds the work below, which has also been offered upstream:
[#12](https://github.com/phobos665/dream-recomp/pull/12),
[#13](https://github.com/phobos665/dream-recomp/pull/13) and
[#14](https://github.com/phobos665/dream-recomp/issues/14). When those are merged, this fork follows
upstream again. The licence is unchanged: GPL-2.0.

**Titles tested on Windows 11 with an Xbox pad.** Each game's `[status]` in its `games/<id>/<id>.toml`
records how far it gets.

| Title | Status | Notes |
|---|---|---|
| San Francisco Rush 2049 (PAL) | Playable | Menus, memory card, races, sound. |
| Daytona USA 2001 (PAL) | Playable | Menus, memory card, races, sound. Some grass at the screen edge draws white or black; no shadows yet. |
| Crazy Taxi 2 (PAL) | Playable | Menus, memory card, missions, sound. No code changes were needed. |
| Crazy Taxi (PAL) | Playable | Menus, memory card, play, sound. |
| F355 Challenge (PAL) | Playable | Menus, memory card, races, sound. The save screen's help text overlaps. |
| Sega GT (PAL) | Playable | Intro movie, menus, a full race. |
| 18 Wheeler: American Pro Trucker (PAL) | Playable | Menus, truck select, races. |
| V-Rally 2 Expert Edition (PAL) | Playable | Menus, memory card, races, sound. |
| Tokyo Highway Challenge (PAL) | Boots | Added by the launcher alone, first try; played once, not yet rated. |
| Crazy Taxi (USA), Rez, Ikaruga | From upstream | Not re-tested here. |
| Windows CE titles (Sega Rally 2...) | Unsupported | They boot through IP.BIN and run with the MMU on. |

Windows CE development now has an [experimental runtime](docs/wince-support.md): a strict SH-4
memory adapter, a PE32 DLL loader, a COREDLL HLE subset with guest heaps/virtual pages/CRT services,
recursive critical sections/mutexes and event waits, and guest DirectDraw/Direct3D/DirectSound
COM interfaces backed by CPU rendering and PCM mixing. `dream-wince` runs original SH-4 PE fixtures,
captures their graphics/audio, tests synchronization across an original EXE/DLL pair and audits
real module imports. Commercial CE titles
remain unsupported; this path does not yet provide a complete Windows CE or DirectX environment.

**What the fork adds**

- Translator fixes:
  - a hang on stores near 4 GiB;
  - branches into delay slots;
  - resumed functions losing their resume pc;
  - `LDTLB`;
  - jumps that keep the return address their function was called with (direct, through a
    register, and back to the caller with `bra`);
  - byte switch tables read through `extu.b`.
- Emulation fixes:
  - HLE fixes for the BIOS checksums Rush 2049 runs, and for GD-ROM PIO reads;
  - video textures;
  - per-pixel sorting of translucent polygons;
  - double-buffered TA parameter lists, and TA resets that keep what was sent;
  - the interpreter leaving a run entered by a jump at its own return;
  - audio paced on the sound device, so a slow frame no longer stutters or drops samples;
  - a 64 MB stack for game executables on Windows;
  - polygons reaching far off screen cut to a guard band, and the frame's write clip honoured;
  - textures dropped when the CPU writes over them in video memory.
- A settings screen for the controller and memory cards (F1 in a game).
- `dream_launcher`: a game library window, in French, with a bar along the bottom of every screen
  (JOUER, AJOUTER UN JEU, CARTES VMU, MANETTE, QUITTER) that the shoulder buttons, Q/W or a click
  move along.
  - **JOUER** lists your games with their status and starts them.
  - **AJOUTER UN JEU** lists the disc images in your dumps folder, marks those already added, and
    brings a title up alone: it extracts the disc, builds the game, runs it headless to find the
    code discovery missed, folds that into the configuration and builds again
    ([docs/per-game-bring-up.md](docs/per-game-bring-up.md#the-launcher-does-the-first-three-steps)).
    After each session it does the same with what the session reached, so a game improves as it is
    played.
  - **CARTES VMU**: memory cards shared by every game, as real ones are. A VMU drawn by the program
    shows each save on its screen; saves are copied between cards or deleted, new cards made, and a
    card plugged into the pad's slot A1 or A2 for every game. The first start gathers the older
    one-card-per-game saves onto VMU 1.
  - **MANETTE**: the controller bindings every game reads, changed in the launcher's own look.

**Playing a game here**, once the build in [docs/dev-setup.md](docs/dev-setup.md) works:

1. Run `build/launcher/dream_launcher`. With MSVC, start it where the compiler's environment is set
   (a Developer prompt, or a script that calls `vcvars64.bat`), so it can build games.
2. Choose AJOUTER UN JEU and pick your disc image. The first build of a title takes some minutes.
3. Choose JOUER.

By hand instead, the launcher's first steps are:

```bash
.venv/Scripts/dcdisc new-game path/to/your.gdi
cmake --build build
```

On Linux and macOS, `dcdisc` is in `.venv/bin/` instead.

Bug reports and title reports are welcome in this fork's issues.

## Core concepts

**Translation happens at build time.** The translator reads the game's boot executable, decodes it
into SH-4 instructions, and emits one C++ function per guest function. Guest registers become local
variables. Your compiler then optimises the result like any other source file.

**Coverage is partial and you close the gap by hand.** Discovery starts at the entry point and
follows calls and branches, which finds roughly 79–90% of a typical binary. What it misses is code
reachable only through computed jumps, jump tables and data-driven dispatch. The build prints how
much it decoded:

```text
coverage: 490480 of 621888 bytes decoded (78.9%); 118480 bytes are neither
instruction nor literal pool, in 727 runs of 16 bytes or more
```

The runs it lists are the regions to investigate when the game stops somewhere. You seed the
missing addresses in the title's config and rebuild, and each seed usually pulls in more than one
function. See [docs/emitter-design.md](docs/emitter-design.md).

**A development interpreter covers what is still missing.** Building with
`DREAM_DEV_INTERPRETER=ON` keeps an SH-4 interpreter in the binary as a fallback for untranslated
code. Without it, the program stops at the first gap. No title is complete enough to build without
it yet.

**Hardware is emulated, console software is replaced.**

| Layer | Approach | What that means |
|---|---|---|
| CPU | Compiled | Each guest function becomes a C++ function. Registers are local variables. A release build has no interpreter. |
| Memory | Emulated | 16 MB RAM, 8 MB video memory in both of the hardware's two views, store queues. Address translation is a mask and an index. |
| Graphics | Emulated, low level | PowerVR2 display lists are decoded and drawn through Vulkan. The game talks to the hardware as it always did. |
| Audio | Emulated, low level | The game's own sound driver runs on an emulated ARM7, feeding an emulated mixer. |
| BIOS and disc | Replaced, high level | Console BIOS calls and the GD-ROM filesystem are answered by native code. No BIOS image is needed. |

Anything the game touches directly is emulated at the hardware level, because that is where games
depend on exact behaviour. The console's own firmware is replaced with native equivalents, which
also keeps a BIOS dump out of the requirements.

**Each title is a directory.** `games/<id>/` holds a TOML config describing the binary's load
addresses and any hand-seeded functions, plus a one-line `CMakeLists.txt`. The top-level build
globs `games/*` and picks up anything with a `CMakeLists.txt` in it, so adding a title means adding
a directory. A title's target is a no-op for anyone without that title's extracted files, which is
how the repository can carry configs without carrying games.

## What works today

| Title | Translates | Builds | Runs |
|---|---|---|---|
| Crazy Taxi | yes | yes | **plays**: Full music and gameplay as well as saves. Played a good round without crashes |
| Tech Romancer | yes | yes | boots, draws its options screen |
| Metropolis Street Racer | yes | yes | boots and runs slowly. Its disc carries five code files and only the loader is configured, so almost everything falls back to the interpreter. |
| Charge 'N Blast | yes | yes | boots, reads the disc, renders |
| Ikaruga | yes | yes | boots, renders, saves to the memory card |
| Rez | yes | yes | boots, saves to the memory card, plays Area 1 with sound (its stage overlays run on the interpreter) |

One title is playable up to a point. If you bring your own game, expect it to translate, build, and
then stop somewhere. The tooling for finding out where and why is the mature part of this project,
and it is covered under [When it stops](#when-it-stops).

## Requirements

- CMake 3.24+, a C++20 compiler (Apple clang, clang 16+, or MSVC 2022), Python 3.9+, git
- A disc image you dumped yourself, `.chd` or `.gdi`. A self-boot CD-R image (`.cdi`) also runs.
  The launcher presents it as the GD-ROM that the game was made for. Its boot file usually has
  patches, so prefer a GD-ROM dump.
- For a window and sound: Vulkan (MoltenVK on macOS), SDL3, and `glslc`. These are optional;
  without them everything still builds and runs headless.

## Build the toolchain

```sh
git clone --recurse-submodules <this repo>
cd dream-recomp
cmake -S . -B build -DDREAM_DEV_INTERPRETER=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
pip install -e tools/dcdisc
```

`dcdisc doctor` checks the machine and reports what is present, what is missing, and which of it
is required.

The renderer is on by default (`DREAM_RENDERER`) and skips itself if Vulkan, SDL3 or `glslc` are
missing; CMake prints what it found. Only `--window` needs them.

## Getting started

```sh
tools/new-game.sh ~/discs/mygame.chd
```

This is the fastest way in. It asks for the few things it cannot read off the disc — the title, a
short id, where to put things — then reads the disc, extracts it, writes the config and the build
file, builds, and offers a first run. Point it at a folder of images instead and it lists them to
pick from. It prints every command before running it, so you can follow what it did.

The same steps by hand:

```sh
dcdisc new-game mygame.chd                       # extract, write the config and the build file
cmake --build build --target mygame_boot -j8
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --window
```

`new-game` reads the name, region, product number and boot filename out of the disc's own IP.BIN
header, so it picks up the details that are easy to assume wrongly. Charge 'N Blast's boot file is
`1ST_READ.US`, where most tooling hard-codes `1ST_READ.BIN`. It also refuses Windows CE titles,
which are out of scope: those games do not use the Katana SDK, and they are about a tenth of the
library.

### What it writes

`games/mygame/mygame.toml`, the title's config:

```toml
[game]
id = "mygame"
title = "My Game"
region = "JUE"
product = "T-1234N"

[disc]
image = "../../../discs/mygame.chd"

[binary]
path = "extracted/fs/1ST_READ.BIN"
load_address = 0x8C010000    # where the console's BIOS loads the file
link_address = 0x0C010000    # the same RAM through a different address; what the code assumes
entry = 0x8C010000           # the first instruction
```

Those addresses hold for nearly every Dreamcast game. The disc image is referenced where it
already lives rather than copied in. Relocations, overlays, symbol files and the `[functions]
extra` seeds are all documented in [docs/game-config.md](docs/game-config.md).

`games/mygame/CMakeLists.txt`, one line:

```cmake
dream_add_game(mygame TITLE "My Game")
```

Everything that target does lives in `cmake/DreamAddGame.cmake`. Nothing needs editing at the top
level.

Extraction lands under `games/mygame/extracted/`, with the filesystem in `extracted/fs/` and the
boot executable unscrambled if it needed it. That directory is gitignored and must stay that way.

To look at a disc without committing to it, `dcdisc inspect mygame.chd -o report.md` writes a
report. The number worth reading is how many files look like code: one is the straightforward
case, several means the game loads modules at run time and you will need to describe each one.

## Running

```sh
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --window
```

Arrow keys are the d-pad, `Z` `X` `A` `S` are A, B, X, Y, return is start, `Q` and `W` are the
triggers, `F10` toggles the frame-rate counter, escape quits.

Useful flags: `--scale 2` draws at higher internal resolution, `--vmu FILE.bin` supplies a memory
card, `--fps` starts with the frame-rate counter showing, `--wav OUT.wav` records the audio,
`--rtc-seed 1000000` fixes the console clock so two runs behave the same. `--help` lists the rest.

Without `--window` it runs headless and prints a report, which is often the quicker way to find out
what happened.

## When it stops

It will not work first time. This part is the workflow worth learning.

**It stops at an address.** The usual outcome: the game jumps somewhere discovery never reached.
Let the run write the fix instead of reading addresses off the screen.

```sh
build/games/mygame/mygame_boot --config games/mygame/mygame.toml --suggest-config next.toml
```

`next.toml` holds `[functions] extra` seeds for the addresses it saw reached but untranslated, and
`[[relocations]]` entries for code the program copied and ran elsewhere, skipping anything your
config already covers. Paste it in, rebuild, run again.

Crazy Taxi's release build — the configuration with no interpreter in it — used to fault on frame
0. Ten entries found this way over three rounds brought it to zero untranslated call targets.

**It draws something wrong.** Press `F11`. You get five files: the picture, the display list that
drew it, video memory, the graphics registers from that instant, and a note on where the run had
got to. That bundle replays without the game:

```sh
build/render/dream_render_view --vram capture-000.vram capture-000.ta
```

A rendering bug you can replay offline is one you can bisect.

**It behaves differently from real hardware.** The reference is Flycast's interpreter, run as an
oracle and compared instruction by instruction against the translated code. See
[docs/differential-harness.md](docs/differential-harness.md). Every emitter change is expected to
come with one.

**Something drifts partway through a long run.** `--write-hash FILE` writes a rolling hash of every
memory write, one line per frame. The first line where two runs differ is the frame where they
parted.

## Rules

- Never commit game data, BIOS images, flash, memory-card saves, or SDK files. The `.gitignore`
  covers the usual cases; check `git status` anyway.
- Never paste Sega SDK source or headers into this repository, including into comments. Describe
  the interface in your own words, or point at the equivalent in
  [KallistiOS](https://github.com/KallistiOS/KallistiOS), which is BSD-licensed.
- Windows CE titles are out of scope.
- Emitted code must behave identically on x86-64 and ARM64; see ADR 16.

## Further reading

| Document | What it covers |
|---|---|
| [docs/dev-setup.md](docs/dev-setup.md) | Full setup, every launcher flag, the toolchain container |
| [docs/game-config.md](docs/game-config.md) | The TOML format, relocations, overlays, symbol files |
| [docs/emitter-design.md](docs/emitter-design.md) | How SH-4 becomes C++: delay slots, FPU modes, interrupts, coverage |
| [docs/differential-harness.md](docs/differential-harness.md) | Testing against Flycast as an oracle |
| [docs/runtime-render.md](docs/runtime-render.md), [docs/runtime-aica.md](docs/runtime-aica.md), [docs/runtime-memory.md](docs/runtime-memory.md) | The emulated hardware |
| [docs/decisions/](docs/decisions/) | Architecture decision records, with the reasoning |
| [docs/progress.md](docs/progress.md) | Where every work package stands |
| [docs/implementation-plan.md](docs/implementation-plan.md) | The four phases and what remains |
| [tools/dcdisc/README.md](tools/dcdisc/README.md) | Disc tooling in detail |

## Licence

GPL-2.0, see [LICENSE](LICENSE). Flycast is GPL-2.0 and parts of it are ported here deliberately;
that decision and its consequences are ADR 1.
