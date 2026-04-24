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

- **Zero jitter recipe** (4 ingredients, ALL required):
  1. `__not_in_flash_func()` **+ `__attribute__((noinline))`** on all timing code — prevents XIP cache eviction AND prevents compiler from silently inlining into flash callers
  2. `noInterrupts()` on Core 0 for the entire scan loop
  3. `multicore_lockout_start_blocking()` to pause Core 1 during scan (eliminates bus contention)
  4. 100-trigger warm-up inside lockout before measurement (eliminates cold-start pipeline artifacts)
  - **Without all 4**: jitter is 0.7-7+ µs. **With all 4**: jitter = 0.007 µs (1 CPU cycle).
- **CRITICAL: `noinline` is mandatory with `__not_in_flash_func`**: The compiler can inline `static __not_in_flash_func` functions into flash-resident callers, silently defeating the SRAM placement. This caused a 0→7 µs jitter regression that was very difficult to diagnose. Always use `__attribute__((noinline))` together with `__not_in_flash_func()` on timing-critical functions.
- **Core 1 lockout setup**: `setup1()` must call `multicore_lockout_victim_init()` AND `loop1()` must be defined (even if empty) for Arduino-Pico compatibility
- **DWT cycle counter** for timing: `m33_hw->dwt_cyccnt` via `#include <hardware/structs/m33.h>`
- **PIO** drives columns via `out pins, 20` (GP1 base, 20 contiguous pins); CPU manages rows (split pin ranges)
- **Batch GPIO**: `gpio_set_mask64()` / `gpio_clr_mask64()` for pattern-independent row switching
- **Core 1** runs USB stack independently — paused via multicore lockout during scan bursts, resumes during idle
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
- **Phase 4**: BCM grayscale — single-row-per-trigger BCM with 3 modes (PIO/DMA/MSM). **Zero jitter achieved** with multicore lockout + noInterrupts + noinline + warm-up. Full sweep: 640k measurements, all showing 0.007 µs jitter (1 cycle), 0 outliers. 4-bit BCM at T=0.5 µs: 9.49 µs burst, fits 15 µs with 5.5 µs margin. 400 Hz frame rate. Visually verified (BCMDEMO ramp test).
- **Phase 5a**: RAMBURST — production-realistic frame loading. 8 test frames cycling at 400 Hz from RAM, incremental per-row precompute (38 µs/row during 115 µs idle). **0.007 µs jitter** with frame swaps happening. Critical `noinline` bug found and fixed: compiler was silently inlining `__not_in_flash_func` into flash callers.
- **Phase 6**: External trigger + optical characterization — GP45 external trigger (bodge wire), zero jitter confirmed with real 8 kHz waveform generator. PHOTOCAL command cycles 16 BCM levels. Saleae Logic Pro 8 automation via Python API. Pulse-triggered averaging resolves BCM bit-plane structure (B0-B3 at 1:2:4:8 ratio) in photodiode signal. BCMWEIGHTS command for custom bit-plane weights (6.67 ns resolution) to enable linearity calibration. Ocean Insight Flame X spectrometer: LED peak at 570.8 nm. Spectrometer linearity measurement: 16 BCM levels, 50 ms integration, 20 nm window (560-580 nm). Key finding: brightness-per-microsecond decreases with longer bit-planes (B0=3163 cts/us to B3=2024 cts/us, 56% drop). BCMWEIGHTS optimizer converged to [1, 2, 5.02, 10.19] at T=0.7 us: monotonic, 2.5% max error, 12.7 us burst fits 15 us window. BCMORDER REVERSE command added to test if bit-plane scan order affects brightness.
- **Phase 7**: AD3 integration — Digilent Analog Discovery 3 replaces external function generator + Saleae for optical characterization. W1 wavegen drives GP45 trigger, scope Ch1 captures photodiode, Ch2 captures trigger reference. Two acquisition modes characterized: (1) **Record mode** (USB streaming): max 5 MHz single-ch or 2 MHz dual-ch clean (USB 2.0 bottleneck at ~5 MS/s total); (2) **Triggered single-shot mode** (on-device FPGA buffer): up to 100 MHz at 32K samples/channel (Config 1), no USB bottleneck. At 12.5 MHz dual-channel with 32K buffer = 2,621 µs window, covering full 2.5 ms row cycle. PIXEL command added to firmware for individual pixel control. Trigger-to-LED latency measured: 1.5–1.6 µs (GP45 edge to photodiode onset). Key finding: pixel_data[10] maps to 2 physical rows (trigger offsets 3 and 12 in 20-row cycle) due to coordinate interleaving — needs investigation.

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

