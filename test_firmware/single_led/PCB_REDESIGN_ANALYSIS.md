# G6 Panel PCB Redesign Analysis

## Document Purpose

This document analyzes the current G6 panel PCB pin assignments, evaluates how they interact with PIO-based scanning and hardware SPI, and presents redesign options ranging from minimal (bodge wire) to optimal (full re-route). The analysis is grounded in measured timing data from extensive firmware development and jitter characterization.

---

## 1. Current PCB Design

### 1.1 Pin Assignments (Current Board)

| GPIO | Function | Notes |
|------|----------|-------|
| GP0 | XIP_CS1n | PSRAM chip select (RP2354). **Cannot be repurposed.** |
| GP1-GP20 | Column pins (COL_MCU_00–19) | 20 contiguous pins. Drive LED columns via UCC27517 gate drivers. |
| GP21-GP31 | Row pins (ROW_MCU_00–10) | First 11 of 20 row pins. |
| GP32 | SPI0_TX (MOSI) | Hardware SPI0 slave — receives frame data from arena controller. |
| GP33 | SPI0_CSn (CS) | Hardware SPI0 chip select. |
| GP34 | SPI0_SCK (SCK) | Hardware SPI0 clock (25–30 MHz). |
| GP35 | SPI0_RX (MISO) | Hardware SPI0 data out (slave → master). |
| GP36-GP44 | Row pins (ROW_MCU_11–19) | Remaining 9 of 20 row pins. |
| GP45-GP47 | Unconnected | Not routed to any MCU function. |
| EINT | External interrupt | On header connectors J3-1 and J5-1. **Not routed to any MCU GPIO.** |

### 1.2 Pin Map Diagram

```
GP0  [PSRAM CS]
GP1  ┐
GP2  │
...  │ 20 CONTIGUOUS column pins (PIO0-friendly)
GP19 │
GP20 ┘
GP21 ┐
GP22 │
...  │ 11 row pins (first segment)
GP30 │
GP31 ┘
GP32 ┐
GP33 │ SPI0 (MOSI/CS/SCK/MISO) ← CREATES GAP IN ROW PINS
GP34 │
GP35 ┘
GP36 ┐
GP37 │
...  │ 9 row pins (second segment)
GP43 │
GP44 ┘
GP45 ─ unconnected
GP46 ─ unconnected
GP47 ─ unconnected
```

### 1.3 Consequences of Current Design

**What works well:**
- Column pins GP1-20 are perfectly contiguous for PIO `out pins, 20` — this is the performance-critical data path
- Hardware SPI0 at 25-30 MHz for frame reception — proven, low-latency, no PIO SM consumed
- Row pins work fine with CPU-based `gpio_set_mask64()` / `gpio_clr_mask64()` since row switching only requires one-hot activation (one pin per trigger)

**What's suboptimal:**
- **GP32-35 gap splits rows into two segments** (GP21-31 + GP36-44). PIO1 `out pins, 24` must write through the gap positions. This works only because those pins keep SPI funcsel (PIO output is ignored by the pin mux), but it's fragile and prevents PIO1 from controlling both rows and SPI simultaneously.
- **EINT is not routed to MCU** — external trigger from microscope cannot reach the processor without a bodge wire.
- **GP45-47 are wasted** — three GPIOs with no function.

---

## 2. LED Polarity and PIO Impact

### 2.1 Two Polarity Conventions

The G6 panels exist in two LED polarity configurations:

**Normal polarity (iorodeo panels):**
- **Column HIGH + Row LOW = LED ON** (anode on column, cathode on row)
- This is the convention used in the iorodeo production firmware (`display.cpp`: `digitalWrite(COL_PIN(j), HIGH)` for ON, `gpio_put(ROW_PIN(i), 0)` for active row)

**Reversed polarity (Janelia batch — our test hardware):**
- **Column LOW + Row HIGH = LED ON** (anode on row, cathode on column)
- This is the convention used in our test firmware. Column patterns are inverted before PIO output (`pio_col_word = (~pattern) & 0xFFFFF`)

