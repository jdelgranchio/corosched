// SPDX-License-Identifier: MIT-0

// A clock above a line editor, in a terminal, as three tasks. It runs on a host:
//
//   pio run -e basic_example && .pio/build/basic_example/program
//
//   input    takes keys from a ring and edits the line.     ring
//   clock    reads the time every 100 ms.                   periodic
//   display  repaints the screen, one write at a time.      slot
//
// On a controller, a timer tick, a received byte and a finished transmit would be interrupts.
// Here they are one callback: wait_for_events is the scheduler's idle function, where aio_suspend sleeps as WFI would and the code after it does what
// the handlers would do. A real interrupt handler looks the same, see nucleo_f446re.cpp.

#include <corosched/corosched.hpp>

#include <aio.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ratio>

using namespace corosched::literals;

struct terminal_clock : corosched::tick_list {
    using time_base = corosched::time_base<std::centi>; // a 10 ms tick
};

static terminal_clock g_clock;

static corosched::ring<char, 64> g_keys; // a ring, as every key counts
static corosched::slot<bool> g_repaint; // a slot: whatever changes during a write folds into one repaint
static corosched::slot<bool> g_written; // the terminal took the last write

constexpr char ctrl_c = '\x03', ctrl_d = '\x04', escape = '\x1b', backspace = '\x7f';

// What the display shows. The tasks share it without locking, since a task is never interrupted by another task.
static struct {
    char time[9];
    char line[64];
    std::size_t length;
    char echo[64]; // the last line entered
    bool quit;
} g_screen;

static bool g_running = true;

static aiocb g_rx, g_tx;
static char g_rx_buffer[32], g_tx_buffer[256];
static bool g_tx_busy = false;

static void start_read() noexcept {
    g_rx.aio_fildes = STDIN_FILENO;
    g_rx.aio_buf = g_rx_buffer;
    g_rx.aio_nbytes = sizeof g_rx_buffer;
    aio_read(&g_rx);
}

static void start_write(const std::size_t size) noexcept {
    g_tx.aio_fildes = STDOUT_FILENO;
    g_tx.aio_buf = g_tx_buffer;
    g_tx.aio_nbytes = size;
    g_tx_busy = true;
    aio_write(&g_tx);
}

static void wait_for_events() noexcept {
    using steady = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds tick{10};
    static auto next_tick = steady::now() + tick;

    // Sleep until an operation completes or the next tick is due, whichever comes first. A list may hold null entries.
    const auto until_tick = std::chrono::duration_cast<std::chrono::nanoseconds>(next_tick - steady::now()).count();
    const timespec timeout{.tv_sec = 0, .tv_nsec = static_cast<long>(std::max<std::int64_t>(until_tick, 0))};
    const aiocb *const pending[] = {&g_rx, g_tx_busy ? &g_tx : nullptr};
    aio_suspend(pending, 2, &timeout);

    // The timer interrupt.
    while (steady::now() >= next_tick) {
        g_clock.tick();
        next_tick += tick;
    }

    // The receive interrupt. An arrow key arrives as one escape sequence, which is not text.
    if (aio_error(&g_rx) != EINPROGRESS) {
        const auto received = aio_return(&g_rx);
        if (received <= 0) {
            g_keys.publish(ctrl_d); // stdin closed
            return;
        }
        if (g_rx_buffer[0] != escape) {
            for (ssize_t i = 0; i < received; ++i) g_keys.publish(g_rx_buffer[i]);
        }
        start_read();
    }

    // The transmit-complete interrupt.
    if (g_tx_busy && aio_error(&g_tx) != EINPROGRESS) {
        (void) aio_return(&g_tx);
        g_tx_busy = false;
        g_written.publish(true);
    }
}

static corosched::task input(corosched::task_frame<512> &) {
    for (;;) {
        const auto event = co_await g_keys.next();
        if (!event) continue;
        const char key = event->data;

        if (key == ctrl_c || key == ctrl_d) {
            g_screen.quit = true;
        } else if (key == '\r') {
            std::memcpy(g_screen.echo, g_screen.line, sizeof g_screen.echo);
            g_screen.length = 0;
        } else if (key == backspace) {
            if (g_screen.length > 0) --g_screen.length;
        } else if (key >= ' ' && key < backspace && g_screen.length < sizeof g_screen.line - 1) {
            g_screen.line[g_screen.length++] = key;
        }
        g_screen.line[g_screen.length] = '\0';
        g_repaint.publish(true);
    }
}

static corosched::task clock_task(corosched::task_frame<512> &) {
    corosched::periodic<terminal_clock, 100_ms> period{g_clock};
    for (;;) {
        co_await period.next();
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(g_screen.time, sizeof g_screen.time, "%H:%M:%S", &local);
        g_repaint.publish(true);
    }
}

static corosched::task display(corosched::task_frame<512> &) {
    for (;;) {
        co_await g_repaint.next();
        const bool last = g_screen.quit;
        const int size = last ? std::snprintf(g_tx_buffer, sizeof g_tx_buffer, "\x1b[?1049l") // leave the alternate screen
                              : std::snprintf(g_tx_buffer, sizeof g_tx_buffer,
                                              "\x1b[1;1H\x1b[K%s"
                                              "\x1b[3;1H\x1b[Kecho: %s"
                                              "\x1b[6;1H\x1b[KEnter echoes the line, Ctrl-C quits."
                                              "\x1b[5;1H\x1b[K> %s", // last, so the cursor stays at the end of the line
                                              g_screen.time, g_screen.echo, g_screen.line);
        start_write(static_cast<std::size_t>(size));
        co_await g_written.next(); // only one write at a time, and any repaint asked for meanwhile waits in the slot
        if (last) g_running = false;
    }
}

int main() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fputs("basic_example draws on a terminal, so stdin and stdout must be one.\n", stderr);
        return 1;
    }

    termios saved{};
    tcgetattr(STDIN_FILENO, &saved);
    termios raw = saved;
    cfmakeraw(&raw); // keys arrive as they are pressed, and the terminal does not echo them itself
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    std::fputs("\x1b[?1049h", stdout); // the alternate screen, which puts the shell's text back on exit
    std::fflush(stdout);

    start_read();

    static corosched::task_frame<512> input_frame, clock_frame, display_frame;
    static corosched::scheduler sched{input(input_frame), clock_task(clock_frame), display(display_frame)};

    // sched.run(wait_for_events) would do on a controller. This program has to stop and put the terminal back.
    sched.kick();
    while (g_running) sched.step(wait_for_events);

    aio_cancel(STDIN_FILENO, &g_rx);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);

    std::printf("Scheduler: idle=%d\n", sched.stats().idle);
    std::printf("Input    : count=%4d, total=%9lluns, worst=%7lluns\n", sched.stats().tasks[0].count, sched.stats().tasks[0].total,
                sched.stats().tasks[0].worst);
    std::printf("Clock    : count=%4d, total=%9lluns, worst=%7lluns\n", sched.stats().tasks[1].count, sched.stats().tasks[1].total,
                sched.stats().tasks[1].worst);
    std::printf("Display  : count=%4d, total=%9lluns, worst=%7lluns\n", sched.stats().tasks[2].count, sched.stats().tasks[2].total,
                sched.stats().tasks[2].worst);
}
