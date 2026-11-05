# Windows CE HLE services — experimental v6

These are application-level services implemented by `Kernel`/`KernelServices`/`Synchronization`, not a complete
Windows CE kernel. The process has one guest thread. No NK.EXE boot, scheduler, filesystem,
window/message service or registry implementation is supplied here.
Unknown imports still fail linking; unsupported flags and formatting requests fail explicitly.

The v6 table contains 90 distinct service names: the original 23, 53 added in v4, 10 in v5 and 4 in v6.
Unsuffixed aliases for wide module/event/mutex APIs are additional spellings.
Ordinals are resolved from supplied module metadata; no game-specific ordinal table is embedded.

## SH-4 compiler arithmetic added in v6

`_divls`, `_divlu`, `_modls` and `_modlu` implement signed/unsigned 32-bit division and remainder.
The Microsoft SH-4 helper convention uses R4 for the numerator, R5 for the divisor and R0 for
the result. Signed quotients truncate toward zero; nonzero signed remainders have the numerator's
sign. INT_MIN / -1 returns the 32-bit bit pattern 0x80000000 and its remainder is zero, matching
the supplied SH-4 runtime. Computation uses wider host integers to avoid host signed overflow.

Zero divisors stop execution with an explicit diagnostic; CE exception dispatch and
`__C_specific_handler` remain unimplemented. These helpers preserve last-error.
Tests cover signed boundaries, 1,024 deterministic quotient/remainder identities, zero-divisor
faults, and 32 arithmetic assertions executed by an original SH-4 PE at two image bases.
Separately, 24 local probes of the supplied COREDLL arithmetic exports confirmed argument order,
signs and overflow behavior. Only the original public fixtures are distributed; the private DLL
used for those probes is excluded. Its initializer was bypassed for the isolated comparisons.

## Synchronization added in v5

New names: `InitializeCriticalSection`, `DeleteCriticalSection`, `EnterCriticalSection`,
`LeaveCriticalSection`, `TryEnterCriticalSection`, `CreateMutexW`, `ReleaseMutex`, `EventModify`,
`PulseEvent`, `WaitForMultipleObjects`. `CreateMutex` aliases the wide entry. The original event,
single-wait and CloseHandle trap addresses remain valid and now dispatch through the shared service.

Events and mutexes share a process-local, case-sensitive UTF-16 namespace. A second creation of
the same type opens another handle to the existing object and sets ERROR_ALREADY_EXISTS (183);
new initial state/ownership parameters are ignored. A name belonging to the other type fails
with ERROR_INVALID_HANDLE (6). Names are at most 259 UTF-16 units plus terminator and cannot
contain backslashes; NULL and empty names create unnamed objects. Closing one handle preserves
other handles, object state and mutex recursion. The final close discards the object and name.
CloseHandle now accepts events and mutexes, but not internal critical-section tokens.

Mutex creation can grant initial ownership. Successful waits acquire it recursively for the
current guest thread (ID 2); each acquisition needs one ReleaseMutex. Releasing an unowned mutex
fails with ERROR_NOT_OWNER (288). This does not implement thread termination or abandoned mutexes.
Auto-reset events consume their signal on one successful wait; manual-reset events retain it.
EventModify accepts PULSE=1, RESET=2, SET=3. A pulse with no waiting thread leaves the event reset.

WaitForMultipleObjects supports 1–64 event/mutex handles with wait-all FALSE, as required by CE.
It chooses the lowest ready index and consumes only that object's state. The complete handle
array is validated before consumption; invalid/duplicate handles fail without changing any object.
Wait failures return WAIT_FAILED with last-error. Unsignaled finite waits advance the virtual
clock and return WAIT_TIMEOUT; finite values above 0x7FFFFFFF are clamped. Unsignaled infinite
waits fault explicitly because this runtime cannot run another thread to satisfy them.

Critical sections use a 16-byte, four-byte-aligned legacy Dreamcast CE record: recursion count,
owner, opaque internal token and needtrap. The footprint/offsets were checked against the locally
supplied SH-4 runtime. Later CE records with additional fields are not selected automatically.
Ownership/lifetime metadata is held by the host and mirrored in the guest record. The unowned
count is normalized to zero; internal bytes are opaque and must not be used as a native kernel
object or mixed with native/inline locking routines. This is HLE for calls through COREDLL.

