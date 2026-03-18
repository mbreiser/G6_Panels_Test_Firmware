# G6 20x20 LED Panel -- Production Architecture

Comprehensive reference for the RP2354-driven passive LED matrix,
covering everything learned during Phases 0-4 of timing characterization.
The target application is 2P microscope synchronization (8 kHz trigger,
15 us scan window, zero-jitter requirement).

---

## 1. System Overview

```
                          HOST COMPUTER
                     (pattern upload, control)
                              |
                         USB (debug)
                              |
    +---------------------------------------------------------+
    |                     RP2354 MCU                           |
    |  +-------------------+   +-------------------+          |
    |  |     Core 0        |   |     Core 1        |          |
    |  |  (scan loop,      |   |  (locked out in   |          |
    |  |   row GPIO,       |   |   production;     |          |
    |  |   trigger poll,   |   |   USB debug only) |          |
    |  |   BCM precompute) |   |                   |          |
    |  +--------+----------+   +-------------------+          |
    |           |                                              |
    |  +--------v----------------------------------------------+
    |  |                    PIO0                     |          |
    |  |  +----------------+  +-------------------+  |          |
    |  |  | Column SM      |  | Trigger SM        |  |          |
    |  |  | (led_col_prog) |  | (wait 1 pin GP0) |  |          |
    |  |  | out pins, 20   |  | irq set 2        |  |          |
    |  |  | GP1-GP20       |  | GP0               |  |          |
    |  |  | GPIOBASE=0     |  |                   |  |          |
    |  |  +-------+--------+  +-------------------+  |          |
    |  +----------|----------------------------------+          |
    |             |                                              |
    |  +----------v------------------------------------------+  |
    |  |  PIO1 (unused in production; MSMSCAN only)          |  |
    |  |  Row SM: out pins, 24 on GP21-GP44                  |  |
    |  |  GPIOBASE=16                                        |  |
    |  +-----------------------------------------------------+  |
    |             |                                              |
    |  +----------v------------------------------------------+  |
    |  |  SPI Slave (future: PIO-based on GP32, GP34, GP35)  |  |
    |  +-----------------------------------------------------+  |
    |             |                                              |
    +-----------+-|----------------------------------------------+
                | |
    +-----------v-v-----------+    GP0 <--- External trigger
    |   20x20 LED Matrix      |            (microscope sync)
    |   (Janelia, reversed    |
    |    polarity: ON = col   |
    |    LOW + row HIGH)      |
    |                         |
    |  Cols: GP1-GP20 (PIO0)  |
    |  Rows: GP21-GP31,       |
    |        GP36-GP44 (CPU)  |
    |  Drivers: UCC27517      |
    +-------------------------+
```

---

## 2. GPIO Pin Map

| GPIO    | Function            | Driver      | Notes                                       |
|---------|---------------------|-------------|---------------------------------------------|
| GP0     | External trigger    | PIO0        | `wait 1 pin 0` -> `irq set 2`              |
| GP1     | Column 0            | PIO0        | `out pins, 20` base pin                     |
| GP2     | Column 1            | PIO0        |                                             |
| GP3     | Column 2            | PIO0        |                                             |
| GP4     | Column 3            | PIO0        |                                             |
| GP5     | Column 4            | PIO0        |                                             |
| GP6     | Column 5            | PIO0        |                                             |
| GP7     | Column 6            | PIO0        |                                             |
| GP8     | Column 7            | PIO0        |                                             |
| GP9     | Column 8            | PIO0        |                                             |
| GP10    | Column 9            | PIO0        |                                             |
| GP11    | Column 10           | PIO0        |                                             |
| GP12    | Column 11           | PIO0        |                                             |
| GP13    | Column 12           | PIO0        |                                             |
| GP14    | Column 13           | PIO0        |                                             |
| GP15    | Column 14           | PIO0        |                                             |
| GP16    | Column 15           | PIO0        |                                             |
| GP17    | Column 16           | PIO0        |                                             |
| GP18    | Column 17           | PIO0        |                                             |
| GP19    | Column 18           | PIO0        |                                             |
| GP20    | Column 19           | PIO0        | Last column pin                             |
| GP21    | Row 0               | CPU (SIO)   | `gpio_set_mask64()` / `gpio_clr_mask64()`   |
| GP22    | Row 1               | CPU (SIO)   |                                             |
| GP23    | Row 2               | CPU (SIO)   |                                             |
| GP24    | Row 3               | CPU (SIO)   |                                             |
| GP25    | Row 4               | CPU (SIO)   |                                             |
| GP26    | Row 5               | CPU (SIO)   |                                             |
| GP27    | Row 6               | CPU (SIO)   |                                             |
| GP28    | Row 7               | CPU (SIO)   |                                             |
| GP29    | Row 8               | CPU (SIO)   |                                             |
| GP30    | Row 9               | CPU (SIO)   |                                             |
| GP31    | Row 10              | CPU (SIO)   |                                             |
| GP32    | Available           | --          | Future SPI CS; SPI funcsel, PIO ignores     |
| GP33    | CS_PIN (defined)    | --          | Defined in constants.cpp, currently unused   |
| GP34    | Available           | --          | Future SPI MOSI                             |
| GP35    | Available           | --          | Future SPI SCK                              |
| GP36    | Row 11              | CPU (SIO)   |                                             |
| GP37    | Row 12              | CPU (SIO)   |                                             |
| GP38    | Row 13              | CPU (SIO)   |                                             |
| GP39    | Row 14              | CPU (SIO)   |                                             |
| GP40    | Row 15              | CPU (SIO)   |                                             |
| GP41    | Row 16              | CPU (SIO)   |                                             |
| GP42    | Row 17              | CPU (SIO)   |                                             |
| GP43    | Row 18              | CPU (SIO)   |                                             |
| GP44    | Row 19              | CPU (SIO)   |                                             |
| GP45-47 | Available           | --          |                                             |

