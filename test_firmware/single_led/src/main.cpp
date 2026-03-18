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
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <pico/multicore.h>
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

static void __attribute__((noinline)) __not_in_flash_func(dwt_delay_cycles)(uint32_t cycles) {
    uint32_t start = m33_hw->dwt_cyccnt;
    while ((m33_hw->dwt_cyccnt - start) < cycles) { /* spin */ }
}

static inline float cycles_to_us(uint32_t cycles) {
    return (float)cycles / (float)cycles_per_us;
}

// Check if user wants to stop — only triggers on newline, drains other data
static inline bool user_wants_stop() {
    if (!Serial.available()) return false;
    bool stop = false;
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') stop = true;
    }
    return stop;
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
static uint32_t stat_outlier_count = 0;     // events > 2x nominal
static uint32_t stat_outlier_threshold = 0; // set before measurement

static void stats_reset() {
    stat_on_min  = UINT32_MAX;
    stat_on_max  = 0;
    stat_on_sum  = 0;
    stat_off_min = UINT32_MAX;
    stat_off_max = 0;
    stat_off_sum = 0;
    stat_count   = 0;
    stat_outlier_count = 0;
    // Note: stat_outlier_threshold is set by the caller before measurement
}

static void __attribute__((noinline)) __not_in_flash_func(stats_update)(uint32_t on_cyc, uint32_t off_cyc) {
    if (on_cyc < stat_on_min) stat_on_min = on_cyc;
    if (on_cyc > stat_on_max) stat_on_max = on_cyc;
    stat_on_sum += on_cyc;
    if (off_cyc < stat_off_min) stat_off_min = off_cyc;
    if (off_cyc > stat_off_max) stat_off_max = off_cyc;
    stat_off_sum += off_cyc;
    stat_count++;
    if (stat_outlier_threshold > 0 && on_cyc > stat_outlier_threshold)
        stat_outlier_count++;
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
    float jitter = cycles_to_us(stat_on_max) - cycles_to_us(stat_on_min);
    Serial.print("  Jitter (max-min): ");
    Serial.print(jitter, 3);
    Serial.println("us");
    if (stat_outlier_threshold > 0) {
        Serial.print("  Outliers (>2x nominal): ");
        Serial.print(stat_outlier_count);
        Serial.print(" / ");
        Serial.print(stat_count);
        Serial.print(" (");
        Serial.print(stat_count > 0 ? 100.0f * stat_outlier_count / stat_count : 0.0f, 2);
        Serial.println("%)");
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

// BCM state (Phase 4)
static uint8_t  bcm_bits = 4;                            // N-bit BCM (3-8)
static float    bcm_base_on_us = 0.5f;                   // base time unit T (µs)
static uint8_t  pixel_data[PANEL_SIZE][PANEL_SIZE];       // intensity per pixel
static uint32_t bcm_plane_data[PANEL_SIZE][8][2];         // [row][bit] = {pio_pattern, pio_delay}

// RAMBURST state (Phase 5 — production simulation)
// Test frames stored in PSRAM (not SRAM!) to avoid disrupting SRAM layout
// of timing-critical code. SRAM layout sensitivity causes jitter regression.
#define MAX_TEST_FRAMES 16
static uint8_t  (*test_frames)[PANEL_SIZE][PANEL_SIZE] = nullptr;  // allocated in PSRAM
static uint8_t  n_test_frames = 0;
static uint8_t  current_frame_idx = 0;

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

// PIO ON overhead: the PIO column program takes 5 cycles of overhead per row
// (pull+mov+jmp_loop_entry+mov+out). ON time = (delay_count + 5) PIO cycles.
#define PIO_ON_OVERHEAD_CYCLES 5

// Pre-compute BCM bit-plane data from pixel_data[][]
// For each schematic row and bit-plane: compute PIO column pattern and delay.
// Must index by SCHEMATIC row (not layout row) because row_on_mask[r] activates
// schematic row r. Each layout position (lr,lc) belongs to schematic row
// layout_to_sch_row[lr][lc] and schematic column layout_to_sch_col[lr][lc].
static void precompute_bcm_data() {
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);

    // Precompute delays (same for all rows; vary per bit-plane only)
    uint32_t pio_delays[8];
    for (int b = 0; b < bcm_bits; b++) {
        uint32_t on_cycles_b = base_cycles * (1U << b);
        pio_delays[b] = (on_cycles_b > PIO_ON_OVERHEAD_CYCLES)
                      ? (on_cycles_b - PIO_ON_OVERHEAD_CYCLES) : 0;
    }

    // Start with all columns OFF (HIGH = 0xFFFFF in PIO inverted encoding)
    for (int r = 0; r < PANEL_SIZE; r++) {
        for (int b = 0; b < bcm_bits; b++) {
            bcm_plane_data[r][b][0] = 0xFFFFF;  // all columns HIGH = all OFF
            bcm_plane_data[r][b][1] = pio_delays[b];
        }
    }

    // For each layout pixel, OR its bit-plane contribution into the correct
    // schematic row's column pattern
    for (int lr = 0; lr < PANEL_SIZE; lr++) {
        for (int lc = 0; lc < PANEL_SIZE; lc++) {
            uint8_t sch_row = layout_to_sch_row[lr][lc];
            uint8_t sch_col = layout_to_sch_col[lr][lc];
            uint8_t intensity = pixel_data[lr][lc];
            for (int b = 0; b < bcm_bits; b++) {
                if (intensity & (1 << b)) {
                    // Clear this column's bit → drive LOW → LED ON
                    bcm_plane_data[sch_row][b][0] &= ~(1UL << sch_col);
                }
            }
        }
    }
}

// Incremental precompute: compute BCM plane data for a single schematic row.
// Call once per idle window → spread ~173 µs across 20 windows = ~8.65 µs each.
static void precompute_bcm_row(uint8_t sch_row) {
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);

    // Init this row's planes to all-OFF
    for (int b = 0; b < bcm_bits; b++) {
        uint32_t on_cycles_b = base_cycles * (1U << b);
        bcm_plane_data[sch_row][b][0] = 0xFFFFF;
        bcm_plane_data[sch_row][b][1] = (on_cycles_b > PIO_ON_OVERHEAD_CYCLES)
                                       ? (on_cycles_b - PIO_ON_OVERHEAD_CYCLES) : 0;
    }

    // Scan all layout pixels that map to this schematic row
    for (int lr = 0; lr < PANEL_SIZE; lr++) {
        for (int lc = 0; lc < PANEL_SIZE; lc++) {
            if (layout_to_sch_row[lr][lc] != sch_row) continue;
            uint8_t sch_col = layout_to_sch_col[lr][lc];
            uint8_t intensity = pixel_data[lr][lc];
            for (int b = 0; b < bcm_bits; b++) {
                if (intensity & (1 << b)) {
                    bcm_plane_data[sch_row][b][0] &= ~(1UL << sch_col);
                }
            }
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
//   addr 9: irq wait 0           ; [wrap] signal + stall until CPU/ISR clears flag
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
    0xc020, // 9: irq wait 0          [wrap]  (blocks PIO until ISR/CPU clears flag)
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

// PIO state (Phase 3b/3c — single SM column driver)
static PIO      pio_hw_inst  = nullptr;
static uint     pio_sm_idx   = 0;
static uint     pio_offset   = 0;
static bool     pio_loaded   = false;

// Forward declarations for resource management (cross-phase cleanup)
static void release_phase3bc_resources();
static void release_msm_resources();

static bool pio_init_program() {
    if (pio_loaded) return true;

    // Release multi-SM resources if they were claimed
    release_msm_resources();

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

    // Set column pins as outputs for the PIO SM
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);

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

// ---------------------------------------------------------------------------
// Hybrid DMA + ISR scan infrastructure (Phase 3c)
// ---------------------------------------------------------------------------
// Architecture: DMA feeds column data to PIO TX FIFO. PIO drives columns
// (out pins, 20), runs a hardware delay loop, turns columns OFF, then fires
// `irq wait 0`. A high-priority NVIC ISR switches rows via batch GPIO and
// clears the PIO IRQ, releasing PIO to pull the next DMA-fed column data.
//
// Why not pure DMA for rows? SIO GPIO registers (0xD0000000+) are per-core
// peripherals and silently ignore DMA writes. See RESULTS.md Phase 3c.
//
// Per-row overhead = NVIC entry (~12 cycles) + ISR body (~10 cycles) +
// NVIC exit (~12 cycles) ≈ 0.15-0.25 µs. CPU is free between ISR calls.
// ---------------------------------------------------------------------------

// Per-row column data for DMA: {col_pattern, delay} pairs
static uint32_t dma_col_data[PANEL_SIZE][2];

// Single DMA channel for column feed
static int  ch_col = -1;
static bool dma_channel_claimed = false;

// ISR state (volatile — shared between ISR and main loop)
static volatile int  isr_current_row;
static volatile int  isr_active_rows;
static volatile bool isr_frame_done;
static PIO           isr_pio_inst;  // set once before enabling IRQ

static void all_off();  // forward declaration

// Precompute DMA column data buffer from current scan parameters.
static void precompute_dma_col_data() {
    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;
    for (int r = 0; r < active_rows; r++) {
        dma_col_data[r][0] = pio_col_word;
        dma_col_data[r][1] = pio_delay;
    }
}

// Claim 1 DMA channel
static bool dma_init_channel() {
    if (dma_channel_claimed) return true;
    ch_col = dma_claim_unused_channel(false);
    if (ch_col < 0) {
        Serial.println("ERR: Cannot claim DMA channel");
        return false;
    }
    dma_channel_claimed = true;
    Serial.print("DMA channel: ");
    Serial.println(ch_col);
    return true;
}

// PIO IRQ handler — switches rows between PIO-driven column scans.
// Runs from RAM (no flash latency). Called when PIO executes `irq wait 0`
// after each row's delay completes (columns already OFF).
// Order matters: row switch BEFORE clearing PIO IRQ, because clearing the
// IRQ releases PIO which immediately pulls column data and drives columns ON.
static void __attribute__((noinline)) __not_in_flash_func(pio_row_isr)() {
    // Clear previous row (columns are already OFF from PIO)
    gpio_clr_mask64(row_on_mask[isr_current_row]);

    isr_current_row++;
    if (isr_current_row >= isr_active_rows) {
        // Frame complete — release PIO (it stalls at pull block, FIFO empty)
        pio_interrupt_clear(isr_pio_inst, 0);
        isr_frame_done = true;
        return;
    }

    // Set next row BEFORE releasing PIO
    gpio_set_mask64(row_on_mask[isr_current_row]);

    // Release PIO — resumes from irq wait, wraps to pull block,
    // gets DMA-fed column data, drives columns ON on the new row
    pio_interrupt_clear(isr_pio_inst, 0);
}

// Enable PIO IRQ routing to NVIC
static void hybrid_irq_enable() {
    isr_pio_inst = pio_hw_inst;
    uint pio_idx = pio_get_index(pio_hw_inst);
    uint nvic_irq = PIO0_IRQ_0 + pio_idx * 2;

    pio_set_irq0_source_enabled(pio_hw_inst, pis_interrupt0, true);
    irq_set_exclusive_handler(nvic_irq, pio_row_isr);
    irq_set_priority(nvic_irq, 0);  // highest priority — minimize row-switch latency
    irq_set_enabled(nvic_irq, true);
}

// Disable PIO IRQ routing
static void hybrid_irq_disable() {
    uint pio_idx = pio_get_index(pio_hw_inst);
    uint nvic_irq = PIO0_IRQ_0 + pio_idx * 2;
    irq_set_enabled(nvic_irq, false);
    pio_set_irq0_source_enabled(pio_hw_inst, pis_interrupt0, false);
    pio_interrupt_clear(pio_hw_inst, 0);
    irq_remove_handler(nvic_irq, pio_row_isr);
}

// Debug: step-by-step hybrid scan test
static void hybrid_debug_test() {
    if (!pio_loaded && !pio_init_program()) return;
    if (!dma_channel_claimed && !dma_init_channel()) return;

    precompute_scan_masks();
    precompute_dma_col_data();

    Serial.println("=== DMATEST: Hybrid DMA+ISR debug ===");

    // Step 1: PIO irq wait 0 test (CPU push, 1 row)
    Serial.println("Step 1: PIO irq wait 0 test (CPU push, 1 row)");
    col_pins_to_pio();

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);  // init Y
    gpio_set_mask64(row_on_mask[0]);

    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0);  // min delay

    uint32_t t0 = micros();
    while (!pio_interrupt_get(pio_hw_inst, 0)) {
        if (micros() - t0 > 1000) {
            Serial.println("  TIMEOUT waiting for PIO IRQ");
            break;
        }
    }
    if (pio_interrupt_get(pio_hw_inst, 0)) {
        Serial.println("  PIO irq wait 0: OK");
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);

    // Step 2: DMA column feed test (DMA pushes data, CPU polls IRQ)
    Serial.println("Step 2: DMA column feed test (1 row)");
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);  // init Y
    gpio_set_mask64(row_on_mask[0]);

    dma_channel_config cfg = dma_channel_get_default_config(ch_col);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio_hw_inst, pio_sm_idx, true));
    channel_config_set_chain_to(&cfg, ch_col);
    dma_channel_configure(ch_col, &cfg,
        &pio_hw_inst->txf[pio_sm_idx],
        &dma_col_data[0][0],
        2, false);  // 1 row = 2 words

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    dma_channel_start(ch_col);

    t0 = micros();
    while (!pio_interrupt_get(pio_hw_inst, 0)) {
        if (micros() - t0 > 1000) {
            Serial.println("  TIMEOUT waiting for PIO IRQ (DMA feed)");
            break;
        }
    }
    if (pio_interrupt_get(pio_hw_inst, 0)) {
        Serial.println("  DMA -> PIO -> IRQ: OK");
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    dma_channel_abort(ch_col);

    // Step 3: Full hybrid frame (DMA + ISR, all rows)
    Serial.println("Step 3: Full hybrid frame (DMA + ISR, all rows)");
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    isr_current_row = 0;
    isr_active_rows = active_rows;
    isr_frame_done = false;

    gpio_set_mask64(row_on_mask[0]);

    dma_channel_configure(ch_col, &cfg,
        &pio_hw_inst->txf[pio_sm_idx],
        &dma_col_data[0][0],
        active_rows * 2, false);

    hybrid_irq_enable();
    dma_channel_start(ch_col);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    t0 = micros();
    while (!isr_frame_done) {
        if (micros() - t0 > 100000) {
            Serial.print("  TIMEOUT! isr_current_row=");
            Serial.print(isr_current_row);
            Serial.print(" frame_done=");
            Serial.println(isr_frame_done);
            break;
        }
    }
    uint32_t elapsed = micros() - t0;

    hybrid_irq_disable();
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    dma_channel_abort(ch_col);

    if (isr_frame_done) {
        Serial.print("  Full frame OK! Rows=");
        Serial.print(active_rows);
        Serial.print(" elapsed~");
        Serial.print(elapsed);
        Serial.println("us");
    }

    col_pins_to_sio();
    all_off();
    Serial.println("=== DMATEST DONE ===");
}

