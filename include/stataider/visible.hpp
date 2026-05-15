// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

namespace stataider {
    /// Tells the compiler that @c value is observed from outside the program, as a debugger does. A stat nothing reads is otherwise dead, and LTO
    /// removes the stores and the variable with them. Emits no instructions, and unlike @c volatile adds no loads.
    ///
    /// Call it after every store to a variable kept for a debugger, as the counters and aggregates here do; a custom @c tick_sink needs it too.
    template<typename T>
    void keep_visible(const T &value) noexcept {
        __asm volatile("" : : "m"(value));
    }
}
