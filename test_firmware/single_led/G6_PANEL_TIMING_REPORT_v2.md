# G6 20×20 LED Panel: Timing Characterization & Application Modes

**Date**: 2026-03-16
**Author**: M. Reiser (characterization), Claude (firmware & analysis)
**Hardware**: RP2354 MCU (RP2350 dual Cortex-M33 @ 150 MHz, 8 MB PSRAM), Janelia batch 20×20 passive LED matrix, UCC27517 gate drivers

---

## Executive Summary

We characterized the GPIO timing precision of the RP2354 MCU driving a G6 20×20 passive LED panel and evaluated whether the system can support temporally gated display for 2-photon microscopy.

**What works well:**
- The MCU achieves **zero-jitter GPIO control** down to sub-microsecond pulse widths (tested to 0.05 µs), using RAM-resident code and protected execution on a dedicated core
- A full 20-row panel scan takes **22–32 µs** depending on per-row ON time, using PIO (Programmable I/O) hardware for column driving
- **4-bit grayscale (16 brightness levels)** via Binary Code Modulation is feasible at refresh rates up to 8 kHz
- Timing is completely **pattern-independent** — the number of active LEDs has no effect on scan speed

**For 2P microscope integration** (~12–20 µs display window at 8 kHz):
- An external trigger/gate initiates a rolling scan of N rows with BCM grayscale, then the display goes dark. Three parameters trade off within the window: **rows per trigger** (refresh rate), **bit depth** (grayscale levels), and **base time unit T** (brightness).
- Example configurations (15 µs window): 1 row/trigger at 4-bit → 400 Hz, 16 levels; or 2 rows at 3-bit → 800 Hz, 8 levels; or 4 rows at 2-bit → 1,600 Hz, 4 levels
- **T-control** provides continuous brightness adjustment (similar role to G4 stretch mode) — larger T = brighter display
- All configurations work with current firmware — no overclocking or DMA needed.

**What we haven't verified yet:**
- Optical LED response time (do sub-µs pulses produce meaningful light?)
- Intensity linearity (is brightness proportional to ON time?)
- LED-to-LED uniformity under reversed polarity

These require photodiode + external ADC measurements. If nonlinearity is found, it can likely be compensated with a calibration lookup table.

---

## 1. What We Measured

We characterized the **software-side GPIO timing precision** of the RP2354 driving a 20×20 passive LED matrix. The panel uses row scanning: at any instant, one row is active while all 20 column pins set the per-LED on/off pattern.

### Measurement Method

- **ARM DWT cycle counter**: 6.67 ns resolution (1 cycle at 150 MHz), hardware counter in the Cortex-M33 debug unit
- **All timing-critical code in RAM** (`__not_in_flash_func`) to eliminate XIP flash cache contention from Core 1's USB stack
- **Automated serial test harness** (Python + pyserial): 10,000-frame runs per configuration
- **"Protected mode"**: throughout this report, "protected" means interrupts are disabled on the timing core (`noInterrupts()` on Core 0) AND all timing-critical code runs from RAM. This eliminates two sources of jitter: (1) interrupt preemption on the timing core, and (2) XIP flash cache eviction by Core 1's USB stack. "Unprotected" means interrupts remain enabled, allowing USB/serial to preempt timing code.

### What We Did NOT Measure

- **Optical LED response**: We do not yet know the LED turn-on/turn-off time, which sets a floor on useful pulse width
- **Intensity linearity**: We have not verified that time-averaged LED intensity is linear with duty cycle — this is essential for grayscale (BCM) and requires a photodiode + external ADC
- **Per-LED uniformity**: The Janelia batch has reversed LED polarity (row sources current through 1.3Ω instead of sinking through 0.55Ω). Based on circuit analysis, this is expected to produce higher LED-to-LED brightness variation than the intended polarity design. This estimate has not been validated optically and may not capture all relevant factors.

---

## 2. Key Results

### 2.1 Timing Precision

