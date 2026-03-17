//
// Stage 3b: PIO-based row scanning + CPU scan + all Phase 1 commands
//
// New in 3b:
//   - PIOSCAN <N_frames>: PIO-driven column scanning, CPU handles rows
//   - PIOROWTIME <N>: measure PIO row-switching overhead (no delay)
//   - PIO program drives 20 contiguous column pins (GP1-20) with
//     single-cycle output, precise hardware-timed delays
//   - CPU manages row pins and pushes column data to PIO FIFO
//
// Retained from 3a: SCAN, ROWTIME, ROWS, PATTERN (CPU-based)
// Retained from 1e: ON, OFF, POS, RUN, STOP, STATS, JITTER, SWEEP, SERIAL
//
// ---------------------------------------------------------------------------
#include <hardware/structs/m33.h>
#include <hardware/pio.h>
#include "constants.h"
#include "utilities.h"

// ---------------------------------------------------------------------------
// DWT cycle counter
// ---------------------------------------------------------------------------
static bool     dwt_available = false;
static uint32_t cycles_per_us = 150;

static void dwt_init() {
    cycles_per_us = F_CPU / 1000000UL;
    m33_hw->demcr |= (1UL << 24);
    if (m33_hw->dwt_ctrl & (1UL << 25)) {
        dwt_available = false;
        Serial.println("WARNING: DWT cycle counter NOT available");
        return;
    }
    m33_hw->dwt_ctrl |= (1UL << 0);
    m33_hw->dwt_cyccnt = 0;
    volatile uint32_t a = m33_hw->dwt_cyccnt;
    volatile uint32_t b = m33_hw->dwt_cyccnt;
    if (b > a) {
        dwt_available = true;
        Serial.print("DWT OK  cycles_per_us=");
        Serial.println(cycles_per_us);
    } else {
        dwt_available = false;
        Serial.println("WARNING: DWT not incrementing");
    }
}

static void __not_in_flash_func(dwt_delay_cycles)(uint32_t cycles) {
    uint32_t start = m33_hw->dwt_cyccnt;
    while ((m33_hw->dwt_cyccnt - start) < cycles) { /* spin */ }
}

static inline float cycles_to_us(uint32_t cycles) {
    return (float)cycles / (float)cycles_per_us;
}

// ---------------------------------------------------------------------------
// Timing stats (reused for both single-LED and scan measurements)
// ---------------------------------------------------------------------------
static uint32_t stat_on_min  = UINT32_MAX;
static uint32_t stat_on_max  = 0;
static uint64_t stat_on_sum  = 0;
static uint32_t stat_off_min = UINT32_MAX;
static uint32_t stat_off_max = 0;
static uint64_t stat_off_sum = 0;
static uint32_t stat_count   = 0;

static void stats_reset() {
    stat_on_min  = UINT32_MAX;
    stat_on_max  = 0;
    stat_on_sum  = 0;
    stat_off_min = UINT32_MAX;
    stat_off_max = 0;
    stat_off_sum = 0;
    stat_count   = 0;
}

static void __not_in_flash_func(stats_update)(uint32_t on_cyc, uint32_t off_cyc) {
    if (on_cyc < stat_on_min) stat_on_min = on_cyc;
    if (on_cyc > stat_on_max) stat_on_max = on_cyc;
    stat_on_sum += on_cyc;
    if (off_cyc < stat_off_min) stat_off_min = off_cyc;
    if (off_cyc > stat_off_max) stat_off_max = off_cyc;
    stat_off_sum += off_cyc;
    stat_count++;
}

// Generic stats printer with custom label for the two fields
static void stats_print_fields(const char* label,
                               const char* field1_name, const char* field2_name) {
    if (stat_count == 0) {
        Serial.println("No stats yet");
        return;
    }
    float f1_mean = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
    float f2_mean = cycles_to_us((uint32_t)(stat_off_sum / stat_count));

    Serial.print(label);
    Serial.print("  N=");
    Serial.println(stat_count);
    Serial.print("  ");
    Serial.print(field1_name);
    Serial.print(": min=");
    Serial.print(cycles_to_us(stat_on_min), 3);
    Serial.print("us  max=");
    Serial.print(cycles_to_us(stat_on_max), 3);
    Serial.print("us  mean=");
    Serial.print(f1_mean, 3);
    Serial.println("us");
    if (field2_name) {
        Serial.print("  ");
        Serial.print(field2_name);
        Serial.print(": min=");
        Serial.print(cycles_to_us(stat_off_min), 3);
        Serial.print("us  max=");
        Serial.print(cycles_to_us(stat_off_max), 3);
        Serial.print("us  mean=");
        Serial.print(f2_mean, 3);
        Serial.println("us");
    }
}

static void stats_print(const char* label) {
    stats_print_fields(label, "ON", "OFF");
}

