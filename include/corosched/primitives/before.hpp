// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <concepts>
#include <coroutine>
#include <expected>
#include <functional>
#include <type_traits>
#include <utility>

#include <corosched/core/awaitable.hpp>
#include <corosched/core/proactor.hpp>

namespace corosched {

    /// Runs an invocable @c body whose value is consistent only iff @c source does not fire while it evaluates.
    /// It is expected that @c body will compute before @c proactor fires, should it not @c std::expected carries the value @c source produced.
    ///
    /// Can be used to guard against DMA shared corruption.
    template<proactor P, std::invocable F>
    auto before(P &source, F &&body) noexcept {

        class before_awaiter {
            using body_result_type = std::invoke_result_t<F>;

            P &proactor_;
            F &&body_;
            task_state *state_{};

        public:
            before_awaiter(P &source, F &&body) noexcept : proactor_(source), body_(std::forward<F>(body)) {}

            void bind(task_state &state) noexcept { state_ = &state; }

            static bool await_ready() noexcept { return true; }

            static void await_suspend(std::coroutine_handle<>) noexcept {}

            static void await_disarm() noexcept {}

            auto await_resume() noexcept -> std::expected<body_result_type, typename P::result_type> {
                const auto w = state_->waker();
                proactor_.arm(w);

                // The body cannot suspend, so the only thing that can set the task's ready bit meanwhile is the proactor.
                // transform on an empty expected<void> calls the body and gives expected<void> back when the body returns void.
                auto result = std::expected<void, typename P::result_type>{}.transform(std::forward<F>(body_));
                proactor_.disarm(w);
                if (state_->ready()) {
                    state_->clear();
                    return std::unexpected{proactor_.produce()};
                }
                return result;
            }
        };

        static_assert(awaitable<before_awaiter>, "before_awaiter does not implement awaitable");
        return before_awaiter{source, std::forward<F>(body)};
    }
}