// Run a single hybrid DMA+ISR scan frame. Returns frame time in DWT cycles.
static uint32_t __not_in_flash_func(run_hybrid_single_frame)() {
    // Reset PIO
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);

    // Init PIO Y register (all-OFF mask)
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Reset ISR state
    isr_current_row = 0;
    isr_active_rows = active_rows;
    isr_frame_done = false;

    // Reset DMA
    dma_channel_set_read_addr(ch_col, &dma_col_data[0][0], false);
    dma_channel_set_trans_count(ch_col, active_rows * 2, false);

    // Set first row (ISR handles subsequent rows)
    gpio_set_mask64(row_on_mask[0]);

    uint32_t t0 = m33_hw->dwt_cyccnt;

    // Start DMA and PIO
    dma_channel_start(ch_col);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Wait for ISR to signal frame complete
    while (!isr_frame_done) {
        tight_loop_contents();
    }

    uint32_t t1 = m33_hw->dwt_cyccnt;

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);

    return t1 - t0;
}

// Run N hybrid scan frames with timing statistics.
static void __attribute__((noinline)) __not_in_flash_func(run_hybrid_scan_frames)(uint32_t n_frames) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();
    precompute_dma_col_data();

    col_pins_to_pio();

    // Configure DMA channel (config reused across frames)
    dma_channel_config cfg = dma_channel_get_default_config(ch_col);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio_hw_inst, pio_sm_idx, true));
    channel_config_set_chain_to(&cfg, ch_col);
    dma_channel_configure(ch_col, &cfg,
        &pio_hw_inst->txf[pio_sm_idx],
        &dma_col_data[0][0],
        active_rows * 2,
        false);

    // Enable ISR for row switching
    hybrid_irq_enable();

    // Warm-up frame (discard timing)
    run_hybrid_single_frame();

    for (uint32_t f = 0; f < n_frames; f++) {
        uint32_t frame_cyc = run_hybrid_single_frame();
        uint32_t row_avg_cyc = frame_cyc / active_rows;
        stats_update(frame_cyc, row_avg_cyc);

        if ((f % 100 == 99) && user_wants_stop()) {
            Serial.print("  (interrupted at frame ");
            Serial.print(f + 1);
            Serial.println(")");
            break;
        }
    }

    hybrid_irq_disable();
    col_pins_to_sio();
    all_off();
}

// Release Phase 3b/3c PIO + DMA resources so multi-SM can claim them.
static void release_phase3bc_resources() {
    if (pio_loaded) {
        pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
        pio_remove_program_and_unclaim_sm(&led_col_program, pio_hw_inst, pio_sm_idx, pio_offset);
        pio_loaded = false;
        pio_hw_inst = nullptr;
    }
    if (dma_channel_claimed) {
        dma_channel_abort(ch_col);
        dma_channel_unclaim(ch_col);
        ch_col = -1;
        dma_channel_claimed = false;
    }
}

// ---------------------------------------------------------------------------
// Multi-SM PIO scan (Phase 3d) — cross-PIO with IRQ bridge
// ---------------------------------------------------------------------------
// GPIO range constraint forces SMs onto different PIO blocks:
//   Row SM on PIO1 (GPIOBASE=16, GPIO 16-47): drives GP21-44 via out pins, 24
//   Col SM on PIO0 (GPIOBASE=0,  GPIO  0-31): drives GP1-20  via out pins, 20
//
// Synchronization via IRQ bridge ISRs (~100ns per crossing):
//   Row SM: out pins → irq set 0 (fires PIO1 system IRQ) → wait 1 irq 1
//   Bridge ISR: PIO1 IRQ0 → forces flag 0 on PIO0 (col SM wakes)
//   Col SM: wait 1 irq 0 → columns ON → delay → columns OFF → irq set 1 (fires PIO0 system IRQ)
//   Bridge ISR: PIO0 IRQ0 → forces flag 1 on PIO1 (row SM wakes)
//
// GP32-35 (SPI/PSRAM) not pio_gpio_init'd — pin mux ignores PIO output.
// DMA feeds both SMs: CH_ROW pushes row patterns, CH_COL pushes {pattern, delay}.
// ---------------------------------------------------------------------------

// Row SM program (PIO1): 5 instructions
//   addr 0: pull block         ; get first row pattern from DMA
//   addr 1: out pins, 24       ; [wrap target] drive row pins (GP21-44)
//   addr 2: irq set 0          ; signal: row ready → fires PIO1 system IRQ
//   addr 3: wait 1 irq 1       ; wait for bridge ISR to force flag 1 (cols done)
//   addr 4: pull block          ; get next row pattern
//                                [wrap] → addr 1
static const uint16_t msm_row_program_insn[] = {
    0x80a0, //  0: pull block
    0x6018, //  1: out pins, 24       [wrap target]
    0xc000, //  2: irq set 0          (fires system IRQ)
    0x20c1, //  3: wait 1 irq 1       (auto-clear, set by bridge ISR)
    0x80a0, //  4: pull block
};

#define MSM_ROW_WRAP_TARGET 1
#define MSM_ROW_WRAP        4

static const pio_program_t msm_row_program = {
    .instructions = msm_row_program_insn,
    .length = 5,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x06,  // ranges 1+2: GPIO 16-47 (PIO1)
#endif
};

// Column SM program (PIO0): 11 instructions
//   addr 0: pull block         ; init: get all-OFF mask (0xFFFFF)
//   addr 1: mov y, osr         ; y = permanent all-OFF value
//   addr 2: wait 1 irq 0       ; [wrap target] wait for bridge ISR to force flag 0
//   addr 3: pull block          ; get column pattern from DMA
//   addr 4: out pins, 20        ; columns ON
//   addr 5: pull block          ; get delay count from DMA
//   addr 6: mov x, osr          ; x = delay
//   addr 7: jmp x--, 7          ; delay loop
//   addr 8: mov osr, y          ; restore all-OFF
//   addr 9: out pins, 20        ; columns OFF
//  addr 10: irq set 1           ; signal: cols done → fires PIO0 system IRQ
//                                [wrap] → addr 2
static const uint16_t msm_col_program_insn[] = {
    0x80a0, //  0: pull block
    0xa047, //  1: mov y, osr
    0x20c0, //  2: wait 1 irq 0       [wrap target]  (auto-clear, set by bridge ISR)
    0x80a0, //  3: pull block
    0x6014, //  4: out pins, 20
    0x80a0, //  5: pull block
    0xa027, //  6: mov x, osr
    0x0047, //  7: jmp x--, 7
    0xa0e2, //  8: mov osr, y
    0x6014, //  9: out pins, 20
    0xc001, // 10: irq set 1          (fires system IRQ)
};

#define MSM_COL_WRAP_TARGET 2
#define MSM_COL_WRAP       10

static const pio_program_t msm_col_program = {
    .instructions = msm_col_program_insn,
    .length = 11,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x03,  // ranges 0+1: GPIO 0-31 (PIO0 or PIO2)
#endif
};

// Multi-SM PIO state — separate PIO blocks
static PIO  msm_row_pio   = nullptr;  // PIO1 for rows
static PIO  msm_col_pio   = nullptr;  // PIO0 for columns
static uint msm_row_sm    = 0;
static uint msm_col_sm    = 0;
static uint msm_row_off   = 0;
static uint msm_col_off   = 0;
static bool msm_loaded    = false;

// DMA channels for multi-SM
static int  msm_ch_row    = -1;
static int  msm_ch_col    = -1;
static bool msm_dma_claimed = false;

// Release multi-SM resources so Phase 3b/3c can reclaim them.
static void release_msm_resources() {
    // Disable bridge ISRs first
    if (msm_loaded) {
        // Disable PIO1 IRQ0 (row→col bridge)
        pio_set_irq0_source_enabled(msm_row_pio, pis_interrupt0, false);
        irq_set_enabled(PIO1_IRQ_0, false);
        // Disable PIO0 IRQ0 (col→row bridge)
        pio_set_irq0_source_enabled(msm_col_pio, pis_interrupt1, false);
        irq_set_enabled(PIO0_IRQ_0, false);

        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
        pio_remove_program_and_unclaim_sm(&msm_row_program, msm_row_pio, msm_row_sm, msm_row_off);
        pio_remove_program_and_unclaim_sm(&msm_col_program, msm_col_pio, msm_col_sm, msm_col_off);
        msm_row_pio->gpiobase = 0;  // restore PIO1 default GPIOBASE
        msm_loaded = false;
        msm_row_pio = nullptr;
        msm_col_pio = nullptr;
    }
    if (msm_dma_claimed) {
        dma_channel_abort(msm_ch_row);
        dma_channel_abort(msm_ch_col);
        dma_channel_unclaim(msm_ch_row);
        dma_channel_unclaim(msm_ch_col);
        msm_ch_row = msm_ch_col = -1;
        msm_dma_claimed = false;
    }
}

// Row pattern buffer: 24-bit one-hot patterns + all-zero terminator
// Bit positions relative to GP21 (out_base for row SM on PIO1)
// Pin mapping: GP21=bit0 ... GP31=bit10, GP32-35=bits11-14 (always 0), GP36=bit15 ... GP44=bit23
static uint32_t msm_row_patterns[PANEL_SIZE + 1];

// Column SM ON time overhead: same as Phase 3b.
// pull(1) + mov(1) + jmp_loop(x+1) + mov(1) + out(1) = x + 5 cycles
#define MSM_COL_ON_OVERHEAD 5

// Precompute 24-bit one-hot row patterns.
static void msm_precompute_row_patterns() {
    for (int r = 0; r < active_rows; r++) {
        msm_row_patterns[r] = 1UL << (ROW_PIN[r] - 21);
    }
    msm_row_patterns[active_rows] = 0;  // all-OFF terminator
}

// --- IRQ bridge ISRs (RAM-resident for minimum latency) ---

// Completed-row counter (volatile, shared between ISR and main loop)
static volatile uint32_t msm_rows_done;

// PIO1 IRQ0 handler: row SM set flag 0 → wake col SM on PIO0
static void __attribute__((noinline)) __not_in_flash_func(msm_row_to_col_isr)() {
    pio_interrupt_clear(msm_row_pio, 0);       // clear PIO1 flag 0
    msm_col_pio->irq_force = (1u << 0);        // force PIO0 flag 0 → col SM wakes
}

// PIO0 IRQ0 handler: col SM set flag 1 → wake row SM on PIO1
static void __attribute__((noinline)) __not_in_flash_func(msm_col_to_row_isr)() {
    pio_interrupt_clear(msm_col_pio, 1);       // clear PIO0 flag 1
    msm_row_pio->irq_force = (1u << 1);        // force PIO1 flag 1 → row SM wakes
    msm_rows_done++;                            // count completed rows
}