static void stats_print_csv_row(float commanded_on_us) {
    if (stat_count == 0) return;
    float on_mean  = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
    float off_mean = cycles_to_us((uint32_t)(stat_off_sum / stat_count));
    Serial.print(commanded_on_us, 3);
    Serial.print(",");
    Serial.print(cycles_to_us(stat_on_min), 3);
    Serial.print(",");
    Serial.print(cycles_to_us(stat_on_max), 3);
    Serial.print(",");
    Serial.print(on_mean, 3);
    Serial.print(",");
    Serial.print(cycles_to_us(stat_off_min), 3);
    Serial.print(",");
    Serial.print(cycles_to_us(stat_off_max), 3);
    Serial.print(",");
    Serial.print(off_mean, 3);
    Serial.print(",");
    Serial.println(stat_count);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Single-LED state (Phase 1)
static float    on_us  = 10.0f;
static float    off_us = 40.0f;
static uint32_t on_cycles  = 0;
static uint32_t off_cycles = 0;
static bool     running = true;

static uint8_t  led_row_layout = 0;
static uint8_t  led_col_layout = 0;
static uint8_t  active_row_pin = 0;
static uint8_t  active_col_pin = 0;

static uint32_t count = 0;
static uint32_t last_heartbeat_us = 0;

// Scan state (Phase 3)
static uint8_t  active_rows = 20;         // how many rows to scan (1-20)
static uint32_t col_pattern = 0xFFFFF;    // 20-bit: which columns ON (default: all)

// Pre-computed GPIO masks for scanning
static uint64_t row_on_mask[PANEL_SIZE];   // mask to enable each row
static uint64_t col_on_mask;               // mask of column pins to set LOW (ON)
static uint64_t col_off_mask;              // mask of column pins to set HIGH (OFF)
static uint64_t all_col_mask;              // mask of ALL column pins

static void precompute_scan_masks() {
    // Row masks (schematic row pins)
    for (int r = 0; r < PANEL_SIZE; r++) {
        row_on_mask[r] = (1ULL << ROW_PIN[r]);
    }

    // Column masks based on current pattern
    // Reversed polarity: LOW = ON, HIGH = OFF
    all_col_mask = 0;
    col_on_mask = 0;
    col_off_mask = 0;
    for (int c = 0; c < PANEL_SIZE; c++) {
        uint64_t pin_mask = (1ULL << COL_PIN[c]);
        all_col_mask |= pin_mask;
        if (col_pattern & (1UL << c)) {
            col_on_mask |= pin_mask;   // this column should be ON (driven LOW)
        } else {
            col_off_mask |= pin_mask;  // this column should be OFF (driven HIGH)
        }
    }
}

// ---------------------------------------------------------------------------
// PIO column driver program (Phase 3b)
// ---------------------------------------------------------------------------
// Protocol:
//   1. Push all-OFF mask (0xFFFFF) once at start → stored in Y register
//   2. Per row: push column pattern (pre-inverted), push delay count
//   3. PIO sets columns, delays, clears columns, signals IRQ
//
// Pin mapping: OUT pins base=GP1, count=20
// Timing: ON time = (delay_count + 5) PIO cycles from column set to clear
//
// Assembly:
//   addr 0: pull block           ; get all-OFF mask (0xFFFFF)
//   addr 1: mov y, osr           ; y = permanent all-OFF value
//   addr 2: pull block           ; [wrap_target] get column pattern
//   addr 3: out pins, 20         ; set all 20 column pins → LEDs ON
//   addr 4: pull block           ; get delay count
//   addr 5: mov x, osr           ; x = delay count
//   addr 6: jmp x--, 6           ; delay loop (x+1 cycles)
//   addr 7: mov osr, y           ; restore all-OFF mask
//   addr 8: out pins, 20         ; all columns OFF (HIGH)
//   addr 9: irq wait 0           ; [wrap] signal CPU, stall until cleared
// ---------------------------------------------------------------------------
static const uint16_t led_col_program_insn[] = {
    0x80a0, // 0: pull block
    0xa047, // 1: mov y, osr
    0x80a0, // 2: pull block        [wrap target]
    0x6014, // 3: out pins, 20
    0x80a0, // 4: pull block
    0xa027, // 5: mov x, osr
    0x0046, // 6: jmp x--, 6
    0xa0e2, // 7: mov osr, y
    0x6014, // 8: out pins, 20
    0xc020, // 9: irq wait 0        [wrap]
};

#define LED_COL_WRAP_TARGET 2
#define LED_COL_WRAP        9

static const pio_program_t led_col_program = {
    .instructions = led_col_program_insn,
    .length = 10,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x03,   // pins in both 0-15 and 16-31 ranges
#endif
};

// PIO state
static PIO      pio_hw_inst  = nullptr;
static uint     pio_sm_idx   = 0;
static uint     pio_offset   = 0;
static bool     pio_loaded   = false;

static bool pio_init_program() {
    if (pio_loaded) return true;

    // Find a free SM and load the program
    bool ok = pio_claim_free_sm_and_add_program(
        &led_col_program, &pio_hw_inst, &pio_sm_idx, &pio_offset);
    if (!ok) {
        Serial.println("ERR: No free PIO SM available");
        return false;
    }

    // Configure the state machine
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, pio_offset + LED_COL_WRAP_TARGET,
                           pio_offset + LED_COL_WRAP);
    sm_config_set_out_pins(&c, COL_PIN[0], PANEL_SIZE);  // GP1, 20 pins
    sm_config_set_clkdiv(&c, 1.0f);                       // full speed 150 MHz
    sm_config_set_out_shift(&c, true, false, 32);          // shift right, manual pull

    pio_sm_init(pio_hw_inst, pio_sm_idx, pio_offset, &c);

    pio_loaded = true;
    Serial.print("PIO OK: pio");
    Serial.print(pio_get_index(pio_hw_inst));
    Serial.print(" sm");
    Serial.println(pio_sm_idx);
    return true;
}

