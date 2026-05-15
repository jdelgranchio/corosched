# corosched

A small cooperative scheduler for Cortex-M microcontrollers, built on C++23 coroutines. \
Tasks are coroutines executed by a **cooperative** scheduler with **fixed priorities**, set by task order. \
**No heap** is used for the coroutines' frames, which are required to be statically allocated,
and a convenience script checks that they are large enough. \
The library is built in a **debugger-friendly** manner, exposing by default performance and debug counters,
that allow evaluating schedulability of the running systems.

## Install

With PlatformIO, add the library to `platformio.ini`:
```ini
platform_packages = toolchain-gccarmnoneeabi@1.140201.0
build_flags = -std=c++23
lib_deps = https://github.com/jdelgranchio/corosched.git
```

PlatformIO compiles `src/` with the library.
With any other build system, add `include/` to the include path and compile `src/corosched.cpp` and `src/stataider.cpp` into your program.

On a core with no DWT cycle counter, such as a Cortex-M0, the program also defines the tick source the scheduler times tasks with,
or the link fails on `stataider_ticks`. Define both hooks:
```cpp
stataider::tick_t stataider_ticks() noexcept { return LPC_CT32B1->TC; } // a free-running 32-bit timer
unsigned stataider_start_ticks() noexcept { start_ct32b1(); return 32; } // start it, and say how wide it is
```
or, with no timer to spare, turn timing off with `STATAIDER_ZERO_TICKS();`.

The library needs a C++23 compiler and is built with `-fno-exceptions -fno-rtti -fno-threadsafe-statics`.
It is developed against `arm-none-eabi-gcc` 14.2 and builds for the Cortex-M cores from M0 to M85, and on a host for tests.

## Example

```cpp
#include <corosched/corosched.hpp>

using namespace corosched::literals;

struct sys_clock : corosched::tick_list { // the timer interrupt calls g_clock.tick()
    using time_base = corosched::time_base<std::milli>;
};

sys_clock g_clock;
corosched::ring<char, 64> g_keys; // the UART interrupt calls g_keys.publish(byte)
corosched::slot<bool> g_repaint; // a slot: changes made during a repaint fold into one

corosched::task input(corosched::task_frame<512> &) {
    for (;;) {
        if (const auto event = co_await g_keys.next()) {
            // edit the line with event->data
            g_repaint.publish(true);
        }
    }
}

corosched::task clock_task(corosched::task_frame<512> &) {
    corosched::periodic<sys_clock, 100_ms> period{g_clock};
    for (;;) {
        co_await period.next();
        // read the time
        g_repaint.publish(true);
    }
}

corosched::task display(corosched::task_frame<512> &) {
    for (;;) {
        co_await g_repaint.next();
        // draw the time and the line
    }
}

int main() {
    static corosched::task_frame<512> input_frame, clock_frame, display_frame;
    static corosched::scheduler sched{input(input_frame), clock_task(clock_frame), display(display_frame)}; // highest priority first
    sched.run(); // never returns
}
```

`examples/basic_example.cpp` is the complete version.
It runs in a terminal on a host, with the scheduler's idle function standing in for the interrupts:
```shell
pio run -e basic_example && .pio/build/basic_example/program
```

## Module map

Everything is under `include/corosched/`, and `corosched.hpp` includes it all.
The counters are apart, in `include/stataider/`.

| Header                       | Role                                                                                                                  |
|------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| `core/task.hpp`              | `task` and `co<T>`, and the `frame<N>` buffers they run in                                                            |
| `core/runqueue.hpp`          | `runqueue_word`: the scheduler's ready bits, masked on ARMv6-M                                                        |
| `core/waker.hpp`             | `waker`, a runqueue word and the bits of the tasks it wakes                                                           |
| `core/task_state.hpp`        | `task_state`, what an awaiter shares with its task                                                                    |
| `core/scheduler.hpp`         | `scheduler`: static priorities, idle callable, per-task timing                                                        |
| `core/awaitable.hpp`         | `awaitable` concept: `bind`, `await_ready`, `await_suspend`, `await_disarm`, `await_resume`                           |
| `core/critical.hpp`          | Cortex-M detection, `critical_section`: PRIMASK, or a mutex on a host                                                 |
| `core/fatal.hpp`             | `fatal()`, `fault`, `fault_record` and the `corosched_fatal` hook                                                     |
| `core/proactor.hpp`          | `proactor` concept, `proactor_base`, `proactor_awaiter`                                                               |
| `proactors/slot.hpp`         | `slot<T>`: holds the newest value                                                                                     |
| `proactors/ring.hpp`         | `ring<T, N>`: queues events, so none is skipped                                                                       |
| `proactors/periodic.hpp`     | `periodic<Clock, Period>`: fires every period, with no drift                                                          |
| `proactors/delay.hpp`        | `delay<Clock, Duration>`: fires once                                                                                  |
| `proactors/time.hpp`         | `time_base`, `nanos`, the `_ms` style literals, `periodic_clock`, and `tick_list`/`tick_hook`/`clock_hook` for clocks |
| `primitives/select.hpp`      | `select(a, b, ...)`: resume on whichever fires first                                                                  |
| `primitives/before.hpp`      | `co_await before(proactor, body)`: did the proactor fire during `body`?                                               |
| `primitives/yield.hpp`       | `yield()`: let higher-priority tasks that are ready run first                                                         |
| `../stataider/stataider.hpp` | Counters and timers that a debugger reads live, and `keep_visible`                                                    |

## Learn more

- `examples/basic_example.cpp` is a clock and a line editor that run in a terminal.
- `examples/nucleo_f446re.cpp` is a self-test for a NUCLEO-F446RE, with UART, DAC and ADC loopbacks:
  `pio run -e nucleo_f446re -t upload && pio device monitor -e nucleo_f446re`.

## Contributing and Testing

Enable the git hooks once per clone with `git config core.hooksPath .hooks`. \
The pre-commit hook formats staged C++ files and needs `clang-format`.

```shell
pio test -e native          # the suite
pio test -e native_asan     # with ASan and UBSan
pio test -e native_tsan     # with TSan
pio test -e cortex-m0 --without-uploading --without-testing   # build the suite for a core; the other cores are in platformio.ini
```

## License

MPL-2.0, see `LICENSE`, but examples and docs code samples are MIT-0.