// Initialize both PIO programs on SEPARATE PIO blocks.
static bool msm_init_programs() {
    if (msm_loaded) return true;

    // Release Phase 3b/3c resources to free PIO SMs and instruction memory
    release_phase3bc_resources();

    // Row program needs GPIO 16-47 → PIO1
    // Column program needs GPIO 0-31 → PIO0 or PIO2
    bool ok;

    // Load row program onto PIO1, with GPIOBASE=16 to reach GP21-44
    msm_row_pio = pio1;
    msm_row_pio->gpiobase = 16;  // shift PIO1's GPIO window to 16-47
    int row_sm = pio_claim_unused_sm(msm_row_pio, false);
    if (row_sm < 0) {
        Serial.println("ERR: No free SM on PIO1 for row program");
        return false;
    }
    msm_row_sm = (uint)row_sm;

    if (!pio_can_add_program(msm_row_pio, &msm_row_program)) {
        Serial.println("ERR: No instruction space on PIO1 for row program");
        Serial.println("  (PIO1 may be used by PSRAM/SPI driver)");
        pio_sm_unclaim(msm_row_pio, msm_row_sm);
        return false;
    }
    msm_row_off = pio_add_program(msm_row_pio, &msm_row_program);

    // Load column program onto PIO0 (or PIO2 if PIO0 unavailable)
    msm_col_pio = pio0;
    int col_sm = pio_claim_unused_sm(msm_col_pio, false);
    if (col_sm < 0) {
        // Try PIO2
        msm_col_pio = pio2;
        col_sm = pio_claim_unused_sm(msm_col_pio, false);
        if (col_sm < 0) {
            Serial.println("ERR: No free SM on PIO0/PIO2 for column program");
            pio_remove_program(msm_row_pio, &msm_row_program, msm_row_off);
            pio_sm_unclaim(msm_row_pio, msm_row_sm);
            return false;
        }
    }
    msm_col_sm = (uint)col_sm;
    if (!pio_can_add_program(msm_col_pio, &msm_col_program)) {
        Serial.println("ERR: No instruction space for column program");
        pio_sm_unclaim(msm_col_pio, msm_col_sm);
        pio_remove_program(msm_row_pio, &msm_row_program, msm_row_off);
        pio_sm_unclaim(msm_row_pio, msm_row_sm);
        return false;
    }
    msm_col_off = pio_add_program(msm_col_pio, &msm_col_program);

    // Configure row SM on PIO1 (GPIOBASE=16)
    // SDK takes absolute GPIO numbers — hardware subtracts GPIOBASE internally
    pio_sm_config rc = pio_get_default_sm_config();
    sm_config_set_wrap(&rc, msm_row_off + MSM_ROW_WRAP_TARGET,
                            msm_row_off + MSM_ROW_WRAP);
    sm_config_set_out_pins(&rc, 21, 24);          // absolute: GP21-44
    sm_config_set_clkdiv(&rc, 1.0f);
    sm_config_set_out_shift(&rc, true, false, 32);
    pio_sm_init(msm_row_pio, msm_row_sm, msm_row_off, &rc);

    // Set PIO1 pindirs: all 24 out pins as output (absolute GPIO numbers)
    pio_sm_set_consecutive_pindirs(msm_row_pio, msm_row_sm, 21, 24, true);

    // Configure column SM on PIO0 (GPIOBASE=0): out_base = GP1 (absolute = relative)
    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, msm_col_off + MSM_COL_WRAP_TARGET,
                            msm_col_off + MSM_COL_WRAP);
    sm_config_set_out_pins(&cc, COL_PIN[0], PANEL_SIZE); // GP1, 20 pins
    sm_config_set_clkdiv(&cc, 1.0f);
    sm_config_set_out_shift(&cc, true, false, 32);
    pio_sm_init(msm_col_pio, msm_col_sm, msm_col_off, &cc);

    // Set PIO0 pindirs: all 20 column pins as output
    pio_sm_set_consecutive_pindirs(msm_col_pio, msm_col_sm, COL_PIN[0], PANEL_SIZE, true);

    // Configure row pins for PIO1 (skip GP32-35 — SPI funcsel stays)
    for (int r = 0; r < PANEL_SIZE; r++) {
        pio_gpio_init(msm_row_pio, ROW_PIN[r]);
    }
    // Configure column pins for PIO0
    for (int c = 0; c < PANEL_SIZE; c++) {
        pio_gpio_init(msm_col_pio, COL_PIN[c]);
    }

    // Set up IRQ bridge ISRs
    // PIO1 IRQ0: row SM flag 0 → row_to_col ISR
    irq_set_exclusive_handler(PIO1_IRQ_0, msm_row_to_col_isr);
    irq_set_priority(PIO1_IRQ_0, 0);  // highest priority
    pio_set_irq0_source_enabled(msm_row_pio, pis_interrupt0, true);

    // PIO0 IRQ0: col SM flag 1 → col_to_row ISR
    // Note: flag 1 on PIO0 maps to pis_interrupt1
    uint col_pio_idx = pio_get_index(msm_col_pio);
    uint col_irq_num = (col_pio_idx == 0) ? PIO0_IRQ_0 :
                       (col_pio_idx == 2) ? PIO2_IRQ_0 : PIO0_IRQ_0;
    irq_set_exclusive_handler(col_irq_num, msm_col_to_row_isr);
    irq_set_priority(col_irq_num, 0);
    pio_set_irq0_source_enabled(msm_col_pio, pis_interrupt1, true);

    msm_loaded = true;
    Serial.print("Multi-SM PIO OK: row=pio");
    Serial.print(pio_get_index(msm_row_pio));
    Serial.print("/sm");
    Serial.print(msm_row_sm);
    Serial.print("  col=pio");
    Serial.print(pio_get_index(msm_col_pio));
    Serial.print("/sm");
    Serial.println(msm_col_sm);
    return true;
}

// Claim 2 DMA channels for multi-SM
static bool msm_init_dma() {
    if (msm_dma_claimed) return true;
    msm_ch_row = dma_claim_unused_channel(false);
    msm_ch_col = dma_claim_unused_channel(false);
    if (msm_ch_row < 0 || msm_ch_col < 0) {
        Serial.println("ERR: Cannot claim 2 DMA channels for multi-SM");
        if (msm_ch_row >= 0) dma_channel_unclaim(msm_ch_row);
        if (msm_ch_col >= 0) dma_channel_unclaim(msm_ch_col);
        msm_ch_row = msm_ch_col = -1;
        return false;
    }
    msm_dma_claimed = true;
    Serial.print("Multi-SM DMA: row=");
    Serial.print(msm_ch_row);
    Serial.print(" col=");
    Serial.println(msm_ch_col);
    return true;
}

// Switch column pins to col PIO instance
static void msm_col_pins_to_pio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        pio_gpio_init(msm_col_pio, COL_PIN[c]);
    }
}

static void msm_row_pins_to_pio() {
    for (int r = 0; r < PANEL_SIZE; r++) {
        pio_gpio_init(msm_row_pio, ROW_PIN[r]);
    }
}

static void msm_all_pins_to_pio() {
    msm_col_pins_to_pio();
    msm_row_pins_to_pio();
}

// Restore column and row pins to SIO
static void msm_pins_to_sio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        gpio_set_function(COL_PIN[c], GPIO_FUNC_SIO);
        gpio_set_dir(COL_PIN[c], GPIO_OUT);
        gpio_put(COL_PIN[c], 1);  // OFF
    }
    for (int r = 0; r < PANEL_SIZE; r++) {
        gpio_set_function(ROW_PIN[r], GPIO_FUNC_SIO);
        gpio_set_dir(ROW_PIN[r], GPIO_OUT);
        gpio_put(ROW_PIN[r], 0);  // OFF
    }
}

// Configure DMA channels for multi-SM scan.
static void msm_configure_dma() {
    // CH_ROW: feeds row patterns to row SM TX FIFO on PIO1
    dma_channel_config rc = dma_channel_get_default_config(msm_ch_row);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_32);
    channel_config_set_read_increment(&rc, true);
    channel_config_set_write_increment(&rc, false);
    channel_config_set_dreq(&rc, pio_get_dreq(msm_row_pio, msm_row_sm, true));
    channel_config_set_chain_to(&rc, msm_ch_row);  // self-chain = stop
    dma_channel_configure(msm_ch_row, &rc,
        &msm_row_pio->txf[msm_row_sm],
        &msm_row_patterns[0],
        active_rows,       // exactly N row patterns (no terminator)
        false);

    // CH_COL: feeds {col_pattern, delay} pairs to col SM TX FIFO on PIO0
    dma_channel_config cc = dma_channel_get_default_config(msm_ch_col);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, true);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_dreq(&cc, pio_get_dreq(msm_col_pio, msm_col_sm, true));
    channel_config_set_chain_to(&cc, msm_ch_col);  // self-chain = stop
    dma_channel_configure(msm_ch_col, &cc,
        &msm_col_pio->txf[msm_col_sm],
        &dma_col_data[0][0],
        active_rows * 2,
        false);
}

// Enable/disable bridge ISRs
static void msm_irq_enable() {
    irq_set_enabled(PIO1_IRQ_0, true);
    uint col_pio_idx = pio_get_index(msm_col_pio);
    uint col_irq_num = (col_pio_idx == 0) ? PIO0_IRQ_0 :
                       (col_pio_idx == 2) ? PIO2_IRQ_0 : PIO0_IRQ_0;
    irq_set_enabled(col_irq_num, true);
}

static void msm_irq_disable() {
    irq_set_enabled(PIO1_IRQ_0, false);
    uint col_pio_idx = pio_get_index(msm_col_pio);
    uint col_irq_num = (col_pio_idx == 0) ? PIO0_IRQ_0 :
                       (col_pio_idx == 2) ? PIO2_IRQ_0 : PIO0_IRQ_0;
    irq_set_enabled(col_irq_num, false);
}

// Run a single multi-SM PIO scan frame. Returns frame time in DWT cycles.
// Helper: set pindirs for row and col SMs via forced exec.
// Uses `set pindirs, 0x1F` (5 bits at a time) with temporarily set SET_BASE.
// This avoids issues with pio_sm_set_consecutive_pindirs + GPIOBASE.
static void __attribute__((noinline)) __not_in_flash_func(msm_set_all_pindirs)() {
    // Row SM on PIO1 (GPIOBASE=16): set 24 pins as output starting at GP21
    // Use out exec: push 0xFFFFFFFF, then exec "out pindirs, 24"
    {
        uint32_t saved_pinctrl = msm_row_pio->sm[msm_row_sm].pinctrl;
        // Set OUT_BASE=21, OUT_COUNT=24 for pindirs
        msm_row_pio->sm[msm_row_sm].pinctrl =
            (21u << PIO_SM0_PINCTRL_OUT_BASE_LSB) |
            (24u << PIO_SM0_PINCTRL_OUT_COUNT_LSB);
        pio_sm_put(msm_row_pio, msm_row_sm, 0xFFFFFFFF);      // push all-ones to TX FIFO
        pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_pull(false, false)); // blocking pull
        pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_out(pio_pindirs, 24));
        msm_row_pio->sm[msm_row_sm].pinctrl = saved_pinctrl;
    }
    // Col SM on PIO0 (GPIOBASE=0): set 20 pins as output starting at GP1
    {
        uint32_t saved_pinctrl = msm_col_pio->sm[msm_col_sm].pinctrl;
        msm_col_pio->sm[msm_col_sm].pinctrl =
            ((uint32_t)COL_PIN[0] << PIO_SM0_PINCTRL_OUT_BASE_LSB) |
            (20u << PIO_SM0_PINCTRL_OUT_COUNT_LSB);
        pio_sm_put(msm_col_pio, msm_col_sm, 0xFFFFFFFF);
        pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_pull(false, false));
        pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_out(pio_pindirs, 20));
        msm_col_pio->sm[msm_col_sm].pinctrl = saved_pinctrl;
    }
}