Entry is recursive, including TryEnterCriticalSection. Each entry requires one leave. Deletion
requires an initialized, unowned record, zeroes its 16 bytes and frees host metadata. Repeated or
overlapping initialization, copied/overwritten records, leave without ownership and deletion
while owned fault explicitly. Reads/writes for all 16 bytes are checked before mutation, including
cross-page protection boundaries. As with the heap, unchanged bytes in freed suballocations are
not a general use-after-free detector; delete the critical section before freeing its storage.
TryEnterCriticalSection is a CE 3.0 extension exposed by name, with ordinals supplied only by
verified metadata. All acquisitions in the current one-thread process are uncontended.

Resource bounds are 1,024 simultaneously open event/mutex handles and 4,096 initialized critical
sections. Handles and critical tokens are not recycled within a process; their separate address
ranges each allow 262,144 lifetime allocations. Exhaustion fails instead of reusing stale handles.
No cross-process namespace, wait queues, fairness, priority inheritance, preemption or asynchronous
signaling is implemented. A future scheduler must integrate ownership, wakeups and per-thread state.

The original EXE/DLL fixture runs DllMain, recursive critical sections and mutexes, shared named
events, static/dynamic exports, wait-any, handle lifetime and explicit cleanup. At two EXE bases
it executes 594 main-run instructions, 44 HLE calls and 55 DLL initialization instructions, with
the guest checking results itself. A second DLL deliberately enters an uninitialized critical
section to verify that attach stops with a fault rather than reaching the EXE.

## Heap ownership and reallocation

New names: `GetProcessHeap`, `HeapCreate`, `HeapDestroy`, `HeapAlloc`, `HeapReAlloc`, `HeapFree`,
`HeapSize`, `HeapValidate`, `LocalReAlloc`, `LocalSize`, `malloc`, `calloc`, `realloc`, `free`.
The existing `LocalAlloc`/`LocalFree` now use the same guest heap manager with a separate ownership tag.

Heaps use eight-byte-aligned blocks from normally 64 KiB arenas. Small blocks share pages;
freed ranges coalesce and can be reused. Metadata stays outside guest memory. Private heaps can
have a maximum committed size; the process heap cannot be destroyed. Destroying a private heap
invalidates its allocations and returns its arenas to the process budget. Empty extra arenas are
returned to the budget; one arena per heap is retained for reuse.

Heap, Local and CRT allocation families are checked separately. LocalFree/free cannot release
DirectX buffers, stack allocations or PE sections. Invalid Win32 heap calls return failure and set
last-error; invalid CRT frees stop execution with a fault. This is not a use-after-free detector:
suballocated freed bytes remain physically accessible until the containing arena is released.

Reallocation preserves the old prefix. It grows in place when possible; otherwise it moves if
allowed. A failed reallocation leaves the old pointer/data valid. HEAP_ZERO_MEMORY zeroes newly
added bytes, and HEAP_REALLOC_IN_PLACE_ONLY prohibits moving. LocalReAlloc needs LMEM_MOVEABLE
to permit moving a fixed block; LocalAlloc itself still accepts only fixed/zero-init flags.
LocalAlloc zeroes all returned bytes for deterministic startup. Heap/CRT allocations without a
zeroing flag may contain reused bytes. HeapSize/LocalSize report the requested byte count.

Supported heap flags: NO_SERIALIZE, ZERO_MEMORY on allocation, REALLOC_IN_PLACE_ONLY on
reallocation; HeapCreate accepts NO_SERIALIZE only. GENERATE_EXCEPTIONS and movable local
handles are unavailable. Bounds: 256 heaps including the process heap, 65,536 live blocks, and
the shared 64 MiB committed-memory budget. Fixed-heap requests at or above 0x7FFF8 bytes fail.
`calloc` multiplication is checked; `realloc(NULL,n)` allocates and `realloc(p,0)` frees.

## Reservations, pages and protections

