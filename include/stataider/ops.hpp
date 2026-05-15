// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <concepts>
#include <limits>

#include <stataider/visible.hpp>

namespace stataider {
    /// An unsigned integer type other than @c bool.
    template<typename T>
    concept unsigned_word = std::unsigned_integral<T> && !std::same_as<T, bool>;

    /// Add one to @c counter, stopping at the type's maximum.
    /// Not atomic, so update a counter from one context only.
    /// A debugger keeps seeing it even if nothing in the program reads it.
    template<unsigned_word T>
    void inc(T &counter) noexcept {
        if (counter != std::numeric_limits<T>::max()) {
            ++counter;
        }
        keep_visible(counter);
    }

    /// Raise @c previous to @c value if @c value is larger.
    /// Not atomic, so update from one context only.
    /// A debugger keeps seeing it even if nothing in the program reads it.
    template<unsigned_word T>
    void max(T &previous, const T value) noexcept {
        if (value > previous) {
            previous = value;
        }
        keep_visible(previous);
    }
}