static uint32_t __not_in_flash_func(msm_run_single_frame)() {
    // Reset both SMs — skip pio_sm_restart to preserve pindirs
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
    pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
    pio_sm_clear_fifos(msm_row_pio, msm_row_sm);
    pio_sm_clear_fifos(msm_col_pio, msm_col_sm);
    // Jump back to program start (resets PC without touching pindirs)
    pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_jmp(msm_row_off));
    pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_jmp(msm_col_off));

    // Clear IRQ flags on both PIO blocks
    pio_interrupt_clear(msm_row_pio, 0);
    pio_interrupt_clear(msm_row_pio, 1);
    pio_interrupt_clear(msm_col_pio, 0);
    pio_interrupt_clear(msm_col_pio, 1);

    // Push all-OFF init mask to column SM (addr 0-1: pull, mov y)
    pio_sm_put_blocking(msm_col_pio, msm_col_sm, 0xFFFFF);

    // Reset DMA read addresses and transfer counts
    // No terminator — send exactly active_rows row patterns
    dma_channel_set_read_addr(msm_ch_row, &msm_row_patterns[0], false);
    dma_channel_set_trans_count(msm_ch_row, active_rows, false);
    dma_channel_set_read_addr(msm_ch_col, &dma_col_data[0][0], false);
    dma_channel_set_trans_count(msm_ch_col, active_rows * 2, false);

    // Reset row completion counter
    msm_rows_done = 0;

    uint32_t t0 = m33_hw->dwt_cyccnt;

    // Start DMA and both SMs
    dma_channel_start(msm_ch_row);
    dma_channel_start(msm_ch_col);
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, true);
    pio_sm_set_enabled(msm_col_pio, msm_col_sm, true);

    // Wait for all rows to complete via the col→row bridge ISR counter.
    // Each col→row ISR fires after col SM finishes a row (delay + cols OFF).
    // After N rows, row SM stalls at pull block (no more data) — harmless.
    while (msm_rows_done < (uint32_t)active_rows) {
        tight_loop_contents();
    }

    uint32_t t1 = m33_hw->dwt_cyccnt;

    // Stop both SMs
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
    pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);

    return t1 - t0;
}

// Run N multi-SM scan frames with timing statistics.
static void __attribute__((noinline)) __not_in_flash_func(msm_run_scan_frames)(uint32_t n_frames) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();
    msm_precompute_row_patterns();

    // Fill column data buffer
    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    uint32_t pio_delay = (on_cycles > MSM_COL_ON_OVERHEAD)
                       ? (on_cycles - MSM_COL_ON_OVERHEAD) : 0;
    for (int r = 0; r < active_rows; r++) {
        dma_col_data[r][0] = pio_col_word;
        dma_col_data[r][1] = pio_delay;
    }

    msm_all_pins_to_pio();
    msm_configure_dma();
    msm_irq_enable();

    // Warm-up frame
    msm_run_single_frame();

    for (uint32_t f = 0; f < n_frames; f++) {
        uint32_t frame_cyc = msm_run_single_frame();
        uint32_t row_avg_cyc = frame_cyc / active_rows;
        stats_update(frame_cyc, row_avg_cyc);

        if ((f % 100 == 99) && user_wants_stop()) {
            Serial.print("  (interrupted at frame ");
            Serial.print(f + 1);
            Serial.println(")");
            break;
        }
    }

    msm_irq_disable();
    msm_pins_to_sio();
    all_off();
}

// Debug test for multi-SM PIO
static void msm_debug_test() {
    precompute_scan_masks();
    msm_precompute_row_patterns();

    Serial.println("=== MSMTEST: Multi-SM PIO debug ===");
    Serial.print("Row PIO: pio");
    Serial.print(pio_get_index(msm_row_pio));
    Serial.print("/sm");
    Serial.print(msm_row_sm);
    Serial.print("  Col PIO: pio");
    Serial.print(pio_get_index(msm_col_pio));
    Serial.print("/sm");
    Serial.println(msm_col_sm);

    // Print row patterns
    Serial.println("Row patterns (24-bit one-hot, base GP21):");
    for (int r = 0; r < active_rows; r++) {
        Serial.print("  row ");
        Serial.print(r);
        Serial.print(": pin GP");
        Serial.print(ROW_PIN[r]);
        Serial.print(" → 0x");
        Serial.println(msm_row_patterns[r], HEX);
    }

    // Step 0: Light one LED manually — row SM drives row, col SM drives cols
    Serial.println("Step 0: Manual single-LED test (row 0, all cols, 2 sec hold)");
    {
        msm_all_pins_to_pio();

        // Reset both SMs
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
        pio_sm_clear_fifos(msm_row_pio, msm_row_sm);
        pio_sm_clear_fifos(msm_col_pio, msm_col_sm);
        pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_jmp(msm_row_off));
        pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_jmp(msm_col_off));
        pio_interrupt_clear(msm_row_pio, 0);
        pio_interrupt_clear(msm_row_pio, 1);
        pio_interrupt_clear(msm_col_pio, 0);
        pio_interrupt_clear(msm_col_pio, 1);

        // Row SM: pull, out pins (row 0 HIGH), irq set 0, wait irq 1
        pio_sm_put_blocking(msm_row_pio, msm_row_sm, msm_row_patterns[0]);

        // Col SM: pull (all-OFF), mov y, wait irq 0, pull (col), out pins (ON),
        //         pull (delay), ...
        pio_sm_put_blocking(msm_col_pio, msm_col_sm, 0xFFFFF); // all-OFF init

        // Enable bridge ISRs
        msm_irq_enable();

        // Start both SMs
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, true);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, true);

        // Row SM runs: pull → out pins (row 0) → irq set 0 → wait irq 1
        // Bridge: PIO1 flag 0 → PIO0 flag 0
        // Col SM runs: pull (all-OFF) → mov y → wait irq 0 (wakes) → pull block (stalls)
        delayMicroseconds(100);

        // Now push column data: pattern + very long delay
        uint32_t col_word = (~col_pattern) & 0xFFFFF;  // inverted for ON
        pio_sm_put_blocking(msm_col_pio, msm_col_sm, col_word);
        pio_sm_put_blocking(msm_col_pio, msm_col_sm, 0xFFFFFFFF); // max delay ~28 sec

        // Read GPIO state while LED should be on
        delayMicroseconds(100);
        uint32_t gpio_lo = sio_hw->gpio_in;
        uint32_t gpio_hi = sio_hw->gpio_hi_in;
        uint64_t gpio_in = (uint64_t)gpio_hi << 32 | gpio_lo;

        Serial.print("  gpio_lo=0x"); Serial.println(gpio_lo, HEX);
        Serial.print("  gpio_hi=0x"); Serial.println(gpio_hi, HEX);
        Serial.print("  Row 0 (GP"); Serial.print(ROW_PIN[0]);
        Serial.print(") HIGH? ");
        Serial.println((gpio_in & row_on_mask[0]) ? "YES" : "NO");

        // Check column pins — should be LOW (ON) for active columns
        bool any_col_low = false;
        bool any_col_high = false;
        for (int c = 0; c < PANEL_SIZE; c++) {
            bool pin_high = (gpio_lo >> COL_PIN[c]) & 1;
            if (pin_high) any_col_high = true;
            else any_col_low = true;
        }
        Serial.print("  Columns: low(ON)=");
        Serial.print(any_col_low ? "YES" : "NO");
        Serial.print("  high(OFF)=");
        Serial.println(any_col_high ? "YES" : "NO");

        Serial.println("  LED should be ON now — holding 5 seconds...");
        delay(5000);

        // Stop
        msm_irq_disable();
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
        msm_pins_to_sio();
        all_off();
        Serial.println("  Done — LED off");
    }

    // Step 1: Test row SM alone (CPU push, verify pin output)
    Serial.println("Step 1: Row SM pin output test");
    msm_row_pins_to_pio();
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
    pio_sm_clear_fifos(msm_row_pio, msm_row_sm);
    pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_jmp(msm_row_off));
    pio_interrupt_clear(msm_row_pio, 0);
    pio_interrupt_clear(msm_row_pio, 1);

    // Push row 0 pattern — row SM: pull, out pins, irq set 0, wait 1 irq 1
    pio_sm_put_blocking(msm_row_pio, msm_row_sm, msm_row_patterns[0]);
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, true);
    delayMicroseconds(10);

    bool flag0 = msm_row_pio->irq & (1u << 0);
    Serial.print("  PIO1 flag 0 set? ");
    Serial.println(flag0 ? "YES (GOOD)" : "NO (BAD)");

    // Read actual pad state (not SIO output register — that only shows SIO's value)
    uint64_t gpio_in = (uint64_t)sio_hw->gpio_hi_in << 32 | sio_hw->gpio_in;
    bool row0_on = (gpio_in & row_on_mask[0]) != 0;
    Serial.print("  Row 0 (GP");
    Serial.print(ROW_PIN[0]);
    Serial.print(") active? ");
    Serial.println(row0_on ? "YES (GOOD)" : "NO (BAD)");

    // Also print raw GPIO state for debug
    Serial.print("  gpio_in=0x");
    Serial.print((uint32_t)(gpio_in >> 32), HEX);
    Serial.print("_");
    Serial.println((uint32_t)gpio_in, HEX);

    // Release row SM: force flag 1 on PIO1
    msm_row_pio->irq_force = (1u << 1);
    delayMicroseconds(1);
    pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);

    // Step 2: Test bridge ISR (row irq → col SM wakes)
    Serial.println("Step 2: Bridge ISR test");
    pio_interrupt_clear(msm_row_pio, 0);
    pio_interrupt_clear(msm_col_pio, 0);
    msm_irq_enable();

    // Manually fire PIO1 flag 0 — bridge ISR should force PIO0 flag 0
    msm_row_pio->irq_force = (1u << 0);
    delayMicroseconds(10);
    bool col_flag0 = msm_col_pio->irq & (1u << 0);
    Serial.print("  PIO1 flag0 → bridge → PIO0 flag0? ");
    Serial.println(col_flag0 ? "YES (GOOD)" : "NO (BAD)");

    // Test reverse: force PIO0 flag 1 → bridge → PIO1 flag 1
    pio_interrupt_clear(msm_col_pio, 1);
    pio_interrupt_clear(msm_row_pio, 1);
    msm_col_pio->irq_force = (1u << 1);
    delayMicroseconds(10);
    bool row_flag1 = msm_row_pio->irq & (1u << 1);
    Serial.print("  PIO0 flag1 → bridge → PIO1 flag1? ");
    Serial.println(row_flag1 ? "YES (GOOD)" : "NO (BAD)");

    msm_irq_disable();

    // Step 3: Single-row bridge test (1 row via DMA + ISR)
    Serial.println("Step 3: Single-row DMA + bridge test");
    {
        uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
        // Set up for 1 row only
        dma_col_data[0][0] = pio_col_word;
        dma_col_data[0][1] = 0;  // min delay
        uint32_t one_row_pattern[2] = { msm_row_patterns[0], 0 }; // row 0 + terminator

        msm_all_pins_to_pio();

        // Reset both SMs
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
        pio_sm_clear_fifos(msm_row_pio, msm_row_sm);
        pio_sm_clear_fifos(msm_col_pio, msm_col_sm);
        pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_jmp(msm_row_off));
        pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_jmp(msm_col_off));
        pio_interrupt_clear(msm_row_pio, 0);
        pio_interrupt_clear(msm_row_pio, 1);
        pio_interrupt_clear(msm_col_pio, 0);
        pio_interrupt_clear(msm_col_pio, 1);

        // Init col SM: push all-OFF mask
        pio_sm_put_blocking(msm_col_pio, msm_col_sm, 0xFFFFF);

        // Configure DMA for 1 row
        dma_channel_config rc = dma_channel_get_default_config(msm_ch_row);
        channel_config_set_transfer_data_size(&rc, DMA_SIZE_32);
        channel_config_set_read_increment(&rc, true);
        channel_config_set_write_increment(&rc, false);
        channel_config_set_dreq(&rc, pio_get_dreq(msm_row_pio, msm_row_sm, true));
        channel_config_set_chain_to(&rc, msm_ch_row);
        dma_channel_configure(msm_ch_row, &rc,
            &msm_row_pio->txf[msm_row_sm], &one_row_pattern[0], 2, false);

        dma_channel_config cc = dma_channel_get_default_config(msm_ch_col);
        channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
        channel_config_set_read_increment(&cc, true);
        channel_config_set_write_increment(&cc, false);
        channel_config_set_dreq(&cc, pio_get_dreq(msm_col_pio, msm_col_sm, true));
        channel_config_set_chain_to(&cc, msm_ch_col);
        dma_channel_configure(msm_ch_col, &cc,
            &msm_col_pio->txf[msm_col_sm], &dma_col_data[0][0], 2, false);

        msm_irq_enable();

        // Start everything
        dma_channel_start(msm_ch_row);
        dma_channel_start(msm_ch_col);
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, true);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, true);

        // Wait with timeout
        uint32_t t0 = micros();
        bool row_dma_done = false, col_dma_done = false;
        while (micros() - t0 < 100000) {
            if (!row_dma_done && !dma_channel_is_busy(msm_ch_row)) {
                row_dma_done = true;
            }
            if (!col_dma_done && !dma_channel_is_busy(msm_ch_col)) {
                col_dma_done = true;
            }
            if (row_dma_done && col_dma_done) break;
        }
        uint32_t elapsed = micros() - t0;

        Serial.print("  row_dma_done=");
        Serial.print(row_dma_done ? "YES" : "NO");
        Serial.print(" col_dma_done=");
        Serial.print(col_dma_done ? "YES" : "NO");
        Serial.print(" elapsed=");
        Serial.print(elapsed);
        Serial.println("us");

        // Print PIO state
        Serial.print("  PIO1 flags=0x");
        Serial.print(msm_row_pio->irq, HEX);
        Serial.print("  PIO0 flags=0x");
        Serial.println(msm_col_pio->irq, HEX);
        Serial.print("  Row TX empty=");
        Serial.print(pio_sm_is_tx_fifo_empty(msm_row_pio, msm_row_sm));
        Serial.print("  Col TX empty=");
        Serial.println(pio_sm_is_tx_fifo_empty(msm_col_pio, msm_col_sm));

        msm_irq_disable();
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
    }

    // Step 4: Full scan frame with long ON — read GPIO mid-frame
    Serial.println("Step 4: Scan frame GPIO check (ON=500us, row 0)");
    {
        uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
        // 500us ON = 75000 cycles at 150MHz. Overhead = 5. Delay = 74995.
        uint32_t long_delay = 74995;
        dma_col_data[0][0] = pio_col_word;
        dma_col_data[0][1] = long_delay;
        // Only scan 1 row for this test
        uint32_t one_pattern = msm_row_patterns[0];

        msm_all_pins_to_pio();

        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);
        pio_sm_clear_fifos(msm_row_pio, msm_row_sm);
        pio_sm_clear_fifos(msm_col_pio, msm_col_sm);
        pio_sm_exec(msm_row_pio, msm_row_sm, pio_encode_jmp(msm_row_off));
        pio_sm_exec(msm_col_pio, msm_col_sm, pio_encode_jmp(msm_col_off));
        pio_interrupt_clear(msm_row_pio, 0);
        pio_interrupt_clear(msm_row_pio, 1);
        pio_interrupt_clear(msm_col_pio, 0);
        pio_interrupt_clear(msm_col_pio, 1);

        pio_sm_put_blocking(msm_col_pio, msm_col_sm, 0xFFFFF);

        // Configure DMA for 1 row (no terminator)
        dma_channel_set_read_addr(msm_ch_row, &one_pattern, false);
        dma_channel_set_trans_count(msm_ch_row, 1, false);
        dma_channel_set_read_addr(msm_ch_col, &dma_col_data[0][0], false);
        dma_channel_set_trans_count(msm_ch_col, 2, false);

        msm_rows_done = 0;
        msm_irq_enable();

        dma_channel_start(msm_ch_row);
        dma_channel_start(msm_ch_col);
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, true);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, true);

        // Wait 100us — should be mid-delay (500us ON)
        delayMicroseconds(100);

        uint32_t gpio_lo = sio_hw->gpio_in;
        uint32_t gpio_hi = sio_hw->gpio_hi_in;

        // Wait for completion
        uint32_t t0 = micros();
        while (msm_rows_done < 1 && (micros() - t0 < 100000)) {
            tight_loop_contents();
        }

        msm_irq_disable();
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);

        Serial.print("  Mid-frame gpio_lo=0x");
        Serial.print(gpio_lo, HEX);
        Serial.print("  gpio_hi=0x");
        Serial.println(gpio_hi, HEX);

        bool row0_on = (gpio_lo >> 21) & 1;
        Serial.print("  Row 0 (GP21) HIGH? ");
        Serial.println(row0_on ? "YES" : "NO");

        // Check columns
        int cols_low = 0, cols_high = 0;
        for (int c = 0; c < PANEL_SIZE; c++) {
            if ((gpio_lo >> COL_PIN[c]) & 1) cols_high++;
            else cols_low++;
        }
        Serial.print("  Columns: ");
        Serial.print(cols_low);
        Serial.print(" LOW(ON), ");
        Serial.print(cols_high);
        Serial.println(" HIGH(OFF)");

        Serial.print("  msm_rows_done=");
        Serial.println(msm_rows_done);
    }

    // Step 5: Full 20-row scan for 5 seconds (visual verification)
    Serial.println("Step 5: Full panel scan — all rows, 5 seconds (watch the panel!)");
    {
        uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
        // 10us ON per row → ~200us/frame → ~5000 fps
        uint32_t vis_delay = (uint32_t)(10.0f * cycles_per_us) - MSM_COL_ON_OVERHEAD;
        for (int r = 0; r < active_rows; r++) {
            dma_col_data[r][0] = pio_col_word;
            dma_col_data[r][1] = vis_delay;
        }
        msm_all_pins_to_pio();
        msm_configure_dma();
        msm_irq_enable();

        uint32_t t_start = millis();
        uint32_t frames = 0;
        while (millis() - t_start < 5000) {
            msm_run_single_frame();
            frames++;
        }
        msm_irq_disable();
        pio_sm_set_enabled(msm_row_pio, msm_row_sm, false);
        pio_sm_set_enabled(msm_col_pio, msm_col_sm, false);

        Serial.print("  Ran ");
        Serial.print(frames);
        Serial.println(" frames in 5 seconds");
    }

    msm_pins_to_sio();
    all_off();
    Serial.println("=== MSMTEST DONE ===");
}