### Burst-mode BCM achieves ZERO jitter with multicore lockout (Phase 4 — RESOLVED)
Full jitter sweep: 4 T values × 16 intensities × 10k frames at 8 kHz = **640,000 measurements**. With the zero-jitter recipe (multicore lockout + noInterrupts for entire loop + 100-trigger warm-up), **every single measurement shows 0.000 µs jitter**. Zero outliers.

**Jitter root cause and fix**: Without multicore lockout, Core 1 USB stack creates bus contention (0.7-2.2 µs jitter). `noInterrupts()` alone is insufficient because it only disables Core 0 interrupts — Core 1 continues running. The fix requires all three ingredients: (1) `multicore_lockout_start_blocking()` to pause Core 1, (2) `noInterrupts()` for the entire measurement loop, (3) 100-trigger warm-up inside the lockout to stabilize pipeline/branch predictor state.

### BCM burst timing budget (Phase 4 — zero jitter)
At 150 MHz, 8 kHz trigger, Mode A (PIO + multicore lockout + noInterrupts + warm-up):
| T (µs) | Burst (µs) | Jitter (µs) | Outliers | Fits 13 µs? | Fits 15 µs? | Margin |
|---------|-----------|-------------|----------|-------------|-------------|--------|
| 0.25 | 5.687 | **0.000** | 0/160k | YES | YES | 9.3 µs |
| 0.50 | 9.420 | **0.000** | 0/160k | YES | YES | 5.6 µs |
| 0.75 | 13.187 | **0.000** | 0/160k | ~YES | YES | 1.8 µs |
| 1.00 | 16.920 | **0.000** | 0/160k | NO | NO | -1.9 µs |

**Recommended**: 4-bit BCM, T=0.5 µs → 16 intensity levels, 9.42 µs burst, 5.6 µs margin to 15 µs budget. Frame rate = 400 Hz.

Modes B (DMA) and C (MSM/DMA) are ~1.3 µs faster but introduce jitter from DMA interrupt overhead. Mode A is the only mode suitable for 2P sync.

### External trigger zero jitter confirmed (Phase 6)
BCMBURST 10000 with real 8 kHz external trigger on GP45: 0.000 µs jitter, 9.507 µs burst. Per-burst noInterrupts + Core 1 lockout + warm-up achieves zero jitter even with external trigger.

### BCM bit-plane weights are fully configurable
`BCMWEIGHTS` command accepts arbitrary float weights (e.g., 1.2, 2.2, 4.1, 8.0) with 6.67 ns resolution (1 CPU cycle at 150 MHz). `BCMORDER REVERSE`/`NORMAL` toggles scan direction (B3->B0 vs B0->B3). Linearity optimizer (`bcm_weight_optimizer.py`) iteratively adjusts weights using spectrometer feedback. Default [1,2,4,8] weights are non-monotonic at T<=0.7 us due to per-bit-plane brightness decay (B0=3163 cts/us to B3=2024 cts/us). Optimized weights [1, 2, 5.02, 10.19] at T=0.7 us achieve monotonic linear response with 2.5% max error.

### Saleae Logic Pro 8 integration
Python automation via `saleae_capture.py` using Logic 2 API. Captures digital trigger + analog photodiode simultaneously. Pulse-triggered averaging across 499 pulses at 50 MHz resolves individual BCM bit-planes and ~0.4 µs PIO overhead gaps.

### Digilent Analog Discovery 3 (AD3) integration
Replaces external function generator + Saleae for optical characterization. Python control via raw ctypes to DWF SDK (`/Library/Frameworks/dwf.framework/dwf`). **Must use homebrew Python 3.14** — macOS Sequoia's system Python 3.9 crashes on dyld circular rpath when loading the DWF framework. The `pydwf` package has a version mismatch (expects SDK 3.20.1, installed 3.25.1) — use raw ctypes instead.

