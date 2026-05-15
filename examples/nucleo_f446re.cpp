// SPDX-License-Identifier: MIT-0

// A hardware self-test for a NUCLEO-F446RE: five tasks that check each other through real peripherals.
//
//   pong    UART4 answers every byte it receives with its complement.                   ring
//   ping    every 20 ms, USART1 sends a byte and waits up to 5 ms for the answer.        periodic, co<T>, select, delay, ring
//   analog  every 10 ms, the DAC sets a level and the ADC reads it back on the same pin.  periodic, slot
//   blink   LD2 blinks at 1 Hz while everything passes, and at 5 Hz for a second after a failure.
//   report  every second, prints the counters on the ST-LINK virtual COM port.          periodic, slot from a HAL callback
//
// Wiring, all on the Arduino headers: D8 to A1 (USART1 TX to UART4 RX) and A0 to D2 (UART4 TX to USART1 RX).
// Without the wires, ping counts timeouts and the LED blinks fast. The DAC and the ADC share PA4 (A2), so the analog check needs no wire.
// A trap in the library leaves LD2 on, with the fault in g_fault.
//
//   pio run -e nucleo_f446re -t upload && pio device monitor -e nucleo_f446re
//
// The HAL sets up the clocks and peripherals. The interrupt handlers use the registers directly, so each is a couple of lines: read the
// value, publish it to a proactor. The tasks share g_stats without locking, since a task is never interrupted by another task.

#include <corosched/corosched.hpp>

#include <stm32f4xx_hal.h>

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ratio>

using namespace corosched::literals;

// SysTick, which the HAL already runs at 1 kHz, ticks this clock.
struct sys_clock : corosched::tick_list {
    using time_base = corosched::time_base<std::milli>;
};

static sys_clock g_clock;

// What the interrupts hand to the tasks.
static corosched::ring<std::uint8_t, 8> g_usart1_rx; // a ring, as every byte counts
static corosched::ring<std::uint8_t, 8> g_uart4_rx;
static corosched::slot<std::uint16_t> g_adc; // a slot: only the newest conversion matters
static corosched::slot<bool> g_vcp_sent; // the HAL finished sending on the virtual COM port

static UART_HandleTypeDef g_usart1, g_uart4, g_vcp;
static DAC_HandleTypeDef g_dac;
static ADC_HandleTypeDef g_adc1;

static struct {
    std::uint32_t pings, pongs, wrong, timeouts;
    std::uint32_t samples, bad_samples, worst_error;
} g_stats;

// Set by main, for report to print the time each task spends between two suspensions.
static const std::array<stataider::counter_ticks_max, 5> *g_task_times;

static corosched::fault_record g_fault;

// ---------------------------------------------------------------------------------------------------------------------------- interrupts

extern "C" void SysTick_Handler() {
    HAL_IncTick();
    g_clock.tick();
}

static void on_rx(USART_TypeDef *const uart, corosched::ring<std::uint8_t, 8> &rx) {
    const std::uint32_t sr = uart->SR;
    const auto byte = static_cast<std::uint8_t>(uart->DR); // reading SR then DR also clears an overrun
    if (sr & USART_SR_RXNE) rx.publish(byte);
}

extern "C" void USART1_IRQHandler() { on_rx(USART1, g_usart1_rx); }

extern "C" void UART4_IRQHandler() { on_rx(UART4, g_uart4_rx); }

extern "C" void ADC_IRQHandler() { g_adc.publish(static_cast<std::uint16_t>(ADC1->DR)); } // reading DR clears EOC

extern "C" void USART2_IRQHandler() { HAL_UART_IRQHandler(&g_vcp); }

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *) { g_vcp_sent.publish(true); }

// Replaces the library's default handler, so a trap shows on the board.
void corosched_fatal(const corosched::fault_record record) noexcept {
    __disable_irq();
    g_fault = record;
    stataider::keep_visible(g_fault);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    for (;;) {
    }
}

// ----------------------------------------------------------------------------------------------------------------------------- tasks