// ---------------------------------------------------------------------------
// Burst-mode scan: simulates 8 kHz external trigger (Phase 3e)
// ---------------------------------------------------------------------------
// Simulates the actual 2P microscopy use case:
//   - External trigger at configurable rate (default 8 kHz = 125 µs period)
//   - noInterrupts() → PIO scan all rows → interrupts() → idle until next trigger
//   - Measures scan burst time only (not idle), reports jitter over many triggers
//
// This is the definitive jitter test for the application.

static void __attribute__((noinline)) __not_in_flash_func(run_burst_scan)(
    uint32_t n_triggers, float trigger_rate_hz)
{
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);

    // Reset and enable PIO SM (restart clears pindirs on RP2350, so re-set them)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Push all-OFF mask for Y register init
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Warm-up frame (with interrupts disabled)
    noInterrupts();
    for (int r = 0; r < active_rows; r++) {
        gpio_set_mask64(row_on_mask[r]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_col_word);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, pio_delay);
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
        gpio_clr_mask64(row_on_mask[r]);
    }
    interrupts();

    // Main burst loop: simulate external trigger
    uint32_t trigger_start = m33_hw->dwt_cyccnt;

    for (uint32_t t = 0; t < n_triggers; t++) {
        // --- Wait for next trigger edge ---
        // (In real use, this would be a GPIO interrupt or PIO wait pin)
        while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {
            // Idle — interrupts are ON, USB/system can run freely
        }
        trigger_start += trigger_period_cyc;  // Maintain phase (no drift)

        // --- Scan burst: interrupts OFF for the entire frame ---
        noInterrupts();
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
        interrupts();
        // --- End scan burst ---

        uint32_t frame_cyc = frame_end - frame_start;
        uint32_t row_avg_cyc = frame_cyc / active_rows;
        stats_update(frame_cyc, row_avg_cyc);

        // Allow early stop every 1000 triggers
        if ((t % 1000 == 999) && user_wants_stop()) {
            Serial.print("  (interrupted at trigger ");
            Serial.print(t + 1);
            Serial.println(")");
            break;
        }
    }

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();
}

static void cmd_burst(const char* arg) {
    // Parse: BURST <n_triggers> [rate_hz]
    // Default rate = 8000 Hz
    uint32_t n = 10000;
    float rate = 8000.0f;

    if (arg && *arg) {
        char* end;
        n = strtoul(arg, &end, 10);
        if (n == 0 || n > 1000000) {
            Serial.println("ERR: BURST <1-1000000> [rate_hz]");
            return;
        }
        // Optional rate argument
        while (*end == ' ') end++;
        if (*end) {
            rate = strtof(end, nullptr);
            if (rate < 100.0f || rate > 100000.0f) {
                Serial.println("ERR: rate must be 100-100000 Hz");
                return;
            }
        }
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();
    col_pins_to_pio();

    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;
    float pio_on_us = (float)(pio_delay + PIO_ON_OVERHEAD_CYCLES) / (float)cycles_per_us;
    float period_us = 1000000.0f / rate;

    Serial.println("--- BURST START ---");
    Serial.print("Rows=");
    Serial.print(active_rows);
    Serial.print("  ON/row=");
    Serial.print(on_us, 3);
    Serial.print("us cmd (");
    Serial.print(pio_on_us, 3);
    Serial.print("us PIO actual)  Pattern=0x");
    Serial.print(col_pattern, HEX);
    Serial.print("  Triggers=");
    Serial.println(n);
    Serial.print("  Trigger rate=");
    Serial.print(rate, 1);
    Serial.print("Hz  Period=");
    Serial.print(period_us, 1);
    Serial.print("us  Scan budget=");
    float budget = period_us * 0.12f;  // ~12% of period for turnaround
    Serial.print(budget, 1);
    Serial.println("us (est 12% turnaround)");

    run_burst_scan(n, rate);

    // Scan burst stats
    stats_print_fields("  BURST_SCAN", "frame", "row_avg");

    if (stat_count > 0) {
        float mean_frame_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float jitter_us = cycles_to_us(stat_on_max) - cycles_to_us(stat_on_min);
        float frame_rate = rate;  // One scan per trigger
        Serial.print("  Jitter (max-min): ");
        Serial.print(jitter_us, 3);
        Serial.println("us");
        Serial.print("  Effective rate: ");
        Serial.print(frame_rate, 1);
        Serial.print("Hz  Duty cycle: ");
        Serial.print(100.0f * mean_frame_us / period_us, 1);
        Serial.println("%");
        Serial.print("  Fits in 15us window? ");
        Serial.println(cycles_to_us(stat_on_max) <= 15.0f ? "YES" : "NO");
    }

    Serial.println("--- BURST END ---");

    col_pins_to_sio();
    precompute_scan_masks();
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

// ---------------------------------------------------------------------------
// BCM Burst-Mode Scanning (Phase 4)
// ---------------------------------------------------------------------------
// Single-row-per-trigger BCM: each 8 kHz trigger scans ONE row through
// all bit-planes. Rows cycle across triggers → full frame at 400 Hz.

// Mode A: PIO burst BCM (PIOSCAN-based, noInterrupts during burst)
// noinline prevents compiler from inlining this into flash-resident callers,
// which would defeat __not_in_flash_func and cause XIP cache jitter.
static void __attribute__((noinline)) __not_in_flash_func(run_bcm_burst_pio)(
    uint32_t n_triggers, float trigger_rate_hz)
{
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();
    precompute_bcm_data();

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);

    // Compute expected burst time for outlier threshold (2x nominal)
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        uint32_t on_cyc_b = base_cycles * (1U << b);
        nominal_burst_cyc += on_cyc_b + PIO_ON_OVERHEAD_CYCLES + 50; // ~50 cyc CPU overhead per pass
    }
    stat_outlier_threshold = nominal_burst_cyc * 2;

    // Reset and enable PIO SM (restart clears pindirs on RP2350, so re-set them)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Push all-OFF mask for Y register init
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Warm-up: one row with all bit-planes
    noInterrupts();
    gpio_set_mask64(row_on_mask[0]);
    for (int b = 0; b < bcm_bits; b++) {
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][0]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][1]);
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    interrupts();

    // Lock out Core 1 — eliminates bus contention from USB stack.
    // Core 1 stays locked for the entire measurement (~1.25s at 10k/8kHz).
    multicore_lockout_start_blocking();

    // Warm-up: 100 trigger cycles with noInterrupts to stabilize pipeline.
    noInterrupts();
    {
        uint8_t wr = 0;
        uint32_t wu_start = m33_hw->dwt_cyccnt;
        for (uint32_t w = 0; w < 100; w++) {
            while ((m33_hw->dwt_cyccnt - wu_start) < trigger_period_cyc) {}
            wu_start += trigger_period_cyc;
            gpio_set_mask64(row_on_mask[wr]);
            for (int b = 0; b < bcm_bits; b++) {
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][0]);
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][1]);
                while (!pio_interrupt_get(pio_hw_inst, 0)) {}
                pio_interrupt_clear(pio_hw_inst, 0);
            }
            gpio_clr_mask64(row_on_mask[wr]);
            wr++;
            if (wr >= active_rows) wr = 0;
        }
    }
    interrupts();
    stats_reset();  // clear any stats from warm-up

    // Full-loop noInterrupts for zero-jitter measurement
    noInterrupts();

    // Main trigger loop
    uint32_t trigger_start = m33_hw->dwt_cyccnt;
    uint8_t row = 0;

    for (uint32_t t = 0; t < n_triggers; t++) {
        while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
        trigger_start += trigger_period_cyc;

        uint32_t burst_start = m33_hw->dwt_cyccnt;

        gpio_set_mask64(row_on_mask[row]);
        for (int b = 0; b < bcm_bits; b++) {
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][0]);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][1]);
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
        }
        gpio_clr_mask64(row_on_mask[row]);

        uint32_t burst_end = m33_hw->dwt_cyccnt;

        stats_update(burst_end - burst_start, 0);

        row++;
        if (row >= active_rows) row = 0;
    }

    interrupts();
    multicore_lockout_end_blocking();

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();
}

