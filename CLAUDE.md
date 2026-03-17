# G6 Panels Test Firmware

## Project Goal

Characterize and optimize LED timing on G6 20×20 passive LED matrix panels driven by RP2354 MCU (RP2350 + 8MB PSRAM) for two use cases:

1. **2P microscope synchronization**: externally triggered display synchronized to laser scanning
2. **General-purpose linear intensity control**: free-running BCM with calibrated brightness, suitable for behavioral experiments

## Critical Application Constraints (2P Microscopy)

- **Frame period**: 125 µs (8 kHz line rate)
- **Scan window**: ~12–15 µs (turnaround/flyback time) — **all 20 rows must be scanned within this window**
- **Per-row budget**: 15 µs ÷ 20 rows = **0.75 µs/row total** (ON time + overhead)
- **Relevant ON times**: 0.25–1.0 µs/row. ON times ≥ 5 µs are irrelevant to this application.
- **Idle time**: ~110 µs between scan bursts — USB/system interrupts can run freely here
- **Jitter tolerance**: Frame-to-frame timing must be consistent. Jitter > a few µs on a 15 µs window is problematic — it means the display update bleeds into the active imaging region.
- **Grayscale**: BCM intensity control (4-bit = 16 levels) within the scan window
- **Trigger**: External GPIO trigger from microscope marks the start of each scan window

**Key insight for jitter management**: The scan is a short burst (~15 µs) followed by long idle (~110 µs). If interrupts can be fully disabled during just the scan burst and re-enabled during idle, jitter should be near-zero. This favors PIOSCAN (`noInterrupts()` compatible) over MSMSCAN/DMASCAN (which require ISRs during scan).

## Hardware

- **MCU**: RP2354 (dual ARM Cortex-M33 @ 150 MHz, 520KB SRAM, 8MB PSRAM)
- **Panel**: 20×20 passive LED matrix, Janelia batch (reversed LED polarity: ON = col LOW + row HIGH)
- **Drivers**: UCC27517 gate drivers
- **Column pins**: GP1–GP20 (contiguous, ideal for PIO)
- **Row pins**: GP21–GP31 + GP36–GP44 (gap at GP32–35)
- **Framework**: Arduino-Pico (earlephilhower), PlatformIO build

## Key Technical Details

- **Zero jitter** requires: `__not_in_flash_func()` on all timing code (avoids XIP cache eviction from Core 1 USB stack) + `noInterrupts()` on Core 0
- **DWT cycle counter** for timing: `m33_hw->dwt_cyccnt` via `#include <hardware/structs/m33.h>`
- **PIO** drives columns via `out pins, 20` (GP1 base, 20 contiguous pins); CPU manages rows (split pin ranges)
- **Batch GPIO**: `gpio_set_mask64()` / `gpio_clr_mask64()` for pattern-independent row switching
- **Core 1** runs USB stack independently — do not disable interrupts on Core 1
- **Coordinate mapping**: layout (row,col) → schematic (row,col) uses NUM_COLOR=4 interleaving; see `utilities.cpp`

## Build & Flash

```bash
# Build
~/.platformio/penv/bin/pio run -d test_firmware/single_led

# Flash option 1: REBOOT command (preferred — no manual button press)
# Send "REBOOT" over serial → device enters BOOTSEL → cp UF2 → auto-reboots
python3 test_firmware/single_led/auto_test.py flash

# Flash option 2: manual BOOTSEL + tap RESET on board, then:
cp test_firmware/single_led/.pio/build/pico/firmware.uf2 /Volumes/RP2350/

# Serial port (after reboot) — port name varies between connections
/dev/cu.usbmodem2101  # or /dev/cu.usbmodem21401, etc.
```

## Autonomous Test Loop

**User expectation**: Claude should run the full build→flash→measure→evaluate loop autonomously, only prompting the user for decisions (not routine steps). Use the tools below for visibility and robustness.

### Status visibility
The user can monitor progress from a separate terminal:
```bash
# Live status (updates at least once per minute):
watch -n 2 cat /tmp/g6_test_status.txt

# Full log with timestamps:
tail -f /tmp/g6_test_log.txt
```

### Test runner tool
```bash
# Full pipeline: build, flash, burst sweep
python3 test_firmware/single_led/auto_test.py full

# Just burst jitter sweep (firmware already loaded):
python3 test_firmware/single_led/auto_test.py burst_sweep

# Compare PIOSCAN (continuous) vs BURST (triggered):
python3 test_firmware/single_led/auto_test.py comparison

# Single command:
python3 test_firmware/single_led/auto_test.py cmd "BURST 10000"
```

### Robustness rules for Claude
- **Always** use `auto_test.py` or at minimum `status_dashboard.update_status()` so the user has visibility
- **Never** run serial commands in bare Python snippets — use `auto_test.py cmd` which handles reconnection and timeouts
- **On serial errors**: reconnect and retry (up to 3 times), don't just stop
- **On flash failures**: report clearly, ask user to manually BOOTSEL only as last resort
- **Post results after each measurement**, don't batch — the user wants to see progress
- **Keep serial port name flexible** — it changes between connections (usbmodem2101, usbmodem21401, etc.)