**Important**: The iorodeo firmware was developed for their normal-polarity panels. It does NOT confirm reversed polarity. Our Janelia batch panels are reversed, which we discovered during Phase 1 testing. A future PCB revision should document which polarity the assembled panels use.

### 2.2 Impact on PIO Column Driving

| Polarity | PIO pattern for "pixel ON" | All-OFF state | PIO program |
|----------|---------------------------|---------------|-------------|
| Normal (iorodeo) | Bit = 1 (HIGH) | `0x00000` | `out pins, 20` directly |
| **Reversed (Janelia batch)** | Bit = 0 (LOW) | `0xFFFFF` | Invert in software before push, or use `mov pins, ~osr` in PIO |

**Conclusion: LED polarity is a trivial software concern.** One XOR or one PIO instruction handles either polarity. It does not constrain pin assignment or PIO architecture.

### 2.3 Impact on PIO Row Driving

For rows, only one row is active at a time:

| Polarity | Active row signal | PIO pattern | CPU alternative |
|----------|------------------|-------------|-----------------|
| Normal (iorodeo) | LOW (one-cold) | One `0` in field of `1`s | `gpio_put(row, 0)` for ON, `gpio_put(row, 1)` for OFF |
| **Reversed (Janelia)** | HIGH (one-hot) | One `1` in field of `0`s | `gpio_set_mask64` for ON, `gpio_clr_mask64` for OFF |

Again, trivially handled in software. No architectural impact.

---

## 3. PIO Pin Range Constraints (RP2350)

### 3.1 GPIOBASE Mechanism

The RP2350 extends GPIO to 48 pins (GP0-47). Each PIO block can only access a 32-pin window determined by its GPIOBASE register:

```
PIO0 (GPIOBASE=0):   GP0  ─────────────── GP31
PIO1 (GPIOBASE=16):       GP16 ─────────────── GP47
```

**Overlap region**: GP16-31 is visible to both PIO blocks.

### 3.2 Current Design PIO Coverage

```
PIO0 (GPIOBASE=0):  [GP0 ──── GP20(cols) ──── GP31]
                      ✅ All 20 column pins visible

PIO1 (GPIOBASE=16): [GP16 ── GP21-31(rows) ── GP32-35(SPI!) ── GP36-44(rows) ── GP47]
                      ✅ All 20 row pins visible
                      ⚠️  GP32-35 SPI pins IN THE MIDDLE of PIO1's output range
```

PIO1 `out pins, 24` starting at GP21 writes bits 5-28 of PIO1's output register (since GP21 = GPIOBASE+5). Bits 16-19 correspond to GP32-35 (SPI pins). These bits are written by PIO but ignored by the pin mux because GP32-35 retain SPI funcsel.

**This works today but has limitations:**
- Cannot use PIO1 for both row driving AND SPI slave simultaneously
- If SPI funcsel is ever accidentally changed, PIO would interfere with SPI communication
- Cannot use PIO1's `out pins` for exactly 20 row pins — must output 24 bits (wasting 4 bit positions)

### 3.3 Hardware SPI Pin Groups (RP2350)

SPI peripherals can only be mapped to specific GPIO groups (funcsel repeats every 8 pins):

| Peripheral | Pin Groups |
|------------|------------|
| **SPI0** | GP0-3, GP4-7, GP16-19, GP20-23, **GP32-35**, GP36-39 |
| **SPI1** | GP8-11, GP12-15, GP24-27, GP28-31, GP40-43, **GP44-47** |

---

## 4. Redesign Options

### 4.1 Option A: Minimal Change (Bodge Wire Only)

**Change**: Wire EINT header to GP45 on MCU. No PCB re-route.

```
GP0         XIP_CS1n (PSRAM)
GP1-20      Column pins (unchanged)
GP21-31     Row pins lower (unchanged)
GP32-35     SPI0 (unchanged)
GP36-44     Row pins upper (unchanged)
GP45        EINT (bodge wire from header J3-1 or J5-1)
GP46-47     Spare
```

