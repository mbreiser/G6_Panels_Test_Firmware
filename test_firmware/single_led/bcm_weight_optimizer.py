#!/usr/bin/env python3
"""
Iterative BCMWEIGHTS optimizer for linear intensity response.

Runs PHOTOCAL, measures linearity, adjusts ONE weight per iteration,
converges toward monotonic linear response. Saves data at each step.

Usage:
    python3 bcm_weight_optimizer.py [T_us] [max_iterations]
    # defaults: T=0.7, max_iterations=6
"""

import serial, serial.tools.list_ports, time, numpy as np, json, sys, os, statistics
import seabreeze
seabreeze.use('cseabreeze')
from seabreeze.spectrometers import Spectrometer, list_devices

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from status_dashboard import update_status

BAUD = 115200
HOLD_SEC = 3
GAP_SEC = 1.0
PERIOD = HOLD_SEC + GAP_SEC
IT_MS = 50

def find_port():
    for p in serial.tools.list_ports.comports():
        if 'usbmodem' in p.device and '306NTVSCY' not in p.device:
            return p.device
    for p in serial.tools.list_ports.comports():
        if 'usbmodem' in p.device:
            return p.device
    return None

def run_photocal_sweep(ser, spec, wl, peak_mask):
    """Run one PHOTOCAL sweep and return per-level signals."""
    TOTAL = 16 * PERIOD + 3

    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.write(f'PHOTOCAL {HOLD_SEC}\r\n'.encode())

    t_start = time.time()
    readings = []
    full_spectra = {}
    spectra_captured = set()
    spec.intensities()  # discard

    while (time.time() - t_start) < TOTAL:
        s = spec.intensities()
        t = time.time() - t_start
        peak_val = float(np.mean(s[peak_mask]))
        readings.append({'t': round(t, 3), 'counts': round(peak_val, 1)})

        level_est = min(15, int(t / PERIOD))
        t_in_level = t - level_est * PERIOD
        in_gap = t_in_level >= HOLD_SEC

        if not in_gap and level_est not in spectra_captured and abs(t_in_level - HOLD_SEC/2) < 0.5:
            full_spectra[level_est] = s.tolist()
            spectra_captured.add(level_est)

        status = 'GAP' if in_gap else 'ON '
        if len(readings) % 20 == 0:
            print(f'    t={t:5.1f}s L{level_est:2d} {status} {peak_val:7.0f}')
            sys.stdout.flush()

    # Drain serial
    time.sleep(2)
    while ser.in_waiting:
        ser.read(ser.in_waiting)

    # Compute per-level averages using level 0 as dark
    level0_vals = [r['counts'] for r in readings if 0.5 < r['t'] < HOLD_SEC]
    dark = statistics.mean(level0_vals)

    level_avgs = []
    for level in range(16):
        vals = [r['counts'] for r in readings if level*PERIOD+0.5 < r['t'] < level*PERIOD+HOLD_SEC]
        if vals:
            mean_v = statistics.mean(vals)
            level_avgs.append({
                'level': level,
                'mean': mean_v,
                'std': statistics.stdev(vals) if len(vals) > 1 else 0,
                'signal': mean_v - dark,
                'n': len(vals),
            })

    max_sig = level_avgs[15]['signal'] if len(level_avgs) > 15 else 1
    for la in level_avgs:
        la['normalized'] = la['signal'] / max_sig if max_sig > 0 else 0
        la['ideal'] = la['level'] / 15.0
        la['error_pct'] = (la['normalized'] - la['ideal']) * 100

    return {
        'dark': dark,
        'max_signal': max_sig,
        'level_avgs': level_avgs,
        'readings': readings,
        'full_spectra': full_spectra,
    }

