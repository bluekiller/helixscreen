// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <thread>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/mman.h>
#define HELIX_THREAD_ALTSTACK 1
#endif

// Header-only so the dlopen()ed Bluetooth plugin, which links none of the
// app's objects, starts its threads the same way.

namespace helix {

/// Signal stack size for every thread. The crash handler formats the whole
/// record on it, stack dump and memory map included.
inline constexpr std::size_t kAltStackSize = 64 * 1024;

/// Give the calling thread its own signal stack.
///
/// sigaltstack is per thread, and crash_handler::install() covers only the
/// thread that calls it. Without one, a stack overflow on any other thread
/// leaves the handler no stack to run on and the process dies with no crash
/// file. Allocated once per thread and released at thread exit; repeat calls,
/// and calls on a thread that already has a signal stack, do nothing.
inline void install_thread_altstack() noexcept {
#ifdef HELIX_THREAD_ALTSTACK
    struct ThreadAltStack {
        void* mem = nullptr;
        ThreadAltStack() {
            stack_t current{};
            if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) {
                return;
            }
            // mmap rather than the heap: pages stay unbacked until a crash
            // touches them, so an idle thread costs no RSS.
            void* p = mmap(nullptr, kAltStackSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                return;
            }
            stack_t ss{};
            ss.ss_sp = p;
            ss.ss_size = kAltStackSize;
            if (sigaltstack(&ss, nullptr) != 0) {
                munmap(p, kAltStackSize);
                return;
            }
            mem = p;
        }
        ~ThreadAltStack() {
            if (mem == nullptr) {
                return;
            }
            // Detach before unmapping, so a signal in a later thread-exit
            // destructor cannot land on freed memory.
            stack_t ss{};
            ss.ss_flags = SS_DISABLE;
            sigaltstack(&ss, nullptr);
            munmap(mem, kAltStackSize);
        }
    };
    thread_local ThreadAltStack s_thread_alt_stack;
    (void)s_thread_alt_stack;
#endif
}

/// std::thread(f, args...) for a thread that leaves a crash file if it
/// overflows its stack: the thread installs its own signal stack before
/// running f. Every app-owned thread starts through here (lint-gated).
template <typename F, typename... Args> std::thread make_thread(F&& f, Args&&... args) {
    return std::thread(
        [](auto&& fn, auto&&... a) {
            install_thread_altstack();
            std::invoke(std::move(fn), std::move(a)...);
        },
        std::forward<F>(f), std::forward<Args>(args)...);
}

} // namespace helix