| Aspect | Assessment |
|--------|------------|
| Columns | ✅ Contiguous (GP1-20), PIO0 `out pins, 20` |
| Rows | ⚠️ Split (GP21-31 + GP36-44), CPU `gpio_mask64` only |
| SPI | ✅ Hardware SPI0 on GP32-35 |
| EINT | ✅ Routed to GP45 |
| PIO row driving | ⚠️ Possible but writes through SPI pins (fragile) |
| Effort | Trivial — one wire |

**Best for**: Current boards. Gets EINT working immediately with zero risk.

### 4.2 Option B: Optimal Redesign (SPI1 on GP44-47)

**Change**: Move SPI from SPI0 (GP32-35) to SPI1 (GP44-47). Make rows fully contiguous.

```
GP0         XIP_CS1n (PSRAM)
GP1-20      Column pins (20 contiguous)   → PIO0 out pins, 20
GP21-40     Row pins (20 contiguous)      → PIO1 out pins, 20 (or CPU mask)
GP41        EINT (external trigger)       → PIO1 wait pin / CPU GPIO
GP42-43     Spare (sync output, debug)
GP44        SPI1_RX (MISO)
GP45        SPI1_CSn (CS)
GP46        SPI1_SCK (SCK)
GP47        SPI1_TX (MOSI)
```

| Aspect | Assessment |
|--------|------------|
| Columns | ✅ Contiguous (GP1-20), PIO0 `out pins, 20` |
| Rows | ✅ Contiguous (GP21-40), PIO1 `out pins, 20` — clean, no gap |
| SPI | ✅ Hardware SPI1 on GP44-47 (identical capability to SPI0) |
| EINT | ✅ GP41, visible to PIO1 for `wait pin` and CPU for GPIO read |
| PIO row driving | ✅ Clean — no SPI pins in the output range |
| Spare pins | GP42-43 free for sync output, debug, or second trigger |
| Firmware change | One line: `spi_init(spi0, ...)` → `spi_init(spi1, ...)` |
| Arena controller change | Re-wire SPI to new pin positions (same signals, different header pins) |
| Effort | PCB re-route + firmware update on both panel and controller |

**PIO coverage verification:**
```
PIO0 (GPIOBASE=0):  GP0-31  → columns GP1-20 ✅  (also sees rows GP21-31)
PIO1 (GPIOBASE=16): GP16-47 → rows GP21-40 ✅, EINT GP41 ✅, SPI GP44-47 (no conflict)
```

**What this enables:**
1. **Fully autonomous PIO scanning** — PIO1 can drive all 20 row pins via `out pins, 20` with no CPU involvement during scan bursts. Combined with PIO0 driving columns, the entire scan could be PIO-only.
2. **PIO1 `wait pin` for EINT** — GP41 is in PIO1's range. The trigger can wake a PIO state machine directly, enabling hardware-timed scan initiation with zero CPU latency.
3. **No funcsel conflicts** — SPI pins are completely outside the row range. PIO1 row output and SPI can operate simultaneously without interference.
4. **Simpler firmware** — No need for the GP32-35 gap workaround. Row precompute becomes a clean 20-bit mask instead of 24-bit with don't-care holes.

### 4.3 Option C: Maximum PIO Autonomy (PIO-Based SPI)

**Change**: Replace hardware SPI with PIO-based SPI slave. Frees GP32-35 entirely.

```
GP0         XIP_CS1n (PSRAM)
GP1-20      Column pins (20 contiguous)   → PIO0 out pins, 20
GP21-40     Row pins (20 contiguous)      → PIO1 out pins, 20
GP41        EINT (external trigger)
GP42        SPI MOSI (PIO-based slave)    → PIO0 or PIO1 SM
GP43        SPI SCK                       → PIO0 or PIO1 SM
GP44        SPI MISO                      → PIO0 or PIO1 SM
GP45        SPI CS                        → PIO0 or PIO1 SM
GP46-47     Spare
```