## Completed Phases

- **Phase 0**: PlatformIO setup
- **Phase 1**: Single-LED pulse timing (serial-controlled sweep, jitter characterization, RAM fix)
- **Phase 2**: Skipped (DWT+RAM is optimal)
- **Phase 3a**: CPU-based full-panel row scanning (batch GPIO, automated + visual tests)
- **Phase 3b**: PIO-based scanning (PIO columns + CPU rows, 0.62 µs/row overhead)
- **Phase 3c**: DMA-fed PIO — hybrid DMA columns + ISR row switching (0.58 µs/row overhead, CPU free between ISRs)
- **Phase 3d**: Multi-SM PIO — dual PIO blocks (row SM on PIO1, col SM on PIO0) with bridge ISRs (0.37 µs/row overhead, ~3 µs jitter at 10 µs ON). Visually verified LEDs active during scanning.
- **Phase 3e**: Burst-mode scanning — simulated 8 kHz trigger, `noInterrupts()` during scan burst. Confirmed zero jitter (0.000 µs) for PIOSCAN burst mode.
- **Phase 4**: BCM grayscale — single-row-per-trigger BCM with 3 modes (PIO/DMA/MSM). Mode A (PIO) achieves 0.007 µs jitter, 0 outliers. 4-bit BCM at T=0.5 µs: 9.5 µs burst, fits 13 µs budget with 3.5 µs margin. 400 Hz frame rate.

## Key Technical Findings

### SIO is NOT DMA-accessible
**DMA cannot write to SIO GPIO registers** (`sio_hw->gpio_set`, `gpio_clr`, etc. at 0xD0000000). The SIO is a per-core peripheral; DMA bus masters are not attributed to either core, so writes are silently ignored. Row switching MUST involve CPU (ISR or polling) or PIO — not pure DMA.

### Row overhead floor is ~0.37 µs with multi-SM PIO bridge ISRs
SCAN (CPU): 0.76 µs/row. PIOSCAN (CPU polling): 0.60 µs/row. DMASCAN (ISR): 0.57 µs/row. MSMSCAN (bridge ISRs): 0.37 µs/row. The bridge ISR approach beats single-ISR because the ISR body is simpler (clear + force flag vs clear + 2× gpio_mask64).

### GPIOBASE is essential for cross-range PIO on RP2350
Column pins (GP1-20) and row pins (GP21-44) span different GPIO ranges. No single PIO block covers both. Solution: PIO0 (GPIOBASE=0, GPIO 0-31) for columns, PIO1 (GPIOBASE=16, GPIO 16-47) for rows. Arduino-Pico defaults all blocks to GPIOBASE=0 — must set `pio1->gpiobase = 16` explicitly before loading programs. SDK pin functions (`sm_config_set_out_pins`, `pio_sm_set_consecutive_pindirs`) take absolute GPIO numbers; hardware subtracts GPIOBASE internally.

### GP32-35 gap is harmless in PIO
PIO1's `out pins, 24` writes to bits 11-14 (GP32-35), but those pins keep SPI funcsel since `pio_gpio_init()` is not called for them. Pin mux ignores PIO output for those positions. No tri-stating needed, no SPI bus instability.

### `pio_sm_restart()` clears pindirs on RP2350
Calling `pio_sm_restart()` resets the SM's pin directions to input, causing LEDs to go dark. Fix: skip restart, use `pio_sm_clear_fifos()` + `pio_sm_exec(pio_encode_jmp(offset))` to reset PC while preserving pin state. Also: after switching pins to SIO funcsel (for manual GPIO), must switch back to PIO funcsel before PIO-driven scanning.

### Scanning mode tradeoffs (jitter-focused, at application-relevant ON times)

At ON = 0.25–0.5 µs, 10k frames:
| Mode | Row overhead | Jitter (ON=0.25) | Jitter (ON=0.5) | `noInterrupts()` compatible? |
|------|-------------|------------------|------------------|------------------------------|
| PIOSCAN | 0.61 µs | **1.69 µs** | **1.58 µs** | Yes — lowest jitter |
| MSMSCAN | 0.37 µs | 11.77 µs | 23.02 µs | No (needs bridge ISRs) |
| DMASCAN | 0.56 µs | 5.23 µs | 15.94 µs | No (needs row ISR) |
| SCAN (CPU) | 0.76 µs | ~5 µs | — | Yes |

**For 2P sync (15 µs scan window)**: PIOSCAN is the only viable mode. It's `noInterrupts()`-compatible, giving the lowest jitter. Its 0.61 µs overhead yields 0.61+ON total per row. At ON=0.5 µs → 22.3 µs frame (too slow for 20 rows). **Reducing to 12-14 rows or overclocking to 200 MHz may be needed.**