static void put(USART_TypeDef *const uart, const std::uint8_t byte) {
    while (!(uart->SR & USART_SR_TXE)) {
    }
    uart->DR = byte;
}

corosched::task pong(corosched::task_frame<128> &) {
    for (;;) {
        if (const auto event = co_await g_uart4_rx.next()) {
            put(UART4, static_cast<std::uint8_t>(~event->data));
        }
    }
}

enum class reply : std::uint8_t { ok, wrong, timeout };

// One round trip: send seq on USART1 and wait for pong's answer, or for the timeout.
corosched::co<reply> exchange(corosched::co_frame<192> &, const std::uint8_t seq) {
    while (g_usart1_rx.ready()) (void) g_usart1_rx.produce(); // an answer that came after its timeout
    put(USART1, seq);

    corosched::delay<sys_clock, 5_ms> timeout{g_clock};
    auto answer = g_usart1_rx.next();
    auto expired = timeout.next();
    const auto result = co_await corosched::select(answer, expired);

    if (result.index() == 1) co_return reply::timeout;
    const auto &event = *std::get_if<0>(&result); // not std::get, which links in abort() for a variant that cannot be empty here
    co_return event && event->data == static_cast<std::uint8_t>(~seq) ? reply::ok : reply::wrong;
}

corosched::task ping(corosched::task_frame<512> &) {
    corosched::periodic<sys_clock, 20_ms> period{g_clock};
    for (std::uint8_t seq = 0;; ++seq) {
        co_await period.next();
        corosched::co_frame<192> frame; // a fresh one for each call
        switch (co_await exchange(frame, seq)) {
            case reply::ok: ++g_stats.pongs; break;
            case reply::wrong: ++g_stats.wrong; break;
            case reply::timeout: ++g_stats.timeouts; break;
        }
        ++g_stats.pings; // counted once finished, so report never sees one still in flight
    }
}

corosched::task analog(corosched::task_frame<384> &) {
    // A triangle wave, kept away from the rails, where the DAC's output buffer cannot follow.
    constexpr int low = 256, high = 3840, step = 64, tolerance = 48;

    // Start the grid 5 ms after ping's, so a sample never lands during a UART exchange: on a NUCLEO-F446RE, its edges couple into PA4 now
    // and then, and throw the sample off by up to 130 LSB. A periodic counts from its construction.
    corosched::delay<sys_clock, 5_ms> offset{g_clock};
    co_await offset.next();
    corosched::periodic<sys_clock, 10_ms> period{g_clock};
    int level = low, direction = step;
    for (;;) {
        co_await period.next();
        HAL_DAC_SetValue(&g_dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, static_cast<std::uint32_t>(level));
        ADC1->CR2 |= ADC_CR2_SWSTART; // the 480-cycle sampling window outlasts the DAC's settling
        const auto sample = co_await g_adc.next();

        const int diff = sample.data - level;
        const auto error = static_cast<std::uint32_t>(diff < 0 ? -diff : diff);
        ++g_stats.samples;
        if (error > tolerance) ++g_stats.bad_samples;
        if (error > g_stats.worst_error) g_stats.worst_error = error;

        level += direction;
        if (level == low || level == high) direction = -direction;
    }
}

corosched::task blink(corosched::task_frame<256> &) {
    corosched::periodic<sys_clock, 100_ms> period{g_clock};
    std::uint32_t failures_seen = 0;
    unsigned fast_for = 0, slow_count = 0;
    for (;;) {
        co_await period.next();
        if (const auto failures = g_stats.wrong + g_stats.timeouts + g_stats.bad_samples; failures != failures_seen) {
            failures_seen = failures;
            fast_for = 10;
        }
        const bool fast = fast_for > 0;
        if (fast) --fast_for;
        if (fast || ++slow_count == 5) {
            slow_count = 0;
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
        }
    }
}

