# G6 20x20 Panel — Timing Summary

**Hardware**: RP2354 (RP2350 + 8MB PSRAM) @ 150 MHz, passive 20×20 LED matrix, UCC27517 drivers
**Measurement method**: ARM Cortex-M33 DWT cycle counter (6.67 ns resolution). All critical code RAM-resident (`__not_in_flash_func`).
**Caveat**: All timings are software GPIO control — not optical LED measurements. LED turn-on/turn-off time and intensity linearity remain unverified.

---

## Single-LED Pulse Timing (Phase 1)

| Metric | Value |
|---|---|
| Minimum pulse width | 0.213 µs (32 cycles) |
| Fixed GPIO overhead per pulse | ~0.17–0.19 µs |
| Jitter (protected + RAM, 100k pulses) | **0 ns** |
| Jitter (unprotected, 100k pulses) | up to ~8 µs worst-case, ~0.01 µs mean |
| Timing resolution | 6.67 ns (1 DWT cycle) |

**Zero jitter requires**: `noInterrupts()` on Core 0 + `__not_in_flash_func()` to avoid XIP cache contention from Core 1 USB stack.

---

## Full-Panel Row Scanning — CPU vs PIO

### Per-Row Overhead (no LED ON delay)

| Method | Min (µs) | Mean (µs) | Max (µs) |
|---|---|---|---|
| CPU (`gpio_set/clr_mask64`) | 0.647 | 0.647 | 4.04 |
| PIO (`out pins, 20` + CPU rows) | 0.593 | 0.593 | 28.87* |

*PIO max outlier from unprotected IRQ polling — does not affect mean or protected mode.

### Frame Times (20 rows, all columns, 10,000 frames, protected mode)

| ON/row | CPU frame (µs) | CPU rate (Hz) | PIO frame (µs) | PIO rate (Hz) | PIO savings |
|---|---|---|---|---|---|
| 0.5 µs | 25.65 | 38,991 | 22.32 | 44,803 | 3.33 µs |
| 1.0 µs | 35.25 | 28,372 | 32.32 | 30,941 | 2.93 µs |
| 2.0 µs | 55.25 | 18,101 | 52.32 | 19,113 | 2.93 µs |
| 5.0 µs | 115.25 | 8,677 | 112.32 | 8,903 | 2.93 µs |

### Jitter Summary

| Condition | Jitter (10,000 frames) |
|---|---|
| CPU, protected, ON ≥ 2µs | **Zero** (min = max) |
| PIO, protected, ON ≥ 2µs | **Zero** (min = max) |
| PIO, unprotected, ON = 1µs | 10.3 µs max, 0.1 µs mean |

### Key Properties (both CPU and PIO)

- **Pattern-independent**: ALL, NONE, CHECK, single-column produce identical frame times
- **Linear row scaling**: frame time = N × (ON_time + overhead)
- **PIO frees CPU** during ON pulse (hardware state machine runs independently)

---

## BCM Grayscale Feasibility (4-bit, 16 levels)

4-bit BCM: 4 sub-frames per row with weights 1:2:4:8. Total = 15T + 4×overhead.

| Refresh rate | Frame period | Time/row | PIO overhead (4×0.616µs) | Base unit T | Feasible? |
|---|---|---|---|---|---|
| **8 kHz** | 125 µs | 6.25 µs | 2.46 µs | **0.25 µs** | ✅ Yes |
| 5 kHz | 200 µs | 10.0 µs | 2.46 µs | **0.50 µs** | ✅ Comfortable |
| 2 kHz | 500 µs | 25.0 µs | 2.46 µs | **1.50 µs** | ✅ Easy |

T = 0.25 µs at 8 kHz > minimum pulse 0.21 µs. Zero jitter confirmed.

---

## Microscope Integration: Two Operating Modes

### Mode A: Asynchronous / Externally Gated
- Display fires entire 20-row frame during microscope turnaround blanking (~12 µs window)
- Requires DMA-fed PIO for ≤ 12 µs frame time
- Limited to 2-bit (4 levels) grayscale at 150 MHz; 3-bit possible with overclock
- Complex implementation (DMA + multi-SM PIO)

### Mode B: Synchronous / Externally Triggered (recommended)
- Each microscope line-clock trigger fires one row; 20 triggers = one frame
- 10–12 µs available per row → full 4-bit (16 levels) or even 5-bit (32 levels) grayscale
- 400 Hz refresh at 8 kHz line rate
- Works with current PIO + CPU architecture, no overclocking needed

| Property | Mode A (Gated) | Mode B (Triggered) |
|---|---|---|
| Frame time | ≤ 12 µs (all at once) | 20 × line period (distributed) |
| Max grayscale | 2-bit (4 levels) | 5-bit (32 levels) |
| Implementation | DMA-fed PIO | Current PIO |
| Complexity | Very high | Low |

---

## Fastest Possible Frame (Mode A analysis)

Target: ≤ 12 µs for full 20-row frame.

| Approach | Est. frame time | Notes |
|---|---|---|
| Current PIO (150 MHz, ON=0.5µs) | 22.3 µs | Baseline |
| + CPU/PIO work overlap | ~18 µs | Low effort |
| + Overclock to 200 MHz | **~13.5 µs** | One config line change |
| Fewer rows (12 rows) | ~13.4 µs | Application-dependent |
| Multi-SM PIO (all pins in PIO) | ~13.0 µs | High complexity |
| DMA-fed PIO | **~11.1 µs** | Only option that fits 12µs window |

**Recommended**: Overclock to 200 MHz + overlap optimization → ~13.5 µs.

---

## Open Questions

1. **LED optical response**: Do LEDs physically respond to sub-µs pulses? (turn-on/turn-off time)
2. **Intensity linearity**: Is time-averaged intensity linear with duty cycle for BCM?
3. **Minimum distinguishable levels**: Can all 16 BCM brightness levels be resolved optically?
4. **Hardware measurement**: Photodiode + external ADC or Saleae Logic analyzer needed for verification
