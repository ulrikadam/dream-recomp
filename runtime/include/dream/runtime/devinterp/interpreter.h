// Development-time SH-4 interpreter (WP2.7, ADR 2). Compiled only under DREAM_DEV_INTERPRETER and
// reached from System::on_untranslated: when emitted code transfers control to an address with no
// translation, the interpreter executes guest code from memory until control comes back to the
// caller, calling any *translated* function it meets natively on the way. Release builds compile
// this out and the miss becomes a fault (docs/runtime-devinterp.md).
//
// Semantics are the emitter's, executed at run time: the same decoder, the same ops.h helpers, the
// same delay-slot ordering, so translated and interpreted code cannot disagree. The golden replay
// test runs every captured case through both.
#pragma once

#include <cstdint>
#include <functional>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::devinterp {

struct Options {
    // bsr/jsr/bsrf (and jmp/braf tail calls) whose target has a translation call it natively and
    // resume interpreting at the return address; off, everything is interpreted.
    bool call_translated = true;
    // Targets in [native_lo, native_hi) are always called natively even when call_translated is
    // off: the BIOS HLE hooks have no guest code behind them.
    std::uint32_t native_lo = 0, native_hi = 0;
    // A non-local return thrown by natively called code is caught and interpreting continues at
    // its target (the harness); off, it propagates to sh4::run_guest (the runtime).
    bool catch_nonlocal = false;
    // An RTS at the level the run started at that goes anywhere but `return_to` leaves the run as
    // a non-local return (sh4::nonlocal_return), as an RTS to a PR other than the entry PR does in
    // translated code. For runs entered from translated code: a jmp or braf there is a tail call,
    // and PR may still hold the return address of a call made earlier in the same function, so
    // the code's own RTS (to the PR it pops) never matches `return_to`. Without this the run went
    // on interpreting the caller's code, nested, until the stack no longer matched the host
    // frames (18 Wheeler, at a byte-table switch the translator did not recover).
    bool leave_on_outer_return = false;
    std::uint64_t max_instructions = 0;  // 0 = unlimited
    // Optional process-local HLE dispatch. Runs after a call's delay slot with SH-4 arguments
    // in the context; true means it returned normally, false means interpret the guest target.
    std::function<bool(sh4::Ctx&, ::dream::Memory&, std::uint32_t)> hle_call;
};

struct Stats {
    std::uint64_t runs = 0;
    std::uint64_t instructions = 0;  // interpreted, delay slots included
    std::uint64_t native_calls = 0;  // transfers handed to translated functions
};

enum class Stop {
    Returned,   // control reached `return_to`
    Rte,        // an RTE ran (exception handler done; the delivering frame resumes)
    StepLimit,  // Options::max_instructions reached
};

// Interprets from c.pc until control transfers to `return_to` (the PR the caller left), an RTE
// executes, or the step limit is hit. c.pc is the current instruction throughout (faults and
// traps see it). Polls the interrupt deadline like emitted code does: at entry, at back-edges and
// after calls.
Stop run(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t return_to, const Options& opt = {},
         Stats* stats = nullptr);

}  // namespace dream::devinterp