// Switch column pins between SIO (regular GPIO) and PIO modes
static void col_pins_to_pio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        pio_gpio_init(pio_hw_inst, COL_PIN[c]);
    }
}

static void col_pins_to_sio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        gpio_set_function(COL_PIN[c], GPIO_FUNC_SIO);
        gpio_set_dir(COL_PIN[c], GPIO_OUT);
        gpio_put(COL_PIN[c], 1);  // OFF state (HIGH)
    }
}

// PIO scan overhead per row:
//   From "out pins, 20" (columns ON) to next "out pins, 20" (columns OFF):
//   pull(1) + mov(1) + jmp_loop(x+1) + mov(1) + out(1) = x + 5 cycles
//   So ON time = (delay_count + 5) PIO cycles
//   For target on_cycles: delay_count = on_cycles - 5 (min 0)
#define PIO_ON_OVERHEAD_CYCLES 5

static char     cmd_buf[64];
static uint8_t  cmd_len = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void update_timing() {
    on_cycles  = (uint32_t)(on_us * cycles_per_us);
    off_cycles = (uint32_t)(off_us * cycles_per_us);
    if (on_cycles == 0) on_cycles = 1;
    if (off_cycles == 0) off_cycles = 1;
}

static void update_led_pins() {
    active_row_pin = ROW_PIN[layout_to_sch_row[led_row_layout][led_col_layout]];
    active_col_pin = COL_PIN[layout_to_sch_col[led_row_layout][led_col_layout]];
}

static void all_off() {
    // All columns HIGH (OFF), all rows LOW (OFF)
    gpio_set_mask64(all_col_mask);
    for (int r = 0; r < PANEL_SIZE; r++) {
        gpio_clr_mask64(row_on_mask[r]);
    }
}

static void led_off() {
    gpio_put(active_col_pin, 1);
    gpio_put(active_row_pin, 0);
}

// ---------------------------------------------------------------------------
// Single-LED pulse functions (from Phase 1e)
// ---------------------------------------------------------------------------
static void __not_in_flash_func(run_n_pulses)(uint32_t n, int mode) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    // Warm-up pulse
    {
        if (mode == 0) noInterrupts();
        gpio_put(active_col_pin, 0);
        gpio_put(active_row_pin, 1);
        dwt_delay_cycles(on_cycles);
        gpio_put(active_col_pin, 1);
        gpio_put(active_row_pin, 0);
        dwt_delay_cycles(off_cycles);
        if (mode == 0) interrupts();
    }

    for (uint32_t i = 0; i < n; i++) {
        if (mode == 0) noInterrupts();

        uint32_t start_cyc = m33_hw->dwt_cyccnt;
        gpio_put(active_col_pin, 0);
        gpio_put(active_row_pin, 1);
        dwt_delay_cycles(on_cycles);
        uint32_t mid_cyc = m33_hw->dwt_cyccnt;
        gpio_put(active_col_pin, 1);
        gpio_put(active_row_pin, 0);
        dwt_delay_cycles(off_cycles);
        uint32_t end_cyc = m33_hw->dwt_cyccnt;

        if (mode == 0) interrupts();
        stats_update(mid_cyc - start_cyc, end_cyc - mid_cyc);

        if (mode == 1 && (i % 1000 == 0)) Serial.print(".");
    }
    if (mode == 1) Serial.println();
}

// ---------------------------------------------------------------------------
// Scan functions (Phase 3a) — all RAM-resident
// ---------------------------------------------------------------------------

// Measure row-switching overhead: column setup + row enable/disable, NO delay.
// Runs N iterations, each doing active_rows row transitions.
// Reports per-row overhead time.
static void __not_in_flash_func(run_rowtime)(uint32_t n_iters) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    // Warm-up
    noInterrupts();
    for (int r = 0; r < active_rows; r++) {
        gpio_set_mask64(col_off_mask);
        gpio_clr_mask64(col_on_mask);
        gpio_set_mask64(row_on_mask[r]);
        gpio_clr_mask64(row_on_mask[r]);
    }
    gpio_set_mask64(all_col_mask);
    interrupts();

    for (uint32_t i = 0; i < n_iters; i++) {
        noInterrupts();

        for (int r = 0; r < active_rows; r++) {
            uint32_t t0 = m33_hw->dwt_cyccnt;

            // Set column pattern
            gpio_set_mask64(col_off_mask);   // OFF columns HIGH
            gpio_clr_mask64(col_on_mask);    // ON columns LOW

            // Enable row
            gpio_set_mask64(row_on_mask[r]);

            // NO delay — measuring overhead only

            // Disable row
            gpio_clr_mask64(row_on_mask[r]);

            // All columns OFF
            gpio_set_mask64(all_col_mask);

            uint32_t t1 = m33_hw->dwt_cyccnt;

            stats_update(t1 - t0, 0);
        }

        interrupts();
    }
}

