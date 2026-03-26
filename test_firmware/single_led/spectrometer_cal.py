#!/usr/bin/env python3
"""
Spectrometer-based BCM linearity characterization for G6 LED panel.

Uses Ocean Insight Flame X spectrometer (via seabreeze) to measure LED output
at each BCM intensity level. Coordinates with firmware PHOTOCAL command.

Requires:
    pip install seabreeze numpy

Usage:
    # Full linearity sweep (Thursday experiment):
    python3 spectrometer_cal.py sweep

    # Quick sanity check (dark, level 0, level 15):
    python3 spectrometer_cal.py sanity

    # Plot previously saved data:
    python3 spectrometer_cal.py plot <json_path>
"""

import sys
import os
import time
import json
import numpy as np

# ── Serial helpers ─────────────────────────────────────────────────────────

BAUD = 115200

def find_serial_port():
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device and "306NTVSCY" not in p.device:
            return p.device
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device:
            return p.device
    return None

def open_serial():
    import serial
    port = find_serial_port()
    if not port:
        print("ERROR: No serial port found")
        return None
    ser = serial.Serial(port, BAUD, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()
    print(f"  Serial: {port}")
    return ser

def send_cmd(ser, cmd, timeout=5):
    ser.reset_input_buffer()
    ser.write((cmd.strip() + '\r\n').encode())
    time.sleep(0.3)
    out = ''
    deadline = time.time() + timeout
    while time.time() < deadline:
        n = ser.in_waiting
        if n > 0:
            out += ser.read(n).decode(errors='replace')
        else:
            if out:
                break
            time.sleep(0.05)
    return out

# ── Spectrometer helpers ───────────────────────────────────────────────────

def open_spectrometer():
    import seabreeze
    seabreeze.use('cseabreeze')
    from seabreeze.spectrometers import Spectrometer, list_devices
    devs = list_devices()
    if not devs:
        print("ERROR: No spectrometer found. Is it plugged in?")
        return None
    spec = Spectrometer(devs[0])
    print(f"  Spectrometer: {spec.model} ({spec.serial_number})")
    print(f"  Range: {spec.wavelengths()[0]:.0f}–{spec.wavelengths()[-1]:.0f} nm, {len(spec.wavelengths())} pixels")
    return spec

# ── Sanity Check ───────────────────────────────────────────────────────────

def run_sanity(integration_ms=100):
    """Quick sanity check: dark spectrum, level 0 spectrum, level 15 spectrum.
    Run with box open first (visual check), then box closed (data check).
    """
    import serial
    spec = open_spectrometer()
    if not spec:
        return
    ser = open_serial()
    if not ser:
        spec.close()
        return

    wl = spec.wavelengths()
    spec.integration_time_micros(integration_ms * 1000)

    # Setup panel
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'EXTTRIG OFF']:
        send_cmd(ser, cmd)

    results = {}

    # 1. Dark spectrum (panel idle — no BCMBURST running)
    print("\n[1/3] DARK — panel idle, no scanning")
    print("       Press Enter when ready...")
    input()
    spec.intensities()  # discard
    dark_spectra = [spec.intensities() for _ in range(5)]
    results['dark'] = {
        'spectrum': np.mean(dark_spectra, axis=0).tolist(),
        'std': np.std(dark_spectra, axis=0).tolist(),
    }
    dark_mean = np.mean(dark_spectra, axis=0)
    print(f"       Dark: mean={dark_mean.mean():.0f}, peak={dark_mean.max():.0f}")

    # 2. Level 0 (BCM scanning but intensity=0 → all columns OFF)
    print("\n[2/3] LEVEL 0 — BCM scanning, all pixels intensity 0")
    send_cmd(ser, 'FILL 0')
    ser.reset_input_buffer()
    ser.write(b'BCMBURST 500000 8000 A\r\n')
    time.sleep(2)
    spec.intensities()
    lev0_spectra = [spec.intensities() for _ in range(5)]
    results['level_0'] = {
        'spectrum': np.mean(lev0_spectra, axis=0).tolist(),
        'std': np.std(lev0_spectra, axis=0).tolist(),
    }
    lev0_mean = np.mean(lev0_spectra, axis=0)
    print(f"       Level 0: mean={lev0_mean.mean():.0f}, peak={lev0_mean.max():.0f}")
    ser.write(b'\n')
    time.sleep(1)
    while ser.in_waiting:
        ser.read(ser.in_waiting)

    # 3. Level 15 (BCM scanning, full intensity)
    print("\n[3/3] LEVEL 15 — BCM scanning, all pixels full intensity")
    send_cmd(ser, 'FILL 15')
    ser.reset_input_buffer()
    ser.write(b'BCMBURST 500000 8000 A\r\n')
    time.sleep(2)
    spec.intensities()
    lev15_spectra = [spec.intensities() for _ in range(5)]
    results['level_15'] = {
        'spectrum': np.mean(lev15_spectra, axis=0).tolist(),
        'std': np.std(lev15_spectra, axis=0).tolist(),
    }
    lev15_mean = np.mean(lev15_spectra, axis=0)
    print(f"       Level 15: mean={lev15_mean.mean():.0f}, peak={lev15_mean.max():.0f}")
    ser.write(b'\n')
    time.sleep(1)

    # Compare
    diff = lev15_mean - dark_mean
    vis_mask = (wl > 500) & (wl < 700)
    peak_idx = np.argmax(diff[vis_mask]) + np.where(vis_mask)[0][0]
    peak_nm = wl[peak_idx]
    peak_signal = diff[peak_idx]

    print(f"\n=== SANITY CHECK RESULTS ===")
    print(f"LED peak: {peak_nm:.1f} nm")
    print(f"Signal (level15 - dark) at peak: {peak_signal:.0f} counts")
    print(f"Level 0 vs dark at peak: {(lev0_mean[peak_idx] - dark_mean[peak_idx]):.0f} counts")
    print(f"Integration: {integration_ms} ms")

    if peak_signal < 50:
        print(f"\n⚠ Signal is weak ({peak_signal:.0f} counts). Consider:")
        print(f"  - Moving fiber closer to panel")
        print(f"  - Using 250ms integration (Plan B)")
    elif peak_signal > 50000:
        print(f"\n⚠ Signal near saturation. Reduce integration time.")
    else:
        print(f"\n✓ Signal looks good for linearity sweep.")

    # Save
    results['wavelengths'] = wl.tolist()
    results['peak_nm'] = float(peak_nm)
    results['integration_ms'] = integration_ms
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'spectrometer_sanity.json')
    with open(out_path, 'w') as f:
        json.dump(results, f)
    print(f"Saved: {out_path}")

    # Generate comparison plot
    generate_sanity_plot(wl, dark_mean, lev0_mean, lev15_mean, peak_nm, integration_ms)

    spec.close()
    ser.close()
    return results