**Row pin gap**: GP32-GP35 sit between the two row groups but are NOT row pins.
When MSMSCAN uses PIO1 with `out pins, 24` (base GP21), bits 11-14 map to
GP32-35. Those pins retain SPI funcsel (not PIO funcsel), so PIO output is
ignored by the pin mux. No tri-stating or special handling is needed.

---

## 3. Timing Budget (per trigger at 8 kHz)

```
|<----------- 125 us trigger period ----------->|
|                                                |
|<- burst ->|<----------- idle ---------------->|
   9.42 us              ~115.6 us
|            |                                   |
|  noIntr()  |  interrupts() -- USB, SPI, etc.  |
```

| Item                             | Duration  | Notes                                   |
|----------------------------------|-----------|-----------------------------------------|
| Trigger period (8 kHz)           | 125 us    | Fixed by microscope line rate           |
| Scan burst (4-bit BCM, T=0.5us) | 9.42 us   | 1 row, all 4 bit-planes                |
| Available idle window            | ~115.6 us | USB, SPI DMA, buffer swap run here      |
| Scan window budget (2P)          | 15 us     | Turnaround/flyback time                 |
| Margin within scan window        | 5.6 us    | 15 - 9.42 = 5.58 us                    |
| `precompute_bcm_data()`          | ~0.4 us   | Estimated; runs once per 20 triggers    |
| Buffer swap (pointer swap)       | ~0.01 us  | Negligible                              |
| Per-row budget (20 rows)         | 0.75 us   | 15 us / 20 rows (full-frame reference)  |

**Frame rate**: At 8 kHz trigger with 20 rows scanned round-robin (1 row per
trigger), one complete frame = 20 triggers = 2.5 ms = **400 Hz frame rate**.

---

## 4. Zero-Jitter Recipe

Every ingredient below is required. Removing any single one reintroduces
measurable jitter. Listed in order of impact.

### 4.1 `__not_in_flash_func()` on all timing-critical code

**Why**: Core 1 runs the USB stack, which accesses flash via XIP. On RP2350,
XIP cache is shared between cores. A Core 1 flash access can evict a cache
line that Core 0 needs, causing a cache miss and multi-microsecond stall in
the scan loop. Marking functions `__not_in_flash_func()` places them in SRAM,
making execution latency deterministic regardless of Core 1 activity.

### 4.2 `multicore_lockout_start_blocking()`

**Why**: Pauses Core 1 entirely, eliminating all bus contention from the USB
stack. Without this, Core 1 flash/SRAM accesses cause 0.7-2.2 us jitter on
Core 0's scan timing. Core 1 stays locked for the entire measurement period
(~1.25s at 10k triggers / 8 kHz).

**Prerequisite**: Core 1 must have called `multicore_lockout_victim_init()` in
its `setup1()` function. This installs the lockout handler that allows Core 1
to be paused.

### 4.3 `noInterrupts()` during scan burst