def find_worst_bitplane(level_avgs):
    """Find which bit-plane weight needs the biggest correction.

    Strategy: look at the transitions where BCM levels cross bit-plane
    boundaries (3→4, 7→8, 11→12). If lower level > higher level,
    the higher bit-plane is too short.

    Returns: (bit_plane_index, correction_factor, description)
    """
    sigs = {la['level']: la['signal'] for la in level_avgs}
    max_sig = sigs.get(15, 1)

    # Check each bit-plane boundary
    # Level 3 (B0+B1) vs Level 4 (B2): if 3 > 4, B2 too short
    # Level 7 (B0+B1+B2) vs Level 8 (B3): if 7 > 8, B3 too short
    # Level 11 (B0+B1+B3) vs Level 12 (B2+B3): if 11 > 12, this is secondary

    issues = []

    # B2 check: level 3 vs 4
    if sigs.get(3, 0) > 0 and sigs.get(4, 0) > 0:
        ratio = sigs[4] / sigs[3] if sigs[3] > 0 else 999
        expected_ratio = 4 / 3  # ideal: level 4 should be 4/3 of level 3
        correction = expected_ratio / ratio if ratio > 0 else 1
        error = abs(sigs[4] - sigs[3] * expected_ratio) / max_sig * 100
        if sigs[3] >= sigs[4]:  # non-monotonic
            issues.append((2, correction, f'B2: L3={sigs[3]:.0f} >= L4={sigs[4]:.0f}, correction={correction:.3f}', error))
        elif error > 2:
            issues.append((2, correction, f'B2: L3/L4 ratio off by {error:.1f}%', error))

    # B3 check: level 7 vs 8
    if sigs.get(7, 0) > 0 and sigs.get(8, 0) > 0:
        ratio = sigs[8] / sigs[7] if sigs[7] > 0 else 999
        expected_ratio = 8 / 7
        correction = expected_ratio / ratio if ratio > 0 else 1
        error = abs(sigs[8] - sigs[7] * expected_ratio) / max_sig * 100
        if sigs[7] >= sigs[8]:
            issues.append((3, correction, f'B3: L7={sigs[7]:.0f} >= L8={sigs[8]:.0f}, correction={correction:.3f}', error))
        elif error > 2:
            issues.append((3, correction, f'B3: L7/L8 ratio off by {error:.1f}%', error))

    # Also check overall linearity error per bit-plane
    # B1 affects levels where bit 1 matters most
    if not issues:
        # No boundary issues — check overall max error
        max_err_level = max(level_avgs, key=lambda la: abs(la['error_pct']))
        if abs(max_err_level['error_pct']) > 3:
            # Find which bit-plane contributes most to this level
            level = max_err_level['level']
            # The highest set bit is the dominant contributor
            for b in range(3, -1, -1):
                if level & (1 << b):
                    # This bit-plane is on for this level — adjust it
                    direction = 1 if max_err_level['error_pct'] < 0 else -1
                    correction = 1 + direction * abs(max_err_level['error_pct']) / 200
                    issues.append((b, correction, f'B{b}: level {level} error={max_err_level["error_pct"]:+.1f}%',
                                  abs(max_err_level['error_pct'])))
                    break

    if not issues:
        return None  # linearity is good!

    # Return the worst issue
    issues.sort(key=lambda x: x[3], reverse=True)
    return issues[0]

