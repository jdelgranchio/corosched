// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/awaitable.hpp>
#include <corosched/core/fatal.hpp>
#include <corosched/core/scheduler.hpp>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace corosched {
    /// @c T, or @c std::monostate in place of @c void.
    template<class T>
    using regular_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    /// Calls @c f and gives back its result, or @c std::monostate if it returns @c void.
    template<std::invocable F>
    regular_t<std::invoke_result_t<F>> invoke_regular(F &&f) {
        if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
            std::invoke(std::forward<F>(f));
            return {};
        } else {
            return std::invoke(std::forward<F>(f));
        }
    }

    /// Waits on several awaitables at once and resumes on the first one that fires.
    /// Returns a @c std::variant for each awaitable, with @c std::monostate for @c void awaitables.
    ///
    /// If several are ready when the task runs, the earliest argument wins and the others gets disarmed, behaviour depends on the awaitable.
    /// If one is ready at once, none is armed.
    template<awaitable... Awaitables>
    auto select(Awaitables &...awaitables) {
        class select_awaiter {
            std::tuple<Awaitables &...> awaitables_;
            task_state *state_{};

        public:
            using result_type = std::variant<regular_t<decltype(std::declval<Awaitables &>().await_resume())>...>;

            explicit select_awaiter(Awaitables &...awaitables) : awaitables_{awaitables...} {}

            void bind(task_state &state) noexcept {
                state_ = &state;
                std::apply([&state](auto &...awaitable) { (awaitable.bind(state), ...); }, awaitables_);
            }

            bool await_ready() noexcept {
                return std::apply([](auto &...awaitable) { return (awaitable.await_ready() || ...); }, awaitables_);
            }

            void await_suspend(const std::coroutine_handle<> handle) noexcept {
                std::apply([handle](auto &...awaitable) { (awaitable.await_suspend(handle), ...); }, awaitables_);
            }

            void await_disarm() noexcept {
                // Once the ready bit is clear no waker can fire, so a repeat call does nothing.
                if (state_->ready()) {
                    std::apply([](auto &...awaitable) { (awaitable.await_disarm(), ...); }, awaitables_);
                }
            }

            result_type await_resume() noexcept {
                await_disarm();
                state_->clear();

                // Take the first branch that is ready: the task only runs because one fired.
                std::optional<result_type> result;
                const auto take = [&result]<std::size_t Index>(std::integral_constant<std::size_t, Index>, auto &awaitable) {
                    if (!result && awaitable.await_ready()) {
                        result.emplace(std::in_place_index<Index>, invoke_regular([&awaitable] { return awaitable.await_resume(); }));
                    }
                };
                [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                    (take(std::integral_constant<std::size_t, Index>{}, std::get<Index>(awaitables_)), ...);
                }(std::index_sequence_for<Awaitables...>{});

                // A proactor that wakes with nothing to yield leaves no result; trap rather than hand back a garbage value.
                if (!result) {
                    fatal(fault::select_no_branch); // woken with no branch ready
                }
                return std::move(*result);
            }
        };

        static_assert(awaitable<select_awaiter>, "select_awaiter does not implement awaitable");
        return select_awaiter(awaitables...);
    }
}
