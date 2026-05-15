// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>

#include <stataider/monotonic.hpp>
#include <stataider/ops.hpp>

namespace stataider {
    /// Total ticks spent across every timed scope. Like the aggregates below it takes one timed sample at a time through @c absorb, and is meant to
    /// be a @c static object that a debugger reads. Pick the smallest aggregate that answers the question, as each field costs RAM and cycles.
    struct ticks {
        tick_t total = 0;

        void absorb(const tick_t elapsed) noexcept {
            total += elapsed;
            keep_visible(total);
        }
    };

    /// Total ticks, plus the longest single scope.
    struct ticks_max {
        tick_t total = 0;
        tick_t worst = 0;

        void absorb(const tick_t elapsed) noexcept {
            total += elapsed;
            keep_visible(total);
            max(worst, elapsed);
        }
    };

    /// Total ticks, and how many times a scope was timed. The count saturates.
    struct counter_ticks {
        tick_t total = 0;
        std::uint32_t count = 0;

        void absorb(const tick_t elapsed) noexcept {
            total += elapsed;
            keep_visible(total);
            inc(count);
        }
    };

    /// Total ticks, the longest single scope, and how many times a scope was timed. The count saturates.
    struct counter_ticks_max {
        tick_t total = 0;
        tick_t worst = 0;
        std::uint32_t count = 0;

        void absorb(const tick_t elapsed) noexcept {
            total += elapsed;
            keep_visible(total);
            max(worst, elapsed);
            inc(count);
        }
    };

    /// Anything a @c scoped_sample can hand elapsed ticks to: it has an @c absorb taking a @c tick_t.
    template<typename T>
    concept tick_sink = requires(T sink, tick_t elapsed) { sink.absorb(elapsed); };

    /// Times its own scope: reads the monotonic source when constructed and again when destroyed, then hands the elapsed ticks to @c sink.
    /// Make one with @c record.
    template<tick_sink Sink>
    class scoped_sample {
        Sink &sink;
        tick_t start;

    public:
        explicit scoped_sample(Sink &s) noexcept : sink(s), start(monotonic_ticks()) {}

        ~scoped_sample() { sink.absorb(elapsed(start, monotonic_ticks())); }

        scoped_sample(const scoped_sample &) = delete;

        scoped_sample &operator=(const scoped_sample &) = delete;
    };

    /// Time the calling scope into @c sink. Bind the result to a variable that lives as long as the scope to measure:
    /// @code
    /// static stataider::counter_ticks_max adc_irq;
    ///
    /// extern "C" void ADC_IRQHandler() {
    ///     auto sample = stataider::record(adc_irq);
    ///     // ...
    /// }
    /// @endcode
    template<tick_sink Sink>
    [[nodiscard]] scoped_sample<Sink> record(Sink &sink) noexcept {
        return scoped_sample<Sink>(sink);
    }
}