// Mode B: DMA burst BCM (DMA feeds PIO, CPU polls completion)
static void __attribute__((noinline)) __not_in_flash_func(run_bcm_burst_dma)(
    uint32_t n_triggers, float trigger_rate_hz)
{
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();
    precompute_bcm_data();

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);

    // Compute outlier threshold
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        nominal_burst_cyc += base_cycles * (1U << b) + PIO_ON_OVERHEAD_CYCLES + 30;
    }
    stat_outlier_threshold = nominal_burst_cyc * 2;

    // Reset and enable PIO SM (same PIO program as Mode A)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Claim DMA channel for feeding PIO TX FIFO
    int dma_ch = dma_claim_unused_channel(false);
    if (dma_ch < 0) {
        Serial.println("ERR: No free DMA channel for BCM");
        pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
        return;
    }

    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio_hw_inst, pio_sm_idx, true));

    // Warm-up
    noInterrupts();
    gpio_set_mask64(row_on_mask[0]);
    for (int b = 0; b < bcm_bits; b++) {
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][0]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][1]);
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    interrupts();

    // Main trigger loop
    uint32_t trigger_start = m33_hw->dwt_cyccnt;
    uint8_t row = 0;

    for (uint32_t t = 0; t < n_triggers; t++) {
        while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
        trigger_start += trigger_period_cyc;

        noInterrupts();
        uint32_t burst_start = m33_hw->dwt_cyccnt;

        gpio_set_mask64(row_on_mask[row]);

        // Start DMA: feed all bit-plane data to PIO TX FIFO
        dma_channel_configure(dma_ch, &dma_cfg,
            &pio_hw_inst->txf[pio_sm_idx],   // write to PIO TX FIFO
            &bcm_plane_data[row][0][0],       // read from pre-computed data
            bcm_bits * 2,                     // 2 words per bit-plane
            true);                            // start immediately

        // Poll PIO IRQ for each bit-plane completion
        for (int b = 0; b < bcm_bits; b++) {
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
        }

        gpio_clr_mask64(row_on_mask[row]);
        uint32_t burst_end = m33_hw->dwt_cyccnt;
        interrupts();

        stats_update(burst_end - burst_start, 0);

        row++;
        if (row >= active_rows) row = 0;

        if ((t % 1000 == 999) && user_wants_stop()) {
            Serial.print("  (interrupted at trigger ");
            Serial.print(t + 1);
            Serial.println(")");
            break;
        }
    }

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    dma_channel_unclaim(dma_ch);
    all_off();
}

// Mode C: MSM burst BCM (col SM on PIO0, row held by CPU)
// Uses the MSM column SM but row is held constant by CPU GPIO.
// No bridge ISRs needed since row doesn't change during burst.
static void __attribute__((noinline)) __not_in_flash_func(run_bcm_burst_msm)(
    uint32_t n_triggers, float trigger_rate_hz)
{
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();
    precompute_bcm_data();

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);

    // Compute outlier threshold
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        nominal_burst_cyc += base_cycles * (1U << b) + PIO_ON_OVERHEAD_CYCLES + 30;
    }
    stat_outlier_threshold = nominal_burst_cyc * 2;

    // For Mode C, we reuse the same single-SM PIO column program (led_col_program)
    // since we don't need the MSM row SM (row is held by CPU).
    // This makes Mode C functionally similar to Mode A but we DMA-feed the col SM.
    // The key difference: DMA paces data via DREQ, reducing CPU involvement.

    // Reset PIO SM
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // DMA channel
    int dma_ch = dma_claim_unused_channel(false);
    if (dma_ch < 0) {
        Serial.println("ERR: No free DMA channel for BCM-C");
        pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
        return;
    }

    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio_hw_inst, pio_sm_idx, true));

    // Warm-up
    noInterrupts();
    gpio_set_mask64(row_on_mask[0]);
    dma_channel_configure(dma_ch, &dma_cfg,
        &pio_hw_inst->txf[pio_sm_idx], &bcm_plane_data[0][0][0],
        bcm_bits * 2, true);
    for (int b = 0; b < bcm_bits; b++) {
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    interrupts();

    // Main trigger loop
    uint32_t trigger_start = m33_hw->dwt_cyccnt;
    uint8_t row = 0;

    for (uint32_t t = 0; t < n_triggers; t++) {
        while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
        trigger_start += trigger_period_cyc;

        noInterrupts();
        uint32_t burst_start = m33_hw->dwt_cyccnt;

        gpio_set_mask64(row_on_mask[row]);

        // DMA feeds all bit-plane data
        dma_channel_configure(dma_ch, &dma_cfg,
            &pio_hw_inst->txf[pio_sm_idx], &bcm_plane_data[row][0][0],
            bcm_bits * 2, true);

        // Wait for all bit-planes to complete
        for (int b = 0; b < bcm_bits; b++) {
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
        }

        gpio_clr_mask64(row_on_mask[row]);
        uint32_t burst_end = m33_hw->dwt_cyccnt;
        interrupts();

        stats_update(burst_end - burst_start, 0);

        row++;
        if (row >= active_rows) row = 0;

        if ((t % 1000 == 999) && user_wants_stop()) {
            Serial.print("  (interrupted at trigger ");
            Serial.print(t + 1);
            Serial.println(")");
            break;
        }
    }

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    dma_channel_unclaim(dma_ch);
    all_off();
}

static void cmd_bcmburst(const char* arg) {
    // Parse: BCMBURST <n_triggers> [rate_hz] [A|B|C]
    uint32_t n = 10000;
    float rate = 8000.0f;
    char mode = 'A';

    if (arg && *arg) {
        char* end;
        n = strtoul(arg, &end, 10);
        if (n == 0 || n > 1000000) {
            Serial.println("ERR: BCMBURST <1-1000000> [rate_hz] [A|B|C]");
            return;
        }
        while (*end == ' ') end++;
        if (*end && (*end >= '0' && *end <= '9')) {
            rate = strtof(end, &end);
            if (rate < 100.0f || rate > 100000.0f) {
                Serial.println("ERR: rate must be 100-100000 Hz");
                return;
            }
        }
        while (*end == ' ') end++;
        if (*end == 'A' || *end == 'B' || *end == 'C' ||
            *end == 'a' || *end == 'b' || *end == 'c') {
            mode = *end & ~0x20;  // uppercase
        }
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();
    col_pins_to_pio();

    precompute_bcm_data();

    // Compute expected burst time
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    float est_burst_us = 0;
    for (int b = 0; b < bcm_bits; b++) {
        est_burst_us += bcm_base_on_us * (1 << b) + 0.40f; // ~0.40 µs overhead per pass
    }
    float period_us = 1000000.0f / rate;

    // Drain any buffered serial data (from host setup commands)
    delay(50);
    while (Serial.available()) Serial.read();

    Serial.println("--- BCMBURST START ---");
    Serial.print("BCM bits=");
    Serial.print(bcm_bits);
    Serial.print("  Base T=");
    Serial.print(bcm_base_on_us, 3);
    Serial.print("us  Rows=");
    Serial.print(active_rows);
    Serial.print("  Triggers=");
    Serial.print(n);
    Serial.print("  Mode=");
    Serial.println(mode);
    Serial.print("  Rate=");
    Serial.print(rate, 1);
    Serial.print("Hz  Period=");
    Serial.print(period_us, 1);
    Serial.print("us  Est burst=");
    Serial.print(est_burst_us, 1);
    Serial.print("us  Frame rate=");
    Serial.print(rate / active_rows, 1);
    Serial.println("Hz");

    switch (mode) {
        case 'A': run_bcm_burst_pio(n, rate); break;
        case 'B': run_bcm_burst_dma(n, rate); break;
        case 'C': run_bcm_burst_msm(n, rate); break;
    }

    // Stats
    stats_print_fields("  BCM_BURST", "burst", nullptr);

    if (stat_count > 0) {
        float mean_burst = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        Serial.print("  Duty cycle: ");
        Serial.print(100.0f * mean_burst / period_us, 1);
        Serial.println("%");
        Serial.print("  Fits in 13us? ");
        Serial.println(cycles_to_us(stat_on_max) <= 13.0f ? "YES" : "NO");
        Serial.print("  Fits in 15us? ");
        Serial.println(cycles_to_us(stat_on_max) <= 15.0f ? "YES" : "NO");
    }

    Serial.println("--- BCMBURST END ---");

    col_pins_to_sio();
    precompute_scan_masks();
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_bcm_set(const char* arg) {
    int val = atoi(arg);
    if (val < 1 || val > 8) {
        Serial.println("ERR: BCM <1-8>");
        return;
    }
    bcm_bits = val;
    Serial.print("BCM bits=");
    Serial.print(bcm_bits);
    Serial.print("  Levels=");
    Serial.println(1 << bcm_bits);
}

static void cmd_bcmon(const char* arg) {
    float val = atof(arg);
    if (val < 0.01f || val > 100.0f) {
        Serial.println("ERR: BCMON <0.01-100> us");
        return;
    }
    bcm_base_on_us = val;
    Serial.print("BCM base T=");
    Serial.print(bcm_base_on_us, 3);
    Serial.println("us");
}

static void cmd_fill(const char* arg) {
    int val = atoi(arg);
    int max_val = (1 << bcm_bits) - 1;
    if (val < 0) val = 0;
    if (val > max_val) val = max_val;
    for (int r = 0; r < PANEL_SIZE; r++)
        for (int c = 0; c < PANEL_SIZE; c++)
            pixel_data[r][c] = (uint8_t)val;
    Serial.print("FILL ");
    Serial.print(val);
    Serial.print(" (max=");
    Serial.print(max_val);
    Serial.println(")");
}

static void cmd_gradient() {
    int max_val = (1 << bcm_bits) - 1;
    for (int r = 0; r < PANEL_SIZE; r++)
        for (int c = 0; c < PANEL_SIZE; c++)
            pixel_data[r][c] = (uint8_t)((r * max_val) / (PANEL_SIZE - 1));
    Serial.print("GRADIENT set (row-varying, 0-");
    Serial.print(max_val);
    Serial.println(")");
}

// BCM visual demo: ramp intensity from 0 to max, hold each level for ~1 second
// Runs Mode A (PIO burst) at 8 kHz, stepping intensity every second.
// Reports timing stats for each intensity level.
static void cmd_bcmdemo() {
    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();
    col_pins_to_pio();

    int max_val = (1 << bcm_bits) - 1;
    float rate = 8000.0f;
    uint32_t triggers_per_step = (uint32_t)rate;  // 1 second per step
    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / rate);

    // Compute outlier threshold
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        nominal_burst_cyc += base_cycles * (1U << b) + PIO_ON_OVERHEAD_CYCLES + 50;
    }

    Serial.println("--- BCMDEMO START ---");
    Serial.print("BCM bits=");
    Serial.print(bcm_bits);
    Serial.print("  Base T=");
    Serial.print(bcm_base_on_us, 3);
    Serial.print("us  Levels=0-");
    Serial.print(max_val);
    Serial.print("  Hold=1s each  Rate=");
    Serial.print(rate, 0);
    Serial.println("Hz");
    Serial.println("intensity,mean_us,jitter_us,outliers,count");

    // Drain any buffered serial data (prevents early stop from host setup cmds)
    while (Serial.available()) Serial.read();

    // Reset PIO SM without clearing pindirs (pio_sm_restart clears them on RP2350)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Warm-up frame
    for (int r = 0; r < PANEL_SIZE; r++)
        for (int c = 0; c < PANEL_SIZE; c++)
            pixel_data[r][c] = 0;
    precompute_bcm_data();
    noInterrupts();
    gpio_set_mask64(row_on_mask[0]);
    for (int b = 0; b < bcm_bits; b++) {
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][0]);
        pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[0][b][1]);
        while (!pio_interrupt_get(pio_hw_inst, 0)) {}
        pio_interrupt_clear(pio_hw_inst, 0);
    }
    gpio_clr_mask64(row_on_mask[0]);
    interrupts();

    // Ramp through each intensity level
    // Free-running: scan ALL rows continuously for max brightness (visual test)
    uint32_t hold_us = 1000000;  // 1 second per intensity level
    uint32_t hold_cyc = (uint32_t)(hold_us * cycles_per_us);

    for (int intensity = 0; intensity <= max_val; intensity++) {
        // Set all pixels to this intensity
        for (int r = 0; r < PANEL_SIZE; r++)
            for (int c = 0; c < PANEL_SIZE; c++)
                pixel_data[r][c] = (uint8_t)intensity;
        precompute_bcm_data();

        stats_reset();
        stat_outlier_threshold = nominal_burst_cyc * 2;

        uint32_t level_start = m33_hw->dwt_cyccnt;
        uint8_t row = 0;
        uint32_t frames = 0;

        // Scan all rows continuously for 1 second
        while ((m33_hw->dwt_cyccnt - level_start) < hold_cyc) {
            // Scan one full frame (all rows)
            noInterrupts();
            for (uint8_t r = 0; r < active_rows; r++) {
                uint32_t burst_start = m33_hw->dwt_cyccnt;

                gpio_set_mask64(row_on_mask[r]);
                for (int b = 0; b < bcm_bits; b++) {
                    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[r][b][0]);
                    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[r][b][1]);
                    while (!pio_interrupt_get(pio_hw_inst, 0)) {}
                    pio_interrupt_clear(pio_hw_inst, 0);
                }
                gpio_clr_mask64(row_on_mask[r]);

                uint32_t burst_end = m33_hw->dwt_cyccnt;
                stats_update(burst_end - burst_start, 0);
            }
            interrupts();
            frames++;
        }

        // Print CSV row for this intensity level
        float mean_us = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float jitter = cycles_to_us(stat_on_max) - cycles_to_us(stat_on_min);
        Serial.print(intensity);
        Serial.print(",");
        Serial.print(mean_us, 3);
        Serial.print(",");
        Serial.print(jitter, 3);
        Serial.print(",");
        Serial.print(stat_outlier_count);
        Serial.print(",");
        Serial.print(stat_count);
        Serial.print(",frames=");
        Serial.println(frames);

        if (user_wants_stop()) {
            Serial.println("  (stopped by user)");
            break;
        }
    }

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();

    Serial.println("--- BCMDEMO END ---");

    col_pins_to_sio();
    precompute_scan_masks();
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