MSMSCAN has lower overhead but its ISR-dependent jitter (11–23 µs at short ON) makes it unsuitable for the tight 15 µs window. MSMSCAN may still be useful for free-running BCM display where jitter tolerance is higher.

### Burst-mode BCM achieves zero jitter (Phase 4 — RESOLVED)
Single-row-per-trigger BCM with `noInterrupts()` during the scan burst (Mode A / PIOSCAN) gives **0.007 µs jitter** (1 CPU cycle) and **0 outliers** across all tested configurations. The burst architecture — `noInterrupts()` for ~10 µs scan, `interrupts()` for ~115 µs idle — completely eliminates timing variation.

### BCM burst timing budget (Phase 4 results)
At 150 MHz, 8 kHz trigger, Mode A (PIO + noInterrupts):
| Config | Burst (µs) | Jitter (µs) | Fits 13µs? | Margin |
|--------|-----------|-------------|------------|--------|
| 4-bit T=0.50µs | 9.51 | 0.007 | YES | 3.5µs |
| 4-bit T=0.25µs | 5.77 | 0.007 | YES | 7.2µs |
| 4-bit T=0.75µs | 13.27 | 0.007 | NO | -0.3µs |
| 3-bit T=1.00µs | 8.59 | 0.007 | YES | 4.4µs |
| 3-bit T=1.50µs | 12.09 | 0.007 | YES | 0.9µs |

**Recommended**: 4-bit BCM, T=0.5 µs → 16 intensity levels, 9.5 µs burst, 3.5 µs margin. Frame rate = 400 Hz.

Modes B (DMA) and C (MSM/DMA) are ~1.3 µs faster but introduce 0.7–5.5 µs jitter from DMA interrupt overhead. Mode A is the only mode suitable for 2P sync.

## Next Steps

### Immediate
1. **External trigger interface** — GPIO interrupt or PIO `wait pin` for 2P sync. Replace simulated DWT trigger with real hardware input from microscope.
2. **Overclock to 200 MHz** — Optional. Would expand T range (4-bit T≤1.0 µs in 13 µs) but not required since T=0.5 µs already works at 150 MHz.

### After trigger is working
- **Optical characterization** — photodiode + external ADC to measure LED linearity, rise/fall time, load-dependent brightness. Build calibration LUT if needed.
- **Per-pixel pattern loading** — USB or SPI interface for host to send pixel_data[20][20] frames

### Completed explorations
- ~~**Multi-SM PIO (MSMSCAN)**~~ — Implemented (Phase 3d). Lowest overhead (0.37 µs) but ISR-dependent jitter makes it unsuitable for 2P sync.
- ~~**Pure DMA-fed PIO**~~ — Ruled out. SIO GPIO is not DMA-accessible.
- ~~**Burst-mode jitter**~~ — Measured (Phase 3e). Zero jitter confirmed with `noInterrupts()` burst architecture.
- ~~**BCM grayscale**~~ — Implemented (Phase 4). 4-bit BCM at T=0.5 µs fits in 9.5 µs with 3.5 µs margin. Zero jitter, zero outliers.

**Guiding principle**: Jitter and timing budget compliance come first. Mode A (PIO + noInterrupts) is the production architecture.

## Important Files

- `test_firmware/single_led/src/main.cpp` — primary firmware (all commands, scan loops, PIO setup)
- `test_firmware/single_led/src/constants.h/.cpp` — pin definitions
- `test_firmware/single_led/src/utilities.h/.cpp` — coordinate mapping
- `test_firmware/single_led/platformio.ini` — build config
- `test_firmware/single_led/G6_PANEL_TIMING_REPORT_v2.md` — shareable timing report
- `test_firmware/single_led/RESULTS.md` — detailed engineering results
- `test_firmware/single_led/auto_test.py` — autonomous build/flash/test runner (preferred for all testing)
- `test_firmware/single_led/status_dashboard.py` — status visibility for user (watch /tmp/g6_test_status.txt)
- `test_firmware/single_led/run_tests.py` — automated serial test harness (legacy)
- `test_firmware/single_led/visual_test.py` — interactive visual verification

## Serial Commands (current firmware)

Single-LED: `ON`, `OFF`, `POS row col`, `RUN n`, `STOP`, `STATS`, `JITTER n`, `SWEEP`, `SERIAL`
Scan: `ROWS n`, `PATTERN hex`, `SCAN n`, `ROWTIME n`
PIO: `PIOSCAN n`, `PIOSCAN2 n` (unprotected), `PIOROWTIME n`
Hybrid DMA+ISR: `DMASCAN n`, `DMATEST`
Multi-SM PIO: `MSMSCAN n`, `MSMTEST`
Burst mode (2P sync sim): `BURST n [rate_hz]` — default 8 kHz trigger rate, `noInterrupts()` during scan burst
BCM burst (Phase 4): `BCM bits`, `BCMON us`, `FILL intensity`, `GRADIENT`, `BCMBURST n [Hz] [A|B|C]`
System: `REBOOT` (enters BOOTSEL mode for flashing)
`HELP` for full list.
