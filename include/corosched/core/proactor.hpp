// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <concepts>
#include <coroutine>
#include <type_traits>

#include <corosched/core/critical.hpp>
#include <corosched/core/fatal.hpp>
#include <corosched/core/task_state.hpp>
#include <corosched/core/waker.hpp>

namespace corosched {
    /// Something a task awaits whose work an interrupt completes: the interrupt stores the value, or counts the period, and wakes the task,
    /// and @c co_await hands the task the result.
    ///
    /// - @c arm(waker) registers the task's waker, and wakes it at once if a value is already waiting.
    /// - @c disarm(waker) must make sure that, once it returns, the proactor does not call @c wake on that waker again.
    /// - @c ready() says whether a value is waiting.
    /// - @c produce() takes it, and the @c result_type it returns is what @c co_await gives back.
    ///
    /// @c proactor_base implements @c arm and @c disarm, and a @c next() to await it with.
    template<class Self>
    concept proactor = requires(Self &self, waker w) {
        { self.arm(w) } -> std::same_as<void>;
        { self.disarm(w) } -> std::same_as<void>;
        typename Self::result_type;
        { self.ready() } -> std::same_as<bool>;
        { self.produce() } -> std::same_as<typename Self::result_type>;
    };

    /// What @c next() returns for a proactor: @c co_await it from a task to get @c P::result_type. The proactor @c P must outlive it.
    template<proactor P>
    class proactor_awaiter {
        P *const proactor_;
        task_state *state_{};
        bool non_blocking_{false}; // await_ready found a value, so nothing was armed

    public:
        using result_type = P::result_type;

        explicit proactor_awaiter(P &p) noexcept : proactor_(&p) {}

        /// Called by @c co_await before anything else: stores the awaiting task's state.
        void bind(task_state &state) noexcept { state_ = &state; }

        /// Takes the value at once if the proactor already has one, without arming a waker.
        bool await_ready() noexcept { return non_blocking_ = proactor_->ready(); }

        /// Arms the proactor with the task's waker.
        void await_suspend(const std::coroutine_handle<>) noexcept { proactor_->arm(state_->waker()); }

        /// Disarms the proactor. Does nothing if the value was taken at once or the task's ready bit is already clear, so a repeat call is harmless.
        void await_disarm() noexcept {
            if (!non_blocking_ && state_->ready()) {
                proactor_->disarm(state_->waker());
            }
        }

        /// Disarms, clears the task's ready bit if the task suspended, and takes the value with @c produce().
        result_type await_resume() noexcept {
            await_disarm();
            if (!non_blocking_) state_->clear();
            return proactor_->produce();
        }
    };

    /// The waker handling of a proactor that one task awaits at a time, as the built-in ones are.
    /// A class deriving from it adds @c ready(), @c produce() and a @c result_type, and calls @c wake() when its event fires:
    /// @code
    /// class my_proactor : public corosched::proactor_base {
    /// public:
    ///     using result_type = int;
    ///     [[nodiscard]] bool ready() const noexcept;
    ///     result_type produce() noexcept;
    /// };
    /// @endcode
    class proactor_base {
        waker armed_{};

    protected:
        /// Wake the armed task, if any. Safe from an interrupt.
        void wake() const noexcept { armed_.wake(); }

    public:
        /// Called by the awaiter. Stores @c w, then wakes it at once if a value is already waiting.
        /// Traps with @c fault::second_consumer if another task's waker is armed.
        void arm(this auto &self, const waker w) noexcept {
            critical_section _;
            auto &armed = static_cast<proactor_base &>(self).armed_;
            if (armed.word != nullptr && armed != w) {
                fatal(fault::second_consumer);
            }
            armed = w;
            // The event may have fired after await_ready, while no waker was armed.
            if (self.ready()) w.wake();
        }

        /// Called by the awaiter. After this, nothing wakes @c w until it is armed again.
        void disarm(const waker w) noexcept {
            critical_section _;
            if (armed_ == w) armed_ = waker{};
        }

        /// Awaiter for the next value, which @c co_await returns as the proactor's @c result_type.
        auto next(this auto &self) noexcept { return proactor_awaiter<std::remove_cvref_t<decltype(self)>>{self}; }
    };
}
