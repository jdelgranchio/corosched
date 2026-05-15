// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A contract violation reaches `corosched_fatal` with the fault and the call site.

#include <corosched/corosched.hpp>
#include <csetjmp>
#include <cstdint>
#include <ratio>
#include <unity.h>

using namespace corosched;

namespace {
    std::jmp_buf g_stop;
    fault_record g_hook_saw{.code = fault::none, .pc = nullptr};

    task empty_task(task_frame<1024> &) { co_return; }

    // A periodic_clock ticked by hand, with tick() from tick_list.
    struct fake_clock : corosched::tick_list {
        using time_base = corosched::time_base<std::ratio<1, 1'000'000>>; // 1 µs tick
    };
}

// The real program would stop or reset here. The test carries on instead.
[[noreturn]] void corosched_fatal(const fault_record record) noexcept {
    g_hook_saw = record;
    std::longjmp(g_stop, 1);
}

void test_reused_frame_is_recorded(void) {
    static task_frame<1024> frame;
    const auto first = empty_task(frame);
    (void) first;

    if (setjmp(g_stop) == 0) {
        (void) empty_task(frame);
        TEST_FAIL_MESSAGE("reusing a frame did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::frame_reused);
    TEST_ASSERT_NOT_NULL(g_hook_saw.pc);
}

// The longjmps below leave a critical_section without its destructor: harmless here, as on a host it is a recursive mutex and this thread is
// the only one that takes it.

void test_second_consumer_is_recorded(void) {
    slot<int> s;
    runqueue_word word{};
    s.arm(waker{&word, 1u << 0});

    if (setjmp(g_stop) == 0) {
        s.arm(waker{&word, 1u << 1});
        TEST_FAIL_MESSAGE("a second task arming the slot did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::second_consumer);
}

void test_spent_delay_is_recorded(void) {
    using namespace corosched::literals;
    fake_clock clock;
    delay<fake_clock, 1_us> d{clock};
    clock.tick();
    clock.tick(); // one tick for the duration, one for the margin
    TEST_ASSERT_TRUE(d.ready());
    (void) d.produce();

    runqueue_word word{};
    if (setjmp(g_stop) == 0) {
        d.arm(waker{&word, 1u << 0});
        TEST_FAIL_MESSAGE("awaiting a spent delay did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::spent_delay);
}

std::uint32_t again_tick(void *) noexcept { return 1; }

void test_attaching_a_hook_twice_is_recorded(void) {
    tick_list list;
    tick_hook hook{nullptr, again_tick};
    list.attach(hook, 1);

    if (setjmp(g_stop) == 0) {
        list.attach(hook, 1);
        TEST_FAIL_MESSAGE("attaching a hook twice did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::hook_attached_twice);
}

void test_attaching_a_hook_to_a_second_list_is_recorded(void) {
    tick_list first, second;
    tick_hook hook{nullptr, again_tick};
    first.attach(hook, 1);

    if (setjmp(g_stop) == 0) {
        second.attach(hook, 1);
        TEST_FAIL_MESSAGE("attaching a hook to a second list did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::hook_attached_twice);
}

void test_detaching_a_hook_from_the_wrong_list_is_recorded(void) {
    tick_list first, second;
    tick_hook hook{nullptr, again_tick};
    first.attach(hook, 1);

    if (setjmp(g_stop) == 0) {
        second.detach(hook);
        TEST_FAIL_MESSAGE("detaching a hook from a list it is not on did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::hook_not_on_list);
}

struct detacher {
    tick_list *list;
    tick_hook *victim;
    std::uint32_t on_due() noexcept {
        list->detach(*victim);
        return 1;
    }
};

void test_detaching_during_tick_is_recorded(void) {
    tick_list list;
    tick_hook other{nullptr, again_tick};
    detacher d{&list, &other};
    tick_hook hook{&d, member_tick<&detacher::on_due>};
    list.attach(other, 5);
    list.attach(hook, 1);

    if (setjmp(g_stop) == 0) {
        list.tick();
        TEST_FAIL_MESSAGE("a callback detaching a hook during tick() did not stop the program");
    }
    TEST_ASSERT_TRUE(g_hook_saw.code == fault::hook_changed_during_tick);
}

void setUp(void) {}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reused_frame_is_recorded);
    RUN_TEST(test_second_consumer_is_recorded);
    RUN_TEST(test_spent_delay_is_recorded);
    RUN_TEST(test_attaching_a_hook_twice_is_recorded);
    RUN_TEST(test_attaching_a_hook_to_a_second_list_is_recorded);
    RUN_TEST(test_detaching_a_hook_from_the_wrong_list_is_recorded);
    RUN_TEST(test_detaching_during_tick_is_recorded);
    return UNITY_END();
}
