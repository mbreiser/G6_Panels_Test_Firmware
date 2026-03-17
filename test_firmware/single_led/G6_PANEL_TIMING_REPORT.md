# G6 20×20 LED Panel: Timing Characterization & Application Modes

**Date**: 2026-03-16
**Author**: M. Reiser (characterization), Claude (firmware & analysis)
**Hardware**: RP2354 MCU (RP2350 dual Cortex-M33 @ 150 MHz, 8 MB PSRAM), Janelia batch 20×20 passive LED matrix, UCC27517 gate drivers

---

## 1. What We Measured

We characterized the **software-side GPIO timing precision** of the RP2354 driving a 20×20 passive LED matrix. The panel uses row scanning: at any instant, one or more rows are active while all 20 column pins set the per-LED on/off pattern.

### Measurement Method

- **ARM DWT cycle counter**: 6.67 ns resolution (1 cycle at 150 MHz), hardware counter in the Cortex-M33 debug unit
- **All timing-critical code in RAM** (`__not_in_flash_func`) to eliminate XIP flash cache contention from Core 1's USB stack
- **`noInterrupts()` on Core 0** during timing-critical sections
- **Automated serial test harness** (Python + pyserial): 10,000-frame runs per configuration

### What We Did NOT Measure

- **Optical LED response**: We do not yet know the LED turn-on/turn-off time, which sets a floor on useful pulse width
- **Intensity linearity**: We have not verified that time-averaged LED intensity is linear with duty cycle — this is essential for grayscale (BCM) and requires a photodiode + external ADC
- **Per-LED uniformity**: The Janelia batch has reversed LED polarity (row sources current through 1.3Ω instead of sinking through 0.55Ω), causing ~8–13% brightness variation across load conditions. Acceptable but not characterized optically.
[MR]: we should be clear here that the brightness variation is estimated based on circuit analysis, hasn't been validated, and might be missing things. We should not give an opinion on whether it is character adopted or particularly. We should say that the expected variability is higher than in the polarity of the design 
---

## 2. Key Results

### 2.1 Timing Precision

| Metric | Value |
|---|---|
| Timing resolution | 6.67 ns |
| Minimum achievable pulse | 0.21 µs (hardware floor from GPIO operations) |
| Jitter (protected mode, 100k pulses) | **0 ns** — true zero, min = max |
| Jitter (unprotected, 100k pulses) | ~8 µs worst-case, ~0.01 µs mean |
[MR]: I don't believe we have defined what protected mode even means. Can we define it above and/or here? 
Zero jitter is achievable and repeatable! The key requirements are RAM-resident code and disabled interrupts on the timing core.

### 2.2 Row Scanning Performance

We implemented two scan architectures and compared them:

| Architecture | Per-row overhead | 20-row frame (ON=1µs) | Notes |
|---|---|---|---|
| **CPU-only** (batch GPIO masks) | 0.76 µs | 35.2 µs | Simple, all timing via DWT spin-wait |
| **PIO + CPU hybrid** | 0.62 µs | 32.3 µs | PIO drives columns, CPU manages rows |

PIO saves ~3 µs per frame (19% less overhead per row). Both achieve zero jitter at ON ≥ 2 µs. [MR]: Why greater than or equal to 2 microseconds? I believe there is zero jitter at lower speeds also. Please confirm and update accordingly 

Column pattern has zero effect on timing — batch GPIO/PIO operations set all 20 pins simultaneously.

### 2.3 Scaling Properties

- **Linear with row count**: frame time = N_rows × (ON_time + overhead). Verified 1–20 rows.
- **Pattern-independent**: identical timing for all-on, all-off, checkerboard, single-column patterns
- **PIO frees the CPU** during LED ON time (hardware state machine runs autonomously)

---

## 3. Grayscale via Binary Code Modulation (BCM)

BCM encodes N-bit brightness by splitting each row's ON time into N sub-frames with binary-weighted durations (1T, 2T, 4T, 8T for 4-bit). Each LED's brightness value determines which sub-frames it participates in. The base time unit T must exceed the minimum pulse width (0.21 µs).