**Why**: Prevents any ISR from inserting into the timing-critical scan path.
Even a zero-work ISR adds NVIC entry/exit overhead (~0.15-0.25 us). In
production, `noInterrupts()` is active for only ~9.5 us (the burst duration),
then `interrupts()` re-enables them for the ~115 us idle window.

### 4.4 `systick_hw->csr &= ~1u` -- disable SysTick

**Why**: SysTick is the last remaining Core 0 interrupt source after
`noInterrupts()` is lifted during idle. While SysTick alone was not measured
as the cause of the 0.8 us per-burst jitter, disabling it is good practice to
eliminate the possibility of SysTick firing at the exact moment `noInterrupts()`
is called, before the mask takes effect. `millis()`/`micros()` freeze during
the scan but resume after re-enabling.

### 4.5 100-trigger warm-up inside lockout + noInterrupts

**Why**: The first few dozen triggers after entering the lockout+noInterrupts
state show 0.7 us sporadic jitter. This is caused by cold CPU pipeline and
branch predictor state. Running 100 warm-up triggers (identical to the
measurement loop) stabilizes the pipeline. Stats are reset after warm-up so
these transient events do not pollute the measurement.

### 4.6 `setup1()` with `multicore_lockout_victim_init()`

**Why**: Core 1 must install the lockout handler before Core 0 can call
`multicore_lockout_start_blocking()`. Without this, the lockout call hangs
indefinitely. This is a one-time setup in Core 1's initialization.

```cpp
void setup1() {
    multicore_lockout_victim_init();
}
```

### 4.7 BCM plane data and row masks in SRAM (not PSRAM)

**Why**: The RP2354 has 8 MB of PSRAM, but PSRAM access latency is variable
(~100-200 ns with refresh pauses). All scan-critical data structures
(`bcm_plane_data[]`, `row_on_mask[]`) must reside in the 520 KB on-chip SRAM
for deterministic access. Static/global arrays default to SRAM in the
Arduino-Pico framework; no special annotation is needed unless using
`malloc()`/`new`.

### 4.8 PIO column driver at system clock (no divider)

**Why**: The PIO state machine runs at `clkdiv = 1.0` (150 MHz), giving
6.67 ns resolution per PIO cycle. Any clock divider would reduce timing
resolution proportionally and add quantization jitter to BCM plane durations.

---

## 5. Mode Comparison Table

### 5.1 Full-frame scanning modes (Phase 3a-3d, 20 rows per frame)

These modes scan all 20 rows in a single burst. Row overhead is the time
between one row's LED-OFF and the next row's LED-ON.

| Mode     | Architecture                  | Row overhead | Jitter ON=0.25us | Jitter ON=0.5us | noInterrupts compatible |
|----------|-------------------------------|-------------|------------------|------------------|------------------------|
| SCAN     | CPU rows + CPU cols           | 0.76 us     | ~5 us            | --               | Yes                    |
| PIOSCAN  | PIO cols + CPU row poll       | 0.61 us     | 1.69 us          | 1.58 us          | Yes (lowest jitter)    |
| DMASCAN  | DMA->PIO cols + ISR rows      | 0.56 us     | 5.23 us          | 15.94 us         | No (row ISR required)  |
| MSMSCAN  | PIO1 rows + PIO0 cols + ISRs  | 0.37 us     | 11.77 us         | 23.02 us         | No (bridge ISRs)       |

**Key takeaway**: Lower row overhead does NOT mean lower jitter. PIOSCAN has
the highest overhead of the PIO modes but the lowest jitter because it is the
only mode compatible with `noInterrupts()`.

### 5.2 Single-row-per-trigger BCM modes (Phase 4, 8 kHz trigger)

These modes scan one row per trigger with N-bit BCM (Binary Code Modulation).
"Full lockout" = `multicore_lockout` + warm-up + `systick` disable.

| Mode | Architecture     | Burst (4-bit, T=0.5us) | Jitter (full lockout) | Jitter (per-burst noInterrupts only) |
|------|-----------------|------------------------|----------------------|--------------------------------------|
| A    | PIO cols + CPU   | 9.42 us                | **0.000 us**         | 0.8 us max                           |
| B    | DMA->PIO cols    | ~8.1 us                | 0.7-5.5 us           | 0.7-5.5 us                           |
| C    | MSM/DMA cols     | ~8.1 us                | 0.7-5.5 us           | 0.7-5.5 us                           |

**Production choice**: Mode A. The ~1.3 us speed advantage of Modes B/C is
irrelevant given the 5.6 us margin. Mode A's zero jitter is essential for 2P
synchronization.

---