| Metric | Value |
|---|---|
| Timing resolution | 6.67 ns |
| Minimum achievable pulse | 0.21 µs (hardware floor from GPIO operations) |
| Jitter (protected, single-LED, 100k pulses) | **0 ns** — true zero at all tested ON times (0.05–20 µs) |
| Jitter (unprotected, single-LED, 100k pulses) | ~8 µs worst-case, ~0.01 µs mean |

In protected mode, **zero jitter is achieved at all ON times down to 0.05 µs** for single-LED pulses. For full-panel scanning, zero jitter is achieved at all ON times in the scan loop itself; occasional small outliers (~4 µs) appear at ON < 2 µs due to the brief interrupt window between frames used for serial communication. These outliers are at the frame level, not within the scan — the LED timing within each frame is always precise.

### 2.2 Row Scanning Performance

We implemented two scan architectures and compared them:

| Architecture | Per-row overhead | 20-row frame (ON=1µs) | Notes |
|---|---|---|---|
| **CPU-only** (batch GPIO masks) | 0.76 µs | 35.2 µs | Simple, all timing via DWT spin-wait |
| **PIO + CPU hybrid** | 0.62 µs | 32.3 µs | PIO drives columns, CPU manages rows |

PIO saves ~3 µs per frame (19% less overhead per row). Column pattern has zero effect on timing — batch GPIO/PIO operations set all 20 pins simultaneously.

### 2.3 Scaling Properties

- **Linear with row count**: frame time = N_rows × (ON_time + overhead). Verified 1–20 rows.
- **Pattern-independent**: identical timing for all-on, all-off, checkerboard, single-column patterns
- **PIO frees the CPU** during LED ON time (hardware state machine runs autonomously)

---

## 3. Grayscale via Binary Code Modulation (BCM)

### Why BCM over PWM?

Both PWM and BCM can achieve N-bit grayscale, but BCM is dramatically more efficient because it requires fewer sub-frames — and each sub-frame carries a fixed overhead (0.62 µs for row switching).

**Example: 4-bit grayscale (16 brightness levels), 1 row**

| Method | Sub-frames per row | Switching overhead | Total overhead |
|---|---|---|---|
| **PWM** | 15 (one per time slot) | 15 × 0.62 µs | **9.3 µs** |
| **BCM** | 4 (weights 1T, 2T, 4T, 8T) | 4 × 0.62 µs | **2.5 µs** |

Both methods produce the same 15T total ON time for full brightness, but BCM achieves this with **4 switching events instead of 15**. For 20 rows, PWM overhead alone would be 186 µs — far too slow. BCM overhead is 50 µs, leaving the rest for actual LED ON time.

**BCM works by encoding brightness in binary**: each LED's 4-bit brightness value determines which of the 4 sub-frames it is ON during. Sub-frame durations are 1T, 2T, 4T, 8T, so a pixel with value 0b1010 = 10 is ON during the 2T and 8T sub-frames (total 10T out of 15T maximum). The base time unit T must exceed the minimum pulse width (0.21 µs).

### Linearity Caveat and Mitigation

BCM assumes that LED intensity is proportional to ON time. This has **not been verified optically**. If the LED response is nonlinear (e.g., slow turn-on, phosphor persistence), the actual brightness levels will be distorted relative to their intended values.

However, given sufficient timing resolution and characterization, **nonlinearity can be compensated**. By measuring the actual intensity response as a function of ON time and number of simultaneously active LEDs, a calibration lookup table (LUT) can remap the commanded brightness values to achieve linear perceptual intensity. This correction can also account for load-dependent brightness variation (from shared current paths). With calibration, near-perfect linearity should be achievable (up to limit of inter-component variability).

**Next step**: optical characterization with a photodiode and external ADC to measure intensity vs. duty cycle across operating conditions.

### BCM Feasibility (PIO, 150 MHz)

| Bit depth | Levels | Sub-frames | Time/row needed | At 8 kHz (6.25µs/row) | At 2 kHz (25µs/row) |
|---|---|---|---|---|---|
| 2-bit | 4 | 2 | 3T + 2×0.62µs | T = 0.56 µs ✅ | T = 4.72 µs ✅ |
| 3-bit | 8 | 3 | 7T + 3×0.62µs | T = 0.30 µs ✅ | T = 1.93 µs ✅ |
| 4-bit | 16 | 4 | 15T + 4×0.62µs | T = 0.25 µs ✅ | T = 1.50 µs ✅ |
| 5-bit | 32 | 5 | 31T + 5×0.62µs | T = 0.10 µs ⚠️ | T = 0.71 µs ✅ |

