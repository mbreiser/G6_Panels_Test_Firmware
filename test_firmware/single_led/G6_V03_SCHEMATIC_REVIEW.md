# G6 Panel v0.3.1 Schematic Review

**Date**: 2026-04-09 | **Reviewer**: M. Reiser (with automated trace verification) | **Status**: APPROVED for production

**Source**: `floesche/LED-Display_G6_Hardware_Panel` @ `prod_v0p2r0`, folder `panel_rp2354_20x20_v0p3`
**Commit**: `29bb98e fix XIP_CS1n pind` | **Board**: 45x45 mm | **MCU**: RP2354 (QFN-80)

---

## Verdict: ALL 32 CHECKS PASS

| Sub-sheet | Checks | Result |
|-----------|--------|--------|
| MCU (`panel_mcu`) | GPIO map, XIP_CS1n, SPI1, EINT, PIO coverage, QSPI, USB, crystal, power, debug | 11/11 PASS |
| LED matrix (`panel_led`) | 400 LEDs, polarity, resistors, column/row connectivity, type pattern, floating nets | 7/7 PASS |
| Drivers (`drivers`) | 40x UCC27517, signal flow, symbol pinout vs TI datasheet, decoupling | 4/4 PASS |
| Power (`panel_usb_power`) | USB input, voltage rails, AP2112K LDO, decoupling | 4/4 PASS |
| Headers (`panel_header`) | J2-J5 pinout, EINT routing, SPI signals, MISO 33 ohm termination | 4/4 PASS |
| Top-level (`panel_rp2354_20x20`) | 87 hierarchical signals connected, zero orphans | 2/2 PASS |

---

## Pin Map

| GPIO | Signal | Function |
|------|--------|----------|
| GP0-GP19 | COL_MCU_00-19 | Columns (20 contiguous, PIO0 `out pins 20`) |
| GP20-GP39 | ROW_MCU_00-19 | Rows (20 contiguous, PIO1 `out pins 20`) |
| GP40 | MOSI | SPI1_RX — slave receives from arena controller |
| GP41 | CS0 | SPI1_CSn — chip select |
| GP42 | SCK | SPI1_SCK — clock |
| GP43 | MISO via R29 (33 ohm) | SPI1_TX — slave transmits to arena controller |
| GP44 | NC | Spare (ADC4 capable) |
| GP45 | EINT | External trigger input (on headers J3/J5) |
| GP46 | NC | Spare (ADC6 capable) |
| GP47 | XIP_CS1n | PSRAM chip select (funcsel 9, QMI CS1n) |

SPI labels (MOSI/MISO) are from the arena controller (master) perspective. RP2350 funcsel 1 mapping verified: GP40=SPI1_RX, GP41=SPI1_CSn, GP42=SPI1_SCK, GP43=SPI1_TX.

---

## Critical items verified

### XIP_CS1n on GP47
Original draft had XIP_CS1n on GP44 -- this was a bug (GP44 funcsel 9 = USB VBUS EN, not QMI CS1n). Fixed to GP47, which is one of only four pins supporting XIP_CS1n (GP0, GP8, GP19, GP47). Source: RP2350 datasheet Table 3, pico-sdk `gpio.h` funcsel table.

### LED polarity: NORMAL
All 400 LEDs traced: anode connects to column, cathode connects to row. This is normal polarity (iorodeo convention), opposite of the reversed Janelia v0.1 batch. Firmware drives COL HIGH + ROW LOW for LED ON. Verified across 10 sample LEDs spanning all 4 types and multiple rows/columns, plus systematic check of all 400 connections.

### UCC27517 driver symbol
Custom `panel_custom:UCC27517` pin mapping matches TI UCC27517DBVR SOT-23-5 datasheet: Pin1=VDD, Pin2=GND, Pin3=IN+, Pin4=IN-, Pin5=OUT. Non-inverting: MCU HIGH produces driver output HIGH. All 40 drivers (20 column + 20 row) use identical, correct wiring.

### MISO 33 ohm termination
R29 (33 ohm) in series on MISO between GP43 and header connector. Confirmed present.

### Resistors
20 column resistors (R9-R28), one per column, none on rows. Symbolic values R_T0-R_T3 follow NUM_COLOR=4 interleaving pattern matching LED types. Actual resistance value set in BOM -- should be **160 ohm uniform** for single LED type (XL0402YGC yellow-green).

### PIO coverage
- PIO0 (GPIOBASE=0): columns GP0-19, `out pins 20` base=GP0 -- contiguous, no offset
- PIO1 (GPIOBASE=16): rows GP20-39, `out pins 20` base=GP20 -- contiguous, no gap
- Both fully autonomous PIO scanning now possible (major improvement over v0.1 split rows)

### Power
+5V from USB (JST-SH J1) direct to UCC27517 drivers. +3.3V from AP2112K-3.3 LDO (600 mA) for MCU. Internal RP2354 VREG generates +1.1V core. Decoupling: 10uF on regulator in/out, 100nF per driver (40x), 10uF bulk (16x).

### Connectivity
87 hierarchical signals across 5 sub-sheets, all with matching pairs. Zero unconnected labels. Zero floating nets in LED matrix. All 20x20 grid positions populated.

---

## Non-blocking flags

1. **No SWD debug port** -- SWCLK/SWD unconnected. Programming via USB BOOTSEL only. Consider test pads if SWD recovery path is desired for bring-up.

2. **LDO thermal margin** -- AP2112K-3.3 rated 600 mA. RP2354 + PSRAM typical draw well under, but monitor during sustained operation.

3. **Spare GP44/GP46** -- currently NC. Recommend routing to test pads or unused header pin (J5 Pin 4 is NC) for sync output / debug without adding components.

---

## Changes from v0.1

| Item | v0.1 (Janelia batch) | v0.3.1 |
|------|---------------------|--------|
| XIP_CS1n | GP0 | GP47 |
| Columns | GP1-20 | GP0-19 (starts at 0) |
| Rows | GP21-31 + GP36-44 (gap) | GP20-39 (contiguous) |
| SPI | SPI0 on GP32-35 | SPI1 on GP40-43 |
| EINT | Unrouted (bodge wire to GP45) | GP45 (native trace) |
| MISO termination | None | R29 33 ohm |
| LED polarity | Reversed (Janelia assembly) | Normal (anode=column) |
| Board size | Oversized test board | 45x45 mm |
| Resistor values | 270/165 ohm (mixed) | 160 ohm uniform (TBD in BOM) |

No signals from v0.1 were accidentally disconnected. All changes are intentional.

---

## Action items before ordering

- [ ] Set resistor BOM values: R_T0 = R_T1 = R_T2 = R_T3 = **160 ohm** (Yageo RC0201FR-07160RL)
- [ ] Confirm board fab house accepts 45x45 mm panel size
- [ ] Optional: route GP44/GP46 to test pads
- [ ] Run KiCad DRC/ERC (schematic review only -- board layout not reviewed here)