## 6. Dead Ends -- What Was Tried and Why It Failed

Each entry documents the attempt, measured result, root cause, and why it is
conclusive (i.e., not worth revisiting).

### 6.1 Pure DMA row switching

**Attempted**: Write row on/off patterns to `sio_hw->gpio_set` and
`sio_hw->gpio_clr` (address 0xD0000000+) via DMA transfers, eliminating the
CPU from row switching entirely.

**Result**: DMA writes were silently ignored. No GPIO transitions occurred.

**Root cause**: The SIO (Single-cycle I/O) block is a per-core peripheral on
RP2350. Each core has its own SIO instance. DMA bus masters are not attributed
to either core, so SIO does not recognize DMA writes. This is documented in
the RP2350 datasheet.

**Conclusive**: Hardware architecture limitation. Cannot be worked around with
different DMA configuration, addresses, or priorities. Row switching MUST
involve CPU (ISR or polling) or a PIO state machine.

### 6.2 MSMSCAN for zero-jitter scanning

**Attempted**: Row SM on PIO1 (GPIOBASE=16, covering GP16-47) drives rows via
`out pins, 24`. Column SM on PIO0 (GPIOBASE=0, covering GP0-31) drives columns
via `out pins, 20`. Synchronized via bridge ISRs: row SM fires `irq set 0`,
CPU ISR clears PIO0 flag 0 to release col SM; col SM fires `irq set 1`, CPU
ISR pushes next row pattern and clears PIO1 flag 1 to release row SM.

**Result**: 11-23 us jitter at ON times of 0.25-0.5 us (10k frames measured).
Lowest row overhead of all modes (0.37 us) but highest jitter.

**Root cause**: PIO IRQ flags are local to each PIO block. There is no
hardware path for PIO0 to directly signal PIO1 or vice versa. All cross-block
synchronization must go through CPU ISRs. ISRs cannot fire during
`noInterrupts()`, and without `noInterrupts()`, other interrupt sources cause
variable ISR latency.

**Conclusive**: RP2350 PIO architecture limitation. No firmware change can
create a hardware signaling path between PIO blocks.

### 6.3 Same-PIO-block row + column driving

**Attempted**: Place both the row SM and column SM on the same PIO block to
use PIO-local IRQ flags for synchronization (eliminating CPU ISRs).

**Result**: Not possible due to pin range constraints.

**Root cause**: Column pins GP1-GP20 require GPIOBASE=0 (covers GP0-31). Row
pins GP21-GP44 span two ranges: GP21-31 (covered by GPIOBASE=0) and GP36-44
(requires GPIOBASE >= 16). No single GPIOBASE value covers both GP1-15 (needs
base 0) and GP36-44 (needs base >= 16, covering up to base+31). A PIO block
can only have one GPIOBASE setting.

**Conclusive**: Pin assignment + PIO GPIOBASE architecture. Would require
board redesign to place all row pins within a 32-pin window that also
includes the column pins.

### 6.4 DMA-fed PIO for lower jitter than CPU polling

**Attempted**: Use DMA to feed column patterns to PIO TX FIFO instead of CPU
`pio_sm_put_blocking()`, hoping that removing CPU from the data path would
reduce jitter.

**Result**: 0.7-5.5 us jitter, worse than Mode A's CPU-polling approach.

**Root cause**: DMA is a separate bus master. When both CPU and DMA access the
bus simultaneously (even if CPU is just reading DWT cycle counter), bus
arbitration introduces variable latency. The AHB-Lite bus arbiter on RP2350
uses round-robin priority, so DMA and CPU contend non-deterministically. CPU
polling, by contrast, is entirely single-master: Core 0 reads PIO status and
writes PIO FIFO with no bus contention.

**Conclusive**: Multi-master bus architecture. DMA inherently introduces bus
arbitration jitter that cannot be eliminated.

### 6.5 `pio_sm_restart()` for SM reset

**Attempted**: Use `pio_sm_restart()` to reset PIO state machine state (PC,
shift registers, etc.) between scan frames.

**Result**: All LEDs went dark after restart. Pin directions were cleared.

**Root cause**: On RP2350, `pio_sm_restart()` resets the SM's pin direction
registers (pindirs) to input (all zeros). This is different from RP2040
behavior. Column pins switch from output to input, and PIO can no longer
drive them.