| Aspect | Assessment |
|--------|------------|
| Columns | ✅ Contiguous (GP1-20), PIO0 |
| Rows | ✅ Contiguous (GP21-40), PIO1 |
| SPI | ⚠️ PIO-based slave (consumes 1 SM, more complex, needs validation) |
| EINT | ✅ GP41 |
| Spare | GP46-47 (more free pins) |
| Effort | PCB re-route + new PIO SPI slave program + validation |

**Tradeoffs vs Option B:**
- **Pro**: SPI pin placement is fully flexible (any GPIO), frees GP32-35 for other uses
- **Con**: Consumes a PIO state machine (3 SM remain across both PIO blocks instead of 4)
- **Con**: PIO SPI slave at 25+ MHz is nontrivial to implement and validate — the iorodeo firmware already has a tested hardware SPI implementation with hot-plug resilience
- **Con**: No practical benefit over Option B since GP32-35 aren't needed for anything else

**Recommendation: Not worth the added complexity.** Option B achieves all the same pin layout benefits with proven hardware SPI.

---

## 5. Comparison of iorodeo Production Firmware vs Our Test Firmware

Understanding the baseline helps evaluate what the redesign enables.

| Aspect | iorodeo production firmware | Our test firmware (Mode A) |
|--------|---------------------------|---------------------------|
| **Column drive** | CPU `digitalWrite()` per column (slow) | PIO0 `out pins, 20` (single-cycle) |
| **Row drive** | CPU `gpio_put()` per row | CPU `gpio_set_mask64()` / `gpio_clr_mask64()` |
| **PIO usage** | None | PIO0 for columns |
| **Grayscale** | Linear PWM (equal-duration slots) | Binary-weighted BCM (4-bit, 16 levels) |
| **Timing** | Busy-wait delay loops (uncalibrated) | DWT cycle-counter (sub-µs precision) |
| **Jitter control** | None | Multicore lockout + noInterrupts + noinline + warm-up |
| **Scan trigger** | On SPI message arrival (async) | 8 kHz DWT-simulated trigger (phase-locked) |
| **Refresh** | Single-shot per SPI frame | Continuous 400 Hz |
| **SPI handling** | Core 0 (hardware SPI0 slave) | N/A (USB serial for testing) |
| **Display scanning** | Core 1 | Core 0 |
| **Frame data** | Inter-core queue (Core 0 → Core 1) | RAM buffer with precomputed BCM planes |
| **Coordinate mapping** | `sch_to_pos_index()` with NUM_COLOR=4 | Same mapping in `utilities.cpp` |

### Key Improvement: BCM Efficiency

The iorodeo firmware uses **linear dimming** (16 equal-duration slots for 16 gray levels). A pixel at intensity 10 stays on for 10 slots. This requires 16 passes per row per frame.

Our firmware uses **binary-coded modulation** (4 bit-plane passes for 16 gray levels). Bit-plane durations are weighted 1:2:4:8. Total ON time for intensity 10 (binary 1010) = T×(2+8) = 10T. Same result, but only **4 passes instead of 16** — a 4× reduction in scan overhead.

---

## 6. Production Architecture with Redesigned PCB (Option B)

### 6.1 System Block Diagram

```
                    ┌─────────────────────────────────────┐
  Arena             │           RP2354 (RP2350+PSRAM)     │
  Controller        │                                     │
    │               │  ┌──────────┐    ┌──────────────┐   │
    │  SPI1 ────────┼──│ Core 0   │    │   Core 1     │   │
    │  (GP44-47)    │  │ SPI recv │───▶│ (locked out  │   │
    │               │  │ + decode │ Q  │  or idle)    │   │
    │               │  └──────────┘    └──────────────┘   │
    │               │       │                             │
    │               │       ▼ pixel_data[20][20]          │
    │               │  ┌──────────┐                       │
    │               │  │ Core 0   │                       │
  Microscope        │  │ Scan     │                       │
  Trigger ──────────┼──│ Loop     │                       │
  (GP41=EINT)       │  │          │                       │
                    │  └──┬───┬───┘                       │
                    │     │   │                            │
                    │     ▼   ▼                            │
                    │  ┌─────┐ ┌──────┐                   │
                    │  │PIO0 │ │ CPU  │                    │
                    │  │cols │ │ rows │                    │
                    │  │GP1- │ │GP21- │                    │
                    │  │ 20  │ │ 40   │                    │
                    │  └──┬──┘ └──┬───┘                   │
                    └─────┼───────┼───────────────────────┘
                          ▼       ▼
                    ┌─────────────────┐
                    │  20×20 LED      │
                    │  Passive Matrix │
                    └─────────────────┘
```