// Full-frame scan: scan active_rows rows with on_cycles delay per row.
// Runs up to N frames (interruptible — stops early if serial input arrives).
// stats "on" field = frame time, "off" field = per-row average
static void __not_in_flash_func(run_scan_frames)(uint32_t n_frames) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    // Warm-up frame
    noInterrupts();
    for (int r = 0; r < active_rows; r++) {
        gpio_set_mask64(col_off_mask);
        gpio_clr_mask64(col_on_mask);
        gpio_set_mask64(row_on_mask[r]);
        dwt_delay_cycles(on_cycles);
        gpio_clr_mask64(row_on_mask[r]);
        gpio_set_mask64(all_col_mask);
    }
    interrupts();

    for (uint32_t f = 0; f < n_frames; f++) {
        noInterrupts();
        uint32_t frame_start = m33_hw->dwt_cyccnt;

        for (int r = 0; r < active_rows; r++) {
            // Set column pattern for this row
            gpio_set_mask64(col_off_mask);   // OFF columns HIGH
            gpio_clr_mask64(col_on_mask);    // ON columns LOW

            // Enable row
            gpio_set_mask64(row_on_mask[r]);

            // Hold for on_cycles
            dwt_delay_cycles(on_cycles);

            // Disable row
            gpio_clr_mask64(row_on_mask[r]);

            // All columns OFF between rows
            gpio_set_mask64(all_col_mask);
        }

        uint32_t frame_end = m33_hw->dwt_cyccnt;
        interrupts();

        uint32_t frame_cyc = frame_end - frame_start;
        uint32_t row_avg_cyc = frame_cyc / active_rows;
        stats_update(frame_cyc, row_avg_cyc);

        // Check for serial input every 100 frames — allows early stop
        if ((f % 100 == 99) && Serial.available()) {
            Serial.print("  (interrupted at frame ");
            Serial.print(f + 1);
            Serial.println(")");
            break;
        }
    }

    // Ensure all off after scan
    all_off();
}

// ---------------------------------------------------------------------------
// PIO scan functions (Phase 3b) — all RAM-resident
// ---------------------------------------------------------------------------

// PIO row overhead: column setup + row enable/disable, minimum PIO delay.
static void __not_in_flash_func(run_pio_rowtime)(uint32_t n_iters) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    // Pre-invert column pattern for PIO (1=HIGH=OFF, 0=LOW=ON)
    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;

    // Reset and enable PIO SM
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Push all-OFF mask for Y register init
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Warm-up iteration
    for (int r = 0; r < active_rows; r++) {
        gpio_set_mask64(row_on_mask[r]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0);  // min delay
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
        gpio_clr_mask64(row_on_mask[r]);
    }

    for (uint32_t i = 0; i < n_iters; i++) {
        for (int r = 0; r < active_rows; r++) {
            uint32_t t0 = m33_hw->dwt_cyccnt;

            gpio_set_mask64(row_on_mask[r]);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0);  // min delay
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
            gpio_clr_mask64(row_on_mask[r]);

            uint32_t t1 = m33_hw->dwt_cyccnt;
            stats_update(t1 - t0, 0);
        }
    }

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
}

