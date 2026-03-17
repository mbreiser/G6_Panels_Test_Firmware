# G6 Panels Test Firmware

## Project Goal

Characterize and optimize LED timing on G6 20×20 passive LED matrix panels driven by RP2354 MCU (RP2350 + 8MB PSRAM) for two use cases:

1. **2P microscope synchronization**: externally triggered display with ~12–20 µs ON windows at 8 kHz, precise temporal alignment with laser scanning, grayscale intensity control via BCM
2. **General-purpose linear intensity control**: free-running BCM with calibrated brightness, suitable for behavioral experiments

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

# Flash: hold BOOTSEL + tap RESET on board, then:
cp test_firmware/single_led/.pio/build/pico/firmware.uf2 /Volumes/RP2350/

# Serial port (after reboot)
/dev/cu.usbmodem2101  # may vary
```

## Completed Phases

- **Phase 0**: PlatformIO setup
- **Phase 1**: Single-LED pulse timing (serial-controlled sweep, jitter characterization, RAM fix)
- **Phase 2**: Skipped (DWT+RAM is optimal)
- **Phase 3a**: CPU-based full-panel row scanning (batch GPIO, automated + visual tests)
- **Phase 3b**: PIO-based scanning (PIO columns + CPU rows, 0.62 µs/row overhead)

## Next Steps

### Near-term
- **Phase 4: BCM grayscale implementation** — 4-bit BCM with configurable T and bit depth. Start with free-running mode for validation.
- **External trigger interface** — GPIO interrupt or PIO `wait pin` for 2P sync mode. Implement rolling row advancement with configurable N rows/trigger.
- **Optical characterization** — photodiode + external ADC (likely Saleae Logic Pro 8) to measure:
  - LED rise/fall time
  - Intensity vs. duty cycle linearity
  - Load-dependent brightness variation (number of active columns)
  - Build calibration LUT if nonlinear

### Stretch (implement only if code stays readable)
- **Overclock to 200 MHz** — trivial config change, ~25% faster overhead. Validate stability first.
- **CPU/PIO work overlap** — restructure scan loop to overlap CPU row prep with PIO delay. Only if the code remains clear and maintainable.
- **Multi-SM PIO** (rows in PIO) — high complexity. Only pursue if we need < 13 µs frames AND the implementation can be well-documented and readable.
- **DMA-fed PIO** — zero CPU during scan. Very high complexity. Only if absolutely required for tight gated windows AND we can keep it understandable.

**Guiding principle for optimizations**: implement only if the resulting code is easy for others to read and maintain. Clever but opaque timing code is worse than slightly slower but clear code.

## Important Files

- `test_firmware/single_led/src/main.cpp` — primary firmware (all commands, scan loops, PIO setup)
- `test_firmware/single_led/src/constants.h/.cpp` — pin definitions
- `test_firmware/single_led/src/utilities.h/.cpp` — coordinate mapping
- `test_firmware/single_led/platformio.ini` — build config
- `test_firmware/single_led/G6_PANEL_TIMING_REPORT_v2.md` — shareable timing report
- `test_firmware/single_led/RESULTS.md` — detailed engineering results
- `test_firmware/single_led/run_tests.py` — automated serial test harness
- `test_firmware/single_led/visual_test.py` — interactive visual verification

## Serial Commands (current firmware)

Single-LED: `ON`, `OFF`, `POS row col`, `RUN n`, `STOP`, `STATS`, `JITTER n`, `SWEEP`, `SERIAL`
Scan: `ROWS n`, `PATTERN hex`, `SCAN n`, `ROWTIME n`
PIO: `PIOSCAN n`, `PIOSCAN2 n` (unprotected), `PIOROWTIME n`
`HELP` for full list.