### 6.2 Timing Budget (Per 125 µs Trigger Period)

```
  0 µs          ~10 µs        ~15 µs                              125 µs
  │◄── BURST ──►│◄── IDLE ────────────────────────────────────────►│
  │             │                                                   │
  │ noInterrupts│  interrupts()                                     │
  │ row ON      │  ┌─ precompute next row BCM data (~2 µs)         │
  │ 4 bit-planes│  ├─ check SPI for new frame (~1 µs)              │
  │ row OFF     │  ├─ swap frame buffer if ready                    │
  │ stats       │  ├─ SysTick / millis() servicing                  │
  │             │  └─ idle until next trigger                       │
  │             │                                                   │
  │◄─ 9.4 µs ─►│◄──────────── 115.6 µs ──────────────────────────►│
```

**The ~115 µs idle window is enormous** compared to the ~10 µs burst. SPI frame reception (65 µs for a 203-byte frame at 25 MHz), BCM precomputation, and buffer swaps all fit comfortably.

### 6.3 Dual-Core Strategy

| Core | Role | Interrupts | Notes |
|------|------|-----------|-------|
| **Core 0** | Scan loop + SPI | `noInterrupts()` during burst only | Handles trigger wait, BCM burst, and SPI between triggers |
| **Core 1** | Locked out (or available for future use) | N/A | `multicore_lockout_start_blocking()` at startup. Could be unlocked for compute tasks if needed. |

**Why Core 0 handles both scanning and SPI**: In the iorodeo firmware, Core 0 does SPI and Core 1 does display. But our zero-jitter architecture requires `noInterrupts()` during the burst, which is simpler on a single core. SPI reception happens during the 115 µs idle window when interrupts are enabled.

---

## 7. Measured Performance Data

All measurements from our test firmware on current hardware (GP32-35 gap present).

### 7.1 Zero-Jitter BCM Results (Mode A: PIO + noInterrupts + Lockout + Warm-up)

640,000 measurements: 4 T values × 16 intensities × 10,000 frames at 8 kHz.

| T (µs) | Burst min (µs) | Burst max (µs) | Jitter (µs) | Outliers | Fits 15 µs? |
|---------|----------------|----------------|-------------|----------|-------------|
| 0.25 | 5.727 | 5.727 | **0.000** | 0/160k | YES |
| 0.50 | 9.433 | 9.433 | **0.000** | 0/160k | YES |
| 0.75 | 13.227 | 13.227 | **0.000** | 0/160k | YES (barely) |
| 1.00 | 16.960 | 16.960 | **0.000** | 0/160k | NO |

### 7.2 Recommended Operating Point

| Parameter | Value |
|-----------|-------|
| BCM bits | 4 (16 intensity levels) |
| Base T | 0.50 µs |
| Burst time | 9.43 µs (deterministic) |
| Budget margin | 5.6 µs (of 15 µs scan window) |
| Frame rate | 400 Hz (8000 Hz trigger / 20 rows) |
| Jitter | 0.000 µs |

### 7.3 Zero-Jitter Recipe (All Required)

1. `__not_in_flash_func()` + `__attribute__((noinline))` on all timing-critical functions
2. `noInterrupts()` during scan burst (not necessarily entire loop)
3. `multicore_lockout_start_blocking()` to pause Core 1
4. 100-trigger warm-up inside lockout before measurement begins

