// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <array>
#include <coroutine>
#include <cstddef>
#include <utility>

#include <corosched/core/fatal.hpp>
#include <corosched/core/task_state.hpp>

namespace corosched {
    /// Single-use storage for one coroutine's frame, passed as the coroutine's first argument:
    /// @c static for a root @c task, a local for a nested @c co<T>.
    ///
    /// A frame too small for the coroutine traps with @c fault::frame_too_small, and using one twice traps with @c fault::frame_reused.
    template<const std::size_t N>
    struct frame {
        alignas(std::max_align_t) std::array<std::byte, N> bytes{};
        std::size_t consumed = 0;
    };

    template<std::size_t N>
    using task_frame = frame<N>;

    template<std::size_t N>
    using co_frame = frame<N>;

    // What both kinds of coroutine share: the frame goes into a frame<N>, and an exception traps.
    struct promise_base {
        template<std::size_t N, class... Rest>
        void *operator new(const std::size_t n, frame<N> &buf, Rest &&...) noexcept {
            if (n > N) fatal(fault::frame_too_small);
            if (buf.consumed > 0) fatal(fault::frame_reused);
            buf.consumed = n;
            return buf.bytes.data();
        }

        constexpr void operator delete(void *) noexcept {}

        static void unhandled_exception() noexcept { fatal(fault::unhandled_exception); }

        task_state *state_{};

        // Every co_await passes through here, so the coroutine making it is recorded as the one to resume and no awaiter has to do it.
        // Self is the real promise type, which from_promise needs.
        template<class Self, class A>
        A &&await_transform(this Self &self, A &&a) noexcept {
            static_assert(
                    requires { a.bind(*self.state_); }, "only corosched awaitables can be co_awaited in a task: "
                                                        "this one has no bind(task_state&)");
            self.state_->resume_with(std::coroutine_handle<Self>::from_promise(self));
            a.bind(*self.state_);
            return std::forward<A>(a);
        }
    };

    /// Return type of a root coroutine, which a @c scheduler runs.
    /// Its first parameter is the @c task_frame that holds it.
    struct [[nodiscard]] task {
        struct promise_type : promise_base {
            constexpr task get_return_object() noexcept { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
            static task get_return_object_on_allocation_failure() noexcept { fatal(fault::allocation_failure); }
            static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
            static constexpr std::suspend_always final_suspend() noexcept { return {}; }
            static constexpr void return_void() noexcept {}
        };

        /// Called by the @c scheduler: the task runs under @c state, starting from its root coroutine.
        void bind(task_state &state) noexcept {
            state.resume_with(handle);
            handle.promise().state_ = &state;
        }

        std::coroutine_handle<promise_type> handle{};
    };

    template<class T>
    class co;

    // A promise has either return_value or return_void, never both, so only this part depends on T being void.
    template<class T>
    struct co_result {
        T value{};

        void return_value(T v) noexcept { value = std::move(v); }

        T take() noexcept { return std::move(value); }
    };

    template<>
    struct co_result<void> {
        static void return_void() noexcept {}

        static void take() noexcept {}
    };

    template<class T>
    struct co_promise : promise_base, co_result<T> {
        std::coroutine_handle<> continuation{};

        struct final_awaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }

            std::coroutine_handle<> await_suspend(const std::coroutine_handle<co_promise> h) noexcept {
                if (const auto c = h.promise().continuation) {
                    // This frame is finishing, so point the scheduler at the coroutine taking over.
                    h.promise().state_->resume_with(c);
                    return c;
                }
                return std::noop_coroutine();
            }

            void await_resume() noexcept {}
        };

        std::suspend_always initial_suspend() noexcept { return {}; }
        final_awaiter final_suspend() noexcept { return {}; }

        co<T> get_return_object() noexcept;

        // Never called, as operator new returns a frame or traps. GCC warns at every co<T> without it, because operator new is noexcept.
        // Defined below, where co<T> is complete.
        static co<T> get_return_object_on_allocation_failure() noexcept;
    };

    /// Return type of a nested coroutine, which a task or another @c co<T> awaits and which gives back a @c T.
    /// Its first parameter is the @c co_frame that holds it, and each call needs a fresh frame:
    /// @code
    /// corosched::co<int> compute(corosched::co_frame<64> &) { co_return 42; }
    ///
    /// // Inside a task or another co<T>:
    /// corosched::co_frame<64> f;
    /// int x = co_await compute(f);
    /// @endcode
    template<class T = void>
    class [[nodiscard]] co {
    public:
        using promise_type = co_promise<T>;

        explicit co(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

        co(co &&o) noexcept : h_(std::exchange(o.h_, {})) {}

        ~co() {
            if (h_) h_.destroy();
        }

        void bind(task_state &state) noexcept { h_.promise().state_ = &state; }

        bool await_ready() const noexcept { return !h_ || h_.done(); }

        // Symmetric transfer, so a long chain of co<T> does not grow the stack.
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> cont) noexcept {
            h_.promise().continuation = cont;
            return h_;
        }

        T await_resume() noexcept { return h_.promise().take(); }

    private:
        std::coroutine_handle<promise_type> h_{};
    };

    template<class T>
    co<T> co_promise<T>::get_return_object() noexcept {
        return co<T>{std::coroutine_handle<co_promise>::from_promise(*this)};
    }

    template<class T>
    co<T> co_promise<T>::get_return_object_on_allocation_failure() noexcept {
        fatal(fault::allocation_failure);
    }
}