# ── Main Linearity Sweep ──────────────────────────────────────────────────

def run_sweep(integration_ms=100, hold_sec=3):
    """Full linearity sweep: PHOTOCAL 16 levels with continuous spectrometer reads.
    Saves full spectrum at middle of each level + raw time trace.
    """
    import serial
    spec = open_spectrometer()
    if not spec:
        return
    ser = open_serial()
    if not ser:
        spec.close()
        return

    wl = spec.wavelengths()
    spec.integration_time_micros(integration_ms * 1000)

    # Setup panel
    for cmd in ['ROWS 20', 'BCM 4', 'BCMON 0.5', 'EXTTRIG OFF']:
        send_cmd(ser, cmd)

    GAP_SEC = 1.0
    PERIOD = hold_sec + GAP_SEC  # seconds per level
    N_LEVELS = 16
    TOTAL_SEC = N_LEVELS * PERIOD + 3  # extra margin

    # Find LED peak from level 15
    print("\nFinding LED peak wavelength...")
    send_cmd(ser, 'FILL 15')
    ser.reset_input_buffer()
    ser.write(b'BCMBURST 200000 8000 A\r\n')
    time.sleep(2)
    spec.intensities()
    ref = spec.intensities()
    ser.write(b'\n')
    time.sleep(1)
    while ser.in_waiting:
        ser.read(ser.in_waiting)

    # Also get dark
    spec.intensities()
    dark = spec.intensities()
    diff = ref - dark
    vis_mask = (wl > 500) & (wl < 700)
    peak_idx = np.argmax(diff[vis_mask]) + np.where(vis_mask)[0][0]
    peak_nm = wl[peak_idx]
    peak_window = (wl > peak_nm - 5) & (wl < peak_nm + 5)
    print(f"  LED peak: {peak_nm:.1f} nm, signal: {diff[peak_idx]:.0f} counts")

    # Start PHOTOCAL
    print(f"\n{'='*60}")
    print(f"STARTING SWEEP: {N_LEVELS} levels × {hold_sec}s + {GAP_SEC}s gaps = {TOTAL_SEC:.0f}s")
    print(f"Integration: {integration_ms}ms | ~{1000//integration_ms} reads/sec")
    print(f"{'='*60}\n")

    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.write(f'PHOTOCAL {hold_sec}\r\n'.encode())

    t_start = time.time()
    readings = []        # (time, peak_counts)
    full_spectra = {}    # level -> spectrum (captured at middle of each level)
    spectra_captured = set()

    spec.intensities()  # discard first

    while (time.time() - t_start) < TOTAL_SEC:
        s = spec.intensities()
        t = time.time() - t_start
        peak_val = float(np.mean(s[peak_window]))
        readings.append({'t': round(t, 3), 'counts': round(peak_val, 1)})

        # Estimate current level
        level_est = min(15, int(t / PERIOD))
        t_in_level = t - level_est * PERIOD
        in_gap = t_in_level >= hold_sec

        # Capture full spectrum at middle of each level (once)
        mid_time = hold_sec / 2.0
        if not in_gap and level_est not in spectra_captured and abs(t_in_level - mid_time) < 0.3:
            full_spectra[level_est] = s.tolist()
            spectra_captured.add(level_est)

        # Live trace
        status = f"GAP" if in_gap else f"ON "
        bar_len = max(0, int((peak_val - 4500) / 20))
        bar = '█' * min(bar_len, 40)
        print(f"  t={t:5.1f}s  L{level_est:2d} {status}  {peak_val:7.0f}  {bar}")

    # Collect serial output
    time.sleep(2)
    serial_out = ''
    deadline = time.time() + 5
    while time.time() < deadline:
        if ser.in_waiting:
            serial_out += ser.read(ser.in_waiting).decode(errors='replace')
        time.sleep(0.1)

    # Compute per-level averages (skip first 0.5s of each level for settling)
    import statistics
    level_avgs = []
    for level in range(N_LEVELS):
        t_start_l = level * PERIOD + 0.5  # skip first 0.5s
        t_end_l = level * PERIOD + hold_sec
        vals = [r['counts'] for r in readings if t_start_l < r['t'] < t_end_l]
        if vals:
            level_avgs.append({
                'level': level,
                'mean': statistics.mean(vals),
                'std': statistics.stdev(vals) if len(vals) > 1 else 0,
                'n': len(vals),
            })

    # Also compute gap averages (dark reference per gap)
    gap_avgs = []
    for level in range(N_LEVELS):
        t_start_g = level * PERIOD + hold_sec + 0.2  # skip transition
        t_end_g = (level + 1) * PERIOD - 0.1
        vals = [r['counts'] for r in readings if t_start_g < r['t'] < t_end_g]
        if vals:
            gap_avgs.append(statistics.mean(vals))

    dark_level = statistics.mean(gap_avgs) if gap_avgs else level_avgs[0]['mean']

    # Results table
    print(f"\n{'='*60}")
    print(f"{'Level':>5} {'N':>4} {'Mean':>8} {'Std':>6} {'Signal':>8} {'Norm':>6}")
    print(f"{'-'*42}")

    max_signal = 0
    for la in level_avgs:
        la['signal'] = la['mean'] - dark_level
        if la['level'] == 15:
            max_signal = la['signal']

    for la in level_avgs:
        norm = la['signal'] / max_signal if max_signal > 0 else 0
        la['normalized'] = norm
        la['ideal'] = la['level'] / 15.0
        la['error_pct'] = (norm - la['ideal']) * 100
        print(f"{la['level']:>5} {la['n']:>4} {la['mean']:>8.0f} {la['std']:>6.1f} "
              f"{la['signal']:>8.1f} {norm:>6.3f}")

    signals = [la['signal'] for la in level_avgs]
    monotonic = all(signals[i] <= signals[i+1] for i in range(len(signals)-1))
    print(f"\nMonotonic: {'YES' if monotonic else 'NO'}")
    print(f"Dark (from gaps): {dark_level:.0f} counts")
    print(f"Dynamic range: {max_signal:.0f} counts")
    print(f"Peak: {peak_nm:.1f} nm")

    # Save everything
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spectrometer_data')
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f'linearity_{timestamp}.json')

    with open(out_path, 'w') as f:
        json.dump({
            'timestamp': timestamp,
            'peak_nm': float(peak_nm),
            'integration_ms': integration_ms,
            'hold_sec': hold_sec,
            'gap_sec': GAP_SEC,
            'dark_from_gaps': float(dark_level),
            'level_avgs': level_avgs,
            'gap_avgs': [float(g) for g in gap_avgs],
            'readings': readings,
            'full_spectra': {str(k): v for k, v in full_spectra.items()},
            'wavelengths': wl.tolist(),
            'serial_output': serial_out,
        }, f, indent=2)
    print(f"\nSaved: {out_path}")

    # Generate plots
    generate_sweep_plots(out_path)

    spec.close()
    ser.close()