// PIO full-frame scan with per-frame timing measurement.
// mode: 0 = with noInterrupts (for fair comparison), 1 = without (show PIO independence)
static void __not_in_flash_func(run_pio_scan_frames)(uint32_t n_frames, int mode) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;

    // Reset and enable PIO SM
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Push all-OFF mask for Y register init
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Warm-up frame
    for (int r = 0; r < active_rows; r++) {
        gpio_set_mask64(row_on_mask[r]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_delay);
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
        gpio_clr_mask64(row_on_mask[r]);
    }

    for (uint32_t f = 0; f < n_frames; f++) {
        if (mode == 0) noInterrupts();
        uint32_t frame_start = m33_hw->dwt_cyccnt;

        for (int r = 0; r < active_rows; r++) {
            gpio_set_mask64(row_on_mask[r]);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_delay);
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
            gpio_clr_mask64(row_on_mask[r]);
        }

        uint32_t frame_end = m33_hw->dwt_cyccnt;
        if (mode == 0) interrupts();

        uint32_t frame_cyc = frame_end - frame_start;
        uint32_t row_avg_cyc = frame_cyc / active_rows;
        stats_update(frame_cyc, row_avg_cyc);

        // Check for serial input every 100 frames — allows early stop
        if ((f % 100 == 99) && Serial.available()) {
            Serial.print("  (interrupted at frame ");
            Serial.print(f + 1);
            Serial.println(")");
            break;
        }
    }

    // Ensure all off after scan
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------
static void cmd_help() {
    Serial.println("--- Stage 3b: PIO + CPU Row Scanning ---");
    Serial.println("== Single-LED (Phase 1) ==");
    Serial.println("ON <us>          Set on-time  (0.01-1000, float)");
    Serial.println("OFF <us>         Set off-time (0.01-10000, float)");
    Serial.println("POS <row> <col>  Set LED position (0-19)");
    Serial.println("RUN              Start single-LED pulsing");
    Serial.println("STOP             Stop pulsing");
    Serial.println("STATS            Print & reset timing stats");
    Serial.println("JITTER <N>       Compare protected/unprotected");
    Serial.println("SWEEP <s> <e> <step> <N>  Sweep on-time (CSV)");
    Serial.println("SERIAL <N>       3-phase serial impact test");
    Serial.println("== CPU Row Scanning (Phase 3a) ==");
    Serial.println("ROWS <N>         Set active row count (1-20)");
    Serial.println("PATTERN <type>   ALL, NONE, CHECK, or hex (e.g. FFFFF)");
    Serial.println("SCAN <N_frames>  CPU scan, measure per-frame time");
    Serial.println("ROWTIME <N>      CPU row-switch overhead (no delay)");
    Serial.println("== PIO Row Scanning (Phase 3b) ==");
    Serial.println("PIOSCAN <N>      PIO scan (protected mode)");
    Serial.println("PIOSCAN2 <N>     PIO scan (NO noInterrupts)");
    Serial.println("PIOROWTIME <N>   PIO row-switch overhead (no delay)");
    Serial.println("HELP             This message");
}

static void cmd_set_on(const char* arg) {
    float val = atof(arg);
    if (val < 0.01f || val > 1000.0f) {
        Serial.println("ERR: ON range 0.01-1000 us");
        return;
    }
    on_us = val;
    update_timing();
    stats_reset();
    Serial.print("ON=");
    Serial.print(on_us, 3);
    Serial.print("us (");
    Serial.print(on_cycles);
    Serial.println(" cycles)");
}

static void cmd_set_off(const char* arg) {
    float val = atof(arg);
    if (val < 0.01f || val > 10000.0f) {
        Serial.println("ERR: OFF range 0.01-10000 us");
        return;
    }
    off_us = val;
    update_timing();
    stats_reset();
    Serial.print("OFF=");
    Serial.print(off_us, 3);
    Serial.print("us (");
    Serial.print(off_cycles);
    Serial.println(" cycles)");
}

static void cmd_set_pos(const char* args) {
    int r, c;
    if (sscanf(args, "%d %d", &r, &c) != 2 ||
        r < 0 || r >= PANEL_SIZE || c < 0 || c >= PANEL_SIZE) {
        Serial.println("ERR: POS <row 0-19> <col 0-19>");
        return;
    }
    led_off();
    led_row_layout = (uint8_t)r;
    led_col_layout = (uint8_t)c;
    update_led_pins();
    stats_reset();
    count = 0;
    Serial.print("POS=(");
    Serial.print(led_row_layout);
    Serial.print(",");
    Serial.print(led_col_layout);
    Serial.print(") row_pin=");
    Serial.print(active_row_pin);
    Serial.print(" col_pin=");
    Serial.println(active_col_pin);
}

static void cmd_stats() {
    stats_print("STATS");
    stats_reset();
}