Without ingredient 1 (`noinline`): **7+ µs jitter** (compiler silently inlines into flash callers).
Without ingredient 3 (lockout): **0.7-2.2 µs jitter** (Core 1 USB bus contention).
With all four: **0.000 µs jitter** across 640,000 measurements.

### 7.4 Scan Mode Comparison (from Earlier Phases)

These modes were evaluated for full-panel scanning before BCM was added:

| Mode | Description | Row Overhead | Jitter (ON=0.25µs) | noInterrupts compatible? |
|------|-------------|-------------|---------------------|--------------------------|
| SCAN | CPU drives both rows and columns | 0.76 µs | ~5 µs | Yes |
| PIOSCAN | PIO0 columns + CPU rows (polling) | 0.61 µs | 1.69 µs | Yes |
| DMASCAN | DMA feeds PIO0 + ISR row switch | 0.57 µs | 5.23 µs | No (needs ISR) |
| MSMSCAN | Dual PIO (PIO0 cols + PIO1 rows) + bridge ISR | 0.37 µs | 11.77 µs | No (needs ISR) |
| BURST | PIOSCAN with simulated 8 kHz trigger | 0.61 µs | 0.00 µs | Yes |

**Key insight**: MSMSCAN has the lowest overhead (0.37 µs) but the worst jitter because it requires bridge ISRs. For 2P sync, PIOSCAN (Mode A) is the only viable architecture — it's `noInterrupts()` compatible, giving zero jitter.

**With Option B redesign** (contiguous rows): MSMSCAN could potentially be revisited with PIO1 `out pins, 20` for rows, eliminating the bridge ISR. This would combine MSMSCAN's low overhead with PIOSCAN's zero jitter — the best of both worlds. However, this requires significant firmware development and validation.

---

## 8. Dead Ends and Lessons Learned

These are documented to prevent future re-exploration of paths we've already ruled out.

### 8.1 SIO is NOT DMA-accessible

**Attempt**: Use DMA to write row GPIO patterns to `sio_hw->gpio_set`/`gpio_clr` (0xD0000000).
**Result**: DMA writes silently ignored. LEDs never lit.
**Root cause**: The SIO (Single-cycle I/O) block is a per-core peripheral. DMA bus masters are not attributed to either core, so SIO ignores their writes.
**Implication**: Row switching MUST involve CPU (ISR or polling) or PIO — never pure DMA.

### 8.2 `pio_sm_restart()` Clears Pindirs on RP2350

**Attempt**: Use `pio_sm_restart()` to reset PIO state machine between scans.
**Result**: LEDs went dark after restart.
**Root cause**: On RP2350, `pio_sm_restart()` resets pin directions to input.
**Fix**: Use `pio_sm_clear_fifos()` + `pio_sm_exec(pio_encode_jmp(offset))` to reset PC while preserving pin state. If restart is needed, re-set pindirs with `pio_sm_set_consecutive_pindirs()` afterward.

### 8.3 Compiler Inlines `__not_in_flash_func` into Flash Callers

**Attempt**: Mark timing-critical functions with `static __not_in_flash_func(func_name)`.
**Result**: Jitter regressed from 0 µs to 7+ µs without any code logic changes.
**Root cause**: The compiler inlined the `static` function into a flash-resident caller, silently defeating the SRAM placement.
**Fix**: Always pair with `__attribute__((noinline))`. This is mandatory and easy to forget.

### 8.4 DMA-Based Scanning (DMASCAN) Adds Jitter

**Attempt**: DMA feeds column patterns to PIO TX FIFO; ISR on DMA completion switches rows.
**Result**: 0.57 µs row overhead (good) but 5-16 µs jitter (bad).
**Root cause**: DMA completion ISR has variable latency due to interrupt priority, pending interrupts, and pipeline state.
**Implication**: Any architecture requiring ISRs during the scan burst will have µs-scale jitter. The scan burst must be fully polling-based.

### 8.5 Multi-SM PIO (MSMSCAN) Has Worst Jitter at Short ON Times

