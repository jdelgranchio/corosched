// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <atomic>
#include <coroutine>

#include <corosched/core/waker.hpp>

/**
 * @file corosched/core/task_state.hpp
 * This header is apart from @file scheduler.hpp so that @file task.hpp can use it without a circular include.
 */

namespace corosched {
    // For the friend declaration below.
    template<class... Tasks>
    class scheduler;

    /// What a task and its awaiters share with the scheduler. There is one per task, owned by the scheduler.
    ///
    /// Every @c co_await in a task or @c co<T> hands it to the awaiter through @c bind(),
    /// before anything else happens through the promise's @c await_transform.
    class task_state {
        corosched::waker waker_{};
        std::coroutine_handle<> handle_{};

        template<class... Tasks>
        friend class scheduler;

    public:
        /// The waker for this task.
        [[nodiscard]] corosched::waker waker() const noexcept { return waker_; }

        /// Record the coroutine handle to be used to resume.
        void resume_with(const std::coroutine_handle<> handle) noexcept { handle_ = handle; }

        /// True if the task's bit in the runqueue is set.
        [[nodiscard]] bool ready() const noexcept {
            const auto [runqueue, mask] = waker_;
            return runqueue->load(std::memory_order_acquire) & mask;
        }

        /// Clear the task's bit.
        void clear() const noexcept {
            const auto [runqueue, mask] = waker_;
            runqueue->fetch_and(~mask, std::memory_order_acq_rel);
        }
    };
}
