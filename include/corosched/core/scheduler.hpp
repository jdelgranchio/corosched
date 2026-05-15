// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include <stataider/stataider.hpp>

#include <corosched/core/critical.hpp>
#include <corosched/core/fatal.hpp>
#include <corosched/core/runqueue.hpp>
#include <corosched/core/task.hpp>
#include <corosched/core/task_state.hpp>

namespace corosched {
    /// The default idle: sleep until an interrupt on a Cortex-M, do nothing on a host.
    struct sleep_until_interrupt {
        void operator()() const noexcept {
#ifdef COROSCHED_CORTEX_M
            __asm volatile("wfi" ::: "memory");
#endif
        }
    };

    /// Runs a fixed set of tasks one at a time, highest priority first.
    /// Pass the @c task values to the constructor in priority order, the first being the highest, at most 32 of them.
    /// A task runs until its next suspension and is never preempted by another task, though interrupts still run.
    /// Call @c run() to start.
    template<class... Tasks>
    class scheduler {
        static_assert((std::is_same_v<Tasks, task> && ...), "scheduler arguments must be corosched::task values");
        static_assert(sizeof...(Tasks) <= 32, "too many tasks for the runqueue word");
        static constexpr auto TASKS = static_cast<std::uint8_t>(sizeof...(Tasks));

        // Each task's waker and the handle to resume. The handle starts as the root coroutine and every co_await replaces it, so resume
        // targets the innermost suspended frame however deep the co<T> chain is.
        std::array<task_state, TASKS> states_{};
        runqueue_word runqueue_{};

    public:
        /// Debugging and performance counters on the scheduler.
        struct stats_type {
            /// Times @c step() found every task blocked
            std::uint_fast32_t idle{};

            /// Per task, in constructor order: ticks spent in its segments, the longest segment, and how many ran.
            /// A segment is the time from a resume to the task's next suspension.
            /// Ticks come from the @c stataider_ticks hook, and read 0 when its source has a width of 0.
            std::array<stataider::counter_ticks_max, TASKS> tasks;
        };

    private:
        stats_type stats_{};

        /// Resume task bit and time the segment.
        /// A finished task whose bit is still set has the bit cleared instead.
        void resume(const std::uint8_t bit) noexcept {
            auto &state = states_[bit];
            const auto handle = state.handle_;
            if (!handle || handle.done()) {
                state.clear();
                return;
            }
            const auto sample = stataider::record(stats_.tasks[bit]);
            handle.resume();
        }

    public:
        explicit scheduler(Tasks... tasks) noexcept {
            std::uint8_t bit = 0;
            (
                    [&] {
                        auto &state = states_[bit];
                        state.waker_ = waker{.word = &runqueue_, .mask = std::uint32_t{1} << bit};
                        tasks.bind(state);
                        ++bit;
                    }(),
                    ...);
        }

        /// Neither copyable nor movable, as the tasks point into the scheduler.
        /// Should be declared static.
        scheduler(const scheduler &) = delete;
        scheduler &operator=(const scheduler &) = delete;

        /// Debugging and performance counters.
        /// A task's own entry does not yet include the segment that is reading it.
        [[nodiscard]] const stats_type &stats() const noexcept { return stats_; }

        /// Start the tick source, then run every task once, from its start to its first suspension.
        /// Traps with @c fault::tick_source_mismatch if only one of the two tick hooks was replaced.
        void kick() noexcept {
            if (!stataider::start_source()) {
                fatal(fault::tick_source_mismatch);
            }
            for (std::uint8_t bit = 0; bit < TASKS; ++bit) {
                resume(bit);
            }
        }

        /// One scheduler iteration: resume the highest-priority ready task until it suspends, or call @c idle if none is ready.
        ///
        /// @c idle runs inside the critical section, so that a wake cannot slip in just before the sleep.
        /// Defaults to @c WFI on Cortex-M cores.
        template<std::invocable Idle = sleep_until_interrupt>
        void step(Idle &&idle = Idle{}) noexcept {
            std::uint32_t ready;
            {
                critical_section _;
                ready = runqueue_;
                if (ready == 0) {
                    stataider::inc(stats_.idle);
                    idle();
                    return;
                }
            }

            const auto bit = static_cast<std::uint8_t>(std::countr_zero(ready));
            resume(bit);
        }

        /// Call @c kick() and then @c step() forever. Never returns.
        /// See @c step() for @c idle.
        template<std::invocable Idle = sleep_until_interrupt>
        [[noreturn]] void run(Idle &&idle = Idle{}) noexcept {
            kick();
            while (true) {
                step(idle);
            }
        }
    };

    template<class... Tasks>
    scheduler(Tasks...) -> scheduler<Tasks...>;
}
