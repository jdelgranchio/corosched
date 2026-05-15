// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <corosched/core/fatal.hpp>
#include <stataider/visible.hpp>

namespace corosched {
    // Not inlined, so that the return address really is the call site.
    [[gnu::noinline]] void fatal(const fault f) noexcept { corosched_fatal({.code = f, .pc = __builtin_return_address(0)}); }
}

/// Default panic handler, saves the @c fault_record to a static variable for inspection.
[[gnu::weak]] void corosched_fatal(const corosched::fault_record record) noexcept {
    static corosched::fault_record last_fault{.code = corosched::fault::none, .pc = nullptr};
    last_fault = record;
    stataider::keep_visible(last_fault);
    __builtin_trap();
}
