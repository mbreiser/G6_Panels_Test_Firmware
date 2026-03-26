# Thursday Spectrometer Linearity Experiment

**Goal**: Measure BCM intensity linearity across all 16 levels using Ocean Insight Flame X spectrometer.

**Time budget**: ~20 minutes

---

## Pre-flight (Claude does this before you arrive)

- [x] Firmware: PHOTOCAL gaps increased to 1.0s
- [x] Firmware: BCMWEIGHTS command for custom bit-plane weights
- [x] Firmware: builds clean
- [x] Script: `spectrometer_cal.py` with sanity check, sweep, full spectrum capture, live trace, auto-plots
- [x] Plan B: 250ms integration if signal is too weak at 100ms

---

## Step 1: Setup (2 min)

1. Plug in: panel USB, spectrometer USB, function generator (if using external trigger)
2. Claude verifies both devices are connected
3. Position spectrometer fiber pointing at the panel face

## Step 2: Sanity Check — Box OPEN (3 min)

**You watch the panel while Claude runs this.**

```
python3 spectrometer_cal.py sanity
```

This does 3 measurements (you press Enter between each):
1. **Dark** — panel idle, no scanning. You see: panel off.
2. **Level 0** — BCM scanning, intensity 0. You see: panel should be dark (all columns OFF).
3. **Level 15** — BCM scanning, full intensity. You see: panel fully lit.

**What to look for**:
- Level 0 should look the same as dark (no visible light)
- Level 15 should be clearly bright
- If level 0 looks bright → firmware bug, stop and investigate

**Output**: Full spectrum plot comparing all 3 conditions. LED peak identified.

## Step 3: Sanity Check — Box CLOSED (2 min)

Close the box over the panel + fiber. Run again:

```
python3 spectrometer_cal.py sanity
```

**What to check in the output**:
- Dark spectrum should be low (~4000-5000 baseline)
- Level 0 spectrum should match dark (within noise)
- Level 15 spectrum should show clear peak at ~570 nm above dark
- Signal (level 15 − dark) at peak should be > 100 counts

**Decision point**:
- Signal > 100 counts at 100ms → proceed with 100ms integration ✓
- Signal 50-100 counts → switch to Plan B: `python3 spectrometer_cal.py sanity 250`
- Signal < 50 counts → move fiber closer, retry

## Step 4: Main Linearity Sweep (10 min)

```
python3 spectrometer_cal.py sweep
```

(or `python3 spectrometer_cal.py sweep 250` for Plan B integration time)

**What happens**:
- PHOTOCAL runs: 16 intensity levels × 3 seconds each + 1 second OFF gaps = **64 seconds**
- Spectrometer reads continuously (~10 reads/sec at 100ms, ~4/sec at 250ms)
- Live trace prints to terminal: you see counts + ASCII bar chart
- Full spectrum captured at the middle of each 3-second hold
- Auto-plots open in browser when done

**What you should see in the live trace**:
- 16 plateaus of increasing height
- Deep dips to dark baseline between each plateau (the 1-second gaps)
- Smooth, low-noise readings within each plateau

## Step 5: Review Results (3 min)

Two plots auto-open:

1. **Raw time trace** — every spectrometer reading vs time, with level bands and dark baseline
2. **Linearity curve** — normalized signal vs BCM level, compared to ideal straight line

**Key questions**:
- Is it monotonic? (each level brighter than the last)
- Is it linear? (points follow the ideal line)
- If nonlinear → we can calibrate with BCMWEIGHTS command

---

## If We Have Extra Time

### Test custom BCM weights

If linearity shows compression (high levels too close together):

```
# Example: stretch upper levels
BCMWEIGHTS 1.0 2.0 4.0 9.0

# Re-run sweep to check
python3 spectrometer_cal.py sweep
```

### Test different T values

```
# Compare T=0.25, 0.5, 0.75 µs linearity
# (modify BCMON between sweeps)
```

---

## Files

| File | Purpose |
|------|---------|
| `spectrometer_cal.py` | Main experiment script (sanity + sweep + plots) |
| `spectrometer_data/` | Output directory for sweep JSON + plots |
| `spectrometer_sanity.json` | Sanity check spectra |
| `spectrometer_sanity_plot.html` | Sanity check comparison plot |

## Serial Commands Reference

```
ROWS 20          All rows active
BCM 4            4-bit BCM (16 levels)
BCMON 0.5        Base time T = 0.5 µs
FILL <0-15>      Set all pixels to intensity
EXTTRIG OFF      Use DWT simulated trigger (no function generator needed)
BCMBURST N Hz A  Run N triggers at Hz rate, Mode A (PIO)
PHOTOCAL 3       Cycle all 16 levels, 3s each, 1s gaps, timing stats
BCMWEIGHTS w0 w1 w2 w3   Set custom bit-plane weights (floats)
BCMWEIGHTS RESET          Reset to default 1,2,4,8
```
