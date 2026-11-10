// WP2.7: the dev interpreter as the function-table miss handler, and the release configuration's
// fault instead. Guest code is hand-assembled into RAM; addresses are chosen so nothing here
// collides with the translated stand-ins other tests register.
#include <initializer_list>
#include <utility>
#include <vector>

#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"

#include "doctest.h"
#ifdef DREAM_DEV_INTERPRETER
#include "dream/runtime/devinterp/interpreter.h"
#endif

using namespace dream;

namespace {

#ifdef DREAM_DEV_INTERPRETER
void put(Memory& m, std::uint32_t at, std::initializer_list<std::uint16_t> words) {
    for (std::uint16_t w : words) {
        m.write16(at, w);
        at += 2;
    }
}
#endif

// Translated stand-ins. `caller` does what emitted code does at a jsr whose target the table
// cannot resolve: sets PR to the return address, PC to the call site, and goes through
// call_indirect. `leaf` is a translated callee for interpreted code to call natively.
void caller(sh4::Ctx& c, Memory& m) {
    c.pr = 0x8C030008u;
    c.pc = 0x8C030004u;
    sh4::call_indirect(c, m, 0x8C020000u);
    c.r[7] = 0x77u;  // runs after the interpreted callee returned
}
void leaf(sh4::Ctx& c, Memory&) {
    c.r[1] += 100u;
}

const sh4::FunctionEntry kFns[] = {{0x8C030000u, caller}, {0x8C030100u, leaf}};
struct Registrar {
    Registrar() { sh4::register_functions(kFns, 2); }
} g_registrar;

}  // namespace

#ifdef DREAM_DEV_INTERPRETER