**Fix (implemented)**:
```cpp
// Instead of pio_sm_restart(), use:
pio_sm_set_enabled(pio, sm, false);
pio_sm_clear_fifos(pio, sm);
pio_sm_set_consecutive_pindirs(pio, sm, COL_PIN[0], PANEL_SIZE, true);
pio_sm_exec(pio, sm, pio_encode_jmp(offset));  // reset PC
pio_sm_set_enabled(pio, sm, true);
```

**Conclusive**: RP2350 SDK behavior. Always re-set pindirs after any SM restart.

### 6.6 Per-burst noInterrupts without warm-up

**Attempted**: Run the measurement loop with `multicore_lockout` active and
`noInterrupts()` only during each burst (not during idle), without a warm-up
phase.

**Result**: 0.7 us sporadic jitter, approximately 1 event per 10,000 triggers.

**Root cause**: Cold CPU pipeline and branch predictor state. The first
execution of the scan loop after entering the lockout state has unpredictable
branch prediction performance. After ~100 iterations, the branch predictor
stabilizes and jitter drops to 0.000 us.

**Fix (implemented)**: Run 100 warm-up triggers inside lockout+noInterrupts
before resetting stats and starting the actual measurement.

**Conclusive**: CPU microarchitecture effect. Warm-up is cheap (100 * 125 us
= 12.5 ms) and completely eliminates the issue.

### 6.7 Disabling SysTick to fix 0.8 us per-burst jitter

**Attempted**: Disable SysTick timer via `systick_hw->csr &= ~1u` to
eliminate the 0.8 us jitter seen with per-burst `noInterrupts()` (without
full lockout).

**Result**: Jitter persisted at 0.8 us even with SysTick disabled.

**Root cause**: The 0.8 us jitter is NOT caused by SysTick interrupts. It is
caused by CPU pipeline state disruption at the `noInterrupts()`/`interrupts()`
boundary itself. The `cpsid i` / `cpsie i` instructions affect the Cortex-M33
pipeline and branch predictor state, causing variable execution time on the
first few instructions after the transition.

**Fix**: Full lockout with warm-up (Section 4) eliminates this entirely.
SysTick is still disabled as defense-in-depth.

**Conclusive**: The root cause is CPU microarchitecture, not an interrupt
source. No amount of interrupt masking fixes it; only pipeline warm-up does.

---

## 7. Production Loop Design

### 7.1 Architecture

- **Core 0**: Runs the scan loop. Owns all GPIO, PIO0, and DMA.
- **Core 1**: Permanently locked out via `multicore_lockout_start_blocking()`.
  USB debug is only available before lockout or after explicit unlock.
- **PIO0 SM0**: Column driver (`led_col_program`), GP1-GP20.
- **PIO0 SM1**: Trigger detector (future), GP0.
  `wait 1 pin 0` -> `irq set 2` -> `wait 0 pin 0` (re-arm).
- **PIO0 SM2**: SPI slave (future), GP32/GP34/GP35.
- **PIO1**: Unused in production (was used for MSMSCAN evaluation).
- **Double buffer**: `display_buf` (active) and `recv_buf` (receiving SPI data).
  Pointer swap at frame boundary (row 0).
- **BCM precompute**: `precompute_bcm_data()` runs during idle after buffer swap.
  Converts `pixel_data[20][20]` intensity values into `bcm_plane_data[20][8][2]`
  PIO words.

### 7.2 Pseudocode

```
setup():
    // Core 1 setup
    setup1():
        multicore_lockout_victim_init()

    // Core 0 setup
    dwt_init()
    init_index_maps()          // layout-to-schematic coordinate mapping
    precompute_scan_masks()    // row_on_mask[], col masks
    pio_init_program()         // load led_col_program on PIO0 SM0
    // Future: init trigger SM on PIO0 SM1
    // Future: init SPI slave on PIO0 SM2, arm DMA -> recv_buf

    // Precompute initial BCM data
    precompute_bcm_data()

    // Lock Core 1 permanently
    multicore_lockout_start_blocking()

    // Disable SysTick
    systick_hw->csr &= ~1u

    // Enable PIO SM, push all-OFF init word
    pio_sm_set_enabled(pio0, sm0, true)
    pio_sm_put_blocking(pio0, sm0, 0xFFFFF)

    // Warm-up: 100 triggers
    noInterrupts()
    for w in 0..99:
        wait_for_trigger()
        scan_one_row(row_warmup)
        row_warmup = (row_warmup + 1) % 20
    interrupts()

production_loop():
    row = 0
    while true:
        // === IDLE phase (~115 us): interrupts enabled ===
        if frame_ready AND row == 0:
            swap(display_buf, recv_buf)
            frame_ready = false
            rearm_spi_dma(recv_buf)
            precompute_bcm_data()   // ~0.4 us

        // Wait for trigger
        // Option A (simulated): DWT cycle counter polling
        while (dwt_cyccnt - trigger_start) < trigger_period_cyc: pass
        trigger_start += trigger_period_cyc
        // Option B (production): PIO trigger flag
        // while !pio_interrupt_get(pio0, 2): pass
        // pio_interrupt_clear(pio0, 2)

        // === BURST phase (~9.5 us): noInterrupts ===
        noInterrupts()

        gpio_set_mask64(row_on_mask[row])       // row ON

        for b in 0..bcm_bits-1:
            pio_sm_put_blocking(pio0, sm0, bcm_plane_data[row][b][0])  // col pattern
            pio_sm_put_blocking(pio0, sm0, bcm_plane_data[row][b][1])  // delay count
            while !pio_interrupt_get(pio0, 0): pass   // wait for PIO done
            pio_interrupt_clear(pio0, 0)

        gpio_clr_mask64(row_on_mask[row])       // row OFF

        interrupts()

        row = (row + 1) % 20
```