static void cmd_jitter(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: JITTER <1-1000000>");
        return;
    }
    bool was_running = running;
    running = false;
    led_off();

    Serial.print("JITTER test: ");
    Serial.print(n);
    Serial.print(" pulses, ON=");
    Serial.print(on_us, 3);
    Serial.print("us OFF=");
    Serial.print(off_us, 3);
    Serial.println("us");

    Serial.println("Phase 1/3: Protected (noInterrupts)...");
    run_n_pulses(n, 0);
    stats_print("  PROTECTED");

    delay(50);
    Serial.println("Phase 2/3: Unprotected + serial...");
    run_n_pulses(n, 1);
    stats_print("  UNPROTECTED+SERIAL");

    delay(50);
    Serial.println("Phase 3/3: Unprotected, quiet...");
    run_n_pulses(n, 2);
    stats_print("  UNPROTECTED");

    Serial.println("JITTER test complete");
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_sweep(const char* args) {
    float s_start, s_end, s_step;
    unsigned int s_count;
    if (sscanf(args, "%f %f %f %u", &s_start, &s_end, &s_step, &s_count) != 4) {
        Serial.println("ERR: SWEEP <start_us> <end_us> <step_us> <count>");
        return;
    }
    if (s_start < 0.01f || s_end < s_start || s_step <= 0.0f ||
        s_count == 0 || s_count > 1000000) {
        Serial.println("ERR: Invalid sweep params");
        return;
    }

    bool was_running = running;
    running = false;
    led_off();

    Serial.println("--- SWEEP START ---");
    Serial.print("ON range: ");
    Serial.print(s_start, 3);
    Serial.print("-");
    Serial.print(s_end, 3);
    Serial.print("us  step=");
    Serial.print(s_step, 3);
    Serial.print("us  count=");
    Serial.print(s_count);
    Serial.print("  OFF=");
    Serial.print(off_us, 3);
    Serial.println("us");
    Serial.println("cmd_on_us,on_min_us,on_max_us,on_mean_us,off_min_us,off_max_us,off_mean_us,n");

    float saved_on_us = on_us;
    for (float t = s_start; t <= s_end + s_step * 0.01f; t += s_step) {
        on_us = t;
        update_timing();
        run_n_pulses(s_count, 0);
        stats_print_csv_row(t);
    }
    on_us = saved_on_us;
    update_timing();
    Serial.println("--- SWEEP END ---");
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_serial_test(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: SERIAL <1-1000000>");
        return;
    }
    bool was_running = running;
    running = false;
    led_off();

    Serial.print("SERIAL impact test: ");
    Serial.print(n);
    Serial.print(" pulses, ON=");
    Serial.print(on_us, 3);
    Serial.print("us OFF=");
    Serial.print(off_us, 3);
    Serial.println("us");

    Serial.println("Phase 1/3: Protected baseline...");
    run_n_pulses(n, 0);
    stats_print("  BASELINE");
    delay(50);
    Serial.println("Phase 2/3: Unprotected + serial...");
    run_n_pulses(n, 1);
    stats_print("  USB+SERIAL");
    delay(50);
    Serial.println("Phase 3/3: Unprotected, no serial...");
    run_n_pulses(n, 2);
    stats_print("  USB_ONLY");

    Serial.println("SERIAL test complete");
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_rows(const char* arg) {
    int val = atoi(arg);
    if (val < 1 || val > PANEL_SIZE) {
        Serial.println("ERR: ROWS 1-20");
        return;
    }
    active_rows = (uint8_t)val;
    Serial.print("ROWS=");
    Serial.println(active_rows);
}

static void cmd_pattern(const char* arg) {
    // Skip leading spaces
    while (*arg == ' ') arg++;

    // Uppercase for comparison
    char upper[16];
    for (int i = 0; i < 15 && arg[i]; i++) {
        upper[i] = (arg[i] >= 'a' && arg[i] <= 'z') ? arg[i] - 32 : arg[i];
        upper[i+1] = '\0';
    }

    if (strcmp(upper, "ALL") == 0) {
        col_pattern = 0xFFFFF;
    } else if (strcmp(upper, "NONE") == 0) {
        col_pattern = 0x00000;
    } else if (strcmp(upper, "CHECK") == 0) {
        col_pattern = 0xAAAAA;  // alternating columns
    } else {
        // Try parsing as hex
        col_pattern = (uint32_t)strtoul(arg, nullptr, 16) & 0xFFFFF;
    }

    precompute_scan_masks();

    // Count set bits
    int n_on = 0;
    for (int i = 0; i < 20; i++) {
        if (col_pattern & (1UL << i)) n_on++;
    }
    Serial.print("PATTERN=0x");
    Serial.print(col_pattern, HEX);
    Serial.print(" (");
    Serial.print(n_on);
    Serial.println(" columns ON)");
}

static void cmd_scan(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: SCAN <1-1000000>");
        return;
    }

    bool was_running = running;
    running = false;
    all_off();

    Serial.println("--- SCAN START ---");
    Serial.print("Rows=");
    Serial.print(active_rows);
    Serial.print("  ON/row=");
    Serial.print(on_us, 3);
    Serial.print("us (");
    Serial.print(on_cycles);
    Serial.print(" cyc)  Pattern=0x");
    Serial.print(col_pattern, HEX);
    Serial.print("  Frames=");
    Serial.println(n);

    run_scan_frames(n);

    // Frame time stats
    stats_print_fields("  FRAME_TIME", "frame", "row_avg");

    // Compute and print frame rate
    if (stat_count > 0) {
        float mean_frame_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float frame_rate = 1000000.0f / mean_frame_us;
        Serial.print("  Frame rate: ");
        Serial.print(frame_rate, 1);
        Serial.print(" Hz (");
        Serial.print(mean_frame_us, 3);
        Serial.println("us/frame)");
    }

    Serial.println("--- SCAN END ---");
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_pioscan(const char* arg, int mode) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: PIOSCAN <1-1000000>");
        return;
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();

    // Switch column pins to PIO mode
    col_pins_to_pio();

    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;
    float pio_on_us = (float)(pio_delay + PIO_ON_OVERHEAD_CYCLES) / (float)cycles_per_us;

    Serial.println("--- PIOSCAN START ---");
    Serial.print("Rows=");
    Serial.print(active_rows);
    Serial.print("  ON/row=");
    Serial.print(on_us, 3);
    Serial.print("us cmd (");
    Serial.print(pio_on_us, 3);
    Serial.print("us PIO actual, delay=");
    Serial.print(pio_delay);
    Serial.print(")  Pattern=0x");
    Serial.print(col_pattern, HEX);
    Serial.print("  Frames=");
    Serial.print(n);
    Serial.print("  Mode=");
    Serial.println(mode == 0 ? "protected" : "unprotected");

    run_pio_scan_frames(n, mode);

    // Frame time stats
    stats_print_fields("  FRAME_TIME", "frame", "row_avg");

    if (stat_count > 0) {
        float mean_frame_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float frame_rate = 1000000.0f / mean_frame_us;
        Serial.print("  Frame rate: ");
        Serial.print(frame_rate, 1);
        Serial.print(" Hz (");
        Serial.print(mean_frame_us, 3);
        Serial.println("us/frame)");
    }

    Serial.println("--- PIOSCAN END ---");

    // Restore column pins to SIO mode
    col_pins_to_sio();
    precompute_scan_masks();  // re-init masks for CPU scan

    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_piorowtime(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 100000) {
        Serial.println("ERR: PIOROWTIME <1-100000>");
        return;
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();

    col_pins_to_pio();

    Serial.println("--- PIOROWTIME START ---");
    Serial.print("Rows=");
    Serial.print(active_rows);
    Serial.print("  Pattern=0x");
    Serial.print(col_pattern, HEX);
    Serial.print("  Iterations=");
    Serial.println(n);
    Serial.println("(Measuring PIO row-switch overhead, min delay)");

    run_pio_rowtime(n);

    stats_print_fields("  PIO_ROW_OVERHEAD", "per_row", nullptr);

    if (stat_count > 0) {
        float mean_row_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float est_frame_overhead = mean_row_us * active_rows;
        Serial.print("  Est frame overhead (");
        Serial.print(active_rows);
        Serial.print(" rows): ");
        Serial.print(est_frame_overhead, 3);
        Serial.println("us");
        Serial.print("  At 8kHz (125us/frame): ");
        Serial.print(125.0f - est_frame_overhead, 3);
        Serial.println("us available for LED ON time");
    }

    Serial.println("--- PIOROWTIME END ---");

    col_pins_to_sio();
    precompute_scan_masks();

    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_rowtime(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 100000) {
        Serial.println("ERR: ROWTIME <1-100000>");
        return;
    }

    bool was_running = running;
    running = false;
    all_off();

    Serial.println("--- ROWTIME START ---");
    Serial.print("Rows=");
    Serial.print(active_rows);
    Serial.print("  Pattern=0x");
    Serial.print(col_pattern, HEX);
    Serial.print("  Iterations=");
    Serial.println(n);
    Serial.println("(Measuring row-switch overhead only, no delay)");

    run_rowtime(n);

    stats_print_fields("  ROW_OVERHEAD", "per_row", nullptr);

    if (stat_count > 0) {
        float mean_row_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float est_frame_overhead = mean_row_us * active_rows;
        Serial.print("  Est frame overhead (");
        Serial.print(active_rows);
        Serial.print(" rows): ");
        Serial.print(est_frame_overhead, 3);
        Serial.println("us");
        Serial.print("  At 8kHz (125us/frame): ");
        Serial.print(125.0f - est_frame_overhead, 3);
        Serial.println("us available for LED ON time");
    }

    Serial.println("--- ROWTIME END ---");
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

// ---------------------------------------------------------------------------
// Serial parser
// ---------------------------------------------------------------------------
static void process_command() {
    cmd_buf[cmd_len] = '\0';
    while (cmd_len > 0 && (cmd_buf[cmd_len-1] == ' ' || cmd_buf[cmd_len-1] == '\r')) {
        cmd_buf[--cmd_len] = '\0';
    }
    if (cmd_len == 0) return;

    char* space = strchr(cmd_buf, ' ');
    char* args = nullptr;
    if (space) {
        *space = '\0';
        args = space + 1;
        while (*args == ' ') args++;
    }

    for (char* p = cmd_buf; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }

    if (strcmp(cmd_buf, "HELP") == 0) {
        cmd_help();
    } else if (strcmp(cmd_buf, "ON") == 0 && args) {
        cmd_set_on(args);
    } else if (strcmp(cmd_buf, "OFF") == 0 && args) {
        cmd_set_off(args);
    } else if (strcmp(cmd_buf, "POS") == 0 && args) {
        cmd_set_pos(args);
    } else if (strcmp(cmd_buf, "RUN") == 0) {
        running = true;
        stats_reset();
        count = 0;
        Serial.println("RUNNING");
    } else if (strcmp(cmd_buf, "STOP") == 0) {
        running = false;
        led_off();
        all_off();
        Serial.println("STOPPED");
    } else if (strcmp(cmd_buf, "STATS") == 0) {
        cmd_stats();
    } else if (strcmp(cmd_buf, "JITTER") == 0 && args) {
        cmd_jitter(args);
    } else if (strcmp(cmd_buf, "SWEEP") == 0 && args) {
        cmd_sweep(args);
    } else if (strcmp(cmd_buf, "SERIAL") == 0 && args) {
        cmd_serial_test(args);
    } else if (strcmp(cmd_buf, "ROWS") == 0 && args) {
        cmd_rows(args);
    } else if (strcmp(cmd_buf, "PATTERN") == 0 && args) {
        cmd_pattern(args);
    } else if (strcmp(cmd_buf, "SCAN") == 0 && args) {
        cmd_scan(args);
    } else if (strcmp(cmd_buf, "ROWTIME") == 0 && args) {
        cmd_rowtime(args);
    } else if (strcmp(cmd_buf, "PIOSCAN") == 0 && args) {
        cmd_pioscan(args, 0);   // protected mode
    } else if (strcmp(cmd_buf, "PIOSCAN2") == 0 && args) {
        cmd_pioscan(args, 1);   // unprotected mode
    } else if (strcmp(cmd_buf, "PIOROWTIME") == 0 && args) {
        cmd_piorowtime(args);
    } else {
        Serial.print("ERR: Unknown '");
        Serial.print(cmd_buf);
        Serial.println("' — type HELP");
    }
}

static void poll_serial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cmd_len > 0) {
                process_command();
                cmd_len = 0;
            }
        } else if (cmd_len < sizeof(cmd_buf) - 1) {
            cmd_buf[cmd_len++] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Arduino setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("BOOT OK — stage 3b (PIO + CPU row scanning)");
    Serial.print("F_CPU=");
    Serial.print(F_CPU / 1000000);
    Serial.println(" MHz");

    dwt_init();
    init_index_maps();
    Serial.println("MAPS OK");

    // Initialize all GPIO pins
    uint64_t COL_PIN_mask = 0;
    for (size_t i = 0; i < PANEL_SIZE; i++) {
        gpio_init(COL_PIN[i]);
        COL_PIN_mask |= (uint64_t(1) << COL_PIN[i]);
    }
    gpio_set_dir_out_masked64(COL_PIN_mask);
    gpio_set_mask64(COL_PIN_mask);  // all columns HIGH (OFF)

    uint64_t ROW_PIN_mask = 0;
    for (size_t i = 0; i < PANEL_SIZE; i++) {
        gpio_init(ROW_PIN[i]);
        ROW_PIN_mask |= (uint64_t(1) << ROW_PIN[i]);
    }
    gpio_set_dir_out_masked64(ROW_PIN_mask);
    gpio_clr_mask64(ROW_PIN_mask);  // all rows LOW (OFF)
    Serial.println("GPIO OK");

    // Pre-compute masks
    precompute_scan_masks();

    update_led_pins();
    update_timing();
    stats_reset();

    Serial.print("LED(0,0) row_pin=");
    Serial.print(active_row_pin);
    Serial.print(" col_pin=");
    Serial.println(active_col_pin);
    Serial.print("ON=");
    Serial.print(on_us, 3);
    Serial.print("us (");
    Serial.print(on_cycles);
    Serial.print(" cyc)  OFF=");
    Serial.print(off_us, 3);
    Serial.print("us (");
    Serial.print(off_cycles);
    Serial.println(" cyc)");
    Serial.print("Scan: rows=");
    Serial.print(active_rows);
    Serial.print("  pattern=0x");
    Serial.println(col_pattern, HEX);
    Serial.println("Type HELP for commands");

    last_heartbeat_us = micros();
}