TEST_CASE(
    "devinterp: an untranslated callee runs from memory and returns to the translated caller") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    sh4::write_sr(c, 0x700000F0u);
    // mov.l @r4,r0 ; add #5,r0 ; rts ; add #1,r0 (in the delay slot)
    put(sys.memory, 0x8C020000u, {0x6042u, 0x7005u, 0x000Bu, 0x7001u});
    sys.memory.write32(0x8C100000u, 40u);
    c.r[4] = 0x8C100000u;
    sh4::call_indirect(c, sys.memory, 0x8C030000u);
    CHECK(c.r[0] == 46u);
    CHECK(c.r[7] == 0x77u);
    CHECK(sys.untranslated.total() == 1);
    CHECK(sys.interpreter_runs == 1);
    CHECK(sys.interpreted_instructions == 4);
    CHECK(sys.interpreter_native_calls == 0);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: interpreted code calls a translated function natively and resumes") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    sh4::write_sr(c, 0x700000F0u);
    // The shape every compiler emits around a call: PR saved and restored across the jsr.
    // sts.l pr,@-r15 ; mov.l @(3,pc),r2 -> 0x8C030100 ; jsr @r2 ; nop ; lds.l @r15+,pr ; rts ; nop
    // ; (pad) ; literal
    put(sys.memory, 0x8C020000u,
        {0x4F22u, 0xD203u, 0x420Bu, 0x0009u, 0x4F26u, 0x000Bu, 0x0009u, 0x0009u});
    sys.memory.write32(0x8C020010u, 0x8C030100u);
    c.r[15] = 0x8C00F400u;  // a stack for the PR save
    c.r[1] = 5u;
    sh4::call_indirect(c, sys.memory, 0x8C030000u);
    CHECK(c.r[1] == 105u);
    CHECK(c.r[7] == 0x77u);
    CHECK(c.r[15] == 0x8C00F400u);  // balanced
    CHECK(sys.interpreter_native_calls == 1);
    CHECK(sys.interpreted_instructions == 7);  // sts.l, mov.l, jsr, nop, lds.l, rts, nop
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: an untranslated interrupt handler runs in bank 1 and its RTE unwinds") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    c.vbr = 0x8C040000u;  // handler at VBR+0x600, an address with no translation
    sh4::write_sr(c, 0x40000000u);
    sys.memory.write32(0xA05F6930u, 0x8u);  // IML6NRM: VBlank-in -> IRL9
    // mov #8,r1 ; mov.l @(2,pc),r0 -> SB_ISTNRM ; mov.l r1,@r0 (acknowledge) ; mov #0x11,r2 ;
    // rte ; nop ; literal 0xA05F6900
    put(sys.memory, 0x8C040600u, {0xE108u, 0xD002u, 0x2012u, 0xE211u, 0x002Bu, 0x0009u});
    sys.memory.write32(0x8C04060Cu, 0xA05F6900u);
    c.r[2] = 0x2222u;  // bank 0 value, must survive
    sys.holly.raise(holly::Irq::VBlankIn);
    c.cycles = 100;
    c.next_event = 0;
    sh4::deliver_irq(c, sys.memory);
    CHECK(sys.interrupts_delivered == 1);
    CHECK(sys.interpreter_runs == 1);
    CHECK(!sys.holly.raised(holly::Irq::VBlankIn));  // acknowledged by the interpreted handler
    CHECK(c.r_bank[2] == 0x11u);                     // written while RB was set
    CHECK(c.r[2] == 0x2222u);                        // bank 0 restored by RTE
    CHECK(sh4::read_sr(c) == 0x40000000u);
    CHECK(sys.nesting == 0);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: the step limit stops a runaway loop") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    put(sys.memory, 0x8C020000u, {0xAFFEu, 0x0009u});  // bra . ; nop
    c.pc = 0x8C020000u;
    devinterp::Options opt;
    opt.max_instructions = 50;
    devinterp::Stats st;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, &st) == devinterp::Stop::StepLimit);
    CHECK(st.instructions >= 50);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: on_return sees every RTS with its return address and result") {
    // sts.l pr,@-r15 ; bsr callee ; nop ; lds.l @r15+,pr ; rts ; nop ; (pad) ;
    // callee: mov #42,r0 ; rts ; add #1,r0 (the slot runs before the hook looks)
    System sys;
    sys.install();
    auto& c = sys.ctx;
    put(sys.memory, 0x8C020000u,
        {0x4F22u, 0xB005u, 0x0009u, 0x4F26u, 0x000Bu, 0x0009u, 0x0009u, 0x0009u, 0xE02Au, 0x000Bu,
         0x7001u});
    c.r[15] = 0x8C00F400u;
    c.pr = 0x8C00FFF0u;
    c.pc = 0x8C020000u;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> seen;  // return address, R0
    devinterp::Options opt;
    opt.on_return = [&](sh4::Ctx& ctx, std::uint32_t to) {
        seen.emplace_back(to, ctx.r[0]);
        ctx.cycles += 1000;  // the hook may let time pass
    };
    const auto before = c.cycles;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, nullptr) == devinterp::Stop::Returned);
    CHECK(seen == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x8C020006u, 43u},
                                                                       {0x8C00FFF0u, 43u}});
    CHECK(c.cycles >= before + 2000);
    CHECK(c.r[15] == 0x8C00F400u);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: a pre-decrement store of its own pointer writes the value from before") {
    // mov.l r15,@-r15 ; mov.w r1,@-r1 ; mov.b r2,@-r2 ; rts ; nop. GCC pushes a struct-return
    // pointer with the first one (Virtua Tennis); storing the decremented r15 put every returned
    // vector 4 bytes off.
    System sys;
    sys.install();
    auto& c = sys.ctx;
    put(sys.memory, 0x8C020000u, {0x2FF6u, 0x2115u, 0x2224u, 0x000Bu, 0x0009u});
    c.r[15] = 0x8C100010u;
    c.r[1] = 0x8C100022u;
    c.r[2] = 0x8C100031u;
    c.pr = 0x8C00FFF0u;
    c.pc = 0x8C020000u;
    devinterp::Options opt;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, nullptr) == devinterp::Stop::Returned);
    CHECK(c.r[15] == 0x8C10000Cu);
    CHECK(sys.memory.read32(0x8C10000Cu) == 0x8C100010u);
    CHECK(c.r[1] == 0x8C100020u);
    CHECK(sys.memory.read16(0x8C100020u) == 0x0022u);
    CHECK(c.r[2] == 0x8C100030u);
    CHECK(sys.memory.read8(0x8C100030u) == 0x31u);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: an instruction rewritten after it ran runs as rewritten") {
    // The interpreter keeps decoded instructions; one whose word changes must be decoded again.
    //   mov #0,r0 ; X: add #1,r0 ; cmp/eq #1,r0 ; bf end ; mov.w r5,@r4 (X := add #16,r0) ;
    //   bra X ; nop ; end: rts ; nop
    // Stale, X would add 1 again and return 2.
    System sys;
    sys.install();
    auto& c = sys.ctx;
    put(sys.memory, 0x8C020000u,
        {0xE000u, 0x7001u, 0x8801u, 0x8B02u, 0x2451u, 0xAFFAu, 0x0009u, 0x000Bu, 0x0009u});
    c.r[4] = 0x8C020002u;
    c.r[5] = 0x7010u;
    c.pr = 0x8C00FFF0u;
    c.pc = 0x8C020000u;
    devinterp::Options opt;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, nullptr) == devinterp::Stop::Returned);
    CHECK(c.r[0] == 17u);
    // Run again with the word put back: decoded again, from what is there now.
    sys.memory.write16(0x8C020002u, 0x7001u);
    c.pr = 0x8C00FFF0u;
    c.pc = 0x8C020000u;
    CHECK(devinterp::run(c, sys.memory, 0x8C00FFF0u, opt, nullptr) == devinterp::Stop::Returned);
    CHECK(c.r[0] == 17u);
    sh4::set_hooks(nullptr);
}