### 7.3 Timing Breakdown of One Burst (4-bit BCM, T=0.5 us)

Each bit-plane b has ON time = T * 2^b:

| Bit-plane | ON time (us)     | PIO delay cycles | CPU overhead (us) |
|-----------|------------------|------------------|-------------------|
| b=0       | 0.50             | 70               | ~0.10             |
| b=1       | 1.00             | 145              | ~0.10             |
| b=2       | 2.00             | 295              | ~0.10             |
| b=3       | 4.00             | 595              | ~0.10             |
| **Total** | **7.50**         |                  | **~0.40**         |
| + row GPIO + PIO overhead |  |                  | **~1.5**          |
| **Burst** | **~9.42**        |                  |                   |

PIO ON overhead per plane = 5 cycles (33 ns): pull + mov + jmp_entry + mov + out.

---

## 8. Jitter Elimination Strategies (to explore)

Four strategies for eliminating the 0.8 us per-burst jitter seen when using
per-burst `noInterrupts()` without full Core 1 lockout. These are relevant
if USB debug access is needed during scanning.

### Strategy A: Pre-emptive noInterrupts with DWT deadline

Disable interrupts slightly before the expected trigger edge (e.g., 5 us
early based on DWT cycle counter). This ensures the CPU pipeline is in a
stable state when the burst begins. Re-enable during idle.

**Tradeoff**: Requires accurate trigger prediction. Trigger jitter from the
microscope itself would need to be < 5 us.

### Strategy B: PIO FIFO pre-load

Pre-load the PIO TX FIFO with the first bit-plane's data during idle (before
the trigger). When the trigger fires, PIO immediately begins driving columns
without waiting for CPU. CPU only needs to feed subsequent bit-planes.

**Tradeoff**: FIFO depth is 8 words (4 entries of pattern+delay). Only helps
with the first 1-2 bit-planes.

### Strategy C: Core 1 SPI with memory isolation

Run the SPI slave on Core 1 with its own dedicated SRAM bank (RP2350 has 10
banks). Core 0 scan loop and Core 1 SPI would access different SRAM banks,
eliminating bus contention without requiring lockout.

**Tradeoff**: Requires careful memory placement with linker scripts. USB
stack would also need to be on Core 1's banks.

### Strategy D: Selective IRQ disable

Instead of `noInterrupts()` (which disables ALL interrupts via `cpsid i`),
selectively disable only specific NVIC IRQs (USB, SysTick, DMA) while leaving
the interrupt system itself enabled. This avoids the pipeline disruption at
the `cpsid`/`cpsie` boundary.

**Tradeoff**: Must identify and disable every possible interrupt source.
Missing one reintroduces jitter.

---

## 9. BCM Burst Timing Budget Table

Definitive zero-jitter results from Phase 4. All measurements use Mode A
(PIO + CPU polling), full lockout, 100-trigger warm-up, 4-bit BCM, 8 kHz
trigger, 160,000 triggers measured.