**Recommended baseline configuration: 2 kHz refresh, 4-bit BCM (16 levels)**. This gives T = 1.50 µs — well above the hardware floor, easy to validate optically, and meets all known application requirements. Higher refresh rates and bit depths are achievable but push timing constraints closer to the limits.

---

## 4. Application Context: 2-Photon Microscope Display

### Problem Statement

G6 LED panels are intended for visual stimulation synchronized to a 2-photon (2P) laser scanning microscope. The panels must be ON only during a brief portion of each scan cycle (~10–15% duty cycle at ~8 kHz line rate) to avoid contaminating the fluorescence detection with LED light. This requires:

- **Precise temporal alignment** with the microscope scan cycle
- **Short, well-controlled ON windows** (~12–20 µs per trigger event)
- **Grayscale intensity control** within those windows
- **Reliability and simplicity** — the system must be easy to troubleshoot and use in a live experiment

The central question is: **which configuration gives the best combination of timing precision, grayscale depth, brightness, and simplicity?**

### 4.1 Free-Running Mode (development/testing only)

The panel scans continuously at its natural rate. No external synchronization. Not suitable for 2P integration (no temporal gating), but useful for validating BCM and optical linearity independently.

| Parameter | Value |
|---|---|
| Example: 20 rows, ON=5µs | 8,903 Hz (112 µs/frame) |
| Example: 20 rows, ON=1µs | 30,941 Hz (32 µs/frame) |
| Grayscale | 4-bit BCM feasible at refresh rates ≥ 2 kHz (see §3) |

---

### 4.2 Externally Triggered Mode (2P microscope integration)

An external signal (trigger pulse or gate) from the microscope initiates a scan of N rows within a brief time window. The display goes dark between events.

Under the real timing constraints, the "triggered" and "gated" variants we initially considered are essentially the same operation: an edge arrives, the firmware scans a pre-configured number of rows for a known duration, then stops. The only difference is the signal interface:

- **Edge-triggered**: a pulse (e.g., line clock) starts the scan; the firmware scans for a pre-configured duration
- **Level-gated**: a gate signal (e.g., blanking window) starts the scan; firmware scans for a pre-configured duration or while the gate is HIGH

In both cases, the firmware behavior is identical: receive signal → scan N rows with BCM → go dark → wait for next signal. We treat these as **one mode with two signal interface options**.

**Implementation**: rolling row advancement — each trigger advances by N rows, wrapping at 20. This ensures uniform display refresh with no visual artifacts. PIO `wait pin` instruction stalls until trigger arrives, then CPU fires the pre-loaded row group. Latency ~50 ns from trigger edge to first LED output.

**Preferred gate implementation**: gate-width specified (trigger starts scan, firmware scans for a pre-configured duration) with partial frame accumulation (rolling subset of rows per event, full frame builds up over multiple events). This is predictable, deterministic, and flexible enough to adapt to different window sizes.

### The Three Design Parameters

Within the constrained time window, three parameters trade off against each other:

1. **Rows per trigger (N)**: how many rows to scan per event. More rows → higher refresh rate but less time per row.
2. **Bit depth**: number of BCM sub-frames per row. More bits → finer grayscale but more overhead per row.
3. **Base time unit (T)**: the duration of the shortest BCM sub-frame. Larger T → brighter display (more photons per frame) but requires more time per row.

These are not independent — they must all fit within the available window:

> **Window = N × (bit_overhead + (2^bits − 1) × T)**
>
> where bit_overhead = bits × 0.62 µs (PIO row switching per sub-frame)

### Brightness Control via T

The base time unit T directly controls display brightness: a fully-ON pixel receives (2^bits − 1) × T of light per row visit. Increasing T makes the display brighter; decreasing T makes it dimmer.