**Important caveat**: BCM assumes intensity is linear with ON time. This has NOT been verified optically. If the LED response is nonlinear (e.g., slow turn-on, phosphor persistence), the actual brightness levels will be distorted. Optical characterization with a photodiode is a prerequisite before BCM can be considered validated.
[MR]: here or elsewhere we should indicate that assuming we have enough time and time resolution, we can compensate for much of the lack of linearity with a different encoding scheme. All we have to do is compensate for both the time and the number of total LEDs on, and we should be able to get quite flat intensity profiles that can be calibrated to near perfection 

### BCM Feasibility (PIO, 150 MHz)

| Bit depth | Levels | Sub-frames | Time/row needed | At 8 kHz (6.25µs/row) | At 2 kHz (25µs/row) |
|---|---|---|---|---|---|
| 2-bit | 4 | 2 | 3T + 2×0.62µs | T = 0.56 µs ✅ | T = 4.72 µs ✅ |
| 3-bit | 8 | 3 | 7T + 3×0.62µs | T = 0.30 µs ✅ | T = 1.93 µs ✅ |
| 4-bit | 16 | 4 | 15T + 4×0.62µs | T = 0.25 µs ✅ | T = 1.50 µs ✅ |
| 5-bit | 32 | 5 | 31T + 5×0.62µs | T = 0.10 µs ⚠️ | T = 0.71 µs ✅ |

4-bit (16 levels) at 8 kHz is feasible: T = 0.25 µs > 0.21 µs minimum. 5-bit requires T below the hardware floor at 8 kHz but works at lower refresh rates.

---

## 4. Application Modes for G6 Panels

[MR]: We haven't actually explained the problem properly. The problem statement is that we want to use the externally triggered display mode where the panels are only on for about 10 to 15% of the duty cycle at 8 kHz, with precise temporal alignment with the scanning of 2P microscope. That's the problem statement and the question of which operation will be most reliable, which one will give us better timing, and which one is going to be simple and easy to troubleshoot and use reliably 

The panel's scan architecture supports multiple operating modes. The right choice depends on the application's timing constraints, grayscale requirements, and synchronization needs.

### 4.1 Free-Running Mode (simplest)

The panel scans continuously at its natural rate. No external synchronization.

| Parameter | Value |
|---|---|
| Scan rate | Determined by ON time and row count |
| Example: 20 rows, ON=5µs | 8,903 Hz (112 µs/frame) |
| Example: 20 rows, ON=1µs | 30,941 Hz (32 µs/frame) |
| Grayscale | 4-bit BCM at ≥ 2 kHz; up to 4-bit at 8 kHz |
| Synchronization | None |
| Use case | Static patterns, behavioral displays, development/testing |

[MR]: I don’t understand this: "Grayscale | 4-bit BCM at ≥ 2 kHz; up to 4-bit at 8 kHz”  What does it mean up to? I finally understand, that’s it’s due to grayscale versus BCM difference wrt. Overhead. Please break this out as an example at the top of the analysis.These stats are good, but should mention that 2khz, 4 bit would be and excellent configuration for the system, easy to validate and meeting all requirements. 

### 4.2 Synchronous / Externally Triggered Mode

An external trigger (e.g., microscope line clock) fires one or more rows per trigger. The display is slaved to the external clock.

**Key insight**: The number of rows per trigger is configurable. This trades refresh rate for time budget per trigger.

| Rows per trigger | Triggers per frame | Refresh rate (at 8 kHz trigger) | Time per trigger for BCM |
|---|---|---|---|
| 1 | 20 | 400 Hz | 125 µs — luxurious |
| 2 | 10 | 800 Hz | 125 µs — luxurious |
| 4 | 5 | 1,600 Hz | 125 µs — luxurious |
| 5 | 4 | 2,000 Hz | 125 µs — luxurious |
| 10 | 2 | 4,000 Hz | 125 µs — comfortable |
| 20 | 1 | 8,000 Hz | 125 µs — comfortable |

At 8 kHz trigger rate, every configuration gets 125 µs between triggers — far more than needed for even 5-bit BCM on the row group. The choice of rows-per-trigger only affects refresh rate, not timing feasibility.
[MR]: this table and analysis is incorrect! We get the trigger at 8khz, but only have <20 microseconds to burn. This is the <15% duty cycle window we need to work with. 

**Grayscale in triggered mode**:

| Rows/trigger | Time for rows (4-bit BCM, T=0.67µs) | Total per trigger | Feasible? |
|---|---|---|---|
| 1 | 15×0.67 + 4×0.62 = 12.5 µs | 12.5 µs | ✅ (of 125 µs available) |
| 2 | 2 × 12.5 = 25.0 µs | 25.0 µs | ✅ |
| 5 | 5 × 12.5 = 62.5 µs | 62.5 µs | ✅ |
| 10 | 10 × 12.5 = 125 µs | 125 µs | ✅ (exactly fits) |
| 20 | 20 × 12.5 = 250 µs | 250 µs | ❌ (exceeds 125 µs) |

For 20 rows per trigger at 8 kHz, you'd need to reduce to 3-bit BCM or lower T. But 10 rows/trigger at 4 kHz refresh is very comfortable.

**Implementation**: PIO `wait pin` instruction can stall until trigger arrives, then CPU fires the pre-loaded row group. Minimal latency (~50 ns from trigger edge to first column output).

**Variants**:
- **Fixed row group**: always scan rows 0–N per trigger, cycling through groups
- **Rolling row**: advance one row per trigger, wrapping at 20
- **Adaptive**: scan as many rows as time allows before next trigger
[MR]: only rolling makes sense. The others would give artifacts/inconsistent displays, would they not?

### 4.3 Asynchronous / Externally Gated Mode

An external gate signal defines a time window during which the display may scan. The display fires as many rows as possible within the window, then stops.

**Key insight**: The gate width is the constraint, not a fixed trigger count. The display adapts to whatever window it gets.

| Gate width | Rows possible (ON=0.5µs, PIO) | Rows possible (DMA PIO) | Grayscale possible |
|---|---|---|---|
| 5 µs | 4 rows | 9 rows | 1-bit only |
| 10 µs | 8 rows | 18 rows | 1-bit; or 4 rows @ 2-bit |
| 12 µs | 10 rows | 20 rows ✅ | 1-bit full panel (DMA); or 5 rows @ 2-bit |
| 15 µs | 13 rows | 20+ rows | 2-bit possible for ~8 rows |
| 25 µs | 20 rows ✅ | 20 rows | 2-bit full panel; 3-bit for ~12 rows |
| 50 µs | 20 rows | 20 rows | 4-bit full panel ✅ |
| 125 µs | 20 rows | 20 rows | 5-bit full panel ✅ |

**Variants**:
- **Gate-width specified**: trigger starts scan, firmware scans for a pre-configured duration (e.g., `GATED_SCAN width_us=12`)
- **Gate-level tracked**: scan while gate pin is HIGH, stop immediately when it goes LOW. PIO can monitor gate pin in parallel.
- **Partial frame accumulation**: if gate is too short for full frame, scan a subset of rows. On next gate, scan the next subset. Full frame builds up over multiple gate events (lower refresh but full panel coverage).
[MR]: for our application, I think gate-width specified and partial frame acc. Seem like the logical way to do this — more predictable with enough flexibility to adjust performance. 


**Microscope turnaround example**: At 8 kHz line rate with ~12 µs turnaround blanking:
- Current PIO: 10 rows per blanking period (1-bit), full panel in 2 blanking periods → 4 kHz frame rate
- DMA-fed PIO: full 20 rows per blanking period (1-bit), 8 kHz frame rate
- With 50 µs gate (slower line rate): full 4-bit BCM, 20 rows per gate

### 4.4 Continuous BCM with External Sync

Free-running BCM display, but frame start is synchronized to an external trigger. This gives deterministic frame timing relative to an external event (e.g., stimulus onset, camera frame).

| Parameter | Value |
|---|---|
| Trigger | One pulse per frame (e.g., 1–8 kHz) |
| Scan | Full 20-row BCM frame after each trigger |
| Latency | < 1 µs from trigger to first LED output |
| Grayscale | Full 4-bit at ≥ 5 kHz; 5-bit at ≥ 2 kHz |
| Use case | Synchronized visual stimulation, closed-loop experiments |

---

## 5. Performance Improvement Paths

Starting from the current PIO baseline (150 MHz, 0.62 µs/row overhead):