// ---------------------------------------------------------------------------
// BCMVISUAL: visual + jitter test with SRAM frame refresh at 400 Hz
// Shows N intensity levels for hold_sec each. Reloads frame from SRAM every
// frame (memcpy + per-row precompute) to simulate production SPI data path.
// ---------------------------------------------------------------------------

static void __attribute__((noinline)) __not_in_flash_func(run_bcm_visual_level)(
    uint8_t intensity, uint32_t n_triggers, float trigger_rate_hz,
    uint8_t* src_frame, uint32_t* out_frame_swaps, uint32_t* out_pc_cyc_total, uint32_t* out_pc_count)
{
    // Fill source frame with uniform intensity
    memset(src_frame, intensity, PANEL_SIZE * PANEL_SIZE);

    // Initial full precompute
    memcpy(pixel_data, src_frame, sizeof(pixel_data));
    precompute_bcm_data();

    stats_reset();

    // Compute outlier threshold
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        nominal_burst_cyc += base_cycles * (1U << b) + PIO_ON_OVERHEAD_CYCLES + 50;
    }
    stat_outlier_threshold = nominal_burst_cyc * 2;

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);
    uint32_t frame_swaps = 0;
    uint32_t pc_cyc_total = 0;
    uint32_t pc_count = 0;

    // Warm-up: 100 triggers with noInterrupts (stabilize pipeline)
    {
        uint8_t wr = 0;
        uint32_t wu_start = m33_hw->dwt_cyccnt;
        noInterrupts();
        for (uint32_t w = 0; w < 100; w++) {
            while ((m33_hw->dwt_cyccnt - wu_start) < trigger_period_cyc) {}
            wu_start += trigger_period_cyc;
            gpio_set_mask64(row_on_mask[wr]);
            for (int b = 0; b < bcm_bits; b++) {
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][0]);
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][1]);
                while (!pio_interrupt_get(pio_hw_inst, 0)) {}
                pio_interrupt_clear(pio_hw_inst, 0);
            }
            gpio_clr_mask64(row_on_mask[wr]);
            wr++;
            if (wr >= active_rows) wr = 0;
        }
        interrupts();
    }
    stats_reset();

    // Disable SysTick
    systick_hw->csr &= ~1u;

    // Main triggered loop
    uint32_t trigger_start = m33_hw->dwt_cyccnt;
    uint8_t row = 0;

    for (uint32_t t = 0; t < n_triggers; t++) {
        // --- IDLE PHASE ---
        // At frame boundary (row == 0): reload pixel_data from SRAM source
        if (row == 0 && t > 0) {
            memcpy(pixel_data, src_frame, sizeof(pixel_data));
            frame_swaps++;
        }

        // Precompute NEXT row's BCM data (incremental, ~38µs spread across idles)
        uint8_t next_row = (row + 1 >= active_rows) ? 0 : row + 1;
        uint32_t pc_start = m33_hw->dwt_cyccnt;
        precompute_bcm_row(next_row);
        uint32_t pc_end = m33_hw->dwt_cyccnt;
        pc_cyc_total += (pc_end - pc_start);
        pc_count++;

        // Wait for trigger, then disable interrupts for burst
        while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
        trigger_start += trigger_period_cyc;
        noInterrupts();

        // --- BURST PHASE: one row, all bit-planes ---
        uint32_t burst_start = m33_hw->dwt_cyccnt;

        gpio_set_mask64(row_on_mask[row]);
        for (int b = 0; b < bcm_bits; b++) {
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][0]);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][1]);
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
        }
        gpio_clr_mask64(row_on_mask[row]);

        uint32_t burst_end = m33_hw->dwt_cyccnt;
        interrupts();

        stats_update(burst_end - burst_start, 0);

        row++;
        if (row >= active_rows) row = 0;
    }

    // Re-enable SysTick
    systick_hw->csr |= 1u;

    *out_frame_swaps = frame_swaps;
    *out_pc_cyc_total = pc_cyc_total;
    *out_pc_count = pc_count;
}

static void cmd_bcmvisual(const char* arg) {
    // Parse: BCMVISUAL [hold_sec]
    float hold_sec = 5.0f;

    if (arg && *arg) {
        hold_sec = strtof(arg, nullptr);
        if (hold_sec < 0.5f || hold_sec > 30.0f) {
            Serial.println("ERR: BCMVISUAL [hold_sec 0.5-30]");
            return;
        }
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();
    col_pins_to_pio();
    precompute_scan_masks();

    float rate = 8000.0f;
    uint32_t triggers_per_level = (uint32_t)(hold_sec * rate);
    int max_val = (1 << bcm_bits) - 1;

    // 4 test levels
    const uint8_t levels[] = {0, 5, 10, (uint8_t)max_val};
    const int n_levels = 4;

    // Allocate source frame in heap (simulate SPI receive buffer)
    uint8_t* src_frame = (uint8_t*)malloc(PANEL_SIZE * PANEL_SIZE);
    if (!src_frame) {
        Serial.println("ERR: malloc failed");
        return;
    }

    // Drain serial buffer
    delay(50);
    while (Serial.available()) Serial.read();

    // Reset PIO SM (restart clears pindirs on RP2350, so re-set them)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    Serial.println("--- BCMVISUAL START ---");
    Serial.print("BCM bits=");
    Serial.print(bcm_bits);
    Serial.print("  Base T=");
    Serial.print(bcm_base_on_us, 3);
    Serial.print("us  Hold=");
    Serial.print(hold_sec, 1);
    Serial.print("s  Rate=");
    Serial.print(rate, 0);
    Serial.print("Hz  Triggers/level=");
    Serial.println(triggers_per_level);
    Serial.println("level,burst_min_us,burst_max_us,burst_mean_us,jitter_us,outliers,triggers,frame_swaps,precompute_avg_us");

    // Lock out Core 1 for all levels
    multicore_lockout_start_blocking();

    for (int i = 0; i < n_levels; i++) {
        uint8_t intensity = levels[i];

        uint32_t frame_swaps = 0, pc_cyc_total = 0, pc_count = 0;
        run_bcm_visual_level(intensity, triggers_per_level, rate,
                             src_frame, &frame_swaps, &pc_cyc_total, &pc_count);

        // Print CSV row
        float burst_min = cycles_to_us(stat_on_min);
        float burst_max = cycles_to_us(stat_on_max);
        float burst_mean = (stat_count > 0) ? cycles_to_us((uint32_t)(stat_on_sum / stat_count)) : 0;
        float jitter = burst_max - burst_min;
        float pc_avg = (pc_count > 0) ? cycles_to_us(pc_cyc_total / pc_count) : 0;

        Serial.print(intensity);
        Serial.print(",");
        Serial.print(burst_min, 3);
        Serial.print(",");
        Serial.print(burst_max, 3);
        Serial.print(",");
        Serial.print(burst_mean, 3);
        Serial.print(",");
        Serial.print(jitter, 3);
        Serial.print(",");
        Serial.print(stat_outlier_count);
        Serial.print(",");
        Serial.print(stat_count);
        Serial.print(",");
        Serial.print(frame_swaps);
        Serial.print(",");
        Serial.println(pc_avg, 3);

        if (user_wants_stop()) {
            Serial.println("  (stopped by user)");
            break;
        }
    }

    multicore_lockout_end_blocking();

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();
    free(src_frame);

    Serial.println("--- BCMVISUAL END ---");

    col_pins_to_sio();
    precompute_scan_masks();
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

// ---------------------------------------------------------------------------
// RAMBURST: production simulation with frame cycling from RAM
// Tests double-buffering + precompute at 400 Hz frame rate.
// Pre-stores test patterns in RAM, cycles through them at frame boundaries.
// ---------------------------------------------------------------------------

static void init_test_frames_ramp(uint8_t n_frames) {
    // Allocate test frames in heap (PSRAM on RP2354) to avoid SRAM disruption
    if (test_frames == nullptr) {
        test_frames = (uint8_t (*)[PANEL_SIZE][PANEL_SIZE])
            malloc(MAX_TEST_FRAMES * PANEL_SIZE * PANEL_SIZE);
        if (test_frames == nullptr) {
            Serial.println("ERR: failed to allocate test frames");
            return;
        }
    }
    // Create n_frames of uniform intensity: 0, 1, 2, ..., max
    n_test_frames = (n_frames > MAX_TEST_FRAMES) ? MAX_TEST_FRAMES : n_frames;
    int max_val = (1 << bcm_bits) - 1;
    for (uint8_t f = 0; f < n_test_frames; f++) {
        uint8_t intensity = (uint8_t)((f * max_val) / (n_test_frames - 1));
        if (n_test_frames == 1) intensity = max_val;
        for (int r = 0; r < PANEL_SIZE; r++)
            for (int c = 0; c < PANEL_SIZE; c++)
                test_frames[f][r][c] = intensity;
    }
}

static void __attribute__((noinline)) __not_in_flash_func(run_ramburst)(
    uint32_t n_triggers, float trigger_rate_hz, bool preemptive_noint)
{
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    uint32_t trigger_period_cyc = (uint32_t)(cycles_per_us * 1000000.0f / trigger_rate_hz);

    // Compute outlier threshold
    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * cycles_per_us);
    uint32_t nominal_burst_cyc = 0;
    for (int b = 0; b < bcm_bits; b++) {
        nominal_burst_cyc += base_cycles * (1U << b) + PIO_ON_OVERHEAD_CYCLES + 50;
    }
    stat_outlier_threshold = nominal_burst_cyc * 2;

    // Pre-emptive noInterrupts guard (Strategy A):
    // Enter noInterrupts this many cycles BEFORE expected trigger.
    // Pipeline/branch predictor settles during the wait.
    uint32_t guard_cyc = preemptive_noint ? (uint32_t)(5.0f * cycles_per_us) : 0;

    // Initial precompute from frame 0
    current_frame_idx = 0;
    memcpy(pixel_data, test_frames[0], sizeof(pixel_data));
    precompute_bcm_data();

    // Reset PIO SM
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0xFFFFF);

    // Lock out Core 1
    multicore_lockout_start_blocking();

    // Warm-up (100 triggers with noInterrupts)
    noInterrupts();
    {
        uint8_t wr = 0;
        uint32_t wu_start = m33_hw->dwt_cyccnt;
        for (uint32_t w = 0; w < 100; w++) {
            while ((m33_hw->dwt_cyccnt - wu_start) < trigger_period_cyc) {}
            wu_start += trigger_period_cyc;
            gpio_set_mask64(row_on_mask[wr]);
            for (int b = 0; b < bcm_bits; b++) {
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][0]);
                pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[wr][b][1]);
                while (!pio_interrupt_get(pio_hw_inst, 0)) {}
                pio_interrupt_clear(pio_hw_inst, 0);
            }
            gpio_clr_mask64(row_on_mask[wr]);
            wr++;
            if (wr >= active_rows) wr = 0;
        }
    }
    interrupts();
    stats_reset();

    // Disable SysTick during scan
    systick_hw->csr &= ~1u;

    // Timing for precompute measurement
    uint32_t precompute_cyc_total = 0;
    uint32_t precompute_count = 0;

    // Main trigger loop
    uint32_t trigger_start = m33_hw->dwt_cyccnt;
    uint8_t row = 0;
    uint32_t frame_swaps = 0;

    for (uint32_t t = 0; t < n_triggers; t++) {
        // --- IDLE PHASE: between triggers ---
        // Incremental precompute: at each trigger, precompute the NEXT row's
        // BCM data. When row wraps to 0 (frame boundary), swap to next frame.
        // This spreads 173µs of precompute across 20 idle windows (~8.7µs each).
        if (n_test_frames > 1) {
            // At frame boundary: swap to next test frame
            if (row == 0 && t > 0) {
                current_frame_idx = (current_frame_idx + 1) % n_test_frames;
                memcpy(pixel_data, test_frames[current_frame_idx], sizeof(pixel_data));
                frame_swaps++;
            }

            // Precompute the NEXT row that will be scanned (current row is
            // already precomputed from the previous idle period or initial setup)
            uint8_t next_row = (row + 1 >= active_rows) ? 0 : row + 1;
            uint32_t pc_start = m33_hw->dwt_cyccnt;
            precompute_bcm_row(next_row);
            uint32_t pc_end = m33_hw->dwt_cyccnt;
            precompute_cyc_total += (pc_end - pc_start);
            precompute_count++;
        }

        if (preemptive_noint && guard_cyc > 0) {
            // Strategy A: enter noInterrupts EARLY, before trigger time
            uint32_t guard_target = trigger_start + trigger_period_cyc - guard_cyc;
            while ((m33_hw->dwt_cyccnt - trigger_start) < (trigger_period_cyc - guard_cyc)) {}
            noInterrupts();
            // Now wait for actual trigger time
            while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
        } else {
            // Wait for trigger, then disable interrupts
            while ((m33_hw->dwt_cyccnt - trigger_start) < trigger_period_cyc) {}
            noInterrupts();
        }
        trigger_start += trigger_period_cyc;

        // --- BURST PHASE: one row, all bit-planes ---
        uint32_t burst_start = m33_hw->dwt_cyccnt;

        gpio_set_mask64(row_on_mask[row]);
        for (int b = 0; b < bcm_bits; b++) {
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][0]);
            pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, bcm_plane_data[row][b][1]);
            while (!pio_interrupt_get(pio_hw_inst, 0)) {}
            pio_interrupt_clear(pio_hw_inst, 0);
        }
        gpio_clr_mask64(row_on_mask[row]);

        uint32_t burst_end = m33_hw->dwt_cyccnt;
        interrupts();

        stats_update(burst_end - burst_start, 0);

        row++;
        if (row >= active_rows) row = 0;
    }

    // Re-enable SysTick and resume Core 1
    systick_hw->csr |= 1u;
    multicore_lockout_end_blocking();

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    all_off();

    // Report
    stats_print_fields("  RAMBURST", "burst", nullptr);

    if (stat_count > 0) {
        float mean_burst = cycles_to_us((uint32_t)(stat_on_sum / stat_count));
        float period_us = 1000000.0f / trigger_rate_hz;
        Serial.print("  Duty cycle: ");
        Serial.print(100.0f * mean_burst / period_us, 1);
        Serial.println("%");
        Serial.print("  Fits in 13us? ");
        Serial.println(mean_burst <= 13.0f ? "YES" : "NO");
        Serial.print("  Fits in 15us? ");
        Serial.println(mean_burst <= 15.0f ? "YES" : "NO");
    }

    Serial.print("  Frame swaps: ");
    Serial.print(frame_swaps);
    Serial.print(" (");
    Serial.print(n_test_frames);
    Serial.println(" frames in rotation)");

    if (precompute_count > 0) {
        float avg_precompute_us = cycles_to_us(precompute_cyc_total / precompute_count);
        Serial.print("  Precompute avg: ");
        Serial.print(avg_precompute_us, 3);
        Serial.print("us (");
        Serial.print(precompute_count);
        Serial.println(" swaps)");
    }

    if (preemptive_noint) {
        Serial.print("  Strategy A: pre-emptive noInterrupts, guard=");
        Serial.print(cycles_to_us(guard_cyc), 1);
        Serial.println("us");
    }
}