| T | Max ON time per row (4-bit) | Relative brightness |
|---|---|---|
| 0.25 µs | 3.75 µs | 1× (dimmest) |
| 0.67 µs | 10 µs | 2.7× |
| 1.50 µs | 22.5 µs | 6× (brightest) |

This "T-control" provides a **continuous brightness adjustment** independent of the number of grayscale levels. It may serve a similar role to the stretch mode implemented in G4, where display timing was extended to increase brightness — but with finer control and explicit parameterization.

### Configuration Space

Given a window W (µs), trigger rate R (Hz), and N rows per trigger:

| Window | N rows | Bit depth | T (µs) | Time used | Refresh | Levels | Relative brightness |
|---|---|---|---|---|---|---|---|
| **12 µs** | 1 | 4-bit | 0.58 | 11.2 µs | 400 Hz | 16 | 2.3× |
| **12 µs** | 1 | 3-bit | 1.10 | 10.6 µs | 400 Hz | 8 | 4.4× |
| **12 µs** | 2 | 2-bit | 1.00 | 10.5 µs | 800 Hz | 4 | 4.0× |
| **12 µs** | 5 | 1-bit | 1.00 | 11.1 µs | 2,000 Hz | 2 | 4.0× |
| **15 µs** | 1 | 4-bit | 0.77 | 14.0 µs | 400 Hz | 16 | 3.1× |
| **15 µs** | 2 | 3-bit | 0.70 | 13.6 µs | 800 Hz | 8 | 2.8× |
| **15 µs** | 2 | 2-bit | 1.60 | 13.8 µs | 800 Hz | 4 | 6.4× |
| **15 µs** | 4 | 2-bit | 0.66 | 14.4 µs | 1,600 Hz | 4 | 2.6× |
| **20 µs** | 1 | 4-bit | 1.11 | 19.1 µs | 400 Hz | 16 | 4.4× |
| **20 µs** | 2 | 4-bit | 0.39 | 19.2 µs | 800 Hz | 16 | 1.6× |
| **20 µs** | 2 | 3-bit | 1.07 | 17.8 µs | 800 Hz | 8 | 4.3× |
| **20 µs** | 4 | 2-bit | 1.13 | 18.4 µs | 1,600 Hz | 4 | 4.5× |
| **20 µs** | 5 | 2-bit | 0.82 | 18.3 µs | 2,000 Hz | 4 | 3.3× |

**Sweet spots** (highlighted configurations):
- **Maximum grayscale**: 1 row/trigger, 4-bit, 15 µs window → 16 levels, 400 Hz, moderate brightness
- **Balanced**: 2 rows/trigger, 3-bit, 15–20 µs window → 8 levels, 800 Hz, good brightness
- **Maximum refresh**: 5 rows/trigger, 2-bit, 12–20 µs window → 4 levels, 2,000 Hz, moderate brightness
- **Maximum brightness**: 2 rows/trigger, 2-bit, 15 µs window → 4 levels, 800 Hz, 6.4× brightness

The optimal configuration depends on the application's priority (grayscale resolution vs. refresh rate vs. brightness) and the actual available window width, which varies with microscope scan settings.

---

## 5. Performance Improvement Paths

Starting from the current PIO baseline (150 MHz, 0.62 µs/row overhead):

| Optimization | Effort | Effect |
|---|---|---|
| **Overclock to 200 MHz** | Trivial (1 config line) | ~25% faster overhead → ~16.7 µs frame |
| **CPU/PIO work overlap** | Low (restructure scan loop) | ~20% less overhead |
| **Both above** | Low | ~13.5 µs frame (20 rows, ON=0.5µs) |
| **Multi-SM PIO** (rows in PIO) | High | ~13.0 µs frame |
| **DMA-fed PIO** | Very high | ~11.1 µs frame; zero CPU during scan |

For the triggered mode at 1 row/trigger, **no optimizations are needed** — current performance is more than sufficient. For gated mode with tight windows (< 15 µs), overclocking is the easiest path to fitting more rows per gate.

---

## 6. Hardware Considerations