| Optimization | Effort | Effect |
|---|---|---|
| **Overclock to 200 MHz** | Trivial (1 config line) | ~25% faster overhead → ~16.7 µs frame |
| **CPU/PIO work overlap** | Low (restructure scan loop) | ~20% less overhead (overlaps CPU prep with PIO delay) |
| **Both above** | Low | ~13.5 µs frame (20 rows, ON=0.5µs) |
| **Multi-SM PIO** (rows in PIO) | High | ~13.0 µs frame; eliminates CPU from row transitions |
| **DMA-fed PIO** | Very high | ~11.1 µs frame; zero CPU during scan |

For most applications, **overclocking + overlap** is sufficient. DMA-fed PIO is only needed for the tightest gated windows (< 12 µs full frame).

---

## 6. Hardware Considerations

### Pin Allocation
- **Columns**: GP1–GP20 (contiguous) — ideal for PIO `out pins, 20`
- **Rows**: GP21–GP31 + GP36–GP44 (gap at GP32–35) — requires CPU or multi-SM PIO
- **ADC pins (GP26–29)**: used as row pins — **no on-chip ADC available** for optical measurement
- **Free GPIOs**: GP0, GP32, GP34, GP35, GP45–47 — available for trigger input, sync output, etc.

### Reversed LED Polarity (Janelia Batch)
LEDs are soldered backwards: ON = column LOW + row HIGH (opposite of schematic intent). This causes row drivers to source current (1.3Ω) instead of sink (0.55Ω), increasing brightness variation from ~4–6% to ~8–13% across load conditions. Functionally equivalent; no impact on timing.
[MR]: see comments from above — give more context. 

[MR]: this measurement detail is not for this report. Just outline what we should measure next to confirm linearity. 
### Recommended Measurement Hardware
For optical validation: **Saleae Logic Pro 8** ($999) — 8 analog + digital channels, Python automation API (`logic2-automation`), simultaneous capture on shared timebase. Allows automated correlation of GPIO control signals with photodiode output.

---

## 7. Open Questions & Next Steps

### Must Answer Before BCM Deployment
1. **LED optical rise/fall time**: Do these LEDs respond to sub-µs pulses? If rise time is > 0.5 µs, the shortest BCM sub-frames won't produce meaningful light.
2. **Intensity linearity**: Is average intensity proportional to duty cycle? BCM assumes this. Nonlinearity would distort brightness levels.
3. **Minimum distinguishable levels**: Even if linear, can all 16 levels of 4-bit BCM be resolved against noise and LED-to-LED variation?

### Planned
- **BCM firmware implementation** (Phase 4): 4-bit BCM with configurable refresh rate
- **Optical characterization** (Phase 3.5): photodiode + Saleae Logic Pro 8, gap-delimited protocol to measure intensity vs. duty cycle
- **External trigger interface**: GPIO interrupt or PIO `wait pin` for synchronous/gated modes
- **Overclock validation**: test 200 MHz stability and re-measure all timing parameters

### Future Possibilities
- **Per-row column patterns**: different brightness image on each row (already supported in scan architecture, just needs BCM buffer)
  [MR]: does this (above) mean intensity correction via timing based on num on LEDs, or something else?
- **DMA-fed scan engine**: fully autonomous display refresh, CPU only updates frame buffer
- **Multi-panel synchronization**: trigger distribution for tiled G6 arrays
  [MR]: update to say we expect trigger to be synchronous across panels, but will need to confirm this. Any ideas how to do this with on panel timing measurements? 
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
| CPU SCAN | 10,000 frames × 4 ON times | 20 rows, 0.5–5.0 µs ON | Zero jitter at ON ≥ 2µs |
| PIO SCAN (protected) | 10,000 frames × 4 ON times | 20 rows, 0.5–5.0 µs ON | Zero jitter at ON ≥ 2µs |
| PIO SCAN (unprotected) | 10,000 frames | 20 rows, ON=1.0µs | 10.3 µs max jitter, 0.1 µs mean |
| PIO row scaling | 10,000 frames × 5 configs | 1–20 rows, ON=1.0µs | Perfect linear scaling |
| PIO pattern independence | 10,000 frames × 4 patterns | 20 rows, ON=1.0µs | Identical timing all patterns |
| Visual verification | 8 interactive tests | Various patterns + brightness | All correct |