static void cmd_ramburst(const char* arg) {
    // Parse: RAMBURST <n_triggers> [rate_hz] [n_frames] [P]
    // P = pre-emptive noInterrupts (Strategy A)
    uint32_t n = 10000;
    float rate = 8000.0f;
    uint8_t nf = 8;
    bool preemptive = false;

    if (arg && *arg) {
        char* end;
        n = strtoul(arg, &end, 10);
        if (n == 0 || n > 1000000) {
            Serial.println("ERR: RAMBURST <1-1000000> [rate_hz] [n_frames] [P]");
            return;
        }
        while (*end == ' ') end++;
        if (*end >= '0' && *end <= '9') {
            rate = strtof(end, &end);
        }
        while (*end == ' ') end++;
        if (*end >= '0' && *end <= '9') {
            nf = (uint8_t)strtoul(end, &end, 10);
        }
        while (*end == ' ') end++;
        if (*end == 'P' || *end == 'p') {
            preemptive = true;
        }
    }

    if (!pio_loaded && !pio_init_program()) return;

    bool was_running = running;
    running = false;
    all_off();
    col_pins_to_pio();

    // Initialize test frames (ramp pattern)
    init_test_frames_ramp(nf);

    // Drain serial buffer
    delay(50);
    while (Serial.available()) Serial.read();

    Serial.println("--- RAMBURST START ---");
    Serial.print("BCM bits=");
    Serial.print(bcm_bits);
    Serial.print("  Base T=");
    Serial.print(bcm_base_on_us, 3);
    Serial.print("us  Rows=");
    Serial.print(active_rows);
    Serial.print("  Triggers=");
    Serial.print(n);
    Serial.print("  Frames=");
    Serial.print(n_test_frames);
    Serial.print("  Rate=");
    Serial.print(rate, 1);
    Serial.print("Hz  Preemptive=");
    Serial.println(preemptive ? "YES" : "NO");

    run_ramburst(n, rate, preemptive);

    Serial.println("--- RAMBURST END ---");

    col_pins_to_sio();
    stats_reset();
    running = was_running;
    if (running) count = 0;
}

// ---------------------------------------------------------------------------

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
static void __attribute__((noinline)) __not_in_flash_func(run_n_pulses)(uint32_t n, int mode) {
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
static void __attribute__((noinline)) __not_in_flash_func(run_rowtime)(uint32_t n_iters) {
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
static void __attribute__((noinline)) __not_in_flash_func(run_scan_frames)(uint32_t n_frames) {
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
        if ((f % 100 == 99) && user_wants_stop()) {
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
static void __attribute__((noinline)) __not_in_flash_func(run_pio_rowtime)(uint32_t n_iters) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    // Pre-invert column pattern for PIO (1=HIGH=OFF, 0=LOW=ON)
    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;

    // Reset and enable PIO SM (restart clears pindirs on RP2350, so re-set them)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
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
static void __attribute__((noinline)) __not_in_flash_func(run_pio_scan_frames)(uint32_t n_frames, int mode) {
    stats_reset();
    if (!dwt_available) {
        Serial.println("ERR: DWT not available");
        return;
    }

    precompute_scan_masks();

    uint32_t pio_col_word = (~col_pattern) & 0xFFFFF;
    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;

    // Reset and enable PIO SM (restart clears pindirs on RP2350, so re-set them)
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx, COL_PIN[0], PANEL_SIZE, true);
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
        if ((f % 100 == 99) && user_wants_stop()) {
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
    Serial.println("DMASCAN <N>      Hybrid DMA+ISR scan (DMA cols, ISR rows)");
    Serial.println("DMATEST          Step-by-step hybrid scan debug");
    Serial.println("== Multi-SM PIO (Phase 3d) ==");
    Serial.println("MSMSCAN <N>      Multi-SM PIO scan (zero CPU, zero jitter)");
    Serial.println("MSMTEST          Step-by-step multi-SM debug");
    Serial.println("== Burst Mode (2P sync simulation) ==");
    Serial.println("BURST <N> [Hz]   Burst scan at trigger rate (default 8000 Hz)");
    Serial.println("                 noInterrupts during scan, free-running idle");
    Serial.println("== BCM Burst Mode (Phase 4) ==");
    Serial.println("BCM <bits>       Set BCM bit depth (1-8, default 4)");
    Serial.println("BCMON <us>       Set BCM base time T (default 0.5 us)");
    Serial.println("FILL <intensity> Fill all pixels with value");
    Serial.println("GRADIENT         Row-varying intensity gradient");
    Serial.println("BCMBURST <N> [Hz] [A|B|C]  BCM burst scan");
    Serial.println("                 A=PIO, B=DMA, C=MSM (default A, 8000 Hz)");
    Serial.println("BCMDEMO          Ramp 0→max intensity, 1s/step, timing data");
    Serial.println("BCMVISUAL [sec]  Visual test: 4 levels x sec (SRAM reload @ 400Hz)");
    Serial.println("REBOOT           Reboot into BOOTSEL (USB flash) mode");
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

static void cmd_dmascan(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: DMASCAN <1-1000000>");
        return;
    }

    if (!pio_loaded && !pio_init_program()) return;
    if (!dma_channel_claimed && !dma_init_channel()) return;

    bool was_running = running;
    running = false;
    all_off();

    uint32_t pio_delay = (on_cycles > PIO_ON_OVERHEAD_CYCLES)
                       ? (on_cycles - PIO_ON_OVERHEAD_CYCLES) : 0;
    float pio_on_us = (float)(pio_delay + PIO_ON_OVERHEAD_CYCLES) / (float)cycles_per_us;

    Serial.println("--- DMASCAN START ---");
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
    Serial.println(n);

    run_hybrid_scan_frames(n);

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

    Serial.println("--- DMASCAN END ---");

    stats_reset();
    running = was_running;
    if (running) count = 0;
}

static void cmd_msmscan(const char* arg) {
    uint32_t n = (uint32_t)atol(arg);
    if (n == 0 || n > 1000000) {
        Serial.println("ERR: MSMSCAN <1-1000000>");
        return;
    }

    if (!msm_loaded && !msm_init_programs()) return;
    if (!msm_dma_claimed && !msm_init_dma()) return;

    bool was_running = running;
    running = false;
    all_off();

    uint32_t pio_delay = (on_cycles > MSM_COL_ON_OVERHEAD)
                       ? (on_cycles - MSM_COL_ON_OVERHEAD) : 0;
    float pio_on_us = (float)(pio_delay + MSM_COL_ON_OVERHEAD) / (float)cycles_per_us;

    Serial.println("--- MSMSCAN START ---");
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
    Serial.println(n);

    msm_run_scan_frames(n);

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

    Serial.println("--- MSMSCAN END ---");

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

    // Drain any trailing bytes from the command (e.g., \n after \r)
    // This prevents scan loops from seeing stale serial data
    delay(1);  // Let USB deliver any remaining bytes from the same packet
    while (Serial.available()) Serial.read();

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
    } else if (strcmp(cmd_buf, "BURST") == 0) {
        cmd_burst(args);
    } else if (strcmp(cmd_buf, "DMASCAN") == 0 && args) {
        cmd_dmascan(args);
    } else if (strcmp(cmd_buf, "DMATEST") == 0) {
        if (!pio_loaded && !pio_init_program()) return;
        if (!dma_channel_claimed && !dma_init_channel()) return;
        bool was_running = running;
        running = false;
        all_off();
        hybrid_debug_test();
        running = was_running;
    } else if (strcmp(cmd_buf, "MSMSCAN") == 0 && args) {
        cmd_msmscan(args);
    } else if (strcmp(cmd_buf, "BCM") == 0 && args) {
        cmd_bcm_set(args);
    } else if (strcmp(cmd_buf, "BCMON") == 0 && args) {
        cmd_bcmon(args);
    } else if (strcmp(cmd_buf, "FILL") == 0 && args) {
        cmd_fill(args);
    } else if (strcmp(cmd_buf, "GRADIENT") == 0) {
        cmd_gradient();
    } else if (strcmp(cmd_buf, "BCMBURST") == 0) {
        cmd_bcmburst(args);
    } else if (strcmp(cmd_buf, "BCMDEMO") == 0) {
        cmd_bcmdemo();
    } else if (strcmp(cmd_buf, "BCMVISUAL") == 0) {
        cmd_bcmvisual(args);
    } else if (strcmp(cmd_buf, "RAMBURST") == 0) {
        cmd_ramburst(args);
    } else if (strcmp(cmd_buf, "REBOOT") == 0) {
        Serial.println("Rebooting into BOOTSEL mode...");
        Serial.flush();
        delay(100);
        reset_usb_boot(0, 0);
    } else if (strcmp(cmd_buf, "MSMTEST") == 0) {
        if (!msm_loaded && !msm_init_programs()) return;
        if (!msm_dma_claimed && !msm_init_dma()) return;
        bool was_running = running;
        running = false;
        all_off();
        msm_debug_test();
        running = was_running;
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
// Core 1: install multicore lockout handler + empty loop.
// Both setup1() and loop1() must be defined for lockout to work with Arduino-Pico.
void setup1() {
    multicore_lockout_victim_init();
}
void loop1() {
    // Empty — Arduino-Pico USB stack runs via interrupts on Core 1
}

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