New names: `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`.

The development HLE reservation range is **0x20000000–0x3FFFFFFF**. It is deliberately separate
from image, ordinary allocation and trap/handle ranges; it does not reproduce CE process slots.
Reservations align to 64 KiB, pages to 4 KiB. An uncommitted reservation consumes no guest byte
storage. Commit allocates zeroed pages. Recommitting already committed pages preserves their
contents/protection. Decommit frees byte storage, retaining the reservation; recommit then starts
at zero. MEM_RELEASE requires the original base and a zero size. MEM_DECOMMIT can cover a page
subrange or the entire reservation using its base and a zero size.

VirtualAlloc supports MEM_RESERVE/COMMIT and MEM_TOP_DOWN for automatic reservations. Each
reservation is at most 64 MiB, with at most 1,024 reservations. All newly committed pages are
staged before publication, so memory exhaustion never leaves a partially committed operation.
PE installation rejects collisions with reservations, including currently uncommitted ones.

Page protection choices are NOACCESS, READONLY, READWRITE, EXECUTE, EXECUTE_READ and
EXECUTE_READWRITE. GUARD, NOCACHE, WRITECOPY, physical mappings and VirtualCopy are unavailable.
VirtualProtect applies only to these reservations, requires every affected page to be committed
within one reservation, and validates the old-protection output before changing access. If that
output is inside the target range it is written before protection changes. Atomic preflight makes
a failing cross-page write leave all destination bytes unchanged.

VirtualQuery writes the 28-byte guest MEMORY_BASIC_INFORMATION layout. Within a reservation it
reports successive pages with matching state/protection, stopping at an allocation boundary.
Other image/allocation regions are reported from the software mapping with page-rounded extents;
actual PE byte-access bounds remain stricter than those rounded descriptions. Queries at or above
0x40000000 fail. Image/heap page protection changes are not implemented.

## Memory, byte strings and UTF-16

New names:

- Memory: `memcpy`, `memmove`, `memset`, `memcmp`.
- Byte strings: `strlen`, `strcpy`, `strcat`, `strncpy`, `strcmp`, `strncmp`, `strchr`, `strrchr`.
- UTF-16: `wcslen`, `wcscpy`, `wcscat`, `wcsncpy`, `wcscmp`, `wcsncmp`, `wcschr`, `wcsrchr`.
- Comparisons/navigation: `_wcsicmp`, `_wcsnicmp`, `lstrcmpW`, `lstrcmpiW`, `CharPrevW`.

These functions read guest memory only and preflight complete writes. memmove snapshots its
source, preserving overlapping copies. memcpy also uses that implementation; callers must still
respect its non-overlap contract. Comparisons use unsigned byte/code-unit ordering. strncpy and
wcsncpy pad to the requested count, and leave an exactly truncated result unterminated, matching
their C contracts. Searches can find the terminating zero. Wide lengths count 16-bit units,
including surrogate pairs as two units.

Case folding implements ASCII only. The CRT routines leave other code units unchanged; Windows
lstrcmpW/lstrcmpiW reject non-ASCII input because linguistic NLS sorting is not implemented.
CharPrevW moves one UTF-16 unit. Multibyte conversion, locale-specific case mapping and the
compiler's floating-point conversion and exception helpers remain unavailable. Memory transfers are bounded by
64 MiB; string scans/counts by 1,048,576 units. Invalid pointers or missing terminators cause faults.

## Integers, timing, debug messages and formatting

New names: `InterlockedIncrement`, `InterlockedDecrement`, `InterlockedExchange`,
`InterlockedCompareExchange`, `InterlockedTestExchange`, `QueryPerformanceCounter`,
`QueryPerformanceFrequency`, `OutputDebugStringW`, `wsprintfW`, `wvsprintfW`.

Interlocked operations use aligned 32-bit guest values and are indivisible in the single-threaded
dispatcher. Increment/decrement wrap modulo 2^32 and return the new value. Exchange/conditional
exchange return the old value; TestExchange takes `(target, old, new)`, while CompareExchange
takes `(target, exchange, comparand)`. A future scheduler must retain atomic dispatch semantics.

