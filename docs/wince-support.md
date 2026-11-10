# Windows CE development — experimental v6

**This is a working foundation, not compatibility with commercial Windows CE games.** The v1
disc inventory from `dream-recomp-wince-v1.zip` is integrated. V6 adds the compiler's signed and
unsigned 32-bit division/remainder helpers, with original SH-4 execution tests. V5 adds recursive critical sections,
mutexes, CE EventModify/PulseEvent and wait-any, exercised through an original SH-4 EXE/DLL pair.
It retains the v4 guest heaps, virtual memory, CRT helpers and corrected stack argument layout
(argument five at SP+16), the DLL loader and guest DirectX interfaces.
See [the kernel service contract](wince-kernel.md).
The game launcher starts a Windows CE title through `dream-wince` (`--run EXE --lazy
--real-directx --window --disc IMAGE`, and the card in A1 as `--vmu`) when its configuration
has a `[wince]` table naming the executable on the extracted disc; such a configuration is
written by hand, as AJOUTER UN JEU still turns Windows CE discs away. The launcher does not try
to improve such a title after a session: there is no translation to rebuild.

## Build and try it

From this checkout on Windows with Visual Studio 2022 Build Tools, CMake and Ninja:

```powershell
.\tools\wince\build-windows.cmd
.\.venv\Scripts\python.exe tools/wince/make_fixture.py build-wince-verified/smoke.exe
.\build-wince-verified\tools\wince\dream-wince.exe --check build-wince-verified/smoke.exe --base 0x20000
.\build-wince-verified\tools\wince\dream-wince.exe --run build-wince-verified/smoke.exe --base 0x20000 --steps 32
.\build-wince-verified\tools\wince\dream-wince.exe --media-demo build-wince-verified/media-demo
.\.venv\Scripts\python.exe tools/wince/make_directx_fixture.py build-wince-verified/directx.exe
.\build-wince-verified\tools\wince\dream-wince.exe --run build-wince-verified/directx.exe --steps 100000 --capture build-wince-verified/guest-output
.\.venv\Scripts\python.exe tools/wince/make_sync_fixture.py build-wince-verified/sync-demo
.\build-wince-verified\tools\wince\dream-wince.exe --run build-wince-verified/sync-demo/guest.exe --base 0x20000 --steps 10000
```

The fixture generator and media demo refuse to overwrite existing output. Use a fresh output name
when repeating them. The program should return after **10 interpreted instructions and 1 HLE call**.
The media demo produces a 320×240 PPM containing a surface blit and a Gouraud triangle, and a
one-second stereo PCM WAV containing a generated 440 Hz tone. `--media-demo` exercises host-side
primitives. The **second fixture**, `directx.exe`, executes **22,315 instructions and 16 guest HLE
calls**, creates a DirectDraw4 surface and Direct3D3 device, draws a triangle, and writes/plays
a DirectSound PCM buffer. `--capture` saves its presented frame (`guest.ppm`) and one second of
stereo audio (`guest.wav`). Audio is mixed offline after the program returns; there is no live
window, audio-device thread or synchronization between emulated video and audio yet.
The **third fixture**, `sync-demo/guest.exe` plus `sync.dll`, checks DLL initialization and
synchronization from guest code: 594 main-run instructions, 44 HLE calls and 55 DLL initialization
instructions. `wince.synchronization` tests it at two load addresses and also verifies a failing
DLL initializer. This test uses only generated binaries, with no OS SDK or game dependencies.

Portable CMake build, in an appropriately configured compiler environment:

```sh
cmake -S . -B build-wince -DDREAM_RENDERER=OFF -DDREAM_DEV_INTERPRETER=ON
cmake --build build-wince --target dream-wince dream_runtime_tests
ctest --test-dir build-wince -R '^(runtime.unit|wince\..*)$' --output-on-failure
```