### Pin Allocation
- **Columns**: GP1–GP20 (contiguous) — ideal for PIO `out pins, 20`
- **Rows**: GP21–GP31 + GP36–GP44 (gap at GP32–35) — requires CPU or multi-SM PIO
- **ADC pins (GP26–29)**: used as row pins — **no on-chip ADC available** for optical measurement
- **Free GPIOs**: GP0, GP32, GP34, GP35, GP45–47 — available for trigger input, sync output, etc.

### Reversed LED Polarity (Janelia Batch)

LEDs in the Janelia batch are soldered with reversed polarity: ON = column LOW + row HIGH (opposite of schematic intent). This causes row drivers to source current through 1.3Ω resistors instead of sinking through 0.55Ω. Based on circuit analysis, this is expected to produce **higher LED-to-LED brightness variation** than the intended polarity design. However, this estimate has not been validated optically and may not capture all relevant factors (e.g., LED forward voltage distribution, thermal effects). No impact on timing.

---

## 7. Open Questions & Next Steps

### Must Validate Before Deployment
1. **LED optical rise/fall time**: Do these LEDs respond to sub-µs pulses?
2. **Intensity vs. duty cycle linearity**: Measure with photodiode + external ADC across a range of ON times and number of simultaneously active LEDs
3. **Minimum distinguishable levels**: Can all target brightness levels be resolved optically against noise and LED-to-LED variation?
4. **Load-dependent brightness variation**: Quantify how brightness changes with number of active columns (shared current path effects)

### Planned Work
- **BCM firmware implementation** (Phase 4): 4-bit BCM with configurable refresh rate
- **Optical characterization**: photodiode + external ADC (or Saleae Logic Pro 8) to measure intensity vs. duty cycle and validate linearity / build calibration LUT
- **External trigger interface**: GPIO interrupt or PIO `wait pin` for synchronous/gated modes
- **Overclock validation**: test 200 MHz stability and re-measure all timing parameters

### Future Possibilities
- **Per-row intensity correction**: adjust sub-frame timing based on number of active LEDs in each row to compensate for load-dependent brightness variation
- **DMA-fed scan engine**: fully autonomous display refresh, CPU only updates frame buffer
- **Multi-panel synchronization**: for tiled G6 arrays, we expect the external trigger to be distributed synchronously across all panels. Verification will require measuring timing across panels — potentially by capturing trigger-to-LED-output latency on each panel with a shared photodiode or by tapping sync output GPIOs with a multi-channel logic analyzer.
- **Closed-loop intensity calibration**: photodiode feedback to auto-correct BCM lookup table for nonlinearity

---

## Appendix: Measurement Configurations

All measurements performed on 2026-03-16 with Stage 3b firmware.

| Test | N | Configuration | Key Result |
|---|---|---|---|
| Single-LED jitter (protected) | 100,000 pulses | ON=10µs, RAM, noInterrupts | Zero jitter |
| Single-LED sweep | 100,000/step | ON=0.05–2.0µs, 25 steps | Zero jitter at all steps |
| CPU ROWTIME | 200,000 rows | 20 rows, all columns | 0.647 µs/row mean |
| PIO ROWTIME | 200,000 rows | 20 rows, all columns | 0.593 µs/row mean |
| CPU SCAN (protected) | 10,000 frames × 4 ON times | 20 rows, 0.5–5.0 µs ON | Zero jitter at ON ≥ 2µs; < 5µs outliers at shorter ON |
| PIO SCAN (protected) | 10,000 frames × 4 ON times | 20 rows, 0.5–5.0 µs ON | Zero jitter at ON ≥ 2µs; < 5µs outliers at shorter ON |
| PIO SCAN (unprotected) | 10,000 frames | 20 rows, ON=1.0µs | 10.3 µs max jitter, 0.1 µs mean |
| PIO row scaling | 10,000 frames × 5 configs | 1–20 rows, ON=1.0µs | Perfect linear scaling |
| PIO pattern independence | 10,000 frames × 4 patterns | 20 rows, ON=1.0µs | Identical timing all patterns |
| Visual verification | 8 interactive tests | Various patterns + brightness | All correct |

Note: scan frame outliers at ON < 2µs are from the brief interrupt window between frames (for serial communication), not from timing imprecision within the scan loop. The LED timing within each frame is always precise.