def main():
    T_us = float(sys.argv[1]) if len(sys.argv) > 1 else 0.7
    max_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 6

    print(f'=== BCM Weight Optimizer ===')
    print(f'T = {T_us} us | Max iterations: {max_iters}')
    print(f'Integration: {IT_MS}ms | Hold: {HOLD_SEC}s | Gap: {GAP_SEC}s')

    # Open devices
    spec = Spectrometer(list_devices()[0])
    wl = spec.wavelengths()
    spec.integration_time_micros(IT_MS * 1000)
    peak_mask = (wl > 560) & (wl < 580)  # 20nm window
    print(f'Spectrometer OK: {spec.model}')

    port = find_port()
    ser = serial.Serial(port, BAUD, timeout=2)
    time.sleep(2); ser.reset_input_buffer()
    print(f'Serial OK: {port}')

    # Setup
    for cmd in ['ROWS 20', 'BCM 4', f'BCMON {T_us}', 'EXTTRIG OFF', 'BCMWEIGHTS RESET']:
        ser.write((cmd + '\r\n').encode())
        time.sleep(0.5)
        while ser.in_waiting: ser.read(ser.in_waiting)

    weights = [1.0, 2.0, 4.0, 8.0]
    all_results = []

    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'spectrometer_data', f'optimizer_T{T_us}_{time.strftime("%Y%m%d_%H%M%S")}')
    os.makedirs(out_dir, exist_ok=True)

    for iteration in range(max_iters + 1):  # +1 for initial measurement
        w_str = f'{weights[0]:.2f} {weights[1]:.2f} {weights[2]:.2f} {weights[3]:.2f}'

        print(f'\n{"="*60}')
        print(f'Iteration {iteration}: weights = [{w_str}]')
        print(f'{"="*60}')

        # Set weights
        ser.write(f'BCMWEIGHTS {w_str}\r\n'.encode())
        time.sleep(0.5)
        resp = ser.read(ser.in_waiting).decode(errors='replace')
        for line in resp.strip().split('\n'):
            if line.strip():
                print(f'  {line.strip()}')

        update_status('Weight optimizer',
                     f'Iter {iteration}/{max_iters} | W=[{w_str}]',
                     f'Running PHOTOCAL...')

        # Run measurement
        result = run_photocal_sweep(ser, spec, wl, peak_mask)
        result['weights'] = weights.copy()
        result['iteration'] = iteration
        result['T_us'] = T_us

        # Print results
        level_avgs = result['level_avgs']
        sigs = [la['signal'] for la in level_avgs]
        mono = all(sigs[i] <= sigs[i+1] for i in range(len(sigs)-1))
        max_err = max(abs(la['error_pct']) for la in level_avgs)

        print(f'\n  {"Level":>5} {"Signal":>8} {"Norm":>6} {"Ideal":>6} {"Err":>7}')
        print(f'  {"-"*38}')
        for la in level_avgs:
            marker = ' *' if abs(la['error_pct']) > 4 else ''
            print(f'  {la["level"]:>5} {la["signal"]:>8.0f} {la["normalized"]:>6.3f} {la["ideal"]:>6.3f} {la["error_pct"]:>+7.1f}%{marker}')

        print(f'\n  Monotonic: {"YES" if mono else "NO"} | Max error: {max_err:.1f}%')

        # Save iteration data
        iter_path = os.path.join(out_dir, f'iter_{iteration:02d}.json')
        with open(iter_path, 'w') as f:
            json.dump({
                'iteration': iteration,
                'weights': weights.copy(),
                'T_us': T_us,
                'monotonic': mono,
                'max_error_pct': max_err,
                'dark': result['dark'],
                'max_signal': result['max_signal'],
                'level_avgs': level_avgs,
                'readings': result['readings'],
                'full_spectra': result['full_spectra'],
                'wavelengths': wl.tolist(),
                'integration_ms': IT_MS,
                'peak_window_nm': '560-580',
            }, f, indent=2)

        all_results.append({
            'iteration': iteration,
            'weights': weights.copy(),
            'monotonic': mono,
            'max_error_pct': max_err,
            'level_signals': sigs,
        })

        # Check convergence
        if mono and max_err < 3.0:
            print(f'\n  ✓ CONVERGED: monotonic with max error {max_err:.1f}%')
            break

        if iteration == max_iters:
            print(f'\n  ✗ Max iterations reached. Best so far: max_err={max_err:.1f}%')
            break

        # Find which weight to adjust
        fix = find_worst_bitplane(level_avgs)
        if fix is None:
            print(f'\n  ✓ No significant issues found.')
            break

        bp, correction, desc, _ = fix
        old_w = weights[bp]
        # Apply correction conservatively (50% of computed correction)
        damped_correction = 1 + (correction - 1) * 0.5
        new_w = old_w * damped_correction
        # Clamp to reasonable range
        nominal = [1, 2, 4, 8][bp]
        new_w = max(nominal * 0.8, min(nominal * 1.3, new_w))

        print(f'\n  → Adjusting B{bp}: {old_w:.3f} → {new_w:.3f} ({desc})')
        weights[bp] = round(new_w, 3)

    # Summary
    print(f'\n{"="*60}')
    print(f'OPTIMIZER SUMMARY (T={T_us}us)')
    print(f'{"="*60}')
    print(f'{"Iter":>4} {"Weights":>28} {"Mono":>5} {"MaxErr":>7}')
    print(f'{"-"*48}')
    for r in all_results:
        w = r['weights']
        print(f'{r["iteration"]:>4} [{w[0]:.2f},{w[1]:.2f},{w[2]:.2f},{w[3]:.2f}]'
              f'  {"YES" if r["monotonic"] else "NO":>5} {r["max_error_pct"]:>6.1f}%')

    # Save summary
    summary_path = os.path.join(out_dir, 'summary.json')
    with open(summary_path, 'w') as f:
        json.dump({'T_us': T_us, 'iterations': all_results, 'final_weights': weights}, f, indent=2)
    print(f'\nAll data saved in: {out_dir}/')

    # Generate comparison plot
    generate_optimizer_plot(out_dir, all_results, wl.tolist())

    update_status('Optimizer DONE',
                 f'T={T_us} | Final W=[{weights[0]:.2f},{weights[1]:.2f},{weights[2]:.2f},{weights[3]:.2f}]',
                 f'{"Monotonic" if all_results[-1]["monotonic"] else "Not monotonic"} | Err={all_results[-1]["max_error_pct"]:.1f}%')

    spec.close(); ser.close()

