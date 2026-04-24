# G6 Panel Design Roadmap: v0.2.0 and v0.3.0

**Date**: 2026-04-02 | **Author**: M. Reiser | **Status**: Ready for review

---

## v0.2.0 — Four changes (launch now)

Same pin assignments and board layout as v0.1.3 Minimal re-routing.

| # | Change | Detail |
|---|--------|--------|
| 1 | **Route EINT to GP45** | External trigger for 2P microscope sync. Trace from header (J3-1/J5-1) to MCU. |
| 2 | **33 ohm series termination on MISO** | SPI signal integrity (GP35/SPI0_RX). |
| 3 | **Normal LED polarity** | Janelia batch had reversed polarity (Col LOW = ON). Restore to normal (Col HIGH = ON) for better brightness uniformity under load. See analysis below. |
| 4 | **160 ohm resistors** | Replace 240 ohm. +50% brightness (18.5 mA vs 12.4 mA), safe at 40 C. See analysis below. |

---

## v0.3.0 — Full redesign (in parallel)

PIO pin rearrangement, SPI relocation, and board shrink.

| # | Change | v0.2.0 | v0.3.0 | Rationale |
|---|--------|--------|--------|-----------|
| 1 | **Contiguous rows** | GP21-31 + GP36-44 (gap at SPI) | **GP21-40 (contiguous)** | Enables full PIO row driving |
| 2 | **SPI relocation** | SPI0 on GP32-35 | **SPI1 on GP44-47** | Frees GP32-35 for rows |
| 3 | **EINT** | GP45 (from v0.2.0) | **GP45 (unchanged)** | Already in PIO1 range, supports `wait pin` |
| 4 | **Board size** | Oversized (test board) | **Target size or smaller** | Production form factor |

---

## LED polarity: why switch to normal?

The original iorodeo design uses normal polarity (Col HIGH = ON) — there is nothing wrong with that convention. The Janelia batch was assembled with reversed LEDs (Col LOW = ON). We are restoring normal polarity for v0.2.0.

The UCC27517 gate drivers have asymmetric output impedance: **0.55 ohm sinking vs 1.3 ohm sourcing**. The row driver carries the summed current of all lit columns (up to 20x). In reversed polarity the row sources through the higher impedance, amplifying voltage droop under load.

| R_col | Normal polarity (1 vs 20 LEDs) | Reversed polarity (1 vs 20 LEDs) |
|-------|-------------------------------|----------------------------------|
| 240 ohm | 4.1% brightness variation | 9.3% variation |
| 160 ohm | **5.4% variation** | 13.4% variation |
| 120 ohm | 7.9% variation | 16.9% variation |

Single-LED brightness is identical either way. The difference only appears under multi-LED load, and worsens at lower R values. Normal polarity keeps uniformity under 6% at 160 ohm.

---

## Resistor: why 160 ohm?

**LED**: Starsealand XL0402YGC (570 nm, IF max = 20 mA DC). Bins ordered: brightness P08/P09, voltage VF/VG, wavelength YG11/YG12.

Our VF/VG voltage bins (at 5 mA, per Starsealand bin spec) narrow the forward voltage range to **1.85-2.15 V** at 5 mA. At operating current (~19 mA), VF rises ~0.1 V, giving a worst-case of ~1.95 V.

| R_col | I (binned worst case, VF~1.95V) | I (typical, VF=2.0V) | vs 240 ohm |
|-------|--------------------------------|---------------------|------------|
| 240 ohm (Janelia batch) | 12.7 mA | 12.4 mA | -- |
| **160 ohm (v0.2.0)** | **18.8 mA** | **18.5 mA** | **+50%** |
| 150 ohm | 20.1 mA | 19.8 mA | +60% (borderline) |

160 ohm guarantees <= 20 mA across all bins with margin. Yageo **RC0201FR-07160RL** (0201, 1%, E96).

---

## Current scan architecture (v0.2.0)

```
Columns GP1-20 (contiguous) ──── PIO0 "out pins, 20"      single-cycle, 20 bits
Rows GP21-31 + GP36-44 (gap) ─── CPU gpio_set/clr_mask64  one-hot activation per row
SPI GP32-35 ──────────────────── Hardware SPI0              frame data from arena controller
```

**Measured performance** (PIOSCAN + noInterrupts + multicore lockout):
- Row overhead: 0.61 us/row | Jitter: **0.000 us** (640k measurements, zero outliers)
- BCM burst at T=0.5 us: 9.42 us for 20 rows x 4 bit-planes
- Fits 15 us 2P scan window with 5.6 us margin | Frame rate: 400 Hz

Zero jitter is achieved by locking out all other activity during the ~10 us burst: Core 1 paused, Core 0 interrupts disabled. This works because the burst is short relative to the ~115 us idle period.

---

## v0.3.0 target architecture: fully autonomous PIO scanning

```
PIO0: out pins, 20 ──── columns GP1-20         (same as v0.2.0)
PIO1: out pins, 20 ──── rows GP21-40           (NEW: replaces CPU row switching)
SPI1: GP44-47 ────────── hardware SPI1          (moved from GP32-35)
EINT: GP45 ───────────── PIO1 wait pin / CPU    (in PIO1 range, unchanged from v0.2.0)
CPU:  free during entire scan burst
```

### Why this matters

| Capability | v0.2.0 (CPU rows) | v0.3.0 (PIO rows) |
|-----------|-------------------|-------------------|
| **Row overhead** | 0.61 us/row | ~0.37 us/row |
| **CPU during burst** | Locked in scan loop | **Free** |
| **noInterrupts required** | Yes (entire burst) | No (hardware-timed) |
| **Max BCM bits at T=0.5 us** | 4 bits (9.4 us burst) | **5-6 bits** (more headroom) |
| **Faster trigger rates** | 8 kHz (needs 115 us idle) | **16+ kHz feasible** |

**What this enables:**

1. **More grayscale** — 5-bit BCM (32 levels) within the same 15 us window. Lower PIO overhead saves 4.8 us per frame.

2. **Faster external triggers** — at 16 kHz (62.5 us period), idle shrinks to ~50 us. CPU rows require `noInterrupts()` for 10 us = 20% of the period. PIO rows need zero CPU time, so SPI and precompute run unimpeded.

3. **CPU processing during scan** — real-time brightness correction, pattern generation, or sensor readback while hardware handles the scan autonomously.

4. **Simpler firmware** — no multicore lockout, no `noInterrupts()` window, no `__not_in_flash_func` / `noinline` workarounds. PIO scan is jitter-free by construction.

---

## Analysis method

Circuit: KVL with UCC27517 impedance (R_OH=1.3, R_OL=0.55 ohm), LED VF from XL0402YGC datasheet + Starsealand bin spec at 5 mA, 5V supply. Timing: 640k-measurement jitter sweep (Phase 4), multi-SM PIO benchmarks (Phase 3d). See PCB_REDESIGN_ANALYSIS.md for full pin assignment analysis.