**Two acquisition modes:**

| Mode | Max Rate | Mechanism | Use Case |
|------|----------|-----------|----------|
| Record (streaming) | 5 MHz 1ch / 2 MHz 2ch | USB 2.0 transfer | Long captures (seconds) |
| Triggered single-shot | **100 MHz** 1ch or 2ch | On-device FPGA buffer | Short bursts at full resolution |

**Triggered mode is preferred** for BCM burst characterization. Key settings:
- `FDwfDeviceConfigOpen(idx, 1, ...)` — Config 1 gives 32K samples/channel (vs 16K default)
- `FDwfAnalogInAcquisitionModeSet(hdwf, acqmodeSingle)` — single-shot, not record
- Trigger on `trigsrcAnalogOut1` (W1 output) for hardware-aligned captures
- At 12.5 MHz: 32K samples = 2,621 µs window, covers full 2.5 ms (20-row) BCM cycle
- Re-arm rate: ~137 captures/sec (dual-channel) or ~727/sec (single-channel)
- **Classify captures by row position**: each trigger hits a random row in the 20-row cycle; post-hoc classification by PD peak location enables row-specific averaging

**AD3 exclusive access**: the WaveForms GUI app cannot be used simultaneously with SDK control. Kill all python3.14 processes if device appears busy. Serial port name changes between USB reconnections.

## Next Steps

### Immediate
1. **Investigate coordinate interleaving**: pixel_data[10] maps to 2 physical rows (trigger offsets 3 and 12). Check `utilities.cpp` NUM_COLOR=4 mapping — is this expected, or a bug?
2. **AD3 high-resolution burst characterization**: Use triggered mode at 100 MHz to resolve individual BCM bit-planes with 10 ns resolution. Capture single burst, classify by row, average.
3. **Test BCMORDER REVERSE** — Re-run optimizer with reverse bit-plane order.
4. **Probe LED pin (GP1) on AD3 Ch2** — Measure trigger-to-LED latency directly (eliminates photodiode bandwidth uncertainty). Currently measured at 1.5–1.6 µs via photodiode.
5. **PCB redesign** — See PCB_REDESIGN_ANALYSIS.md. Option B (add EINT trace to GP45) recommended for next revision.

### After optical calibration
- **Per-pixel pattern loading** — USB or SPI interface for host to send pixel_data[20][20] frames

### Completed explorations
- ~~**Multi-SM PIO (MSMSCAN)**~~ — Implemented (Phase 3d). Lowest overhead (0.37 µs) but ISR-dependent jitter makes it unsuitable for 2P sync.
- ~~**Pure DMA-fed PIO**~~ — Ruled out. SIO GPIO is not DMA-accessible.
- ~~**Burst-mode jitter**~~ — Measured (Phase 3e). Zero jitter confirmed with `noInterrupts()` burst architecture.
- ~~**BCM grayscale**~~ — Implemented (Phase 4). 4-bit BCM at T=0.5 µs: 9.42 µs burst, 0.000 µs jitter, 5.6 µs margin. Zero jitter achieved with multicore lockout + noInterrupts + warm-up.
- ~~**Multicore lockout for zero jitter**~~ — Implemented. lockout + noInterrupts + noinline + warm-up = 0.007 µs jitter.
- ~~**`noinline` requirement for `__not_in_flash_func`**~~ — Discovered (Phase 5a). Compiler silently inlines static `__not_in_flash_func` into flash callers, defeating SRAM placement. Caused 0→7 µs jitter regression. Fix: always pair with `__attribute__((noinline))`.
- ~~**RAM frame loading at 400 Hz**~~ — Validated (Phase 5a RAMBURST). Incremental per-row precompute (38 µs/row) fits in 115 µs idle. 0.007 µs jitter with 8-frame cycling.
- ~~**External trigger interface**~~ — Implemented (Phase 6). GP45 bodge wire, real 8 kHz waveform generator. Zero jitter confirmed with external trigger.
- ~~**Optical characterization (initial)**~~ — Photodiode + Saleae pulse-triggered averaging resolves BCM bit-planes. BCMWEIGHTS enables calibration.
- ~~**Saleae Logic Pro 8 automation**~~ — Python API integration for synchronized digital+analog capture and analysis.
- ~~**Spectrometer linearity characterization**~~ — Ocean Insight Flame X (570.8 nm peak, 50 ms integration, 560-580 nm window). Per-bit-plane brightness decay measured (B0=3163 cts/us to B3=2024 cts/us). BCMWEIGHTS optimizer converged to [1, 2, 5.02, 10.19] at T=0.7 us for monotonic response (2.5% max error, 12.7 us burst).
- ~~**AD3 setup and USB bandwidth characterization**~~ — Record mode caps at 5 MS/s (USB 2.0). Triggered single-shot mode at 100 MHz with 32K buffer (Config 1) is the solution. Dual-channel 12.5 MHz covers full 2.5 ms row cycle. Row-classified averaging recovers full signal from random-trigger captures.