def generate_optimizer_plot(out_dir, all_results, wl_list):
    """Generate HTML plot showing linearity convergence across iterations."""

    # Color palette for iterations
    colors = ['#ff4444', '#ff8844', '#ffcc44', '#88cc44', '#44cc88', '#44aaff', '#8844ff']

    html = """<!DOCTYPE html>
<html><head><title>BCM Weight Optimizer — Convergence</title>
<style>
body { font-family: monospace; background: #1a1a2e; color: #e0e0e0; margin: 20px; }
h2 { color: #00d4ff; }
canvas { border: 1px solid #444; background: #0d0d1a; display: block; margin: 5px 0 15px 0; }
.stats { background: #222; padding: 10px; border-radius: 5px; margin: 10px 0; }
</style></head><body>
<h2>BCM Weight Optimizer — Linearity Convergence</h2>
<canvas id="conv" width="800" height="400"></canvas>
<pre id="summary" style="font-size:12px;"></pre>
<script>
"""

    html += f"const results = {json.dumps(all_results)};\n"
    html += f"const colors = {json.dumps(colors[:len(all_results)])};\n"

    html += """
const c = document.getElementById('conv'), ctx = c.getContext('2d');
const W=c.width, H=c.height, m={l:60,r:150,t:20,b:40};
const pw=W-m.l-m.r, ph=H-m.t-m.b;
const mapX = l => m.l + l/15*pw;
const mapY = v => m.t + ph - v*ph;

// Grid
ctx.strokeStyle='#333';ctx.lineWidth=0.5;
for(let v=0;v<=1;v+=0.2){
    ctx.beginPath();ctx.moveTo(m.l,mapY(v));ctx.lineTo(W-m.r,mapY(v));ctx.stroke();
    ctx.fillStyle='#888';ctx.font='10px monospace';ctx.textAlign='right';
    ctx.fillText(v.toFixed(1),m.l-5,mapY(v)+4);
}
for(let l=0;l<=15;l+=5){
    const x=mapX(l);
    ctx.beginPath();ctx.moveTo(x,m.t);ctx.lineTo(x,H-m.b);ctx.stroke();
    ctx.fillStyle='#888';ctx.textAlign='center';ctx.fillText(l,x,H-m.b+15);
}

// Ideal line
ctx.strokeStyle='#ffffff44';ctx.lineWidth=2;ctx.setLineDash([6,4]);
ctx.beginPath();ctx.moveTo(mapX(0),mapY(0));ctx.lineTo(mapX(15),mapY(1));ctx.stroke();
ctx.setLineDash([]);

// Plot each iteration
results.forEach((r, idx) => {
    const maxSig = r.level_signals[15] || 1;
    ctx.strokeStyle = colors[idx % colors.length];
    ctx.lineWidth = idx === results.length-1 ? 3 : 1.5;
    ctx.globalAlpha = idx === results.length-1 ? 1.0 : 0.5;
    ctx.beginPath();
    r.level_signals.forEach((s, level) => {
        const n = s / maxSig;
        level === 0 ? ctx.moveTo(mapX(level), mapY(n)) : ctx.lineTo(mapX(level), mapY(n));
    });
    ctx.stroke();

    // Dots on final iteration
    if (idx === results.length - 1) {
        ctx.fillStyle = colors[idx % colors.length];
        r.level_signals.forEach((s, level) => {
            const n = s / maxSig;
            ctx.beginPath(); ctx.arc(mapX(level), mapY(n), 3, 0, Math.PI*2); ctx.fill();
        });
    }
    ctx.globalAlpha = 1.0;
});

// Legend
const lx = W - m.r + 15;
ctx.fillStyle='#aaa';ctx.font='11px monospace';ctx.textAlign='left';
ctx.fillText('Iteration', lx, m.t+12);
results.forEach((r, idx) => {
    const y = m.t + 28 + idx * 40;
    ctx.fillStyle = colors[idx % colors.length];
    ctx.fillRect(lx, y-4, 12, 8);
    ctx.fillStyle = '#ccc'; ctx.font='10px monospace';
    const w = r.weights;
    ctx.fillText('#'+idx+' ['+w.map(v=>v.toFixed(1)).join(',')+']', lx+16, y+4);
    ctx.fillStyle = r.monotonic ? '#3fb950' : '#ff4444';
    ctx.fillText((r.monotonic?'Mono':'!Mono')+' '+r.max_error_pct.toFixed(1)+'%', lx+16, y+16);
});

ctx.fillStyle='#aaa';ctx.font='12px monospace';ctx.textAlign='center';
ctx.fillText('BCM Level', m.l+pw/2, H-5);

// Summary text
let txt = 'Iteration  Weights              Monotonic  MaxErr\\n';
txt += '─'.repeat(55) + '\\n';
results.forEach(r => {
    const w = r.weights;
    txt += r.iteration.toString().padStart(5) + '      [' +
           w.map(v=>v.toFixed(2)).join(', ') + ']   ' +
           (r.monotonic ? 'YES' : 'NO ').padEnd(10) +
           r.max_error_pct.toFixed(1) + '%\\n';
});
document.getElementById('summary').textContent = txt;
</script></body></html>"""

    plot_path = os.path.join(out_dir, 'convergence_plot.html')
    with open(plot_path, 'w') as f:
        f.write(html)
    print(f'Convergence plot: {plot_path}')
    os.system(f'open "{plot_path}"')

if __name__ == '__main__':
    main()
