// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <atomic>
#include <cstdint>

#include <corosched/core/runqueue.hpp>

namespace corosched {
    /// The ready bits of one or more tasks in a scheduler's runqueue word.
    /// Proactors hold a copy and call @c wake() when their event fires.
    /// Copying is cheap.
    struct waker {
        runqueue_word *word{}; ///< the scheduler's runqueue word, null in a default waker
        std::uint32_t mask{}; ///< the tasks' bits in the word

        /// Atomically mark the tasks ready.
        void wake() const noexcept {
            if (word != nullptr) {
                word->fetch_or(mask, std::memory_order_release);
            }
        }

        /// Two wakers are equal if they have the same word and mask.
        bool operator==(const waker &) const noexcept = default;
    };
}