**Guiding principle**: Jitter and timing budget compliance come first. Mode A (PIO + noInterrupts) is the production architecture.

## Important Files

- `test_firmware/single_led/src/main.cpp` — primary firmware (all commands, scan loops, PIO setup)
- `test_firmware/single_led/src/constants.h/.cpp` — pin definitions
- `test_firmware/single_led/src/utilities.h/.cpp` — coordinate mapping
- `test_firmware/single_led/platformio.ini` — build config
- `test_firmware/single_led/PRODUCTION_ARCHITECTURE.md` — comprehensive production design document
- `test_firmware/single_led/G6_PANEL_TIMING_REPORT_v2.md` — shareable timing report
- `test_firmware/single_led/RESULTS.md` — detailed engineering results (Phases 0-5a)
- `test_firmware/single_led/auto_test.py` — autonomous build/flash/test runner (preferred for all testing)
- `test_firmware/single_led/status_dashboard.py` — status visibility for user (watch /tmp/g6_test_status.txt)
- `test_firmware/single_led/bcm_jitter_sweep.py` — BCM jitter sweep (4 T × 16 intensities × 10k frames)
- `test_firmware/single_led/run_tests.py` — automated serial test harness (legacy)
- `test_firmware/single_led/visual_test.py` — interactive visual verification
- `test_firmware/single_led/ad3_capture.py` — AD3 integration: wavegen trigger + dual-channel scope capture (record + triggered modes)
- `test_firmware/single_led/build_viewer.py` — builds HTML viewer from AD3 triggered capture data
- `test_firmware/single_led/saleae_capture.py` — Saleae Logic 2 automation + analysis
- `test_firmware/single_led/spectrometer_cal.py` — Ocean Insight Flame X spectrometer calibration and linearity measurement
- `test_firmware/single_led/bcm_weight_optimizer.py` — iterative BCM weight optimizer using spectrometer feedback
- `test_firmware/single_led/PCB_REDESIGN_ANALYSIS.md` — PCB pin assignment analysis and redesign options

## Serial Commands (current firmware)

Single-LED: `ON`, `OFF`, `POS row col`, `RUN n`, `STOP`, `STATS`, `JITTER n`, `SWEEP`, `SERIAL`
Scan: `ROWS n`, `PATTERN hex`, `SCAN n`, `ROWTIME n`
PIO: `PIOSCAN n`, `PIOSCAN2 n` (unprotected), `PIOROWTIME n`
Hybrid DMA+ISR: `DMASCAN n`, `DMATEST`
Multi-SM PIO: `MSMSCAN n`, `MSMTEST`
Burst mode (2P sync sim): `BURST n [rate_hz]` — default 8 kHz trigger rate, `noInterrupts()` during scan burst
BCM burst (Phase 4): `BCM bits`, `BCMON us`, `FILL intensity`, `PIXEL row col intensity`, `GRADIENT`, `BCMBURST n [Hz] [A|B|C]`, `BCMDEMO`
RAM burst (Phase 5a): `RAMBURST n [Hz] [n_frames] [P]` — frame cycling from RAM, P=pre-emptive noInterrupts
External trigger (Phase 6): `EXTTRIG ON|OFF`, `TRIGTEST [N]`, `PHOTOCAL [hold_sec]`, `BCMWEIGHTS w0 w1 ...`, `BCMORDER REV|FWD`
System: `REBOOT` (enters BOOTSEL mode for flashing)
`HELP` for full list.
