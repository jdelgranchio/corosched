// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// `before(proactor, body)` — bracket-validate a non-suspending body against a
// proactor. The body always runs; the result is `expected{body}` if the proactor
// stayed quiet, `unexpected{produce}` if it fired during the body.

#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>
#include <expected>
#include <optional>

using namespace corosched;

namespace {
    struct outcome_t {
        bool has_value;
        int body_or_error; // body result on success; produce().data on error
        std::uint32_t writes; // 0 when has_value
    };

    std::optional<outcome_t> g_out;
    slot<int> *g_publish_during_body = nullptr;
    int g_publish_value = 0;
}

// Body that, when configured, publishes into a sibling slot to simulate an
// ISR firing mid-body. The proactor we're guarding is `r`, but the publish goes
// into the SAME `r` (single-threaded test).
static int run_body() noexcept {
    if (g_publish_during_body != nullptr) {
        g_publish_during_body->publish(g_publish_value);
    }
    return 42;
}

corosched::task before_user(corosched::task_frame<1024> &f, slot<int> &r) {
    (void) f;
    // Suspend once so the test can drive `before` from a known state.
    co_await r.next(); // consumes a priming publish
    const auto r2 = co_await corosched::before(r, run_body);
    if (r2.has_value()) {
        g_out = outcome_t{true, *r2, 0};
    } else {
        g_out = outcome_t{false, r2.error().data, r2.error().writes};
    }
}

// 1. Quiet path: nothing fires during the body. `before` returns the body's
//    value; the runqueue bit is left clean.
void test_before_quiet_returns_body_value(void) {
    slot<int> r;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user(buf, r)};

    g_publish_during_body = nullptr;

    sched.kick();
    r.publish(1); // priming publish so the initial co_await resolves
    sched.step(); // task resumes, runs `before`, completes
    sched.step(); // final-suspend sweep

    TEST_ASSERT_TRUE(g_out.has_value());
    TEST_ASSERT_TRUE(g_out->has_value);
    TEST_ASSERT_EQUAL_INT(42, g_out->body_or_error);
    TEST_ASSERT_FALSE(r.ready());
}

// 2. Fires-during-body (single-threaded simulation): the body itself triggers
//    a publish (stands in for an ISR firing while we compute). `before` must
//    report unexpected with the published value; body's 42 is discarded.
void test_before_fires_during_body_returns_error(void) {
    slot<int> r;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user(buf, r)};

    g_publish_during_body = &r;
    g_publish_value = 7;

    sched.kick();
    r.publish(1); // priming publish
    sched.step();
    sched.step();

    TEST_ASSERT_TRUE(g_out.has_value());
    TEST_ASSERT_FALSE(g_out->has_value);
    TEST_ASSERT_EQUAL_INT(7, g_out->body_or_error);
    TEST_ASSERT_EQUAL_UINT32(1u, g_out->writes);
    TEST_ASSERT_FALSE(r.ready()); // bit was cleared by `before`
}

// 3. Proactor pending before `before` is entered: arm() observes a non-empty
//    slot and synchronously wakes our bit. `before` still runs the body
//    (transactional contract: body always runs), then reports unexpected.
namespace {
    std::optional<outcome_t> g_out3;
    bool g_body_ran3 = false;
}

static int run_body3() noexcept {
    g_body_ran3 = true;
    return 99;
}

corosched::task before_user_prepending(corosched::task_frame<1024> &f, slot<int> &r, slot<int> &trigger) {
    (void) f;
    co_await trigger.next(); // unrelated kick
    const auto r2 = co_await corosched::before(r, run_body3);
    if (r2.has_value())
        g_out3 = outcome_t{true, *r2, 0};
    else
        g_out3 = outcome_t{false, r2.error().data, r2.error().writes};
}

void test_before_prepending_event_still_runs_body(void) {
    slot<int> r, trigger;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user_prepending(buf, r, trigger)};

    sched.kick();
    r.publish(123); // pre-stage the guarded proactor
    trigger.publish(0); // resume the task — it will then call `before`
    sched.step();
    sched.step();

    TEST_ASSERT_TRUE(g_body_ran3); // body always runs
    TEST_ASSERT_TRUE(g_out3.has_value());
    TEST_ASSERT_FALSE(g_out3->has_value); // reported as fired
    TEST_ASSERT_EQUAL_INT(123, g_out3->body_or_error);
    TEST_ASSERT_FALSE(r.ready());
}

