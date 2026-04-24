#!/opt/homebrew/bin/python3.14
"""Build HTML viewer for AD3 triggered mode data."""
import numpy as np
import json

d = np.load("ad3_photodiode_data.npz")
ch1 = d["ch1"]  # (500, 32768)
ch2 = d["ch2"]
ch1_avg = d["ch1_avg"]
ch2_avg = d["ch2_avg"]
avg_a_ch1 = d["avg_a_ch1"]
avg_a_ch2 = d["avg_a_ch2"]
avg_b_ch1 = d["avg_b_ch1"]
avg_b_ch2 = d["avg_b_ch2"]
ref_edges = d["ref_edges"]
row_assignments = d["row_assignments"]
peak_vals = d["peak_vals"]
bright_a = int(d["bright_edge_a"])
bright_b = int(d["bright_edge_b"])
t_us = d["t_us"]
sr = float(d["sample_rate"])
N = int(d["n_captures"])
bl = len(t_us)

# Compute per-edge averages for a heatmap-like view
n_edges = len(ref_edges)
edge_avgs = np.zeros((n_edges, bl))
edge_counts = np.zeros(n_edges, dtype=int)
for i in range(n_edges):
    mask = row_assignments == i
    if mask.sum() > 0:
        edge_avgs[i] = ch1[mask].mean(axis=0)
        edge_counts[i] = mask.sum()

# Downsample for JSON (32K is too much for inline JSON at 21 rows)
dec = 4  # 32768/4 = 8192 points per trace
def ds(a):
    n = len(a) // dec * dec
    return a[:n].reshape(-1, dec).mean(axis=1)

def to_list(a):
    return [round(float(x), 4) for x in a]

t_ds = ds(t_us)

# Signal analysis for row A
baseline_a = avg_a_ch1[:int(bl*0.04)].mean()
noise_a = avg_a_ch1[:int(bl*0.04)].std()
signal_a = (avg_a_ch1 - baseline_a) * 1000

# Row A detail: zoom to burst at bright_a edge
edge_a_idx = ref_edges[bright_a]
zoom_pre = int(10e-6 * sr)
zoom_post = int(25e-6 * sr)
s = max(0, edge_a_idx - zoom_pre)
e = min(bl, edge_a_idx + zoom_post)
detail_t = (np.arange(e - s) - (edge_a_idx - s)) / sr * 1e6
detail_ch1 = avg_a_ch1[s:e] * 1000
detail_ch2 = avg_a_ch2[s:e]

# Also get a single raw capture for row A
raw_a_idx = np.where(row_assignments == bright_a)[0][0]
raw_ch1 = ch1[raw_a_idx, s:e] * 1000
raw_ch2 = ch2[raw_a_idx, s:e]

viewer = {
    't_us': to_list(t_ds),
    # All-capture average
    'all_avg_mV': to_list(ds(ch1_avg * 1000)),
    'all_ch2_V': to_list(ds(ch2_avg)),
    # Row A average (concentrated signal)
    'row_a_mV': to_list(ds(avg_a_ch1 * 1000)),
    'row_a_ch2': to_list(ds(avg_a_ch2)),
    'row_a_edge': bright_a,
    'row_a_n': int((row_assignments == bright_a).sum()),
    'row_a_t_us': round(float(t_us[ref_edges[bright_a]]), 1),
    # Row B average
    'row_b_mV': to_list(ds(avg_b_ch1 * 1000)),
    'row_b_ch2': to_list(ds(avg_b_ch2)),
    'row_b_edge': bright_b,
    'row_b_n': int((row_assignments == bright_b).sum()),
    'row_b_t_us': round(float(t_us[ref_edges[bright_b]]), 1),
    # Detail (not downsampled - short window)
    'detail_t': to_list(detail_t),
    'detail_ch1': to_list(detail_ch1),
    'detail_ch2': to_list(detail_ch2),
    'raw_ch1': to_list(raw_ch1),
    'raw_ch2': to_list(raw_ch2),
    # Per-edge signal heatmap data
    'edge_signals': [to_list(ds((edge_avgs[i] - edge_avgs[i, :int(bl*0.04)].mean()) * 1000))
                     for i in range(n_edges)],
    'edge_counts': [int(c) for c in edge_counts],
    'edge_times': [round(float(t_us[e]), 1) for e in ref_edges],
    # Metadata
    'n': N,
    'sr_MHz': round(sr / 1e6, 2),
    'res_ns': round(1e9 / sr, 1),
    'window_us': round(float(t_us[-1] - t_us[0]), 0),
    'buf_size': bl,
    'n_edges': n_edges,
    'peak_signal_mV': round(float(signal_a.max()), 1),
}

