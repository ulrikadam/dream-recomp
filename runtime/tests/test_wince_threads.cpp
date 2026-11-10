// The guest thread scheduler on its own: thread bodies here are host code standing in for an
// interpreter run, selected by the R4 a thread starts with.
#include <stdexcept>
#include <string>
#include <vector>

#include "dream/runtime/wince/threads.h"

#include "doctest.h"

using dream::wince::Scheduler;

namespace {
using Script = std::function<std::uint32_t(Scheduler&, dream::sh4::Ctx&)>;

// Schedulers are never deleted (their host threads are detached), as in dream-wince.
Scheduler& make(const std::vector<Script>& scripts, dream::sh4::Ctx& main) {
    auto** self = new Scheduler*(nullptr);
    auto* s = new Scheduler(
        [self, scripts](dream::sh4::Ctx& ctx) { return scripts.at(ctx.r[4])(**self, ctx); });
    *self = s;
    s->adopt_main(main);
    return *s;
}
std::uint32_t start(Scheduler& s, std::uint32_t script, bool suspended = false) {
    dream::sh4::Ctx ctx{};
    ctx.r[4] = script;
    std::uint32_t id = 0;
    return s.create(ctx, suspended, id);
}
void join(Scheduler& s, std::uint32_t handle) {
    s.wait([&] { return s.exited(handle); }, std::nullopt);
}
}  // namespace

TEST_CASE("wince threads: equal priorities take turns at each yield") {
    std::vector<std::string> log;
    const std::vector<Script> scripts{[&](Scheduler& s, dream::sh4::Ctx&) -> std::uint32_t {
        for (int i = 0; i < 3; ++i) {
            log.push_back(std::to_string(s.current_id()));
            s.yield();
        }
        return 10 + s.current_id();
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto a = start(s, 0), b = start(s, 0);
    join(s, a);
    join(s, b);
    CHECK(log == std::vector<std::string>{"3", "4", "3", "4", "3", "4"});
    CHECK(s.exit_code(a) == 13u);
    CHECK(s.exit_code(b) == 14u);
    CHECK(s.created() == 2);
}

TEST_CASE("wince threads: a time slice ends a turn only after the quantum, or for a higher one") {
    std::vector<std::string> log;
    const std::vector<Script> scripts{[&](Scheduler&, dream::sh4::Ctx&) -> std::uint32_t {
        log.push_back("worker");
        return 0;
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0);
    main.cycles += Scheduler::kQuantum / 2;
    s.preempt(Scheduler::kQuantum);  // an equal priority waits for the rest of the quantum
    CHECK(log.empty());
    main.cycles += Scheduler::kQuantum / 2;
    s.preempt(Scheduler::kQuantum);  // used up: the other one runs
    CHECK(log == std::vector<std::string>{"worker"});
    join(s, h);
    // A higher priority made ready does not wait for the quantum.
    const auto urgent = start(s, 0, true);
    CHECK(s.set_priority(urgent, 1));
    CHECK(s.resume(urgent) == 1u);
    s.preempt(Scheduler::kQuantum);
    CHECK(log.size() == 2);
    join(s, urgent);
}

TEST_CASE("wince threads: a higher priority runs until it waits; suspend and resume") {
    std::vector<std::string> log;
    const std::vector<Script> scripts{[&](Scheduler& s, dream::sh4::Ctx&) -> std::uint32_t {
        log.push_back("a");
        s.yield();  // still the highest priority: carries on
        log.push_back("b");
        return 0;
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0, true);
    s.yield();  // suspended: does not run
    CHECK(log.empty());
    CHECK(s.set_priority(h, 2));
    CHECK(s.priority(h) == 2);
    CHECK(s.resume(h) == 1u);
    CHECK(s.exit_code(h) == Scheduler::kStillActive);
    s.yield();
    log.push_back("main");
    CHECK(log == std::vector<std::string>{"a", "b", "main"});
}

TEST_CASE("wince threads: when every thread waits, the clock jumps to the next deadline") {
    std::uint64_t woke = 0;
    const std::vector<Script> scripts{[&](Scheduler& s, dream::sh4::Ctx& ctx) -> std::uint32_t {
        CHECK_FALSE(s.wait([] { return false; }, ctx.cycles + 1'000'000));
        woke = ctx.cycles;
        return 0;
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0);
    join(s, h);
    CHECK(woke == 1'000'000);
    CHECK(main.cycles >= 1'000'000);
}

TEST_CASE("wince threads: a signal wakes a waiting thread") {
    bool flag = false, seen = false;
    const std::vector<Script> scripts{[&](Scheduler& s, dream::sh4::Ctx&) -> std::uint32_t {
        seen = s.wait([&] { return flag; }, std::nullopt);
        return 5;
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0);
    s.yield();  // it starts and waits
    CHECK_FALSE(seen);
    flag = true;
    s.notify();
    join(s, h);
    CHECK(seen);
    CHECK(s.exit_code(h) == 5u);
    CHECK(s.close_handle(h));
    CHECK_FALSE(s.owns_handle(h));
}

TEST_CASE("wince threads: a wait nobody can satisfy is a deadlock") {
    const std::vector<Script> scripts{};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    CHECK_THROWS_AS(s.wait([] { return false; }, std::nullopt), std::runtime_error);
}

TEST_CASE("wince threads: a thread's error is raised on the main thread") {
    const std::vector<Script> scripts{
        [](Scheduler&, dream::sh4::Ctx&) -> std::uint32_t { throw std::runtime_error("boom"); }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0);
    CHECK_THROWS_WITH(join(s, h), "boom");
}

TEST_CASE("wince threads: a terminated thread ends where it waits") {
    const std::vector<Script> scripts{[](Scheduler& s, dream::sh4::Ctx&) -> std::uint32_t {
        s.wait([] { return false; }, std::nullopt);
        return 1;  // never reached
    }};
    dream::sh4::Ctx main{};
    auto& s = make(scripts, main);
    const auto h = start(s, 0);
    s.yield();
    CHECK(s.terminate(h, 77));
    join(s, h);
    CHECK(s.exit_code(h) == 77u);
}