// 4. Back-to-back `before` calls: after a fired-and-consumed cycle, the next
//    quiet call must return the body value (no carry-over of the prior bit).
namespace {
    std::optional<outcome_t> g_out4a;
    std::optional<outcome_t> g_out4b;
}

corosched::task before_user_btb(corosched::task_frame<1024> &f, slot<int> &r, slot<int> &trigger) {
    (void) f;
    co_await trigger.next();

    // First call: cause a fire during the body.
    const auto a = co_await corosched::before(r, [&]() noexcept {
        r.publish(11);
        return 1;
    });
    g_out4a = a.has_value() ? outcome_t{true, *a, 0} : outcome_t{false, a.error().data, a.error().writes};

    // Second call: quiet.
    const auto b = co_await corosched::before(r, []() noexcept { return 2; });
    g_out4b = b.has_value() ? outcome_t{true, *b, 0} : outcome_t{false, b.error().data, b.error().writes};
}

void test_before_back_to_back_no_carry(void) {
    slot<int> r, trigger;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user_btb(buf, r, trigger)};

    sched.kick();
    trigger.publish(0);
    sched.step();
    sched.step();

    TEST_ASSERT_TRUE(g_out4a.has_value());
    TEST_ASSERT_FALSE(g_out4a->has_value); // first: fired
    TEST_ASSERT_EQUAL_INT(11, g_out4a->body_or_error);

    TEST_ASSERT_TRUE(g_out4b.has_value());
    TEST_ASSERT_TRUE(g_out4b->has_value); // second: quiet
    TEST_ASSERT_EQUAL_INT(2, g_out4b->body_or_error);
    TEST_ASSERT_FALSE(r.ready());
}

// 5. Void-body specialization: when the body returns `void`, `before` returns
//    `std::expected<void, P::result_type>`. The quiet path is `.has_value()`
//    with no payload; a fire is `unexpected{produce()}`.
namespace {
    std::optional<bool> g_out5_quiet;
    std::optional<std::uint32_t> g_out5_writes;
    bool g_body_ran5 = false;
}

corosched::task before_user_void(corosched::task_frame<1024> &f, slot<int> &r, slot<int> &trigger, bool fire_during_body) {
    (void) f;
    co_await trigger.next();
    const auto r2 = co_await corosched::before(r, [&]() noexcept {
        g_body_ran5 = true;
        if (fire_during_body) r.publish(55);
    });
    g_out5_quiet = r2.has_value();
    g_out5_writes = r2.has_value() ? 0u : r2.error().writes;
}

void test_before_void_body_quiet(void) {
    slot<int> r, trigger;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user_void(buf, r, trigger, false)};

    sched.kick();
    trigger.publish(0);
    sched.step();
    sched.step();

    TEST_ASSERT_TRUE(g_body_ran5);
    TEST_ASSERT_TRUE(g_out5_quiet.has_value());
    TEST_ASSERT_TRUE(*g_out5_quiet);
}

void test_before_void_body_fires(void) {
    slot<int> r, trigger;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{before_user_void(buf, r, trigger, true)};

    sched.kick();
    trigger.publish(0);
    sched.step();
    sched.step();

    TEST_ASSERT_TRUE(g_body_ran5);
    TEST_ASSERT_TRUE(g_out5_quiet.has_value());
    TEST_ASSERT_FALSE(*g_out5_quiet);
    TEST_ASSERT_EQUAL_UINT32(1u, *g_out5_writes);
}

void setUp(void) {
    g_out.reset();
    g_out3.reset();
    g_out4a.reset();
    g_out4b.reset();
    g_out5_quiet.reset();
    g_out5_writes.reset();
    g_publish_during_body = nullptr;
    g_publish_value = 0;
    g_body_ran3 = false;
    g_body_ran5 = false;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_before_quiet_returns_body_value);
    RUN_TEST(test_before_fires_during_body_returns_error);
    RUN_TEST(test_before_prepending_event_still_runs_body);
    RUN_TEST(test_before_back_to_back_no_carry);
    RUN_TEST(test_before_void_body_quiet);
    RUN_TEST(test_before_void_body_fires);
    return UNITY_END();
}
