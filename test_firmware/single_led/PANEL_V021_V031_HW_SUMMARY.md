# G6 Panel Hardware Summary: v0.2.1 and v0.3.1

**Purpose**: A standalone reference capturing all firmware- and test-visible hardware differences between the two G6 panel revisions currently in hand. Use this document to seed a fresh planning/testing session.

**Source**: [iorodeo/LED-Display_G6_Hardware_Panel PR #4](https://github.com/iorodeo/LED-Display_G6_Hardware_Panel/pull/4) (branch `prod_v0p2r0`, head commit `89960365`, "fix EINT note position")

**Production zips**:
- `panel_rp2354_20x20_v0p2/production/v0p2r1/G6_panel_45mm_RP2354_v0.2.1.zip`
- `panel_rp2354_20x20_v0p3/production/v0p3r1/G6_panel_45mm_RP2354_v0.3.1.zip`

---

## TL;DR — the one-paragraph version

**v0.2.1** is a **minimal-change update to the original v0.1 Janelia layout**: same pin assignments as v0.1, but with (a) LED orientation flipped back to normal polarity (anode=column, cathode=row), (b) EINT now properly routed to GP45 (no more bodge wire), (c) 33 Ω MISO series-termination resistor added, (d) current-limit resistor value standardized to a single uniform value across all 20 columns, (e) board shrunk to 45 × 45 mm. **Firmware written for v0.1 still works with only minor pin-constant / polarity updates.**

**v0.3.1** is a **full pin-map redesign** on top of everything in v0.2.1: XIP_CS1n moved from GP0 to GP47 (freeing GP0 for column 0), columns now GP0–GP19 (contiguous from 0), rows now GP20–GP39 (20 contiguous pins, no SPI gap), SPI moved from SPI0 on GP32–35 to SPI1 on GP40–43, EINT still GP45. **Firmware needs substantial pin-constant updates** but the new layout enables both PIO0 columns and PIO1 rows to use clean `out pins, 20` without workarounds — unlocks fully-autonomous PIO scanning.

**BOMs are bit-identical** between the two revisions — same MCU, same LEDs, same current-limit resistor, same drivers, same connectors. Only pin assignments, LED orientation, and PCB routing differ.

---

## Common hardware (identical in both v0.2.1 and v0.3.1)

### Mechanical
- **Board size**: 45 × 45 mm (square panel)
- **Connectors**:
  - J1: JST-SH 4-pin (BM04B-SRSS-TB, LCSC C160390) — USB/Ethernet-style connector, location pending verification
  - J2, J3: 1×5 **male right-angle SMD pin headers** (Harwin M20-8890545R, LCSC C46061678) — bottom side of panel
  - J4, J5: 1×5 **female receptacles** (Samtec SMH-105-02-X-S, LCSC C5142238) — top side of panel for daisy-chain
  - All at 2.54 mm pitch

### Electrical — MCU and memory
- **MCU**: RP2354B (RP2350B QFN-80 with stacked PSRAM, LCSC C39843328)
- **External PSRAM**: APS6404L-3SQR (LCSC C3040877), connected via QSPI with chip-select on XIP_CS1n
- **Crystal**: ABM8-272-T3 (LCSC C20625731), SMD 3.2 × 2.5 mm, with 2× 15 pF load caps (C1, C2)
- **3.3 V LDO**: AP2112K-3.3 (LCSC C51118), SOT-23-5, powered from +5 V USB
- **Decoupling**: 52× 100 nF 0201, 18× 10 µF 0402 bulk, 4× 4.7 µF 0402 (for QSPI/PSRAM)
- **Inductor**: L1, 3.3 µH polarized

### Electrical — LED matrix
- **LEDs**: 400 × Starsealand XL0402YGC (0402 yellow-green, 570 nm dominant wavelength, LCSC C9900113976)
  - Binning: bright bins P08/P09, voltage bins VF/VG, wavelength bins YG11/YG12 (per prior order)
  - IF max 20 mA DC, IFP 30 mA pulsed, forward voltage ≈ 1.95–2.30 V at operating current
- **LED drivers**: 40 × UCC27517 single-channel gate drivers (SOT-23-5, LCSC C99395) — one per MCU row/column pin
- **Current-limiting resistors**: R9–R28 (20 × one-per-column), all populated with LCSC **C851657** 0201 — **uniform value across all 4 symbolic types (R_T0/T1/T2/T3)**
- **LED polarity — NORMAL** (both v0.2.1 and v0.3.1):
  - Anode → column (through current-limit resistor)
  - Cathode → row (direct)
  - Drive logic: **column HIGH + row LOW = LED ON**
  - This is opposite to the original v0.1 Janelia batch, which was reversed polarity.

### Electrical — signal conditioning
- **R29**: 33 Ω 0201 on MISO (SPI MISO series-termination — new in v0.2+, was absent in v0.1)
- **R6**: 33 Ω 0201 — second 33 Ω, check which signal it terminates
- **R7, R8**: 27 Ω 0201 on USB D+/D− (USB impedance matching)
- **R1, R4**: 10 kΩ 0201 — pullups (likely XIP_CS1n and CS0)
- **R2, R3, R5**: 1 kΩ 0201 — pullups, purpose TBD in firmware pin-definition audit

### User interface
- **SW1, SW2**: TS-1088-AR02016 tactile push switches (LCSC C720477) — typically BOOTSEL + RESET for Pico/RP2350 designs

---

## What's DIFFERENT between v0.2.1 and v0.3.1

### 1. RP2354 pin assignment map (firmware-critical)

**Only GPIO pins relevant to firmware are shown. Power/system pins (IOVDD, DVDD, QSPI, VREG, USB, XIN/XOUT, SWCLK/SWD) are identical.**

| GPIO | v0.2.1 | v0.3.1 | Notes |
|------|--------|--------|-------|
| GP0 | **XIP_CS1n** (PSRAM CS) | **COL_MCU_00** (column 0) | PSRAM moved in v0.3 to free GP0 for column use |
| GP1 | COL_MCU_00 | COL_MCU_01 | v0.3 columns shift down by 1 |
| GP2 | COL_MCU_01 | COL_MCU_02 | |
| GP3 | COL_MCU_02 | COL_MCU_03 | |
| GP4 | COL_MCU_03 | COL_MCU_04 | |
| GP5 | COL_MCU_04 | COL_MCU_05 | |
| GP6 | COL_MCU_05 | COL_MCU_06 | |
| GP7 | COL_MCU_06 | COL_MCU_07 | |
| GP8 | COL_MCU_07 | COL_MCU_08 | |
| GP9 | COL_MCU_08 | COL_MCU_09 | |
| GP10 | COL_MCU_09 | COL_MCU_10 | |
| GP11 | COL_MCU_10 | COL_MCU_11 | |
| GP12 | COL_MCU_11 | COL_MCU_12 | |
| GP13 | COL_MCU_12 | COL_MCU_13 | |
| GP14 | COL_MCU_13 | COL_MCU_14 | |
| GP15 | COL_MCU_14 | COL_MCU_15 | |
| GP16 | COL_MCU_15 | COL_MCU_16 | |
| GP17 | COL_MCU_16 | COL_MCU_17 | |
| GP18 | COL_MCU_17 | COL_MCU_18 | |
| GP19 | COL_MCU_18 | COL_MCU_19 | v0.3 ends columns here (20 contiguous) |
| GP20 | COL_MCU_19 | **ROW_MCU_00** (row 0 — NEW) | v0.3 starts rows here |
| GP21 | ROW_MCU_00 | ROW_MCU_01 | |
| GP22 | ROW_MCU_01 | ROW_MCU_02 | |
| GP23 | ROW_MCU_02 | ROW_MCU_03 | |
| GP24 | ROW_MCU_03 | ROW_MCU_04 | |
| GP25 | ROW_MCU_04 | ROW_MCU_05 | |
| GP26 | ROW_MCU_05 | ROW_MCU_06 | |
| GP27 | ROW_MCU_06 | ROW_MCU_07 | |
| GP28 | ROW_MCU_07 | ROW_MCU_08 | |
| GP29 | ROW_MCU_08 | ROW_MCU_09 | |
| GP30 | ROW_MCU_09 | ROW_MCU_10 | |
| GP31 | ROW_MCU_10 | ROW_MCU_11 | |
| **GP32** | **MOSI** (SPI0 MOSI) | ROW_MCU_12 | v0.2.1 has **SPI0 in the middle of row range** (gap). v0.3.1 moved SPI out. |
| **GP33** | **CS0** (SPI0 CSn) | ROW_MCU_13 | |
| **GP34** | **SCK** (SPI0 SCK) | ROW_MCU_14 | |
| **GP35** | **MISO** (SPI0 RX via R29) | ROW_MCU_15 | |
| GP36 | ROW_MCU_11 | ROW_MCU_16 | v0.2 rows resume after SPI gap |
| GP37 | ROW_MCU_12 | ROW_MCU_17 | |
| GP38 | ROW_MCU_13 | ROW_MCU_18 | |
| GP39 | ROW_MCU_14 | ROW_MCU_19 | v0.3 ends rows here (20 contiguous, no gap) |
| GP40 | ROW_MCU_15 | **MOSI** (SPI1 RX, slave) | v0.3 SPI moved to SPI1 on GP40-43 |
| GP41 | ROW_MCU_16 | **CS0** (SPI1 CSn) | |
| GP42 | ROW_MCU_17 | **SCK** (SPI1 SCK) | |
| GP43 | ROW_MCU_18 | **MISO** (SPI1 TX via R29) | |
| GP44 | ROW_MCU_19 | NC (spare) | v0.3 only — ADC4-capable |
| **GP45** | **EINT** (external trigger) | **EINT** (external trigger) | **SAME** — only shared firmware-visible pin |
| GP46 | NC (spare) | NC (spare) | Both spare — ADC6-capable |
| GP47 | NC (spare) | **XIP_CS1n** (PSRAM CS — NEW) | v0.3 moved PSRAM CS to free GP0 |

**Key implications for firmware:**

| Concept | v0.2.1 | v0.3.1 |
|---------|--------|--------|
| Column pins | GP1 – GP20 (20 contiguous) | **GP0 – GP19** (20 contiguous, shifted down by 1) |
| Row pins | GP21 – GP31 + GP36 – GP44 (**split, gap at GP32-35**) | **GP20 – GP39** (20 contiguous, no gap) |
| Column PIO base | GP1 with `out pins, 20` on PIO0 | **GP0** with `out pins, 20` on PIO0 |
| Row PIO feasibility | NO — split pins require CPU `gpio_set/clr_mask64()` (fine for PIOSCAN mode) | **YES** — PIO1 with GPIOBASE=16 + `out pins, 20` starting at GP20 (enables fully-PIO scanning) |
| SPI peripheral | **SPI0** (hardware) on GP32-35 | **SPI1** (hardware) on GP40-43 |
| MISO termination | R29 (33 Ω) between GP35 and header | R29 (33 Ω) between GP43 and header |
| SPI label convention | Same across both: MOSI/MISO from **arena-controller (master) perspective** | |
| PSRAM CS (XIP_CS1n) | GP0 (funcsel 9) | **GP47** (funcsel 9) |
| EINT (external trigger) | GP45 | GP45 — **identical** |
| Spare / unused GPIOs | GP46, GP47 | GP44, GP46 |

### 2. PIO programming differences

The pin rearrangement has direct consequences for the PIO scanning architecture:

**v0.2.1 (identical architecture to v0.1)**:
- PIO0 drives columns via `out pins, 20` starting at GP1
- Rows are driven by CPU via `gpio_set_mask64` / `gpio_clr_mask64` — **cannot use PIO1 for rows**, because the GP32-35 gap sits in the middle of the row range and those pins belong to SPI0
- Production firmware mode: PIOSCAN (PIO columns + CPU rows + `noInterrupts()` + multicore lockout) — the zero-jitter recipe from Phase 4
- Firmware: minimal pin-constant updates vs v0.1 baseline

**v0.3.1**:
- PIO0 drives columns via `out pins, 20` starting at GP0 (shifted by 1)
- PIO1 (with GPIOBASE=16, covering GP16-GP47) can drive rows via `out pins, 20` starting at GP20 — **clean, no gap**
- Enables fully-autonomous PIO scanning: both columns AND rows driven by PIO state machines, CPU free during scan bursts, no `noInterrupts()` needed
- Potential performance upgrade: ~0.37 µs/row overhead (MSMSCAN-like) vs 0.61 µs/row (PIOSCAN)
- Enables 5-bit or 6-bit BCM within the same 15 µs scan window

### 3. MCU pin name re-assignments (test-visible labels on the schematic)

When probing with a scope or multimeter, signals on the two panels appear on different physical MCU pins:

| Signal | v0.2.1 QFN pin | v0.3.1 QFN pin |
|--------|----------------|----------------|
| COL_MCU_00 (first column) | pin 78 (GP1) | **pin 77** (GP0) |
| COL_MCU_19 (last column) | pin 20 (GP20) | **pin 19** (GP19) |
| ROW_MCU_00 (first row) | pin 21 (GP21) | **pin 20** (GP20) |
| ROW_MCU_19 (last row) | pin 55 (GP44) | **pin 48** (GP39) |
| MOSI | pin 40 (GP32) | **pin 49** (GP40) |
| CS0 | pin 42 (GP33) | **pin 52** (GP41) |
| SCK | pin 43 (GP34) | **pin 53** (GP42) |
| MISO | pin 44 (GP35) | **pin 54** (GP43) |
| EINT | pin 56 (GP45) | pin 56 (GP45) — same |
| XIP_CS1n (PSRAM CS) | pin 77 (GP0) | **pin 58** (GP47) |

### 4. Common (unchanged) items

- **LED polarity**: both panels use NORMAL polarity (col HIGH + row LOW = LED ON). Confirmed by tracing D1 in both schematics — pin 1 (K/cathode) connects to `ROW_00`, pin 2 (A/anode) connects through a current-limit resistor to `COL_00`.
- **Current-limiting resistors**: all 20 column resistors are populated with the same LCSC part (C851657), 0201 package. The schematic uses 4 symbolic values (R_T0/T1/T2/T3) for future multi-color support, but physically all 20 are the same value. **Verify in the BOM / on a physical board what that value is** — expected 160 Ω per prior optimization, but confirm before measurements.
- **UCC27517 drivers**: 40 total, SOT-23-5, non-inverting push-pull, drive from Teensy-logic levels to +5 V LED drive rail.
- **EINT on GP45**: both boards use GP45 for the external 8 kHz microscope trigger. Same firmware handler works on both.
- **USB/BOOTSEL**: both use GP0-through-GP47 USB on the RP2350B; BOOTSEL button is SW1 or SW2 (verify from PCB silkscreen).

---

## ERC / automated verification status

Both designs pass kicad-cli ERC with only **one warning each**, identical in nature:

```
warning: lib_symbol_mismatch
  Symbol 'Crystal_GND24' doesn't match copy in library 'Device'
```

This is a KiCad library-version mismatch on the crystal (Y1) symbol — cosmetic, zero wiring impact. No ERC errors. **Designs are clean from an electrical-rules perspective.**

---

## Firmware change matrix (what you'll touch when moving between versions)

Assume your firmware currently targets v0.1 Janelia batch with pin constants and reversed polarity.

| Change area | v0.1 → v0.2.1 | v0.1 → v0.3.1 |
|-------------|---------------|---------------|
| Column pin constants | Small shift if any | **Full rewrite**: COL_PIN[] maps to GP0-GP19 |
| Row pin constants | Small shift if any | **Full rewrite**: ROW_PIN[] maps to GP20-GP39 contiguous |
| LED polarity logic | **FLIP**: was `col LOW + row HIGH`, now `col HIGH + row LOW` | **FLIP** (same change) |
| Column-pattern PIO word | **Remove the `~pattern` inversion** | **Remove the `~pattern` inversion** |
| Row on/off sense | Active LOW → set `gpio_clr_mask64` for ON, `gpio_set_mask64` for OFF | Same swap |
| All-off row state | All rows HIGH by default | All rows HIGH by default |
| SPI peripheral | Still SPI0 on GP32-35 | **Switch to SPI1** on GP40-43 |
| MISO termination | Add R29 handling (no firmware impact — it's just a series R) | Same |
| PIO0 column base | Shift from GP1 to match new CONFIG | **GP0 base** for `out pins, 20` |
| PIO1 row driving | Not usable (split pins) | **New capability** — `out pins, 20` from GP20 base, GPIOBASE=16 |
| XIP_CS1n handling | Still GP0 | **Now GP47** — anything that touched XIP_CS1n pin must update |
| EINT handler | GP45 (same as v0.1 if already routed there; no bodge wire needed) | GP45 (same) |

---

## Testing priorities and differences

### Tests that should pass identically on both v0.2.1 and v0.3.1
- Basic LED lighting (single LED ON, polarity check)
- Row/column scan (with appropriate pin-constant firmware for each version)
- PSRAM XIP reads/writes (PSRAM behavior identical — only the CS pin moved)
- USB enumeration / BOOTSEL behavior (SW1, SW2 operation)
- 3.3 V LDO output level, LED driver current per LED
- Per-row timing jitter under `noInterrupts()` + multicore lockout (PIOSCAN mode should achieve near-zero jitter on both)
- SPI slave frame reception from arena controller (works on either SPI0 for v0.2.1 or SPI1 for v0.3.1)
- External trigger on GP45 → scan burst

### Tests that will only work on v0.3.1
- **Fully-autonomous PIO scanning** (both PIO0 for columns and PIO1 for rows with clean `out pins, 20`) — PIOSCAN-PIO1 mode
- **No-`noInterrupts` scan burst** — with PIO handling rows and columns, CPU doesn't need to be locked during burst
- **5-bit or 6-bit BCM at T=0.5 µs within the 15 µs window** (reduced per-row overhead from ~0.61 µs to ~0.37 µs)

### Differences worth measuring on a scope / spectrometer
- **Brightness uniformity under load** (1 LED vs 20 LEDs ON per row). Both panels are normal polarity, so should behave similarly. Verify the optimized current-limiting resistor value (expected 160 Ω — confirm via multimeter on R9) gives ~18–19 mA drive at worst-case VF.
- **Jitter comparison** — does v0.3.1 in fully-PIO mode genuinely achieve zero jitter without `noInterrupts()`? Compare against v0.2.1 in PIOSCAN + noInterrupts mode.
- **Maximum SPI clock rate** — with MISO termination on both, both should handle 25-30 MHz cleanly. Verify.
- **EINT signal integrity** — same pin (GP45) on both, so no difference expected, but good to baseline on at least one board.

---

## Known open items / latent concerns

1. **Current-limit resistor value (R9–R28) needs confirmation** — schematic uses symbolic R_T0/T1/T2/T3 but BOM shows single LCSC part C851657 across all 20. Look up C851657's actual ohm value or measure in-circuit (expected 160 Ω per prior optimization).

2. **LED binning verification** — order used Starsealand XL0402YGC with bins P08/P09 (brightness) + VF/VG (voltage) + YG11/YG12 (wavelength). Recommend a quick spectrometer / photodiode check to confirm delivered parts match the ordered bin.

3. **Neither version includes the multi-color interleaving hardware** — R_T0/T1/T2/T3 symbolic naming is a placeholder for future multi-color variants; current order is all-same-yellow-green.

4. **Firmware default scanning mode** — current test firmware targets v0.1; a fresh session should (a) identify which mode to port to each new panel, (b) update pin constants, (c) verify the polarity flip is handled correctly.

---

## Files to reference when planning tests

- **Current firmware**: `/Users/reiserm/Documents/GitHub/G6_Panels_Test_Firmware/test_firmware/single_led/`
  - `src/constants.h`, `src/constants.cpp` — pin definitions (need per-version override)
  - `src/main.cpp` — scan modes (SCAN, PIOSCAN, DMASCAN, MSMSCAN, BURST, BCM variants)
  - `CLAUDE.md` — project context, scanning-mode tradeoffs, test-loop workflow
- **Prior review docs** (if helpful for deeper context):
  - `test_firmware/single_led/G6_V03_SCHEMATIC_REVIEW.md` — comprehensive v0.3 panel schematic review
  - `test_firmware/single_led/LED_ORIENTATION_AND_RESISTOR_SUMMARY.md` — polarity / resistor rationale
- **PR #4** (source of this summary): `iorodeo/LED-Display_G6_Hardware_Panel#4`

---

## Automated check outputs (saved to /tmp for this review)

- `/tmp/panel_analysis/v0p2_erc.json`, `v0p3_erc.json` — ERC results (1 warning each, cosmetic)
- `/tmp/panel_analysis/v0p2_netlist.net`, `v0p3_netlist.net` — full hierarchical netlists
- `/tmp/panel_analysis/v0p2_bom.csv`, `v0p3_bom.csv` — schematic-export BOMs (identical in substance to production BOMs)
