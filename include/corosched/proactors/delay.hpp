// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/critical.hpp>
#include <corosched/core/fatal.hpp>
#include <corosched/core/proactor.hpp>
#include <corosched/core/waker.hpp>

#include <corosched/proactors/time.hpp>

namespace corosched {
    /// Fires once, @c Duration after construction, for a single @c co_await of @c next(). @c Duration must be a positive exact multiple of the
    /// clock's tick, checked at compile time like the @c periodic period.
    ///
    /// The clock's first tick lands somewhere within one tick of construction, so @c delay counts one extra tick to never wake early: the wait is
    /// more than @c Duration and at most @c Duration plus one tick. Once it fires it leaves the clock. Awaiting it again once its fire has been
    /// taken traps with @c fault::spent_delay, because no wake could ever come.
    ///
    /// It is attached to the clock from construction until it fires, or until destruction, and cannot be copied or moved.
    /// One interrupt context ticks the clock and one task awaits; a second awaiting task traps with @c fault::second_consumer.
    template<periodic_clock Clock, nanos Duration>
    class delay : public proactor_base {
        static constexpr tick_t duration_ticks = to_ticks<typename Clock::time_base, Duration>();

    public:
        struct result_type {};

        explicit delay(Clock &clock) noexcept : hook_(clock, this, member_tick<&delay::on_due>, duration_ticks + 1) {}

        /// True if the fire has happened and not been taken yet.
        [[nodiscard]] bool ready() const noexcept {
            critical_section cs;
            return pending_;
        }

        /// Called by the awaiter: @c proactor_base::arm, but traps with @c fault::spent_delay if the fire was already taken.
        void arm(const waker w) noexcept {
            critical_section cs;
            if (!hook_.linked() && !pending_) {
                fatal(fault::spent_delay); // it has left the clock, so no wake would ever come
            }
            proactor_base::arm(w);
        }

        /// Take the fire.
        result_type produce() noexcept {
            critical_section cs;
            pending_ = false;
            return {};
        }

    private:
        // Called by the clock from interrupt context, once, when the duration is up. Returning 0 takes it off the clock.
        tick_t on_due() noexcept {
            pending_ = true;
            wake();
            return 0;
        }

        bool pending_{false};
        clock_hook<Clock> hook_; // last: see clock_hook
    };
}