// ---------------------------------------------------------------------------
// Arduino loop — single-LED pulsing (Phase 1 default behavior)
// ---------------------------------------------------------------------------
void loop() {

    if (running) {
        uint32_t start_cyc, mid_cyc, end_cyc;

        noInterrupts();

        start_cyc = m33_hw->dwt_cyccnt;
        gpio_put(active_col_pin, 0);
        gpio_put(active_row_pin, 1);

        if (dwt_available) {
            dwt_delay_cycles(on_cycles);
        } else {
            delayMicroseconds((uint32_t)on_us);
        }

        mid_cyc = m33_hw->dwt_cyccnt;
        gpio_put(active_col_pin, 1);
        gpio_put(active_row_pin, 0);

        if (dwt_available) {
            dwt_delay_cycles(off_cycles);
        } else {
            delayMicroseconds((uint32_t)off_us);
        }

        end_cyc = m33_hw->dwt_cyccnt;
        interrupts();

        if (dwt_available) {
            stats_update(mid_cyc - start_cyc, end_cyc - mid_cyc);
        }
        count++;
    }

    poll_serial();

    uint32_t now_us = micros();
    if (running && (now_us - last_heartbeat_us >= 1000000)) {
        Serial.print("count: ");
        Serial.print(count);
        Serial.print("  ON=");
        Serial.print(on_us, 3);
        Serial.print("us OFF=");
        Serial.print(off_us, 3);
        Serial.println("us");
        last_heartbeat_us = now_us;
    }
}