html = """<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>AD3 Triggered: 12.5 MHz Dual-Channel, 32K Buffer</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
  body { font-family: -apple-system, sans-serif; margin: 20px; background: #1a1a2e; color: #e0e0e0; }
  h1, h2 { color: #00d4ff; margin-top: 30px; }
  .plot { width: 100%; height: 420px; margin-bottom: 10px; }
  .plot-tall { width: 100%; height: 500px; margin-bottom: 10px; }
  .stats { background: #16213e; padding: 15px; border-radius: 8px; margin-bottom: 20px;
           font-family: monospace; line-height: 1.8; font-size: 13px; }
  .stats b { color: #00d4ff; }
  .hl { color: #ffd93d; font-weight: bold; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }
  @media (max-width: 1200px) { .grid { grid-template-columns: 1fr; } }
</style>
</head><body>

<h1>AD3 Triggered Mode: 12.5 MHz Dual-Channel, 32K Buffer (Config 1)</h1>

<div class="stats">
  <b>Mode:</b> Single-shot triggered &mdash; on-device FPGA buffer (no USB streaming bottleneck)<br>
  <b>Sample rate:</b> <span class="hl">12.50 MHz</span> (80 ns/sample) &mdash;
  vs 2 MHz max in USB record mode (6.25x improvement)<br>
  <b>Buffer:</b> <span class="hl">32,768 samples/channel</span> (Config 1) = 2,621 &mu;s window &mdash;
  covers full 2,500 &mu;s row cycle (20 rows &times; 125 &mu;s)<br>
  <b>Channels:</b> Ch1 = photodiode (500 mV range) | Ch2 = GP45 trigger reference (5V range)<br>
  <b>Trigger:</b> W1 &rarr; GP45 (8 kHz square), scope triggers on W1 edge<br>
  <b>Captures:</b> 500 at ~137/sec | <b>21 trigger edges per capture</b><br>
  <b>Setup:</b> pixel_data[10][0..19] = 15 (full row 10), all other rows = 0<br>
  <b>Classification:</b> Each capture is assigned to the trigger edge nearest its PD peak.
  All 500 captures show ~228 mV peak (the row-10 burst), distributed uniformly across 21 edges
  (~25 captures/edge). Edge-classified averaging concentrates the signal.
</div>

<h2>1. All-Capture Average (500 captures, signal diluted across 21 trigger positions)</h2>
<div id="plot_all" class="plot"></div>

<h2>2. Row-Classified Average: Bright Row at Edge EDGE_A (N_A captures)</h2>
<p style="color:#888;">Only captures where row-10 burst appears at trigger edge EDGE_A are averaged.
Full signal amplitude is recovered.</p>
<div id="plot_row_a" class="plot"></div>

<h2>3. Burst Detail: 80 ns Resolution (t=0 = trigger edge)</h2>
<div class="grid"><div>
<h2 style="margin-top:0;">Averaged</h2>
<div id="plot_detail_avg" class="plot"></div>
</div><div>
<h2 style="margin-top:0;">Single Raw Capture</h2>
<div id="plot_detail_raw" class="plot"></div>
</div></div>

<h2>4. Signal Heatmap: All 21 Trigger Edge Positions</h2>
<p style="color:#888;">Each row = captures classified to that trigger edge, averaged.
The bright diagonal band shows row-10's burst appearing at each edge position.</p>
<div id="plot_heatmap" class="plot-tall"></div>

<script>
const D = VIEWER_DATA_PLACEHOLDER;

const dark = {
  paper_bgcolor: '#1a1a2e', plot_bgcolor: '#16213e',
  font: { color: '#e0e0e0' },
  xaxis: { gridcolor: '#2a2a4e', zerolinecolor: '#3a3a6e' },
  yaxis: { gridcolor: '#2a2a4e', zerolinecolor: '#3a3a6e' },
  margin: { t: 40, b: 50, l: 70, r: 40 },
  legend: { bgcolor: 'rgba(22,33,62,0.8)' },
};

// Trigger edge shapes
var trigShapes = D.edge_times.map(function(t) {
  return { type: 'line', x0: t, x1: t, y0: 0, y1: 1, yref: 'paper',
           line: { color: 'rgba(0,212,255,0.12)', width: 1 } };
});

// Plot 1: all-capture average
Plotly.newPlot('plot_all', [
  { x: D.t_us, y: D.all_avg_mV, name: 'Ch1: Photodiode (mV)',
    line: { color: '#ff6b6b', width: 1.5 } },
  { x: D.t_us, y: D.all_ch2_V.map(function(v) { return v * 50 + 30; }),
    name: 'Ch2: Trigger (scaled)', yaxis: 'y2',
    line: { color: '#4ecdc4', width: 0.8 } },
], Object.assign({}, dark, {
  title: { text: 'All 500 Captures Averaged (row-10 signal diluted ~20x)', font: { color: '#00d4ff' } },
  xaxis: Object.assign({}, dark.xaxis, { title: 'Time (us)' }),
  yaxis: Object.assign({}, dark.yaxis, { title: 'Photodiode (mV)' }),
  yaxis2: { overlaying: 'y', side: 'right', showgrid: false, showticklabels: false },
  shapes: trigShapes,
}));

// Plot 2: row-classified average
Plotly.newPlot('plot_row_a', [
  { x: D.t_us, y: D.row_a_mV, name: 'Ch1: Photodiode (mV)',
    line: { color: '#ff6b6b', width: 1.5 } },
  { x: D.t_us, y: D.row_a_ch2.map(function(v) { return v * 50 + 30; }),
    name: 'Ch2: Trigger (scaled)',
    line: { color: '#4ecdc4', width: 0.8 } },
], Object.assign({}, dark, {
  title: { text: 'Row-Classified Average (edge ' + D.row_a_edge + ', ' + D.row_a_n + ' captures)', font: { color: '#00d4ff' } },
  xaxis: Object.assign({}, dark.xaxis, { title: 'Time (us)' }),
  yaxis: Object.assign({}, dark.yaxis, { title: 'Photodiode (mV)' }),
  shapes: trigShapes.concat([{
    type: 'line', x0: D.row_a_t_us, x1: D.row_a_t_us, y0: 0, y1: 1, yref: 'paper',
    line: { color: '#ffd93d', dash: 'dash', width: 2 }
  }]),
  annotations: [{
    x: D.row_a_t_us, y: 0.95, yref: 'paper',
    text: 'Row 10 burst here',
    showarrow: false, font: { color: '#ffd93d', size: 12 }
  }],
}));

// Plot 3a: detail averaged
Plotly.newPlot('plot_detail_avg', [
  { x: D.detail_t, y: D.detail_ch1, name: 'Photodiode (mV)',
    line: { color: '#ff6b6b', width: 2 } },
  { x: D.detail_t, y: D.detail_ch2.map(function(v) { return v * 50 + 30; }),
    name: 'Trigger (scaled)',
    line: { color: '#4ecdc4', width: 1 } },
], Object.assign({}, dark, {
  title: { text: 'Averaged Burst Detail (80 ns/sample)', font: { color: '#00d4ff', size: 13 } },
  xaxis: Object.assign({}, dark.xaxis, { title: 'Time from trigger (us)' }),
  yaxis: Object.assign({}, dark.yaxis, { title: 'mV' }),
  shapes: [{ type: 'line', x0: 0, x1: 0, y0: 0, y1: 1, yref: 'paper',
             line: { color: '#00d4ff', dash: 'dot', width: 1 } }],
}));

// Plot 3b: detail raw
Plotly.newPlot('plot_detail_raw', [
  { x: D.detail_t, y: D.raw_ch1, name: 'Photodiode (mV)',
    line: { color: '#ff6b6b', width: 1.5 } },
  { x: D.detail_t, y: D.raw_ch2.map(function(v) { return v * 50 + 30; }),
    name: 'Trigger (scaled)',
    line: { color: '#4ecdc4', width: 1 } },
], Object.assign({}, dark, {
  title: { text: 'Single Raw Capture (no averaging)', font: { color: '#00d4ff', size: 13 } },
  xaxis: Object.assign({}, dark.xaxis, { title: 'Time from trigger (us)' }),
  yaxis: Object.assign({}, dark.yaxis, { title: 'mV' }),
  shapes: [{ type: 'line', x0: 0, x1: 0, y0: 0, y1: 1, yref: 'paper',
             line: { color: '#00d4ff', dash: 'dot', width: 1 } }],
}));

// Plot 4: heatmap
var heatmap_z = D.edge_signals;
var heatmap_y = D.edge_times.map(function(t, i) {
  return 'Edge ' + i + ' (' + t.toFixed(0) + 'us, n=' + D.edge_counts[i] + ')';
});
Plotly.newPlot('plot_heatmap', [{
  z: heatmap_z,
  x: D.t_us,
  y: heatmap_y,
  type: 'heatmap',
  colorscale: [
    [0, '#16213e'], [0.1, '#1a1a5e'], [0.3, '#2a2a8e'],
    [0.5, '#4ecdc4'], [0.7, '#ffd93d'], [1.0, '#ff6b6b']
  ],
  colorbar: { title: 'Signal (mV)', titlefont: { color: '#e0e0e0' }, tickfont: { color: '#e0e0e0' } },
}], Object.assign({}, dark, {
  title: { text: 'Per-Edge Signal Map (each row = avg of captures at that trigger edge)', font: { color: '#00d4ff' } },
  xaxis: Object.assign({}, dark.xaxis, { title: 'Time (us)' }),
  yaxis: Object.assign({}, dark.yaxis, { title: '', autorange: 'reversed' }),
  margin: { t: 40, b: 50, l: 200, r: 100 },
}));
</script>
</body></html>"""

# Replace placeholders
html = html.replace('EDGE_A', str(bright_a))
html = html.replace('N_A', str((row_assignments == bright_a).sum()))
html = html.replace('VIEWER_DATA_PLACEHOLDER', json.dumps(viewer))

with open("ad3_photodiode_data.html", "w") as f:
    f.write(html)
print(f"Saved: ad3_photodiode_data.html ({len(html)//1024} KB)")