**Attempt**: PIO0 SM for columns, PIO1 SM for rows, bridge ISRs to coordinate.
**Result**: Lowest overhead (0.37 µs) but highest jitter (11-23 µs at ON=0.25-0.5 µs).
**Root cause**: Bridge ISRs between state machines add variable latency. At short ON times, the ISR latency dominates.
**Implication**: Inter-SM coordination via ISRs is unsuitable for 2P sync. Would need a fully autonomous PIO solution (no ISRs) or CPU polling of PIO IRQ flags.

### 8.6 `Serial.available()` Causes Early Scan Termination

**Attempt**: Check `Serial.available()` periodically during scan loops to allow user to stop.
**Result**: Scans terminate after a few frames because host setup commands leave buffered data.
**Fix**: Drain serial buffer at start of scan commands (`while(Serial.available()) Serial.read();`). Use newline-only stop detection.

### 8.7 GPIOBASE Must Be Set Before Loading PIO Programs

**Attempt**: Use PIO1 for row pins (GP21-44) without setting GPIOBASE.
**Result**: PIO1 addressed wrong pins (GP5-28 instead of GP21-44).
**Root cause**: Arduino-Pico defaults all PIO blocks to GPIOBASE=0. Must explicitly set `pio1->gpiobase = 16` before loading programs.
**Note**: SDK pin functions (`sm_config_set_out_pins`, `pio_sm_set_consecutive_pindirs`) take absolute GPIO numbers; hardware subtracts GPIOBASE internally.

---

## 9. Recommendation

### For Current Boards (Immediate)

**Option A**: Bodge wire EINT to GP45. This enables external trigger testing with zero risk to existing functionality. All firmware development can continue.

### For Next PCB Revision

**Option B**: Move SPI to SPI1 (GP44-47), make rows contiguous (GP21-40), route EINT to GP41.

This is the clear winner:
- **Strictly better than current design** in every dimension
- **Hardware SPI preserved** — no PIO SM consumed, proven 25-30 MHz operation
- **Both pin ranges contiguous** — enables future fully-autonomous PIO scanning
- **EINT properly routed** — visible to PIO1 for hardware-timed trigger
- **Firmware change is trivial** — `spi0` → `spi1`, update pin constants
- **2 spare pins** (GP42-43) — room for sync output, debug, or second trigger

Option C (PIO-based SPI) adds complexity and consumes a PIO state machine for no practical benefit over Option B.

---

## 10. Pin Assignment Summary (Option B)

```
┌────────┬────────────────────────────────────┬───────────────────┐
│  GPIO  │  Function                          │  Interface        │
├────────┼────────────────────────────────────┼───────────────────┤
│  GP0   │  XIP_CS1n (PSRAM)                  │  Fixed (hardware) │
│  GP1   │  COL_MCU_00                        │  PIO0 out pins    │
│  GP2   │  COL_MCU_01                        │       │           │
│  ...   │  ...                               │       │           │
│  GP20  │  COL_MCU_19                        │       ▼           │
│  GP21  │  ROW_MCU_00                        │  PIO1 out pins    │
│  GP22  │  ROW_MCU_01                        │  (or CPU mask)    │
│  ...   │  ...                               │       │           │
│  GP40  │  ROW_MCU_19                        │       ▼           │
│  GP41  │  EINT (external trigger)           │  PIO1 wait / GPIO │
│  GP42  │  Spare (sync output?)              │  GPIO             │
│  GP43  │  Spare (debug?)                    │  GPIO             │
│  GP44  │  SPI1_RX (MISO)                    │  Hardware SPI1    │
│  GP45  │  SPI1_CSn (CS)                     │       │           │
│  GP46  │  SPI1_SCK (SCK)                    │       │           │
│  GP47  │  SPI1_TX (MOSI)                    │       ▼           │
└────────┴────────────────────────────────────┴───────────────────┘
```

---

*Document generated from G6 Panel timing characterization (Phases 0-5a). All measurements at 150 MHz, 8 kHz trigger rate, 20×20 passive LED matrix.*