// Start sending on the virtual COM port. The text must stay alive until g_vcp_sent fires.
static void vcp_send(const char *const text, const int length) {
    HAL_UART_Transmit_IT(&g_vcp, reinterpret_cast<const std::uint8_t *>(text), static_cast<std::uint16_t>(length));
}

corosched::task report(corosched::task_frame<768> &) {
    static constexpr char banner[] = "\r\ncorosched self-test on NUCLEO-F446RE. Wire D8 to A1 and A0 to D2.\r\n";
    vcp_send(banner, sizeof banner - 1);
    co_await g_vcp_sent.next();

    corosched::periodic<sys_clock, 1_s> period{g_clock};
    char line[256]; // in the task's frame, so it outlives the suspension while the interrupt sends it
    std::uint64_t busy_before = 0;
    for (std::uint32_t seconds = 1;; ++seconds) {
        co_await period.next();

        // Each task's longest run between two suspensions, in microseconds, and the share of the last second spent running tasks.
        const std::uint32_t cycles_per_us = SystemCoreClock / 1'000'000;
        std::array<std::uint32_t, 5> worst_us{};
        std::uint64_t busy = 0;
        for (std::size_t i = 0; i < worst_us.size(); ++i) {
            worst_us[i] = static_cast<std::uint32_t>((*g_task_times)[i].worst / cycles_per_us);
            busy += (*g_task_times)[i].total;
        }
        const auto busy_permille = static_cast<std::uint32_t>((busy - busy_before) * 1000 / SystemCoreClock);
        busy_before = busy;

        const int length = std::snprintf(line, sizeof line,
                                         "%4" PRIu32 " s  uart %" PRIu32 "/%" PRIu32 " wrong %" PRIu32 " timeout %" PRIu32 "  adc %" PRIu32
                                         " bad %" PRIu32 " worst %" PRIu32 " lsb  busy %" PRIu32 ".%" PRIu32 "%%  worst us: pong %" PRIu32
                                         " ping %" PRIu32 " analog %" PRIu32 " blink %" PRIu32 " report %" PRIu32 "\r\n",
                                         seconds, g_stats.pongs, g_stats.pings, g_stats.wrong, g_stats.timeouts, g_stats.samples, g_stats.bad_samples,
                                         g_stats.worst_error, busy_permille / 10, busy_permille % 10, worst_us[0], worst_us[1], worst_us[2],
                                         worst_us[3], worst_us[4]);
        vcp_send(line, length < static_cast<int>(sizeof line) ? length : static_cast<int>(sizeof line) - 1);
        co_await g_vcp_sent.next();
    }
}

// ----------------------------------------------------------------------------------------------------------------------------- setup

static void check(const HAL_StatusTypeDef status) {
    if (status != HAL_OK) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
        for (;;) {
        }
    }
}

// 180 MHz from the internal 16 MHz oscillator, which every board has: APB1 at 45 MHz, APB2 at 90 MHz.
static void clock_init() {
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitTypeDef osc{};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = 8; // 2 MHz into the PLL
    osc.PLL.PLLN = 180; // 360 MHz VCO
    osc.PLL.PLLP = RCC_PLLP_DIV2; // 180 MHz
    osc.PLL.PLLQ = 2;
    osc.PLL.PLLR = 2;
    check(HAL_RCC_OscConfig(&osc));
    check(HAL_PWREx_EnableOverDrive()); // needed above 168 MHz

    RCC_ClkInitTypeDef clk{};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    check(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5)); // also restarts SysTick at 1 kHz for the new clock
}

static void pins_init(const std::uint32_t pins, const std::uint32_t mode, const std::uint32_t alternate = 0) {
    GPIO_InitTypeDef gpio{};
    gpio.Pin = pins;
    gpio.Mode = mode;
    gpio.Pull = mode == GPIO_MODE_AF_PP ? GPIO_PULLUP : GPIO_NOPULL; // an unwired UART input idles high instead of reading noise
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = alternate;
    HAL_GPIO_Init(GPIOA, &gpio);
}

