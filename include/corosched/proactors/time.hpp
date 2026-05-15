// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/critical.hpp>
#include <corosched/core/fatal.hpp>

#include <concepts>
#include <cstdint>
#include <limits>
#include <ratio>

namespace corosched {
    /// A @c std::ratio, in any spelling: @c std::ratio<2,2000> is accepted as well as @c std::milli.
    template<class P>
    concept any_ratio = requires { typename P::type; } && std::same_as<typename P::type, std::ratio<P::num, P::den>>;

    /// The length of a clock tick.
    /// @c Period is a @c std::ratio of seconds per tick, such as @c std::milli or @c std::ratio<1,10'000> for 100 microseconds.
    ///
    /// The concept only checks that @c Period is a ratio. Its value is checked by the @c static_asserts, which say what is wrong with it.
    template<any_ratio Period>
    class time_base {
        static constexpr std::intmax_t max_num = std::numeric_limits<std::int64_t>::max() / 1'000'000'000;

        static_assert(Period::num > 0, "tick period must be positive");
        static_assert(Period::num <= max_num, "tick period is too long to count in nanoseconds");
        // tick_ns is rounded down otherwise, and every duration converted with it would be off. The first operand keeps a period that already
        // failed the assert above from overflowing here.
        static_assert(Period::num > max_num || std::int64_t{1'000'000'000} * Period::num % Period::den == 0,
                      "tick period must be a whole number of nanoseconds");

    public:
        using period = Period;

        /// Nanoseconds per tick, exact and at least 1.
        static constexpr std::int64_t tick_ns = std::int64_t{1'000'000'000} * Period::num / Period::den;
    };

    /// A specialization of @c time_base. A look-alike struct is not accepted, so the checks on the tick period always run.
    template<class T>
    concept any_time_base = any_ratio<typename T::period> && std::same_as<T, time_base<typename T::period>>;

    /// A duration in nanoseconds that can be a template parameter, as @c std::chrono::duration cannot.
    struct nanos {
        std::int64_t value{};

        constexpr bool operator==(const nanos &) const noexcept = default;
    };

    /// Literals for @c nanos, usable as template arguments: @c 5_ms is five milliseconds.
    /// A multiplication in @c _us, @c _ms or @c _s that overflows 64 bits is a compile error.
    namespace literals {
        consteval nanos operator""_ns(const unsigned long long n) noexcept { return nanos{static_cast<std::int64_t>(n)}; }

        consteval nanos operator""_us(const unsigned long long n) noexcept { return nanos{static_cast<std::int64_t>(n) * 1'000}; }

        consteval nanos operator""_ms(const unsigned long long n) noexcept { return nanos{static_cast<std::int64_t>(n) * 1'000'000}; }

        consteval nanos operator""_s(const unsigned long long n) noexcept { return nanos{static_cast<std::int64_t>(n) * 1'000'000'000}; }
    }

    /// Clock ticks: a tick number such as @c now(), or a number of ticks such as a countdown. Both wrap at 2^32.
    /// Not @c stataider::tick_t, which counts the 64-bit stats source.
    using tick_t = std::uint32_t;

    /// The number of ticks in @c D, or a compile error unless @c D is a positive exact multiple of the tick that fits in a @c tick_list countdown.
    /// One tick is kept spare, for the one @c delay adds.
    template<any_time_base TB, nanos D>
    consteval tick_t to_ticks() {
        static_assert(D.value > 0, "duration must be positive");
        static_assert(D.value % TB::tick_ns == 0, "duration must be an exact multiple of the clock tick");
        static_assert(D.value / TB::tick_ns < std::numeric_limits<tick_t>::max(), "duration is too many ticks to count in 32 bits");
        return static_cast<tick_t>(D.value / TB::tick_ns);
    }

    /// What a clock calls, from interrupt context, when a @c tick_hook comes due, with the @c ctx the hook was given.
    /// It returns the number of ticks until it is due again, or 0 to leave the list.
    using tick_callback = tick_t (*)(void *ctx) noexcept;

    /// A @c tick_callback that calls the member function @c Fn on @c ctx, which must point to an object of @c Fn's class.
    /// @c Fn may be private when it is named from inside its class:
    /// @code
    /// clock_hook<Clock> hook_{clock, this, member_tick<&my_proactor::on_due>, first_ticks};
    /// @endcode
    template<auto Fn>
    tick_t member_tick(void *const ctx) noexcept {
        // The lambda's parameter only deduces C, the class Fn belongs to. The call goes through Fn itself, a constant, so it is a direct call.
        return [ctx]<class C>(tick_t (C::*)() noexcept) noexcept { return (static_cast<C *>(ctx)->*Fn)(); }(Fn);
    }

    /// One callback on a clock's @c tick_list, with its countdown to when it is next due.
    /// The list links the hook by its address, so it cannot be copied or moved, and an object holding one is pinned too.
    /// A hook is on at most one list at a time.
    class tick_hook {
        friend class tick_list;

        tick_callback fn_;
        void *ctx_;
        tick_hook *next_{nullptr};
        tick_t remaining_{0}; // ticks until due: at least 1 while on a list, and 0 exactly when on none

    public:
        constexpr tick_hook(void *const ctx, const tick_callback fn) noexcept : fn_(fn), ctx_(ctx) {}

        tick_hook(const tick_hook &) = delete;
        tick_hook &operator=(const tick_hook &) = delete;

        /// True while the hook is on a list. The clock's interrupt can take it off, so read it inside a @c critical_section.
        [[nodiscard]] bool linked() const noexcept { return remaining_ != 0; }
    };

    /// An intrusive list of @c tick_hook, which a clock can derive from to get @c attach, @c detach and @c now.
    /// The clock's timer interrupt calls @c tick(). Nothing is allocated: each hook lives in the proactor that registered it.
    ///
    /// Each hook counts down to when it is due, and @c tick() decrements every count, so a tick costs O(N) and an attach O(1).
    /// @c attach and @c detach run in a @c critical_section, so the interrupt never sees the list half updated.
    /// @c tick() walks the list without masking anything, as the callbacks expect, and only one interrupt context may call it.
    class tick_list {
        tick_hook *head_{nullptr};
        tick_t now_{0};
        bool ticking_{false};

        void check_not_ticking() const noexcept {
            if (ticking_) {
                fatal(fault::hook_changed_during_tick); // the walk in tick() would lose its place
            }
        }

    public:
        /// Add @c hook, due after @c ticks more calls of @c tick(); 0 counts as 1. O(1).
        /// Traps with @c fault::hook_attached_twice if @c hook is already on a list, this one or another.
        void attach(tick_hook &hook, const tick_t ticks) noexcept {
            critical_section _;
            check_not_ticking();
            if (hook.linked()) {
                fatal(fault::hook_attached_twice); // relinking it would cut off every hook after it on its current list
            }
            hook.remaining_ = ticks != 0 ? ticks : 1;
            hook.next_ = head_;
            head_ = &hook;
        }

        /// Remove @c hook, whose callback is not called again once this returns. O(N) in the hooks before it.
        /// Removing a hook that is on no list does nothing. Traps with @c fault::hook_not_on_list if @c hook is on another list.
        void detach(tick_hook &hook) noexcept {
            critical_section _;
            if (!hook.linked()) return;
            check_not_ticking();
            for (tick_hook **link = &head_; *link != nullptr; link = &(*link)->next_) {
                if (*link == &hook) {
                    *link = hook.next_;
                    hook.next_ = nullptr;
                    hook.remaining_ = 0;
                    return;
                }
            }
            fatal(fault::hook_not_on_list); // it would keep being called by the other list's clock
        }

        /// True if no hook is attached.
        [[nodiscard]] bool empty() const noexcept {
            critical_section _;
            return head_ == nullptr;
        }

        /// Calls of @c tick() so far, wrapping at 2^32. A task reads it inside a @c critical_section.
        [[nodiscard]] tick_t now() const noexcept { return now_; }

        /// Count one tick and call every hook that comes due. Call it from the clock's timer interrupt, once per tick.
        /// A callback leaves the list by returning 0. It must not call @c attach or @c detach on this list, which traps with
        /// @c fault::hook_changed_during_tick.
        void tick() noexcept {
            ++now_;
            ticking_ = true;
            for (tick_hook **link = &head_; *link != nullptr;) {
                tick_hook *const h = *link;
                if (h->remaining_ > 1) { // not due: stopping at 1 rather than 0 keeps the hook marked as linked during its callback
                    --h->remaining_;
                    link = &h->next_;
                    continue;
                }
                if (const tick_t again = h->fn_(h->ctx_); again != 0) {
                    h->remaining_ = again;
                    link = &h->next_;
                } else {
                    *link = h->next_; // off the list; link now points at the next hook
                    h->next_ = nullptr;
                    h->remaining_ = 0;
                }
            }
            ticking_ = false;
        }
    };

    /// A clock with a periodic timer interrupt, which calls each attached @c tick_hook when it comes due.
    ///
    /// - @c time_base names its tick length, and must be a @c corosched::time_base.
    /// - @c attach(hook, ticks) registers @c hook, due after @c ticks ticks. Its callback returns when it is due next, or 0 to leave.
    /// - @c detach(hook) removes it, and its callback is not called again after this returns.
    /// - @c now() counts the ticks, wrapping at 2^32.
    ///
    /// Deriving from @c tick_list provides all three:
    /// @code
    /// struct my_clock : corosched::tick_list {
    ///     using time_base = corosched::time_base<std::micro>;
    /// };
    /// // in the timer interrupt: g_clock.tick();
    /// @endcode
    template<class Self>
    concept periodic_clock = any_time_base<typename Self::time_base> && requires(Self &self, const Self &cself, tick_hook &hook, tick_t ticks) {
        { self.attach(hook, ticks) } -> std::same_as<void>;
        { self.detach(hook) } -> std::same_as<void>;
        { cself.now() } -> std::same_as<tick_t>;
    };

    /// A @c tick_hook attached to @c Clock for as long as this object lives: the constructor attaches it and the destructor detaches it.
    ///
    /// Declare it as the last member of a proactor, so it attaches once every other member is set up and detaches before any is destroyed:
    /// the clock's interrupt may call back as soon as it is attached.
    template<periodic_clock Clock>
    class clock_hook {
        Clock &clock_;
        tick_hook hook_;

    public:
        /// Attach a hook calling @c fn with @c ctx, first due after @c first_ticks ticks.
        clock_hook(Clock &clock, void *const ctx, const tick_callback fn, const tick_t first_ticks) noexcept : clock_(clock), hook_(ctx, fn) {
            clock_.attach(hook_, first_ticks);
        }

        ~clock_hook() { clock_.detach(hook_); }

        clock_hook(const clock_hook &) = delete;
        clock_hook &operator=(const clock_hook &) = delete;

        /// The clock's tick count.
        [[nodiscard]] tick_t now() const noexcept { return clock_.now(); }

        /// True until the callback leaves the list by returning 0. Read it inside a @c critical_section.
        [[nodiscard]] bool linked() const noexcept { return hook_.linked(); }
    };
}