# ── Plotting ──────────────────────────────────────────────────────────────

def generate_sanity_plot(wl, dark, lev0, lev15, peak_nm, integration_ms):
    """Generate HTML plot comparing dark, level 0, level 15 spectra."""
    # Downsample for smaller HTML
    step = max(1, len(wl) // 500)
    wl_ds = wl[::step].tolist()
    dark_ds = dark[::step].tolist()
    lev0_ds = lev0[::step].tolist()
    lev15_ds = lev15[::step].tolist()
    diff_ds = (lev15 - dark)[::step].tolist()

    html = f"""<!DOCTYPE html>
<html><head><title>G6 Spectrometer Sanity Check</title>
<style>
body {{ font-family: monospace; background: #1a1a2e; color: #e0e0e0; margin: 20px; }}
h2 {{ color: #00d4ff; }}
canvas {{ border: 1px solid #444; background: #0d0d1a; display: block; margin: 10px 0; }}
.stats {{ background: #222; padding: 10px; border-radius: 5px; margin: 10px 0; }}
</style></head><body>
<h2>G6 Panel — Spectrometer Sanity Check</h2>
<div class="stats">Integration: {integration_ms}ms | LED peak: {peak_nm:.1f} nm | 5 averages per condition</div>
<canvas id="spectra" width="1000" height="400"></canvas>
<canvas id="diff" width="1000" height="300"></canvas>
<script>
const wl={json.dumps(wl_ds)};
const dark={json.dumps(dark_ds)};
const lev0={json.dumps(lev0_ds)};
const lev15={json.dumps(lev15_ds)};
const diff={json.dumps(diff_ds)};
const peak={peak_nm};

function plotSpectra(id, datasets, labels, colors, title) {{
    const c=document.getElementById(id), ctx=c.getContext('2d');
    const W=c.width,H=c.height,m={{l:70,r:20,t:30,b:40}};
    const pw=W-m.l-m.r, ph=H-m.t-m.b;
    let allV=[]; datasets.forEach(d=>allV.push(...d));
    const vMin=Math.min(...allV), vMax=Math.max(...allV);
    const pad=(vMax-vMin)*0.05;
    const xMin=Math.min(...wl), xMax=Math.max(...wl);
    const mapX=x=>m.l+(x-xMin)/(xMax-xMin)*pw;
    const mapY=v=>m.t+ph-(v-vMin+pad)/(vMax-vMin+2*pad)*ph;
    ctx.fillStyle='#aaa';ctx.font='14px monospace';ctx.fillText(title,m.l,m.t-10);
    ctx.strokeStyle='#333';ctx.lineWidth=0.5;
    for(let v=Math.ceil(vMin/5000)*5000;v<=vMax;v+=5000){{
        ctx.beginPath();ctx.moveTo(m.l,mapY(v));ctx.lineTo(W-m.r,mapY(v));ctx.stroke();
        ctx.fillStyle='#888';ctx.font='10px monospace';ctx.textAlign='right';
        ctx.fillText(v.toFixed(0),m.l-5,mapY(v)+4);
    }}
    for(let x=400;x<=1000;x+=100){{
        ctx.beginPath();ctx.moveTo(mapX(x),m.t);ctx.lineTo(mapX(x),H-m.b);ctx.stroke();
        ctx.fillStyle='#888';ctx.textAlign='center';ctx.fillText(x+'nm',mapX(x),H-m.b+15);
    }}
    // Peak line
    ctx.strokeStyle='#ff4444';ctx.lineWidth=1;ctx.setLineDash([3,3]);
    ctx.beginPath();ctx.moveTo(mapX(peak),m.t);ctx.lineTo(mapX(peak),H-m.b);ctx.stroke();
    ctx.setLineDash([]);
    datasets.forEach((d,i)=>{{
        ctx.strokeStyle=colors[i];ctx.lineWidth=1.5;ctx.beginPath();
        for(let j=0;j<wl.length;j++){{j===0?ctx.moveTo(mapX(wl[j]),mapY(d[j])):ctx.lineTo(mapX(wl[j]),mapY(d[j]));}}
        ctx.stroke();
    }});
    // Legend
    labels.forEach((l,i)=>{{ctx.fillStyle=colors[i];ctx.font='11px monospace';ctx.textAlign='left';
        ctx.fillText(l,m.l+10+i*200,m.t+15);}});
}}
plotSpectra('spectra',[dark,lev0,lev15],['Dark','Level 0','Level 15'],['#888','#ff8800','#00d4ff'],'Full Spectra');
plotSpectra('diff',[diff],['Level 15 − Dark'],['#00ff88'],'Difference Spectrum (LED signal)');
</script></body></html>"""

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spectrometer_sanity_plot.html')
    with open(out, 'w') as f:
        f.write(html)
    print(f"Plot: {out}")
    os.system(f'open "{out}"')


def generate_sweep_plots(json_path):
    """Generate HTML plots from sweep data."""
    with open(json_path) as f:
        data = json.load(f)

    readings = data['readings']
    level_avgs = data['level_avgs']
    peak_nm = data['peak_nm']
    integration_ms = data['integration_ms']
    hold_sec = data['hold_sec']
    gap_sec = data['gap_sec']
    dark = data['dark_from_gaps']

    times = [r['t'] for r in readings]
    counts = [r['counts'] for r in readings]
    period = hold_sec + gap_sec

    # Full spectra at each level (if available)
    full_spectra = data.get('full_spectra', {})
    wl = data.get('wavelengths', [])

    html = f"""<!DOCTYPE html>
<html><head><title>G6 BCM Linearity — Spectrometer Sweep</title>
<style>
body {{ font-family: monospace; background: #1a1a2e; color: #e0e0e0; margin: 20px; }}
h2 {{ color: #00d4ff; }}
h3 {{ color: #aaa; margin-top: 15px; }}
canvas {{ border: 1px solid #444; background: #0d0d1a; display: block; margin: 5px 0 15px 0; }}
.stats {{ background: #222; padding: 10px; border-radius: 5px; margin: 10px 0; }}
.row {{ display: flex; gap: 20px; flex-wrap: wrap; }}
</style></head><body>
<h2>G6 Panel — BCM Linearity (Spectrometer Sweep)</h2>
<div class="stats">
Integration: {integration_ms}ms | Hold: {hold_sec}s | Gap: {gap_sec}s | Peak: {peak_nm:.1f}nm | Dark: {dark:.0f} counts
</div>

<h3>Raw Time Trace (every dot = one {integration_ms}ms read)</h3>
<canvas id="trace" width="1200" height="350"></canvas>

<div class="row">
<div>
<h3>Linearity (signal vs BCM level)</h3>
<canvas id="linearity" width="500" height="350"></canvas>
</div>
<div>
<h3>Results Table</h3>
<pre id="table" style="font-size:12px;line-height:1.4;"></pre>
</div>
</div>

{"<h3>Spectra per Level (captured at mid-hold)</h3><canvas id='spectra' width='1000' height='350'></canvas>" if full_spectra else ""}

<script>
const times={json.dumps(times)};
const counts={json.dumps(counts)};
const levelAvgs={json.dumps(level_avgs)};
const dark={dark};
const period={period};
const holdSec={hold_sec};

// ── Trace ──
(function(){{
const c=document.getElementById('trace'),ctx=c.getContext('2d');
const W=c.width,H=c.height,m={{l:70,r:20,t:15,b:40}};
const pw=W-m.l-m.r,ph=H-m.t-m.b;
const tMax=Math.max(...times)+1;
const cMin=Math.min(...counts)-30,cMax=Math.max(...counts)+30;
const mapX=t=>m.l+t/tMax*pw;
const mapY=v=>m.t+ph-(v-cMin)/(cMax-cMin)*ph;

for(let i=0;i<16;i++){{
    const x1=mapX(i*period),x2=mapX(i*period+holdSec);
    ctx.fillStyle=i%2?'rgba(0,200,100,0.06)':'rgba(0,100,200,0.06)';
    ctx.fillRect(x1,m.t,x2-x1,ph);
    ctx.fillStyle='#556';ctx.font='9px monospace';ctx.textAlign='center';
    ctx.fillText('L'+i,(x1+x2)/2,m.t+10);
}}

// Dark baseline
ctx.strokeStyle='#44ff44';ctx.lineWidth=1;ctx.setLineDash([2,4]);
ctx.beginPath();ctx.moveTo(m.l,mapY(dark));ctx.lineTo(W-m.r,mapY(dark));ctx.stroke();
ctx.setLineDash([]);ctx.fillStyle='#44ff44';ctx.font='10px monospace';ctx.textAlign='right';
ctx.fillText('dark '+dark.toFixed(0),W-m.r-5,mapY(dark)-5);

const yStep=Math.pow(10,Math.floor(Math.log10(cMax-cMin)))/2;
ctx.strokeStyle='#333';ctx.lineWidth=0.5;
for(let v=Math.ceil(cMin/yStep)*yStep;v<=cMax;v+=yStep){{
    ctx.beginPath();ctx.moveTo(m.l,mapY(v));ctx.lineTo(W-m.r,mapY(v));ctx.stroke();
    ctx.fillStyle='#888';ctx.font='10px monospace';ctx.textAlign='right';
    ctx.fillText(v.toFixed(0),m.l-5,mapY(v)+4);
}}
for(let t=0;t<=tMax;t+=10){{ctx.fillStyle='#888';ctx.textAlign='center';ctx.fillText(t+'s',mapX(t),H-m.b+15);}}

ctx.strokeStyle='#00d4ff';ctx.lineWidth=1;ctx.beginPath();
for(let i=0;i<times.length;i++){{i===0?ctx.moveTo(mapX(times[i]),mapY(counts[i])):ctx.lineTo(mapX(times[i]),mapY(counts[i]));}}
ctx.stroke();
ctx.fillStyle='#00d4ff';
for(let i=0;i<times.length;i++){{ctx.beginPath();ctx.arc(mapX(times[i]),mapY(counts[i]),1.5,0,Math.PI*2);ctx.fill();}}
ctx.fillStyle='#aaa';ctx.font='12px monospace';ctx.textAlign='center';
ctx.fillText('Time (s)',W/2,H-5);
}})();

// ── Linearity ──
(function(){{
const c=document.getElementById('linearity'),ctx=c.getContext('2d');
const W=c.width,H=c.height,m={{l:60,r:20,t:15,b:40}};
const pw=W-m.l-m.r,ph=H-m.t-m.b;
const mapX=l=>m.l+l/15*pw;
const mapY=v=>m.t+ph-v*ph;

ctx.strokeStyle='#333';ctx.lineWidth=0.5;
for(let v=0;v<=1;v+=0.2){{
    ctx.beginPath();ctx.moveTo(m.l,mapY(v));ctx.lineTo(W-m.r,mapY(v));ctx.stroke();
    ctx.fillStyle='#888';ctx.font='10px monospace';ctx.textAlign='right';ctx.fillText(v.toFixed(1),m.l-5,mapY(v)+4);
}}
for(let l=0;l<=15;l+=5){{const x=mapX(l);ctx.beginPath();ctx.moveTo(x,m.t);ctx.lineTo(x,H-m.b);ctx.stroke();
    ctx.fillStyle='#888';ctx.textAlign='center';ctx.fillText(l,x,H-m.b+15);}}

ctx.strokeStyle='#555';ctx.lineWidth=1;ctx.setLineDash([4,4]);
ctx.beginPath();ctx.moveTo(mapX(0),mapY(0));ctx.lineTo(mapX(15),mapY(1));ctx.stroke();
ctx.setLineDash([]);ctx.fillStyle='#555';ctx.font='10px monospace';ctx.fillText('ideal',mapX(13),mapY(13/15)-8);

const maxSig=levelAvgs[15]?levelAvgs[15].signal:1;
ctx.strokeStyle='#00d4ff';ctx.lineWidth=2;ctx.beginPath();
levelAvgs.forEach((la,i)=>{{const n=la.signal/maxSig;i===0?ctx.moveTo(mapX(la.level),mapY(n)):ctx.lineTo(mapX(la.level),mapY(n));}});
ctx.stroke();
ctx.fillStyle='#00d4ff';
levelAvgs.forEach(la=>{{const n=la.signal/maxSig;ctx.beginPath();ctx.arc(mapX(la.level),mapY(n),4,0,Math.PI*2);ctx.fill();}});
ctx.fillStyle='#aaa';ctx.font='12px monospace';ctx.textAlign='center';ctx.fillText('BCM Level',W/2,H-5);
}})();

// ── Table ──
(function(){{
const maxSig=levelAvgs[15]?levelAvgs[15].signal:1;
let t='Level  N    Mean     Std   Signal    Norm   Ideal   Err\\n';
t+='─'.repeat(58)+'\\n';
levelAvgs.forEach(la=>{{
    const n=(la.signal/maxSig);
    const ideal=la.level/15;
    const err=(n-ideal)*100;
    t+=la.level.toString().padStart(3)+'  '+la.n.toString().padStart(3)+'  '+
       la.mean.toFixed(0).padStart(7)+'  '+la.std.toFixed(1).padStart(6)+'  '+
       la.signal.toFixed(1).padStart(7)+'  '+n.toFixed(3).padStart(6)+'  '+
       ideal.toFixed(3).padStart(6)+' '+(err>=0?'+':'')+err.toFixed(1).padStart(5)+'%\\n';
}});
const sigs=levelAvgs.map(la=>la.signal);
const mono=sigs.every((s,i)=>i===0||s>=sigs[i-1]);
t+='\\nMonotonic: '+(mono?'YES':'NO');
t+='\\nDark: '+dark.toFixed(0)+' | Range: '+maxSig.toFixed(0)+' counts';
document.getElementById('table').textContent=t;
}})();
</script></body></html>"""

    plot_path = json_path.replace('.json', '_plot.html')
    with open(plot_path, 'w') as f:
        f.write(html)
    print(f"Plot: {plot_path}")
    os.system(f'open "{plot_path}"')


# ── Main ──────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 spectrometer_cal.py sanity [int_ms]    — quick 3-condition check")
        print("  python3 spectrometer_cal.py sweep [int_ms]     — full 16-level linearity")
        print("  python3 spectrometer_cal.py plot <json_path>   — regenerate plots")
        return

    action = sys.argv[1].lower()

    if action == "sanity":
        int_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 100
        run_sanity(integration_ms=int_ms)

    elif action == "sweep":
        int_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 100
        run_sweep(integration_ms=int_ms)

    elif action == "plot":
        if len(sys.argv) < 3:
            print("Usage: python3 spectrometer_cal.py plot <json_path>")
            return
        generate_sweep_plots(sys.argv[2])

    else:
        print(f"Unknown action: {action}")

if __name__ == "__main__":
    main()