static void uart_init(UART_HandleTypeDef &uart, USART_TypeDef *const instance) {
    uart.Instance = instance;
    uart.Init.BaudRate = 115200;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.Mode = UART_MODE_TX_RX;
    uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart.Init.OverSampling = UART_OVERSAMPLING_16;
    check(HAL_UART_Init(&uart));
}

static void peripherals_init() {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_UART4_CLK_ENABLE();
    __HAL_RCC_DAC_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();

    pins_init(GPIO_PIN_5, GPIO_MODE_OUTPUT_PP); // LD2
    pins_init(GPIO_PIN_9 | GPIO_PIN_10, GPIO_MODE_AF_PP, GPIO_AF7_USART1); // D8 TX, D2 RX
    pins_init(GPIO_PIN_0 | GPIO_PIN_1, GPIO_MODE_AF_PP, GPIO_AF8_UART4); // A0 TX, A1 RX
    pins_init(GPIO_PIN_2 | GPIO_PIN_3, GPIO_MODE_AF_PP, GPIO_AF7_USART2); // to the ST-LINK
    pins_init(GPIO_PIN_4, GPIO_MODE_ANALOG); // A2: DAC out and ADC in

    uart_init(g_usart1, USART1);
    uart_init(g_uart4, UART4);
    uart_init(g_vcp, USART2);
    USART1->CR1 |= USART_CR1_RXNEIE;
    UART4->CR1 |= USART_CR1_RXNEIE;

    g_dac.Instance = DAC;
    check(HAL_DAC_Init(&g_dac));
    DAC_ChannelConfTypeDef dac_channel{};
    dac_channel.DAC_Trigger = DAC_TRIGGER_NONE;
    dac_channel.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    check(HAL_DAC_ConfigChannel(&g_dac, &dac_channel, DAC_CHANNEL_1));
    check(HAL_DAC_Start(&g_dac, DAC_CHANNEL_1));

    g_adc1.Instance = ADC1;
    g_adc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4; // 22.5 MHz
    g_adc1.Init.Resolution = ADC_RESOLUTION_12B;
    g_adc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    g_adc1.Init.ScanConvMode = DISABLE;
    g_adc1.Init.ContinuousConvMode = DISABLE;
    g_adc1.Init.DiscontinuousConvMode = DISABLE;
    g_adc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    g_adc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    g_adc1.Init.NbrOfConversion = 1;
    g_adc1.Init.DMAContinuousRequests = DISABLE;
    g_adc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    check(HAL_ADC_Init(&g_adc1));
    ADC_ChannelConfTypeDef adc_channel{};
    adc_channel.Channel = ADC_CHANNEL_4; // PA4
    adc_channel.Rank = 1;
    adc_channel.SamplingTime = ADC_SAMPLETIME_480CYCLES;
    check(HAL_ADC_ConfigChannel(&g_adc1, &adc_channel));
    ADC1->CR1 |= ADC_CR1_EOCIE;
    ADC1->CR2 |= ADC_CR2_ADON;

    for (const auto irq: {USART1_IRQn, UART4_IRQn, USART2_IRQn, ADC_IRQn}) {
        HAL_NVIC_SetPriority(irq, 5, 0);
        HAL_NVIC_EnableIRQ(irq);
    }

    HAL_DBGMCU_EnableDBGSleepMode(); // keep the debugger attached through the scheduler's WFI, to read the stats live
}

int main() {
    HAL_Init();
    clock_init();
    peripherals_init();

    // In priority order: pong answers first, so ping never times out waiting on a lower-priority task.
    static corosched::task_frame<128> pong_frame;
    static corosched::task_frame<256> blink_frame;
    static corosched::task_frame<384> analog_frame;
    static corosched::task_frame<512> ping_frame;
    static corosched::task_frame<768> report_frame;
    static corosched::scheduler sched{
            pong(pong_frame), ping(ping_frame), analog(analog_frame), blink(blink_frame), report(report_frame),
    };
    g_task_times = &sched.stats().tasks;
    sched.run();
}
