// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <concepts>
#include <coroutine>

#include <corosched/core/task_state.hpp>

namespace corosched {

    /// Something a task can @c co_await: the C++ required awaiter functions plus @c bind and @c await_disarm.
    ///
    /// - @c bind(task_state&) is called first, on every @c co_await, and hands over the awaiting task's @c task_state.
    /// - @c await_ready may be called more than once, and should return true iff the awaiter has already fired.
    /// - @c await_suspend arms the proactor.
    /// - @c await_disarm stops the proactor waking the task and must be safe to call again, for which it can check @c task_state::ready.
    /// - @c await_resume returns the result and clears the task's ready bit once no more wakers are registered.
    ///
    /// @c corosched::proactor_awaiter implements this for the common case, with a simpler API.
    template<class Self>
    concept awaitable = requires(Self &self, std::coroutine_handle<> handle, task_state &state) {
        { self.bind(state) };
        { self.await_ready() } -> std::same_as<bool>;
        { self.await_suspend(handle) } -> std::same_as<void>;
        { self.await_disarm() };
        { self.await_resume() };
    };

}