| Bits | T (us) | Burst (us) | Jitter (us) | Outliers   | Fits 13 us? | Fits 15 us? | Margin (vs 15 us) |
|------|--------|-----------|-------------|------------|-------------|-------------|-------------------|
| 4    | 0.25   | 5.687     | 0.000       | 0 / 160k   | YES         | YES         | 9.3 us            |
| 4    | 0.50   | 9.420     | 0.000       | 0 / 160k   | YES         | YES         | 5.6 us            |
| 4    | 0.75   | 13.187    | 0.000       | 0 / 160k   | Marginal    | YES         | 1.8 us            |
| 4    | 1.00   | 16.920    | 0.000       | 0 / 160k   | NO          | NO          | -1.9 us           |
| 3    | 1.00   | 8.590     | 0.000       | 0 / 160k   | YES         | YES         | 6.4 us            |
| 3    | 1.50   | 12.090    | 0.000       | 0 / 160k   | YES         | YES         | 2.9 us            |

**Recommended configuration**: 4-bit BCM, T = 0.50 us.
- 16 intensity levels (0-15)
- 9.42 us burst fits within 15 us scan window with 5.6 us margin
- 400 Hz frame rate (20 rows at 8 kHz trigger)
- Zero jitter, zero outliers across 160k triggers

---

## 10. Key PIO Programs

### 10.1 Column Driver: `led_col_program` (10 instructions)

Used by Mode A (production) and Mode B (DMA variant). Runs on PIO0 with
GPIOBASE=0. OUT pins base=GP1, count=20.

```
addr 0: pull block           ; INIT: get all-OFF mask (0xFFFFF)
addr 1: mov  y, osr          ;       y = permanent all-OFF value
addr 2: pull block           ; [wrap_target] get column pattern (pre-inverted)
addr 3: out  pins, 20        ; set all 20 column pins -> LEDs ON where bit=0
addr 4: pull block           ; get delay count
addr 5: mov  x, osr          ; x = delay count
addr 6: jmp  x--, 6          ; delay loop: x+1 PIO cycles
addr 7: mov  osr, y          ; restore all-OFF mask from Y
addr 8: out  pins, 20        ; all columns OFF (HIGH = all bits 1)
addr 9: irq  wait 0          ; [wrap] signal done + stall until CPU clears flag
```

**Protocol**: Push `0xFFFFF` once at init (stored in Y). Then per bit-plane:
push column pattern (inverted: 0 = LED ON), push delay count. PIO drives
columns, delays, clears columns, signals IRQ, and stalls. CPU clears IRQ to
release PIO for the next bit-plane.

**ON time**: `(delay_count + 5)` PIO cycles. The 5-cycle overhead comes from:
pull(1) + out(1) + pull(1) + mov(1) + jmp_entry(1). At 150 MHz, 1 PIO cycle
= 6.67 ns.

**Column encoding**: Reversed polarity (Janelia panel). Bit = 0 drives pin
LOW = LED ON. Bit = 1 drives pin HIGH = LED OFF. Pattern is pre-inverted by
`precompute_bcm_data()`: `bcm_plane_data[row][bit][0] &= ~(1 << sch_col)` to
turn on a pixel.

### 10.2 MSM Row Program: `msm_row_program` (5 instructions)

Used by MSMSCAN mode (Phase 3d). Runs on PIO1 with GPIOBASE=16.
OUT pins base=GP21, count=24. Not used in production.

```
addr 0: pull block           ; INIT: get first row pattern
addr 1: out  pins, 24        ; [wrap_target] drive 24 row pins
addr 2: irq  set 0           ; signal: row is set -> fires PIO1 system IRQ
addr 3: wait 1 irq 1         ; wait for bridge ISR to set flag 1 (col done)
addr 4: pull block            ; get next row pattern from DMA
                              ; [wrap] -> addr 1
```

**Pin mapping**: The 24-bit output covers GP21-GP44 (relative to GPIOBASE=16,
that is pins 5-28). Bits 11-14 map to GP32-35 (the gap) but those pins
retain SPI funcsel, so PIO output is harmlessly ignored.

### 10.3 MSM Column Program: `msm_col_program` (11 instructions)

Used by MSMSCAN mode. Runs on PIO0 with GPIOBASE=0.
OUT pins base=GP1, count=20. Not used in production.

```
addr  0: pull block           ; INIT: get all-OFF mask (0xFFFFF)
addr  1: mov  y, osr          ;       y = permanent all-OFF value
addr  2: wait 1 irq 0         ; [wrap_target] wait for bridge ISR flag 0
addr  3: pull block            ; get column pattern from DMA
addr  4: out  pins, 20        ; columns ON
addr  5: pull block            ; get delay count from DMA
addr  6: mov  x, osr          ; x = delay
addr  7: jmp  x--, 7          ; delay loop
addr  8: mov  osr, y          ; restore all-OFF
addr  9: out  pins, 20        ; columns OFF
addr 10: irq  set 1           ; [wrap] signal cols done -> fires PIO0 system IRQ
```