PerformanceCounter returns the 64-bit virtual CPU cycle count; Frequency returns 200,000,000.
This matches the existing HLE tick convention, without claiming host-clock or hardware-cycle
accuracy. OutputDebugStringW retains the last 256 messages of at most 4,095 UTF-16 units, available
through `Kernel::debug_messages()`. No external logging or host file writes occur.

Formatting supports `%d`, `%i`, `%u`, `%x`, `%X`, `%s`, `%S`, `%c`, `%C`, `%%`, decimal widths,
numeric precisions, left alignment and zero padding. String/character length modifiers `h`, `l`,
`w` select byte/wide input; integers are 32-bit with optional `l`. Wide strings are UTF-16; narrow
string/character input is limited to ASCII instead of guessing an ANSI code page. Floats, `*`,
`#`, `%p`, `%n`, 64-bit integer formats and other modifiers are rejected. Output is limited to
511 UTF-16 units plus terminator (1,024 bytes). Unsupported requests return zero with last-error;
failed formatting leaves destination contents intact. Callers must provide sufficient storage.

Variadic arguments use the Windows CE register/home-space convention. wvsprintfW reads the
explicit guest argument-list pointer. The original v3 synthetic executables used an incorrect
stack offset and must be regenerated. DLL startup now reserves the register home area as well.

## Verification and sources

The original C++ fixtures check synchronization lifetime/ownership, wait-array validation,
cross-page critical sections, allocation ownership, deterministic fragmentation/reallocation,
budget exhaustion, partial-page commits, protection rollback, executable memory, UTF-16 and
byte-string boundaries, 64-bit counters, arithmetic wrap and variadic SH-4 execution. The guest
DirectX fixture is still checked at two image bases with identical captured pixels/PCM.

Three separate local comparisons ran the existing COREDLL SH-4 formatting export through the
interpreter and compared its output bytes with HLE: stack varargs, zero-padded hexadecimal,
signed minimum/unsigned maximum, negative zero padding and a literal percent. They matched.
The DLL initializer was bypassed for those isolated probes. The private DLL and probe outputs
are excluded from the distributable archive; the public suite uses original fixtures only.

Public contracts consulted: Microsoft [HeapAlloc](https://learn.microsoft.com/en-us/previous-versions/ms908479(v=msdn.10)),
[HeapReAlloc](https://learn.microsoft.com/en-us/previous-versions/ms908485(v=msdn.10)),
[VirtualAlloc](https://learn.microsoft.com/en-us/previous-versions/ms913486(v=msdn.10)),
[VirtualQuery](https://learn.microsoft.com/en-us/previous-versions/ms913500(v=msdn.10)),
[InterlockedTestExchange](https://learn.microsoft.com/en-us/previous-versions/ms956784(v=msdn.10)) and
[wsprintf for CE](https://learn.microsoft.com/en-us/previous-versions/ms942863(v=msdn.10)).

Synchronization contracts: Microsoft [EnterCriticalSection](https://learn.microsoft.com/en-us/previous-versions/ms960342(v=msdn.10)),
[TryEnterCriticalSection](https://learn.microsoft.com/en-us/previous-versions/ms942814(v=msdn.10)),
[CreateEvent](https://learn.microsoft.com/en-us/previous-versions/ms959948(v=msdn.10)),
[CreateMutex](https://learn.microsoft.com/en-us/previous-versions/ms959960(v=msdn.10)),
[ReleaseMutex](https://learn.microsoft.com/en-us/previous-versions/ms942538(v=msdn.10)),
[WaitForMultipleObjects](https://learn.microsoft.com/en-us/previous-versions/ms915515(v=msdn.10)),
[PulseEvent](https://learn.microsoft.com/en-us/previous-versions/ms961329(v=msdn.10)) and
[EventModify wrapper relationship](https://learn.microsoft.com/fr-fr/previous-versions/windows/embedded/ms903993(v=msdn.10)).
Event operation numbers were checked in the upstream [Free Pascal CE API declarations](https://raw.githubusercontent.com/fpc/FPCSource/master/rtl/wince/wininc/defines.inc).
