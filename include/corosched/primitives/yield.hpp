// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <coroutine>

#include <corosched/core/awaitable.hpp>
#include <corosched/core/scheduler.hpp>

namespace corosched {
    /// Yields execution to higher priority tasks.
    ///
    /// Can be used to split blocking computations into smaller pieces, improving latency on the higher priority tasks.
    inline auto yield() noexcept {
        class yield_awaiter {
            task_state *state_{};
            bool suspended = false;

        public:
            void bind(task_state &state) noexcept { state_ = &state; }

            [[nodiscard]] bool await_ready() const noexcept { return suspended; }

            void await_suspend(const std::coroutine_handle<>) noexcept {
                state_->waker().wake();
                suspended = true;
            }

            static void await_disarm() noexcept {}

            void await_resume() const noexcept { state_->clear(); }
        };

        static_assert(awaitable<yield_awaiter>, "yield_awaiter does not implement awaitable");
        return yield_awaiter{};
    }
}