**Bridge ISR synchronization**: PIO1 row SM sets IRQ flag 0 (system). CPU ISR
clears PIO0 local flag 0 (via `pio_sm_exec(pio0, sm, pio_encode_irq_set(false, 0))`),
releasing column SM. Column SM sets IRQ flag 1 (system). CPU ISR pushes next
row data and clears PIO1 local flag 1, releasing row SM. This cross-block
handshake adds 11-23 us jitter at short ON times.

---

## 11. Next Steps

### Phase 5a: External trigger via PIO

Replace the simulated DWT-based trigger with a real hardware trigger on GP0.
PIO program: `wait 1 pin 0` -> `irq set 2` -> `wait 0 pin 0` (re-arm for
next edge). Core 0 polls `pio_interrupt_get(pio0, 2)` in the idle phase.

**Risk**: Low. This is a straightforward PIO program (3 instructions). The
trigger signal from the microscope is a clean TTL edge.

### Phase 5b: SPI slave via PIO + DMA

Implement a PIO-based SPI slave on GP32 (CS), GP34 (MOSI), GP35 (SCK) to
receive pixel data from the host computer. DMA transfers received bytes
directly into `recv_buf`. A DMA completion interrupt (during idle phase)
sets `frame_ready = true`.

**Risk**: Medium. SPI slave timing must not interfere with scan timing. DMA
completion ISR must only fire during idle (enforced by `noInterrupts()` during
burst).

### Phase 5c: Double buffering integration + production loop

Integrate Phases 5a and 5b into the production loop from Section 7. Key
implementation details:
- Pointer swap (`display_buf` / `recv_buf`) at row 0 boundary
- `precompute_bcm_data()` runs once per frame (every 20 triggers) during idle
- SPI DMA re-arm after swap

### Phase 5d: Optical characterization with photodiode

Measure actual LED optical output with a photodiode + external ADC:
- LED rise/fall time
- Intensity linearity across BCM levels (0-15)
- Load-dependent brightness variation (1 LED vs 20 LEDs on same row)
- Build calibration LUT if linearity is poor

### Phase 5e: Per-pixel pattern loading from host

Design the host-side protocol for sending 20x20 pixel frames over SPI:
- Frame format: 400 bytes (1 byte per pixel, 4-bit intensity)
- Frame rate: up to 400 Hz (matching the 20-row scan rate)
- Flow control: CS-based or frame-sync signal
- Host software: Python script using `spidev` or FTDI adapter

---

## Appendix: Quick Reference

### Build and Flash

```bash
# Build
~/.platformio/penv/bin/pio run -d test_firmware/single_led

# Flash (preferred: auto-reboot via serial)
python3 test_firmware/single_led/auto_test.py flash

# Flash (manual: hold BOOTSEL, tap RESET, then copy)
cp test_firmware/single_led/.pio/build/pico/firmware.uf2 /Volumes/RP2350/
```

### Key Serial Commands

| Command                        | Description                                    |
|--------------------------------|------------------------------------------------|
| `BCM <bits>`                   | Set BCM bit depth (3-8)                        |
| `BCMON <us>`                   | Set BCM base time unit T                       |
| `FILL <intensity>`             | Fill all pixels with intensity (0-255)          |
| `GRADIENT`                     | Fill with test gradient pattern                 |
| `BCMBURST <N> [Hz] [A\|B\|C]` | Run N-trigger BCM burst scan                   |
| `PIOSCAN <N>`                  | PIO column scan, N frames                      |
| `BURST <N> [Hz]`              | Burst-mode scan (no BCM)                        |
| `REBOOT`                       | Enter BOOTSEL mode for flashing                |
| `HELP`                         | Full command list                               |

### Key Source Files

| File                        | Purpose                                          |
|-----------------------------|--------------------------------------------------|
| `src/main.cpp`              | All firmware: commands, scan loops, PIO setup     |
| `src/constants.h/.cpp`      | Pin definitions (`COL_PIN[]`, `ROW_PIN[]`, etc.)  |
| `src/utilities.h/.cpp`      | Layout-to-schematic coordinate mapping            |
| `platformio.ini`            | Build configuration                               |
| `auto_test.py`              | Autonomous build/flash/test runner                |
| `RESULTS.md`                | Detailed engineering results from all phases       |