TEST_CASE("devinterp: entered by a jump, an outer RTS elsewhere leaves as a non-local return") {
    // Translated code reached this with a jmp after an earlier call left PR stale, so return_to
    // is not where the code goes back to: its own epilogue pops the real return address. An
    // inner bsr/rts pair stays inside the run.
    System sys;
    sys.install();
    auto& c = sys.ctx;
    // bsr +6 ; nop ; lds.l @r15+,pr ; rts ; add #1,r0     callee: rts ; add #10,r0
    put(sys.memory, 0x8C020000u, {0xB006u, 0x0009u, 0x4F26u, 0x000Bu, 0x7001u});
    put(sys.memory, 0x8C020010u, {0x000Bu, 0x700Au});
    c.r[15] = 0x8C00F000u;
    sys.memory.write32(0x8C00F000u, 0x8C030200u);
    c.r[0] = 0;
    c.pc = 0x8C020000u;
    devinterp::Options opt;
    opt.leave_on_outer_return = true;
    std::uint32_t left_to = 0;
    try {
        devinterp::run(c, sys.memory, 0x8C0300F0u, opt, nullptr);
    } catch (const sh4::NonLocalReturn& n) {
        left_to = n.pc;
    }
    CHECK(left_to == 0x8C030200u);
    CHECK(c.r[0] == 11u);  // both delay slots ran, the inner return did not end the run
    CHECK(c.r[15] == 0x8C00F004u);
    sh4::set_hooks(nullptr);
}

#else

TEST_CASE("release: an untranslated target is recorded and fatal") {
    System sys;
    sys.install();
    auto& c = sys.ctx;
    c.pc = 0x8C030004u;
    CHECK_THROWS_AS(sh4::call_indirect(c, sys.memory, 0x8C020000u), UntranslatedCall);
    CHECK(sys.untranslated.total() == 1);
    CHECK(sys.interpreter_runs == 0);
    sh4::set_hooks(nullptr);
}

#endif
