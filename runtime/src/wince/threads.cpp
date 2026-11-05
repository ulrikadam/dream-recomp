#include "dream/runtime/wince/threads.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "dream/runtime/fenv.h"

namespace dream::wince {
thread_local Scheduler::Thread* Scheduler::self_ = nullptr;

void Scheduler::adopt_main(sh4::Ctx& ctx) {
    std::unique_lock lock(mutex_);
    if (threads_.contains(kMainThread))
        throw std::logic_error("main guest thread adopted twice");
    auto thread = std::make_unique<Thread>();
    thread->id = kMainThread;
    thread->cpu = &ctx;
    thread->state = State::Running;
    self_ = thread.get();
    running_ = kMainThread;
    now_ = ctx.cycles;
    threads_.emplace(kMainThread, std::move(thread));
}

std::uint32_t Scheduler::create(const sh4::Ctx& initial, bool suspended, std::uint32_t& id) {
    std::unique_lock lock(mutex_);
    if (threads_.size() >= 64)
        throw std::runtime_error("guest thread limit (64) reached");
    auto thread = std::make_unique<Thread>();
    thread->id = next_id_++;
    thread->own = initial;
    thread->cpu = &thread->own;
    thread->suspended = suspended ? 1 : 0;
    Thread* raw = thread.get();
    threads_.emplace(raw->id, std::move(thread));
    const auto handle = next_handle_;
    next_handle_ += 4;
    handles_.emplace(handle, raw->id);
    ++created_;
    id = raw->id;
    // Waits for its first turn before touching anything; see the class comment.
    std::thread(&Scheduler::host, this, raw).detach();
    return handle;
}

void Scheduler::host(Thread* thread) {
    self_ = thread;
    {
        std::unique_lock lock(mutex_);
        turn_.wait(lock, [&] { return running_ == thread->id; });
        if (on_switch)
            on_switch(thread->id);
    }
    // Each host thread has its own floating-point environment: give it the guest's mode.
    ::dream::fenv::apply(::dream::fenv::from_fpscr(thread->cpu->fpscr));
    std::uint32_t code = thread->exit_code;
    try {
        if (!thread->killed)
            code = body_(*thread->cpu);
    } catch (const ThreadExit& exit) {
        code = exit.code;
    } catch (...) {
        std::unique_lock lock(mutex_);
        if (!failure_)
            failure_ = std::current_exception();
    }
    std::unique_lock lock(mutex_);
    if (!thread->killed)
        thread->exit_code = code;
    thread->state = State::Exited;
    now_ = std::max(now_, thread->cpu->cycles);
    for (auto& [id, other] : threads_)
        if (other->state == State::Waiting)
            other->state = State::Ready;  // its handle is signalled now
    Thread* next = failure_ ? nullptr : pick(lock);
    if (!next) {
        if (!failure_)
            failure_ = std::make_exception_ptr(std::runtime_error(
                "deadlock: every remaining guest thread waits with no deadline"));
        next = threads_.at(kMainThread).get();  // the main thread reports it
    }
    next->state = State::Running;
    next->cpu->cycles = now_;
    running_ = next->id;
    turn_.notify_all();
}

Scheduler::Thread* Scheduler::pick(std::unique_lock<std::mutex>&) {
    auto wake_due = [&] {
        for (auto& [id, t] : threads_)
            if (t->state == State::Waiting && t->deadline && *t->deadline <= now_)
                t->state = State::Ready;
    };
    auto choose = [&]() -> Thread* {
        Thread* best = nullptr;
        for (auto& [id, t] : threads_) {
            if (t->state != State::Ready || (t->suspended && !t->killed))
                continue;
            // Higher priority first (lower number); among equals, the next id after the
            // thread that ran last, round robin.
            auto rank = [&](const Thread* x) {
                return std::make_pair(
                    x->priority, x->id > running_ ? x->id - running_ : x->id + 0x10000 - running_);
            };
            if (!best || rank(t.get()) < rank(best))
                best = t.get();
        }
        return best;
    };
    wake_due();
    if (Thread* best = choose())
        return best;
    // Everyone waits: let time pass, to the earliest of a thread's deadline and the devices'
    // next event; running the devices may raise the interrupt a thread waits for.
    for (;;) {
        std::optional<std::uint64_t> earliest;
        for (auto& [id, t] : threads_)
            if (t->state == State::Waiting && t->deadline && !t->suspended)
                earliest = earliest ? std::min(*earliest, *t->deadline) : *t->deadline;
        const auto device = device_deadline ? device_deadline() : std::nullopt;
        if (device && (!earliest || *device < *earliest))
            earliest = device;
        if (!earliest)
            return nullptr;
        now_ = std::max(now_, *earliest);
        if (run_devices && run_devices(now_))
            for (auto& [id, t] : threads_)
                if (t->state == State::Waiting)
                    t->state = State::Ready;
        wake_due();
        if (Thread* best = choose())
            return best;
        if (!device)  // only a thread deadline was due, and choose() found nobody: stuck
            return nullptr;
    }
}

void Scheduler::await_turn(std::unique_lock<std::mutex>& lock) {
    turn_.wait(lock, [&] { return running_ == self_->id; });
    self_->state = State::Running;
    if (on_switch)
        on_switch(self_->id);
    if (self_->killed)
        throw ThreadExit{self_->exit_code};
    if (failure_ && self_->id == kMainThread) {
        auto failure = failure_;
        failure_ = nullptr;
        std::rethrow_exception(failure);
    }
}

void Scheduler::switch_to(std::unique_lock<std::mutex>& lock, Thread* next) {
    now_ = std::max(now_, self_->cpu->cycles);
    if (next == self_) {
        self_->state = State::Running;
        self_->cpu->cycles = now_;
        return;
    }
    next->state = State::Running;
    next->cpu->cycles = now_;
    running_ = next->id;
    turn_.notify_all();
    await_turn(lock);
}

void Scheduler::yield() {
    std::unique_lock lock(mutex_);
    if (!self_)
        return;
    self_->state = State::Ready;
    Thread* next = pick(lock);
    if (!next)
        throw std::runtime_error("deadlock: the running guest thread is suspended");
    switch_to(lock, next);
}

bool Scheduler::wait(const std::function<bool()>& ready, std::optional<std::uint64_t> deadline) {
    for (;;) {
        if (ready())  // guest state: only the baton holder touches it, no lock needed
            return true;
        std::unique_lock lock(mutex_);
        now_ = std::max(now_, self_->cpu->cycles);
        if (deadline && now_ >= *deadline)
            return false;
        self_->state = State::Waiting;
        self_->deadline = deadline;
        Thread* next = pick(lock);
        if (!next) {
            self_->state = State::Running;
            self_->deadline.reset();
            throw std::runtime_error("deadlock: every guest thread waits with no deadline");
        }
        switch_to(lock, next);
        self_->deadline.reset();
    }
}

void Scheduler::notify() {
    std::unique_lock lock(mutex_);
    for (auto& [id, t] : threads_)
        if (t->state == State::Waiting)
            t->state = State::Ready;
}

void Scheduler::exit_current(std::uint32_t code) {
    throw ThreadExit{code};
}

Scheduler::Thread* Scheduler::lookup(std::uint32_t handle) const {
    if (handle == kCurrentThreadHandle)
        return self_;
    const auto it = handles_.find(handle);
    if (it == handles_.end())
        return nullptr;
    const auto t = threads_.find(it->second);
    return t == threads_.end() ? nullptr : t->second.get();
}

std::optional<std::uint32_t> Scheduler::suspend(std::uint32_t handle) {
    std::unique_lock lock(mutex_);
    Thread* t = lookup(handle);
    if (!t || t->state == State::Exited)
        return std::nullopt;
    const auto previous = t->suspended++;
    if (t == self_) {
        self_->state = State::Ready;
        Thread* next = pick(lock);
        if (!next)
            throw std::runtime_error("deadlock: the only runnable guest thread suspended itself");
        switch_to(lock, next);
    }
    return previous;
}
std::optional<std::uint32_t> Scheduler::resume(std::uint32_t handle) {
    std::unique_lock lock(mutex_);
    Thread* t = lookup(handle);
    if (!t)
        return std::nullopt;
    const auto previous = t->suspended;
    if (t->suspended)
        --t->suspended;
    return previous;
}
bool Scheduler::terminate(std::uint32_t handle, std::uint32_t code) {
    std::unique_lock lock(mutex_);
    Thread* t = lookup(handle);
    if (!t)
        return false;
    if (t == self_) {
        lock.unlock();
        exit_current(code);
    }
    if (t->state != State::Exited && !t->killed) {
        t->killed = true;
        t->exit_code = code;
        if (t->state == State::Waiting)
            t->state = State::Ready;  // it ends the next time it runs
    }
    return true;
}
std::optional<int> Scheduler::priority(std::uint32_t handle) const {
    std::unique_lock lock(mutex_);
    const Thread* t = lookup(handle);
    if (!t)
        return std::nullopt;
    return t->priority;
}
bool Scheduler::set_priority(std::uint32_t handle, int priority) {
    std::unique_lock lock(mutex_);
    Thread* t = lookup(handle);
    if (!t || priority < 0 || priority > 7)
        return false;
    t->priority = priority;
    return true;
}
std::optional<std::uint32_t> Scheduler::exit_code(std::uint32_t handle) const {
    std::unique_lock lock(mutex_);
    const Thread* t = lookup(handle);
    if (!t)
        return std::nullopt;
    return t->state == State::Exited || t->killed ? t->exit_code : kStillActive;
}
bool Scheduler::owns_handle(std::uint32_t handle) const {
    std::unique_lock lock(mutex_);
    return lookup(handle) != nullptr;
}
bool Scheduler::exited(std::uint32_t handle) const {
    std::unique_lock lock(mutex_);
    const Thread* t = lookup(handle);
    return t && (t->state == State::Exited || t->killed);
}
bool Scheduler::close_handle(std::uint32_t handle) {
    std::unique_lock lock(mutex_);
    return handles_.erase(handle) != 0;
}
bool Scheduler::has_waiters() const {
    std::unique_lock lock(mutex_);
    return std::any_of(threads_.begin(), threads_.end(),
                       [](const auto& p) { return p.second->state == State::Waiting; });
}
std::string Scheduler::describe() const {
    std::unique_lock lock(mutex_);
    static const char* names[] = {"ready", "running", "waiting", "exited"};
    std::string out =
        "baton: thread " + std::to_string(running_) + ", clock " + std::to_string(now_) + "\n";
    for (const auto& [id, t] : threads_) {
        char line[200];
        std::snprintf(line, sizeof line,
                      "  thread %u: %s, priority %d, suspended %u, deadline %s, pc 0x%08x pr "
                      "0x%08x cycles %llu%s\n",
                      id, names[static_cast<int>(t->state)], t->priority, t->suspended,
                      t->deadline ? std::to_string(*t->deadline).c_str() : "none", t->cpu->pc,
                      t->cpu->pr, static_cast<unsigned long long>(t->cpu->cycles),
                      t->killed ? ", killed" : "");
        out += line;
    }
    return out;
}

std::uint32_t Scheduler::current_id() const {
    return self_ ? self_->id : kMainThread;
}
sh4::Ctx& Scheduler::current_ctx() const {
    if (!self_)
        throw std::logic_error("no guest thread on this host thread");
    return *self_->cpu;
}
}  // namespace dream::wince