`--run` defaults to 100,000 instructions and requires a positive `--steps` limit. A delay slot is
executed together with its branch, so a boundary can exceed the limit by one instruction. Exit
status 0 means a clean check/return/guest ExitProcess; the guest's exit code is printed separately.
Status 2 means an invalid file, unsupported operation or fault; status 3 means execution stopped
before returning (or an audit found unsupported imports). `--check` loads and links the dependency
graph without executing DLL entry points. `--inspect` prints one PE's imports/exports without
binding them; `--audit` lists unsupported imports across available dependencies. Auditing is static:
it cannot prove that initializers, dynamic imports or forwarded exports will run. With
`DREAM_DEV_INTERPRETER=OFF`, checking, inspection, auditing and the backend demo remain available;
`--run` is rejected. The instruction limit is one budget for the main run, DLL initializers and
every guest thread (see [Guest threads](#guest-threads)).

For disc inspection, use the Python environment containing `tools/dcdisc`:

```sh
python -m dcdisc wince-report path/to/game.gdi --json
```

GDI, CHD and CDI use the existing disc readers. The inventory reports kernel filenames, loose
PE sections and DLL dependencies. It does not extract or interpret the ROM's internal modules.
The JSON `runtime_supported` field remains false: it denotes commercial-game support.

## Implemented scope

| Component | Behavior | Remaining work |
|---|---|---|
| Hardware MMU adapter | P0/U0/P3 translation; four page sizes; ASIDs, SH, SV, PR and dirty checks; P1/P2 bypass; privilege/alignment checks; mapped on-chip registers; OCRAM gating; UTLB invalidation and LDTLB; replacement counter; SQ write/PREF protection | ITLB entries/cache and LRUI; associative array writes; cache timing/coherency; complete CPU exception/reset delivery |
| PE32 loader | SH-4/CE validation; sections and zero-fill; ABSOLUTE/HIGH/LOW/HIGHLOW/HIGHADJ relocations; two-phase dependency loading, cycles, automatic rebasing, named/ordinal exports, forwarders, atomic IAT binding and installation; DLL_PROCESS_ATTACH | CE ROM/E32/XIP modules, decompression, unload/detach/reference counts, TLS directory initialization and delay imports |
| HLE process memory | Read/write/execute permissions; VirtualAlloc reservations/commits, VirtualFree decommit/release, VirtualProtect, VirtualQuery; small-block heaps with reallocation and ownership checks; atomic preflight of writes | Actual CE process slots, shared mappings, VirtualCopy, cache/guard/write-copy flags, image/heap VirtualProtect and kernel exception delivery |
| COREDLL | 90 named services plus unsuffixed wide-name aliases; dynamic module APIs, recursive critical sections/mutexes, named events, EventModify and wait-any, heap/CRT/virtual-memory calls, compiler integer division/remainder, interlocked integers, performance clock, diagnostics and bounded wide formatting; local DLL metadata supplies ordinal names | Thread/process scheduling and contended synchronization, files, registry, windows/messages, exception and conversion helpers, locale conversion and complete formatting |
| Guest graphics | DirectDraw1/4, Surface1/4, Direct3D3 and Device3 COM objects/vtables; RGB565/XRGB8888 locks, fills, blits, one-back-buffer flips; TL Gouraud triangle lists/strips/fans, indices and culling | Textures, blending, lighting, viewport COM objects, guest depth surfaces, other interface versions, host-window presentation |
| Guest sound | DirectSound/DirectSoundBuffer COM; PCM8/PCM16 mono/stereo, wrapping guest Lock/Unlock, play/stop/seek, looping, volume/frequency controls, stereo 44.1 kHz mix | Notifications, pan, spatial sound, streaming scheduler, device enumeration and live host audio |

`DDRAW.DLL!DirectDrawCreate`, `DirectDrawCreateEx` and `DSOUND.DLL!DirectSoundCreate` now resolve
to guest-callable factories. Their objects contain 32-bit guest vtables; `JSR` calls dispatch to
implemented methods after the SH-4 delay slot. See [the precise DirectX contract](wince-directx.md).
Unknown imports fail linking; unsupported COM methods/states return a failing HRESULT.

## Memory paths and fault handling

`mem::MmuMemory` wraps `DcMemory` and a `sh4::Ctx`. It is opt-in; the existing Katana `System` and
DMA paths retain their physical bus. The strict adapter consults the UTLB for instruction fetches
as well as data accesses. `Memory::fetch16` defaults to `read16` for existing implementations.
Because the strict adapter does not cache an ITLB, its fetches consult the UTLB and advance URC
each time; hardware replacement timing is not reproduced. Software must still install mappings:
the hardware MMU does not automatically walk page tables.

Faults update TEA, EXPEVT and (except multiple hits) PTEH.VPN, then throw `MmuFault` before the
access. Misses identify VBR+0x400, protection/address/initial-write faults VBR+0x100, and a multiple
hit identifies the absolute reset address 0xA0000000. The adapter **does not** deliver that CPU
exception: saving SSR/SPC/SGR, bank switching, delay-slot rollback and retry belong to the next
kernel-execution milestone. Associative UTLB word writes are explicitly rejected in strict mode.
The legacy Katana SQ path continues to use its previous fallback behavior. Both UTLB array views
now keep V and D coherent.

`wince::ProcessMemory` is a separate software memory model for the HLE path. Replacing CE's OS
services means that this path manages virtual regions directly, without booting a CE kernel or
pretending to exercise its hardware TLB management. Image headers are read-only, sections use
their PE permission bits, ordinary allocation memory is read/write and non-executable; explicit
VirtualAlloc/VirtualProtect pages can be executable. Its committed-memory budget
is 64 MiB; the loader caps files and images at 32 MiB. These are development bounds, not a model
of the Dreamcast's physical RAM capacity.

## COREDLL contract

The exported names currently supported are `GetTickCount`, `GetLastError`, `SetLastError`, `Sleep`,
`LocalAlloc`, `LocalFree`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`,
`GetCurrentProcessId`, `GetCurrentThreadId`, and `ExitProcess`, plus `LoadLibraryW`, `LoadLibraryExW`,
`GetModuleHandleW`, `GetProcAddressW`, `GetProcAddressA`, `CreateEventW`, `SetEvent`, `ResetEvent`,
`WaitForSingleObject` and `CloseHandle`. Unsuffixed LoadLibrary/LoadLibraryEx/GetModuleHandle/
GetProcAddress/CreateEvent/CreateMutex resolve to their wide versions, as appropriate to this CE subset.

The additional synchronization, heap, memory, CRT, formatting and diagnostic names are listed in
[wince-kernel.md](wince-kernel.md). Calls use R4–R7, then SP+16 onward, and R0 for the result. The interpreter
dispatches calls after their delay slot and preserves the normal PR/stack return path. State is
single-threaded. Time is deterministic, from CPU cycles at an assumed 200 MHz; finite Sleep
advances that clock and does not block the host. Infinite Sleep is unsupported. LocalAlloc accepts
fixed/zero-init flags only, zeroes memory, and returns 0 on allocation failure. TLS has 64 slots.
Errors and allocation results follow the documented subset; unsupported flags fail explicitly.
Events are process-local, with separate handles to a shared named object. Auto-reset consumes a
signal; manual-reset keeps it. An unsignaled finite wait advances virtual time and returns
WAIT_TIMEOUT. An infinite unsignaled wait fails explicitly because no other thread can signal it.
`CloseHandle` accepts event and mutex handles. EventModify implements pulse/reset/set. Recursive
mutex ownership and critical-section lifetime are tracked; wait-any validates every handle before
consuming a signal. Their full contract and one-thread limits are in the kernel service document.

COREDLL names are case-sensitive; the DLL name is case-insensitive. Ordinals have no default
guessed mapping. Embedders can call `Kernel::bind_ordinal` with verified aliases. The console reads
export metadata from adjacent COREDLL/DDRAW/DSOUND DLLs to name ordinal requests; it implements
only known services from those tables. It does not run those three original DLLs or substitute
host Windows DLLs. No ordinal table copied from a commercial title is embedded in the source.

## Module loading contract

`Modules` prepares a complete graph before binding imports and installing memory. Named exports,
ordinal-only exports, aliases and forwarded names/ordinals are supported; forwarder cycles fail.
Dependencies sharing a preferred address are rebased into free 64 KiB ranges. Ordinary images
occupy the HLE user image area below 0x10000000; allocations use a separate range. A failed link
leaves the previous memory and module map intact. Limits are 128 modules, dependency depth 64,
forwarder depth 32, and the process's 64 MiB memory budget.

The CLI reads case-insensitive basenames from the executable's directory. It rejects path
components and skips symlinks escaping that directory. DLL entry points run in dependency order
with `(hInstance, DLL_PROCESS_ATTACH, NULL)`, using the SH-4 interpreter and a bounded instruction
count. Cycles have a deterministic traversal order. Failed attachment poisons the loader; it
does not attempt transactional rollback of arbitrary initializer side effects.

LoadLibrary returns an existing module or loads and initializes a new DLL. Loading another DLL
recursively from DllMain, FreeLibrary, detach callbacks and module reference counts remain
unsupported. Forwarder dependencies are eagerly loaded. GetProcAddress accepts a UTF-16 name
on its CE wide entry or a low-word ordinal; the explicit A variant accepts bytes. Module/export
names are limited to ASCII, while event/mutex names use UTF-16. These are HLE application services;
they do **not** constitute booting or emulating a complete CE kernel.

## Local title audit (2026-11-04)

The available Sega Rally 2 extraction contains 142 PE modules. Running the following against
the local files traverses five application dependencies and reports **65 unique unsupported
imports** (69 in v5, 76 in v4, 109 in v3), including thread creation, exception helpers, filesystem and window/message
APIs:

```powershell
.\build-wince-verified\tools\wince\dream-wince.exe --audit games/segarally2/extracted/fs/RALLY2.EXE
.\build-wince-verified\tools\wince\dream-wince.exe --check games/segarally2/extracted/fs/RALLY2.EXE
```

The check now stops at `cemm.dll: unresolved import: COREDLL.dll!#87` (`__C_specific_handler`).
The earlier #4 (`EnterCriticalSection`) and #56 (`wsprintfW`) blockers are resolved. The audit names
ordinals using the actual DLL exports. This is an observed blocker, not a prediction that adding
the remaining functions will make the game work: dynamic calls, COM interface versions, OS initialization
and timing still need validation. No game binary is included in the source/patch archive.

## Starting Sega Rally 2 (2026-11-05)

`dream-wince --run RALLY2.EXE --lazy` now loads all six modules and runs the title's own code
up to its first worker thread. `--lazy` links each unsupported COREDLL/DDRAW/DSOUND import to
a stub that stops the run with the function's name, arguments (and any text they point at) and
the calling module, so services are written in the order the title really calls them.
`DREAM_WINCE_WATCH=addr[,addr]` prints the caller of each call to those addresses.

What the start needed, in order, and where it lives (`SystemServices`, trap range 0x1F006000):

| Called by | Service | Behaviour |
|---|---|---|
| DLL CRT start-up | `TlsCall` | CE's TlsAlloc/TlsFree entry (Kernel) |
| OLE32, MGame DLLs | registry `RegOpenKeyExW`, `RegCreateKeyExW`, query/set/enum/info, `RegCloseKey` | in memory, starts empty; a missing key answers ERROR_FILE_NOT_FOUND |
| WDMLIB, DDHAL | `KernelIoControl`, `QueryAPISetID` | the OAL's interrupt codes (see [The console hardware](#the-console-hardware)), other OEM codes refused; no API set registered ("WDMH") |
| RALLY2 | `CreateFileW`, `ReadFile`, `SetFilePointer`, `GetFileSize`, `CloseHandle` | the disc folder, read only; `\CD-ROM\` and `\` name its top; `\Device\...` opens a device whose `DeviceIoControl` codes are refused and reported |
| RALLY2 | windows and messages (`CreateWindowExW`, `PeekMessageW`, `DispatchMessageW`, timers...) | window procedures and timer callbacks run as nested interpreter calls (`Kernel::set_guest_call`) |
| PLATUTIL | `SetKMode`, `SetProcPermissions`, then the BIOS vectors 0x8C0000B0-E0 | SYSINFO and FLASHROM served from `hle::Flash`, formatted as a European PAL console in French; MISC as `hle/bios.cpp` |
| RALLY2 | SH-4 port A (PCTRA/PDTRA) | `DreamcastHardware`: VGA cable |
| RALLY2 | GDI fonts and DCs | handles only, no font installed |
| DPLAYX | `GetVersionExW` | Windows CE 2.11 (the disc's COREDLL subsystem version) |
| MGame DLLs | `operator new`/`delete` | COREDLL `malloc`/`free` |

The OS image's registry (`default.fdf` in 0WINCEOS.BIN, 38,776 bytes stored in 38 compressed
1 KiB blocks) is not decoded: the ROM compression format is undocumented. COM classes are found
instead by asking the disc's DLLs: a miss on `HKCR\CLSID\{...}\InprocServer32` calls each DLL
exporting `DllGetClassObject` (MGame* first) for an IClassFactory and records the one that
answers. So far: input (MGAMEINPUT), registry (MGAMEREGISTRY), vibration (MGAMEVIB). The title's
own `DllRegisterServer` entries return E_FAIL or E_NOTIMPL, so they cannot fill it.

### Guest threads

`Scheduler` (threads.h) gives each guest thread a host thread of its own, but only the thread
holding a baton executes: a thread can block anywhere (inside a window procedure or a DLL
initializer) with its host stack intact, guest state needs no locks, and runs stay
deterministic. CE scheduling: the highest-priority ready thread runs (0 highest, 7 idle, 3
normal). `dream-wince` runs every piece of guest code in slices of 50,000 instructions, nested
runs included, and `--steps` is one budget for all of them. At the end of a slice a
higher-priority thread made ready (an interrupt woke it) takes over at once, but one of equal
priority only once the running thread has used its quantum, CE's 100 ms: switching at every
slice interleaved the title's worker threads in orders the console never produces.
Time is one virtual clock (200 MHz cycles): when every thread waits, it jumps to the earliest
deadline; when every thread waits with no deadline, the run stops with a deadlock error.

Thread services: `CreateThread` (64 KiB-1 MiB stack, CREATE_SUSPENDED), `ExitThread` (the first
thread's ends the process), `GetExitCodeThread`, `Suspend`/`ResumeThread`, `TerminateThread`
(the thread ends where it next runs), `Set`/`GetThreadPriority`, `Sleep` (yields; `Sleep(0)`
gives up the slice), `GetCurrentThreadId`, and `CloseHandle` on thread handles. Synchronization
objects follow the calling thread: waits and contended critical sections block, every signal
(SetEvent, ReleaseMutex, LeaveCriticalSection, a thread exiting) makes waiters check again, and
thread handles are waitable. TLS values and the last error are per thread. A pulse with
waiters leaves the event set until a waiter or the next reset clears it: an approximation.

With threads the run got past `CreateThread` and stopped in MGAMED3D.DLL, which reads the
PowerVR register FB_C_SOF (0xA05F8114) through a kernel-mode accessor: the title's 3D engine
mixes DirectDraw with direct PowerVR accesses, so graphics cannot stay at the DirectDraw API.

### The console hardware

`--real-directx` runs the disc's own drivers (DDRAW, DDHAL, D3DIM, DSOUND, WDMLIB...) on an
emulated console instead of the guest DirectX interfaces above: only COREDLL is served by the
host, and DDRAW/DSOUND imports bind to the disc's modules. `DcHardware` (dc_hardware.h) wires
the devices as `boot_main` does for a Katana title: PowerVR core and Tile Accelerator, the
system block (channel-2 DMA), G2 DMA, the AICA with its ARM7 and real-time clock, register
files for G1, PVR DMA, BSC, UBC, CPG, RTC and SCIF, and the cache arrays. Kernel-mode accesses
at 0x80000000 and up and `VirtualCopy` windows reach it through `ProcessMemory::set_hardware`.
DMA buffers (`HalAllocateCommonBuffer`, `MmProbeAndLockPages`) come from console RAM
0x0C100000-0x0D000000, seen by the process at 0x30000000.

- **Time.** Device events follow the guest clock; when every thread waits, the scheduler
  advances the devices to their next event (`device_deadline`, `run_devices`), so V-blank and
  render-done interrupts wake the drivers that wait for them.
- **Sound.** What the OS image's SYSSTART.EXE does before any title: `ARMCOM.DRV` (after its
  12-byte header) goes to the start of sound RAM with the ARM7 held in reset, then the ARM7 runs.
  Once the driver says it is ready (the address of its work area, 0x38040, at 0xEC), the system
  opens SNDCORE first, which leaves the start of the driver's event ring (what 0xE4, its write
  pointer in the 32-byte ring, held at boot: 0x3E80) at the work area + 8; `DcHardware` does the
  same at the first device time after. A title's SNDCORE only reads it there (its stream update
  takes `position - start` as the ring's end index): left at zero, Sega Rally 2 froze after
  ARCADE.
- **Interrupts.** The OAL's routing, read from nk.exe (handler 0x8C02E000, table 0x8C145EA8):

  | SYSINTR | Holly status bits | Source |
  |---|---|---|
  | 16 | ISTNRM 0x00080FFF | PowerVR (renders, blanking, TA lists, PVR and ch2 DMA) |
  | 17 | ISTERR 0xFF | PowerVR/TA errors |
  | 18 / 19 | ISTNRM 0x3000 / ISTERR 0xF00 | Maple / Maple errors |
  | 20 / 21 | ISTEXT bit 0 / ISTNRM 0x4000 | GD-ROM / GD-ROM DMA |
  | 23 / 24 | ISTEXT bit 1 / ISTNRM 0x8000 | AICA / AICA DMA |
  | 25 | ISTERR 0x00888000 | other errors |
  | 26 / 27 | ISTEXT bits 2 / 3 | modem / external device |

  `InterruptInitialize` binds an event; a SYSINTR fires once, then waits for `InterruptDone`.
  Drivers acknowledge through the OAL's `KernelIoControl` codes, each taking the SYSINTR first:
  0x10000 returns the group's pending status bits and clears them in Holly (the answer is kept
  until `InterruptDone`), 0x20000 and 0x30000 enable and disable status bits of the group. Only
  enabled bits raise the SYSINTR. Without them the status was never cleared and the graphics
  interrupt fired 7,360 times in 66 frames (260 now).
- **Store queues.** `SetStoreQueueBase(physical)` (nk.exe 0x8C014958) returns 0xE0000000, and
  from then on the whole store-queue area maps onto `physical` onward: the kernel's TLB miss
  handler (0x8C012560) loads the 1 MiB page holding each faulting address. DDHAL passes
  0x0E000000 and writes the region array to 0xE3000200, i.e. 0x11000200 on the bus (video
  memory through the TA texture path).

Sega Rally 2 now draws its first screen, the memory-card warning, through the disc's own
drivers: 66 frames and 54 renders in 200 million instructions. `dream-wince` has no renderer, so
frames are seen offline: `--dump-ta F --dump-ta-at-frame N` saves render N's display list,
video memory and registers, which `dream_render_view F --vram F.vram` (a build with the
renderer) draws.

### Drivers: the Maple bus

Pads and memory cards are reached through the disc's own Maple driver, hosted the way
WDEVICE.EXE hosts it on the console. WDMLIB.DLL is the driver model (objects, IRPs, device
queues) and loads the drivers listed under `HKLM\WDMDrivers\BuiltIn\<name>` (values `Dll`,
`Order`, optional `Block`); WDEVICE only starts it and turns file calls on `\Device\...` into
WDMLIB calls. `WdmHost` (wdm_host.h) reproduces WDEVICE.EXE, read from Sega Rally 2's disc:

- at start, the `Files` object directory (`g_OpenFileDir`), the `Maple` registry entry
  (`maple.dll`, order 0), then `InitWDM`. `LoadDriver` is `LoadLibraryW`; MAPLE.DLL's DllMain
  runs `InitWDMDriver` and its `DriverEntry` creates `\Device\Maple`, maps the Maple registers,
  allocates its DMA buffers and starts its interrupt thread (SYSINTR 18 and 19);
- `CreateFileW("\Device\X")`: `IoGetDeviceObjectPointer`, then for a synchronous handle a lock
  event, auto-reset and initially signalled, at file object +52 (WDMLIB takes it around each
  request), and `ObInsertObject` into `Files`. A device no driver serves (`\Device\CDROM0`)
  still opens, with its requests refused as before;
- `DeviceIoControl`, `ReadFile`, `WriteFile` and `CloseHandle`: `ControlWDMDevice`,
  `ReadWriteWDMDevice` (IRP_MJ_READ 2 / WRITE 3) and `ObRemoveObject` + `CloseWDMDevice`, with
  WDEVICE's argument and access checks and its status mapping (STATUS_PENDING is
  ERROR_IO_PENDING, others through `RtlMapStatusToError`).

`DcHardware` carries the Maple bus of the Katana path, with a standard pad in port A and a
memory card in its first slot, blank and in memory unless `--vmu FILE` names a card image
(saves are written back to it). The driver sets the V-blank hardware trigger, so the bus runs
once a frame and a request is answered one or two frames after it is made, as on the console.

What it took, each found by following one failure to its cause:

- the driver's start must come before the title: on the console the bus has been scanned for
  seconds before a title runs (a card is found frames after its pad). The host lets one second
  of guest time pass after `InitWDM`; without it the title saw devices arrive while it started
  and re-created its objects with requests still pending;
- `AddEventAccess` takes a reference: WDMLIB adds one to a caller's overlapped event, then sets
  the event and closes that reference when the request completes. Destroying the event at that
  close left MAPLEDEV.DLL waiting on a dead handle (WAIT_FAILED); it then freed a request buffer
  the driver was still to fill, which corrupted the next object allocated there;
- `VirtualFree` removes the `VirtualCopy` windows over the pages it releases (MAPLE.DLL maps
  video memory once and unmaps it; the window used to capture the next reservation, the title's
  own heap). `memcpy`, `memset` and the other host-side copies go through windows byte by byte;
- the memory card answers GetMediaInfo for its screen (48x32 dots, as Flycast does); it used to
  refuse it, and the driver retried while the title's next request found the card busy;
- COREDLL's `__int64` helpers (`_muli64`, `_divui64`, `_uitoi64`, comparisons and conversions,
  values in memory, low word first), used by MGAMESTORAGE and MGAMESOUND.

Result: the title finds its pad, vibration, storage and screen classes, reads 28 blocks of the
card and leaves the memory-card warning; 400 million instructions run without a fault, with
548 driver requests (2 failed). The pad has no buttons pressed: there is no window to play in.

Bring-up aids, all environment variables of `dream-wince`:

| Variable | Shows |
|---|---|
| `DREAM_WINCE_TRACE_MAPLE=N` | the first N Maple frames other than pad reads and empty ports, with their replies and guest time |
| `DREAM_WINCE_TRACE_DRIVER=N` | the first N driver requests, their input words and status |
| `DREAM_WINCE_STATUS=S` | every S seconds: instructions, memory in use, Maple transfers, every thread |
| `DREAM_WINCE_MODULES=1` | where each module sits |
| `DREAM_WINCE_REGS=1` | a failing thread's registers |
| `DREAM_WINCE_PEEK=A[,A]` | words of process memory when the run fails |
| `DREAM_WINCE_WATCH_WRITE=A[:N]` | each write covering address A, stopping at the Nth |
| `DREAM_WINCE_WATCH_FREE=A[:N]` | each heap free of a block holding A, stopping at the Nth |

`DREAM_WINCE_WATCH` (calls to host services) now also prints the thread and guest time.

### A live window

`dream-wince --window` (the build with the renderer, `build/tools/wince`; SDL3.dll is copied
beside it) shows the console live and reads the pad:

```
dream-wince --run RALLY2.EXE --lazy --real-directx --window --vmu card.bin --disc disc.gdi
```

It runs until the window is closed or escape is pressed, unless `--steps` asks for a limit.
`--vmu` names the memory card in the pad, created blank and formatted when the file does not
exist. The title's threads each run on a host thread of their own while a window answers only
the thread that made it, so `LiveWindow` (tools/wince/live.cpp) keeps the window, Vulkan and all
drawing on a thread of its own:

- at each render (the PowerVR's STARTRENDER), the display list goes over with a copy of video
  memory and of the PowerVR registers, and the texture ranges rewritten since the last render.
  The window thread draws it as boot_main's `Live::render` does (background plane, guard band,
  fog, write clip, palette, per-pixel translucency) into a 640x480 offscreen image; a render not
  yet drawn is replaced by a newer one;
- at each vertical blank (`DcHardware::on_vblank`, before the Maple bus runs), the buffer the
  video output scans out is shown: the renderer's drawing when a render was written there,
  otherwise the pixels the title wrote itself, read from video memory;
- the pad is read on the window thread through the player's bindings (the file the Katana titles
  and the launcher share) and goes into the Maple controller at the vertical blank. A press is
  held for at least three vertical blanks, however short it was: running this slowly, the title
  reads the pad a few times a second of real time, and a tap between two reads was lost. The
  first changes of the buttons are written to the log (`wince: pad buttons`).

F12 writes `screenshot-wince-NNN.ppm` in the working directory, as does
`DREAM_WINCE_SCREENSHOT_AT=N` at the Nth image shown. Not done yet: a render into a texture is
drawn but not written back to video memory.

The interpreter runs about 60 million guest instructions a second, under a third of the console
([runtime-devinterp.md](runtime-devinterp.md), Speed; 28 million before), but a title's menus
need far fewer than the console offers: most of the time they wait for the graphics hardware.
Sega Rally 2 asks `while (FAILED(d3d->Flip()));` (RALLY2.EXE), and
DirectDraw answers `DDERR_WASSTILLDRAWING` until the flip happened at the vertical blank: two
thirds of its instructions were that loop. Nothing changes the answer before the hardware's next
interrupt, so when one call site gets `DDERR_WASSTILLDRAWING` back twice within 20,000 cycles
(seen at each RTS through the interpreter's `on_return`), the devices run on to their next
interrupt (a frame at most, and never past the time a sleeping thread wakes) and the interrupt
thread it woke runs. The title gets the same answers after fewer polls: run with and without it,
Sega Rally 2 does the same renders at the same guest times. In its menus 95% of the guest's time
is skipped so, and without a window it runs about three times faster than the console.

Its races wait for the next frame another way, with a loop that only reads:
`do n = GetFrameCount(); while (n == last);`, the count kept by the vertical-blank interrupt
thread; a third of the race's instructions. Such a loop is skipped likewise when a call site gets
the same R0 back three times within 20,000 cycles and, between each, the call came straight back
from that site (no other call in between) with the same arguments (R4 to R7), the caller's
preserved registers (R8 to R15) are unchanged, nothing changed memory outside the stack below the
caller's frame (`ProcessMemory::outside_writes`: a store of the value already there does not
count, as MGameD3D rewrites its status each turn), and no service was called and no hardware
register read (`device_reads`). Each condition was needed: without the loop and argument checks,
a vector helper several callers share, and a `for` loop keeping its counter on the stack, looked
the same from inside and the race drew six frames a second. With them, Sega Rally 2 draws the
same renders in the same frames as without any skipping (`DREAM_WINCE_FRAMES=300` prints the
renders and host time of every 300 vertical blanks), and its race ran at about 0.6 of the
console's speed without a window instead of 0.17. What is left there is the work itself: vertex
transforms in DDHAL and MGameGL. With the interpreter's own speed-ups (fetching and decoding,
below) the race runs at about 1.2 of the console's speed without a window: 300 frames in about
4 seconds instead of 9.3, the same renders in the same frames.

`--no-wait-skip` runs the loops as they are (`DREAM_WINCE_NO_POLL_SKIP` keeps the DirectDraw rule
only); the report's `busy-waits` line counts both kinds of skip.

A window therefore holds the guest to real time: at each vertical blank, `LiveWindow` waits
while the guest's clock (200 MHz) is ahead of the wall's. A guest more than 100 ms behind (a
load, a heavy scene) is not made to race to catch up afterwards. Sega Rally 2 shows PRESS START
BUTTON a few seconds after it starts, and its menus run at the console's pace. In a window
`DREAM_WINCE_FRAMES` also prints the time spent waiting so (`frames 4200: 300 renders in 5.0 s
(1.0 s waiting)`): the room left. Its race keeps the console's pace with about a fifth of the
time to spare (a tenth at the start, every car in view); the loading before it runs at about 0.8.

A window has sound (`--no-audio` turns it off; a run without a window is silent), in two parts
that the sound device mixes:

- the AICA's output, made by the title's DSOUND on the ARM7 (the effects), goes to an
  `audio::Sink` sample by sample, on the guest's clock. Where the guest runs slower than the
  console the device runs dry between blocks (the `gaps` count in the report) and the effects
  are heard chopped; at the menus' real-time pace, 15 gaps in 6,339 blocks;
- the CD audio (with `--disc`, below) is played by `CdAudio` (tools/wince/cd_audio.cpp) as the
  drive plays it on the console: by itself once told to. A thread of its own reads the image's
  audio sectors (16-bit little-endian stereo PCM, 588 frames a sector) and keeps about 190 ms
  queued on its own `Sink`, on the host's clock, so the music plays at its speed however slowly
  the title runs. It stops at the end the title gave or off the audio tracks; the title's volume
  (`IOCTL_CDROM_SET_VOLUME`, left and right) scales it.

The report's `heard:` line counts the effects' samples, blocks, gaps and drops, and the CD
sectors played.

The CD-ROM device (`\Device\CDROM0`) has no driver here (WSEGACD.DLL needs a GD-ROM drive),
but MGameAudio plays the title's music from the disc's audio tracks and waits for its answers:
refused, it left the main menu black. With `--disc image.gdi` the NT CD-ROM requests are answered
from the disc's tracks: the table of contents of the high-density area (`IOCTL_CDROM_READ_TOC`;
MGameAudio insists on track 3 coming first), play, seek, stop, pause, resume and volume. An
overlapped request answered at once still gets its OVERLAPPED filled and its event set, as the
title waits on it. `wince: CD audio: play track N` in the log names each track played (Sega
Rally 2's menus play track 8).
`ProcessMemory` remembers the regions accesses fell in lately, 256 for data and 256 for
instruction fetches, each under the 4 KiB page an access was in, so nearly every access skips
its map lookups. With one region of each, the stack, the heap, a module's data and the code of
two modules took turns and one access in five took the long way (212 million in a billion
instructions of Sega Rally 2); now some 3 million do, almost all of them writes to the
hardware, and the same billion instructions take 36 seconds instead of 52. Reads and writes that
land in a remembered region are now served without the call into `load`/`store`
(`quick_read`/`quick_write`, the same checks and counting), and code is not fetched through it
at all: the interpreter reads it from the region (`code_span`). `DREAM_WINCE_WATCH_WRITE` is read
once at start-up rather than at every store.

`DREAM_WINCE_PROFILE=N` prints where the time goes: the busiest guest code (the PC each run of
guest code stopped at, weighed by the instructions it ran, in buckets of N bytes; 16 finds
loops), the call sites whose polls were skipped, the host's time split between the guest
services and the console hardware (the rest is the interpreter), the costliest services with
their calls, and the accesses that took the long way. On Sega Rally 2's menus the services take
about 1.3 seconds of 36 and the hardware 0.6: the interpreter is what is slow.
`DREAM_WINCE_PRESS=start@N,...` presses pad buttons at given frames. `DREAM_WINCE_REGS` adds the
registers to the message of a failure on any thread.

What took Sega Rally 2 from ARCADE to a race:

- the sound driver's event ring (`DcHardware::boot_sound`, above): its SNDCORE froze the screen
  after ARCADE, its sound thread looping for ever in its stream update (+0x3b28);
- `SetFilePointer` moves by its 32-bit distance alone, as the console's file systems do:
  MSELECT.DLL points `lpDistanceToMoveHigh` at a stack word it never sets, and adding it in read
  its car models from past their end, as zeros (then a pointer below an allocation);
- `lstrcmpW`/`lstrcmpiW` take a null string as less than any other, as Windows does (the CRT's
  `wcscmp` does not): RALLY2.EXE's table of a stage's objects leaves a name empty;
- the compiler's float conversions `_itos` and `_utos` (R4 to FR0) and `_stoi` and `_stou` (FR4
  to R0, truncated, saturating as FTRC does).

Next for speed: the race keeps the console's pace in a window, with little room at its start;
what is left is the interpreter's dispatch, one instruction at a time (the WinCE executables are
not statically recompiled). Also open: the GD-ROM device
(`\Device\CDROM0`, code 0x25040, served by WSEGACD.DLL once a GD-ROM drive is emulated), WDMLIB's
`KernelIoControl(1)`, a BIOS MISC call 1 (return to the console menu) during start-up that is
ignored, and what the title asks when a track ends (not yet seen: a menu track lasts minutes).

## Validation

`runtime/tests/test_wince_mmu.cpp`, `test_wince_pe.cpp`, `test_wince_media.cpp`,
`test_wince_modules.cpp`, `test_wince_directx.cpp`, `test_wince_kernel.cpp`,
`test_wince_synchronization.cpp`, `test_wince_threads.cpp` (the scheduler: priorities, slices,
the quantum, blocking waits, suspension, deadlock) and `test_wince_hardware.cpp` (store queues
after `SetStoreQueueBase`) use hand-built fixtures only; `test_wince_kernel.cpp` also covers
the `__int64` helpers, `AddEventAccess` references and the CD-ROM requests (the table of
contents, and play, pause, resume, volume and stop reaching the sound output), and
`test_vmu_fs.cpp` the card's screen
media info. They exercise ASID isolation, all page sizes, permissions, dirty bits, MMU register
effects, fault-before-write behavior, virtual instruction fetch, PE rejection cases, rebasing,
named and ordinal IAT binding, an interpreted PE-to-COREDLL call, surface pixels and PCM samples.
The ordinary runtime test suite also runs, including the existing Katana memory tests.
New checks cover dependency cycles, forwarded exports, failed-link rollback, DLL attach, dynamic
module APIs, event lifetime, COM identity/reference counts, guest pointers, stack arguments,
surface flip/lock behavior, indexed triangles, PCM wraparound and interpreted factory/vtable calls.
`wince.directx` generates a PE, executes it at two bases and compares its rendered pixels and
every captured PCM sample. Python 3 is needed for this CTest integration test.
V4 adds fragmentation traces, realloc failure preservation, heap ownership, reservation/commit
rollback, executable page checks, cross-page atomic writes, UTF-16/byte strings, integer wrap,
64-bit counters, formatting limits and an interpreted variadic call with a fifth argument.
V5 adds ten synchronization cases for recursion, shared names, object lifetime, wait-array
validation, pulses, timeouts, resource budgets and atomic critical-section writes. Its interpreted
EXE/DLL test checks attach, shared objects and static/dynamic procedure calls at two image bases.
V6 adds signed/unsigned division/remainder boundaries, 1,024 arithmetic identities, explicit
zero-divisor faults and `wince.arithmetic`: an original SH-4 PE with 32 assertions at two bases.
The local development build passes **145 runtime tests, including 61 Windows CE cases**.

```sh
python -m pytest -q tools/dcdisc/tests
```

All source fixtures and generated demo data are original. The reproducible suite needs no Sega
or Microsoft OS/game binaries. Separately, three local probes compared wsprintf output with
the original SH-4 COREDLL export and matched byte-for-byte. Its DLL initializer was deliberately
bypassed to isolate the formatting routine; this was not an OS startup or commercial-game test.
Those private DLL copies are confined to an ignored build directory and excluded from the archive.

## References and next milestone

The memory behavior follows the public [Renesas SH7750 hardware manual, sections 3 and 4](https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware?r=1055161).
The executable layout and relocations follow Microsoft's [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
The clock API is described in [GetTickCount for Windows CE 3.0](https://learn.microsoft.com/en-us/previous-versions/ms908465(v=msdn.10)).

The next concrete milestone is extending the verified title's OS startup services and graphics
requirements, including SH-4 exception/compiler helpers, thread scheduling, files, windows/messages and
additional DirectX methods.
Kernel boot is a different path: it first needs CPU fault delivery and ITLB behavior. Keep
`new-game` and the launcher's AJOUTER UN JEU gated for Windows CE discs until titles past Sega
Rally 2 complete initialization with implemented services and repeatable reference checks.
