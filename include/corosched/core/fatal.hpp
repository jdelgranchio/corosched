// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>

namespace corosched {

    /// Every contract violation the library detects. They are programming errors, so the library stops instead of carrying on.
    enum class fault : std::uint8_t {
        none = 0, ///< nothing has gone wrong
        frame_too_small, ///< a @c frame<N> smaller than the coroutine frame
        frame_reused, ///< a @c frame<N> handed to a second coroutine call
        allocation_failure, ///< never raised: the placement new cannot fail
        unhandled_exception, ///< never raised when built with -fno-exceptions
        second_consumer, ///< two tasks awaiting one proactor, which holds a single waker
        spent_delay, ///< a @c delay awaited again after its fire was taken
        select_no_branch, ///< a @c select woken with no branch ready
        hook_attached_twice, ///< a @c tick_hook attached while it is already on a list, which would cut that list short
        hook_not_on_list, ///< a @c tick_hook detached from a list it is not on, so it would stay on the other one
        hook_changed_during_tick, ///< a tick callback attaching or detaching a hook on the list that is calling it
        tick_source_mismatch, ///< only one of @c stataider_ticks and @c stataider_start_ticks replaced, so the width belongs to another source
    };

    /// What @c fatal records about the violation.
    struct fault_record {
        fault code;
        void *pc; ///< return address into the code that called @c fatal
    };

    /// Panics, and assembles a @c fault_record, then calls the @c corosched_fatal handler.
    [[noreturn, gnu::cold]] void fatal(fault f) noexcept;
}

/// Defined weak, unless overridden, saves the fault_record to a static location for inspection.
[[noreturn]] void corosched_fatal(corosched::fault_record record) noexcept;
