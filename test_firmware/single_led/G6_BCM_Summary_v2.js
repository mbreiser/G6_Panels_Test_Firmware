const pptxgen = require("pptxgenjs");
const fs = require("fs");

let pres = new pptxgen();
pres.layout = "LAYOUT_16x9";
pres.author = "Reiser Lab";
pres.title = "G6 20x20 LED Panel - BCM Timing & Linearity";

const C = {
  bg: "0D1117", bgCard: "161B22",
  accent: "58A6FF", accent2: "3FB950", accent3: "F78166",
  text: "E6EDF3", textMuted: "8B949E", border: "30363D", white: "FFFFFF",
};

function addBar(slide) {
  slide.background = { color: C.bg };
  slide.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
}

// ═══════════════════════════════════════════════════════════════
// SLIDE 1: Title
// ═══════════════════════════════════════════════════════════════
let s1 = pres.addSlide();
addBar(s1);
s1.addText("G6 20×20 LED Panel", {
  x:0.8, y:1.0, w:8.4, h:1.0, fontSize:42, fontFace:"Arial Black", color:C.white, bold:true, margin:0,
});
s1.addText("Timing, Grayscale & Linearity Characterization", {
  x:0.8, y:1.9, w:8.4, h:0.7, fontSize:26, fontFace:"Calibri", color:C.accent, margin:0,
});
const chips = ["RP2354 MCU (150 MHz)", "PIO-driven BCM", "Zero-jitter architecture", "Spectrometer-calibrated"];
chips.forEach((txt, i) => {
  s1.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x:0.8+i*2.25, y:3.0, w:2.1, h:0.4, fill:{color:C.bgCard}, line:{color:C.border, width:1}, rectRadius:0.1,
  });
  s1.addText(txt, {
    x:0.8+i*2.25, y:3.0, w:2.1, h:0.4, fontSize:10, fontFace:"Calibri", color:C.textMuted, align:"center", valign:"middle", margin:0,
  });
});
s1.addText("Reiser Lab — March 2026", { x:0.8, y:4.8, w:8.4, h:0.4, fontSize:14, fontFace:"Calibri", color:C.textMuted, margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 2: Zero-Jitter Architecture
// ═══════════════════════════════════════════════════════════════
let s2 = pres.addSlide();
addBar(s2);
s2.addText("Zero-Jitter BCM Architecture", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

const stats = [
  { val:"0.000 µs", label:"Jitter", sub:"640k measurements" },
  { val:"9.5 µs", label:"Burst time", sub:"5.5 µs margin in 15 µs" },
  { val:"400 Hz", label:"Frame rate", sub:"8 kHz ÷ 20 rows" },
];
stats.forEach((st, i) => {
  const y = 1.0 + i*1.3;
  s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.6, y, w:4.2, h:1.1, fill:{color:C.bgCard}, line:{color:C.border, width:1}, rectRadius:0.1 });
  s2.addText(st.val, { x:0.8, y:y+0.05, w:3.8, h:0.5, fontSize:30, fontFace:"Consolas", color:C.accent, bold:true, margin:0 });
  s2.addText(st.label, { x:0.8, y:y+0.55, w:1.8, h:0.3, fontSize:13, fontFace:"Calibri", color:C.white, bold:true, margin:0 });
  s2.addText(st.sub, { x:2.6, y:y+0.55, w:2.2, h:0.3, fontSize:11, fontFace:"Calibri", color:C.textMuted, margin:0 });
});

s2.addText("Zero-Jitter Recipe", { x:5.2, y:1.0, w:4.4, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent3, bold:true, margin:0 });
const recipe = ["__not_in_flash_func + noinline", "noInterrupts() during burst", "multicore_lockout (Core 1)", "100-trigger warm-up"];
s2.addText(recipe.map((t,i) => ({
  text: t, options: { bullet:{code:"2713"}, breakLine:i<recipe.length-1, fontSize:12, fontFace:"Consolas", color:C.text }
})), { x:5.2, y:1.4, w:4.4, h:1.3, margin:0 });

s2.addText("Architecture", { x:5.2, y:2.9, w:4.4, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
const arch = ["4-bit BCM: 16 intensity levels","PIO drives 20 columns simultaneously","CPU manages rows (gpio_set_mask64)","Single row per 8 kHz trigger","External trigger on GP45 (zero jitter)"];
s2.addText(arch.map((t,i) => ({
  text: t, options: { bullet:true, breakLine:i<arch.length-1, fontSize:11, color:C.text }
})), { x:5.2, y:3.3, w:4.4, h:1.5, fontFace:"Calibri", margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 3: BCM Waveform (Saleae pulse average)
// ═══════════════════════════════════════════════════════════════
let s3 = pres.addSlide();
addBar(s3);
s3.addText("BCM Waveform — Pulse-Triggered Average", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

s3.addImage({ path: "bcm_pulse_average.png", x:0.4, y:0.9, w:6.2, h:3.6 });

s3.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:6.8, y:0.9, w:2.8, h:3.6, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s3.addText("Key Observations", { x:7.0, y:1.0, w:2.5, h:0.3, fontSize:14, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
s3.addText([
  { text:"499 pulses averaged\n", options:{breakLine:true, bold:true, fontSize:11} },
  { text:"50 MHz analog capture\n\n", options:{breakLine:true, fontSize:10} },
  { text:"4 bit-planes resolved:\n", options:{breakLine:true, bold:true, fontSize:11} },
  { text:"B0: 0.5µs  B1: 1.0µs\n", options:{breakLine:true, fontSize:10, fontFace:"Consolas"} },
  { text:"B2: 2.0µs  B3: 4.0µs\n\n", options:{breakLine:true, fontSize:10, fontFace:"Consolas"} },
  { text:"~0.4µs PIO overhead gaps\nbetween bit-planes\n\n", options:{breakLine:true, fontSize:10} },
  { text:"Peak amplitude decreases\nwith each successive\nbit-plane — key finding\nfor linearity calibration", options:{fontSize:10, color:C.accent3} },
], { x:7.0, y:1.35, w:2.5, h:3.0, fontFace:"Calibri", color:C.text, margin:0 });

s3.addText("Photodiode signal via Saleae Logic Pro 8 analog input", {
  x:0.4, y:4.7, w:6.2, h:0.3, fontSize:10, fontFace:"Calibri", color:C.textMuted, italic:true, margin:0
});

// ═══════════════════════════════════════════════════════════════
// SLIDE 4: LED Spectra across 16 levels
// ═══════════════════════════════════════════════════════════════
let s4 = pres.addSlide();
s4.background = { color: C.bg };
s4.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
s4.addText("LED Spectra — 16 BCM Intensity Levels", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });
s4.addImage({ path:"slide_plots/spectra_16levels.png", x:0.3, y:0.8, w:6.5, h:3.3 });
s4.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:7.0, y:0.8, w:2.7, h:3.3, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s4.addText([
  { text:"Ocean Insight Flame X\n", options:{breakLine:true, fontSize:12, bold:true} },
  { text:"570.8 nm peak\n", options:{breakLine:true, fontSize:11, color:C.accent} },
  { text:"50ms integration\n", options:{breakLine:true, fontSize:10} },
  { text:"560-580nm window (20nm)\n\n", options:{breakLine:true, fontSize:10} },
  { text:"T = 0.7 µs\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"All 400 LEDs active\n\n", options:{breakLine:true, fontSize:10} },
  { text:"Signal grows linearly\nwith BCM level (0→15)\n\n", options:{breakLine:true, fontSize:10} },
  { text:"Spectra captured at\nmiddle of each 3s hold", options:{fontSize:9, color:C.textMuted} },
], { x:7.2, y:0.95, w:2.4, h:3.0, fontFace:"Calibri", color:C.text, margin:0 });
s4.addText("Raw time trace (16 levels × 3s + 1s OFF gaps)", { x:0.3, y:4.2, w:6.5, h:0.2, fontSize:9, fontFace:"Calibri", color:C.textMuted, italic:true, margin:0 });
s4.addImage({ path:"slide_plots/time_trace.png", x:0.3, y:4.35, w:9.4, h:1.05 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 4b: Linearity — Default Weights (data plot)
// ═══════════════════════════════════════════════════════════════
let s4b = pres.addSlide();
s4b.background = { color: C.bg };
s4b.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent3} });
s4b.addText("BCM Linearity — Default Weights [1, 2, 4, 8]", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });
s4b.addImage({ path:"slide_plots/linearity_default.png", x:0.3, y:0.8, w:5.2, h:3.9 });
s4b.addImage({ path:"slide_plots/bitplane_brightness.png", x:5.5, y:0.8, w:4.2, h:3.3 });
s4b.addText([
  { text:"Non-monotonic at bit-plane boundaries:\n", options:{breakLine:true, fontSize:11, bold:true, color:C.accent3} },
  { text:"L3 (B0+B1) ≈ L4 (B2)  |  L7 (B0+B1+B2) > L8 (B3)  |  L11 ≈ L12", options:{fontSize:10} },
], { x:5.5, y:4.2, w:4.2, h:0.6, fontFace:"Calibri", color:C.text, margin:0 });
s4b.addText("Longer bit-planes produce less light per µs — 56% drop from B0 to B3", { x:0.3, y:4.85, w:9.4, h:0.25, fontSize:11, fontFace:"Calibri", color:C.accent, italic:true, margin:0 });

// Data for comparison tables
const baselineNorm = [0.0, 0.1002, 0.1677, 0.2665, 0.2671, 0.3646, 0.4302, 0.5261, 0.5127, 0.6074, 0.6708, 0.7638, 0.7615, 0.8528, 0.9127, 1.0];
const optimizedNorm = [0.0, 0.0832, 0.1391, 0.2216, 0.2746, 0.3561, 0.4108, 0.4917, 0.5391, 0.6183, 0.6719, 0.7498, 0.7989, 0.8754, 0.9258, 1.0];
const ideal = [0.0, 0.0667, 0.1333, 0.2, 0.2667, 0.3333, 0.4, 0.4667, 0.5333, 0.6, 0.6667, 0.7333, 0.8, 0.8667, 0.9333, 1.0];

// ═══════════════════════════════════════════════════════════════
// SLIDE 5: Optimized Weights — Convergence
// ═══════════════════════════════════════════════════════════════
let s5 = pres.addSlide();
s5.background = { color: C.bg };
s5.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent2} });
s5.addText("BCMWEIGHTS Optimization — Converged in 6 Iterations", { x:0.6, y:0.25, w:9, h:0.5, fontSize:26, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Convergence table
const convData = [
  ["Iter", "B0", "B1", "B2", "B3", "Mono?", "Max Err"],
  ["0", "1.00", "2.00", "4.00", "8.00", "NO", "6.7%"],
  ["1", "1.00", "2.00", "4.00", "8.69", "NO", "5.4%"],
  ["2", "1.00", "2.00", "4.66", "8.69", "NO", "5.5%"],
  ["3", "1.00", "2.00", "4.66", "9.41", "YES", "3.5%"],
  ["4", "1.00", "2.00", "4.66", "9.80", "YES", "3.0%"],
  ["5", "1.00", "2.00", "5.02", "9.80", "YES", "3.4%"],
  ["6", "1.00", "2.00", "5.02", "10.19", "YES", "2.5%"],
];
s5.addTable(convData, {
  x:0.6, y:0.9, w:5.5,
  rowH: Array(8).fill(0.35),
  colW: [0.5, 0.7, 0.7, 0.7, 0.8, 0.7, 0.9],
  fontSize: 11, fontFace:"Consolas", color:C.text,
  border: { type:"solid", pt:0.5, color:C.border },
  autoPage: false,
  rowOpts: [
    { fill:{color:C.accent}, color:C.bg, bold:true, fontSize:10, fontFace:"Calibri" },
    {}, {}, {},
    { fill:{color:"1A2B1A"} }, { fill:{color:"1A2B1A"} }, { fill:{color:"1A2B1A"} },
    { fill:{color:"0D2B0D"} },
  ],
});

// Right side: before/after plot
s5.addImage({ path:"slide_plots/linearity_optimized.png", x:5.8, y:0.8, w:4.0, h:3.0 });

// Bottom: before/after comparison
s5.addText("Before → After", { x:0.6, y:3.85, w:9, h:0.3, fontSize:14, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });

const compData = [["Level", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"]];
const beforeErrs = ideal.map((id, i) => ((baselineNorm[i] - id) * 100).toFixed(0) + "%");
const afterErrs = ideal.map((id, i) => ((optimizedNorm[i] - id) * 100).toFixed(0) + "%");
compData.push(["Before", ...beforeErrs]);
compData.push(["After", ...afterErrs]);

s5.addTable(compData, {
  x:0.6, y:4.2, w:9,
  rowH: [0.28, 0.28, 0.28],
  colW: [0.7, ...Array(16).fill(0.52)],
  fontSize: 8, fontFace:"Consolas", color:C.text,
  border: { type:"solid", pt:0.3, color:C.border },
  autoPage: false,
  rowOpts: [
    { fill:{color:C.accent}, color:C.bg, bold:true, fontSize:7, fontFace:"Calibri" },
    { fill:{color:"2B1A1A"} },
    { fill:{color:"1A2B1A"} },
  ],
});

s5.addText("NOT monotonic, 6.7% error", { x:0.6, y:5.05, w:3, h:0.2, fontSize:9, fontFace:"Calibri", color:C.accent3, margin:0 });
s5.addText("MONOTONIC, 2.5% error ✓", { x:5, y:5.05, w:3, h:0.2, fontSize:9, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 6: Timing Budget
// ═══════════════════════════════════════════════════════════════
let s6 = pres.addSlide();
s6.background = { color: C.bg };
s6.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
s6.addText("Timing Budget — Production Configuration", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

const budgetData = [
  ["Config", "T (µs)", "Weights", "Burst (µs)", "Margin", "Monotonic", "Error"],
  ["Default", "0.50", "1, 2, 4, 8", "9.46", "5.5 µs", "—*", "—*"],
  ["Default", "0.70", "1, 2, 4, 8", "12.1", "2.9 µs", "NO", "6.7%"],
  ["Optimized", "0.70", "1, 2, 5.0, 10.2", "12.7", "2.3 µs", "YES", "2.5%"],
  ["Default", "1.00", "1, 2, 4, 8", "16.96", "−2.0 µs", "YES", "5.6%"],
];
s6.addTable(budgetData, {
  x:0.6, y:0.9, w:8.8,
  rowH: [0.38, 0.35, 0.35, 0.35, 0.35],
  colW: [1.1, 0.8, 1.8, 1.1, 1.0, 1.2, 0.9],
  fontSize: 11, fontFace:"Consolas", color:C.text,
  border: { type:"solid", pt:0.5, color:C.border },
  autoPage: false,
  rowOpts: [
    { fill:{color:C.accent}, color:C.bg, bold:true, fontSize:10, fontFace:"Calibri" },
    {},
    {},
    { fill:{color:"0D2B0D"} },
    {},
  ],
});

s6.addText("Recommended: T=0.7µs + optimized weights", { x:0.6, y:2.8, w:5, h:0.3, fontSize:12, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s6.addText("*T=0.5 linearity not yet measured with spectrometer (too dim for full-panel test)", { x:0.6, y:3.1, w:8.8, h:0.2, fontSize:9, fontFace:"Calibri", color:C.textMuted, italic:true, margin:0 });

// Timeline
s6.addText("8 kHz Trigger Period (125 µs)", { x:0.6, y:3.6, w:8.8, h:0.3, fontSize:14, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
const tlY=4.0, tlW=8.8, bf=12.7/125;
s6.addShape(pres.shapes.RECTANGLE, { x:0.6, y:tlY, w:tlW, h:0.5, fill:{color:C.bgCard}, line:{color:C.border,width:1} });
s6.addShape(pres.shapes.RECTANGLE, { x:0.6, y:tlY, w:tlW*bf, h:0.5, fill:{color:C.accent2} });
s6.addText("Burst 12.7µs", { x:0.6, y:tlY, w:tlW*bf, h:0.5, fontSize:9, fontFace:"Calibri", color:C.bg, bold:true, align:"center", valign:"middle", margin:0 });
s6.addText("Idle: 112.3 µs — SPI, precompute, interrupts", { x:0.6+tlW*bf+0.1, y:tlY, w:tlW*(1-bf)-0.2, h:0.5, fontSize:9, fontFace:"Calibri", color:C.textMuted, align:"center", valign:"middle", margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 7: Next Steps
// ═══════════════════════════════════════════════════════════════
let s7 = pres.addSlide();
s7.background = { color: C.bg };
s7.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent3} });
s7.addText("Next Steps", { x:0.6, y:0.25, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

const nextItems = [
  { title:"Reverse Bit-Plane Order", color:C.accent, items:["BCMORDER REVERSE: scan B3→B0","Test if longest-first improves linearity","Compare with spectrometer + photodiode","May eliminate need for weight calibration"] },
  { title:"Single LED vs Full Row", color:C.accent2, items:["Measure 1 LED → 5 → 10 → 20 → 400","Test current-sharing / voltage-drop effects","May explain brightness-per-µs decay","Focal LED on row 7 (nearest to fiber)"] },
  { title:"PCB Redesign", color:C.accent3, items:["Option B: add EINT trace to GP45","Option C: SPI1 on GP44-47, contiguous rows","40 UCC27517 drivers symmetric (both sides)","EINT + sync output pins on next revision"] },
  { title:"Production Path", color:C.textMuted, items:["Per-pixel pattern loading via SPI","Frame double-buffering at 400 Hz","BCMWEIGHTS calibration per panel","PIO wait-pin for hardware trigger"] },
];
nextItems.forEach((item, i) => {
  const col = i % 2, row = Math.floor(i / 2);
  const x = 0.6 + col * 4.6, y = 0.9 + row * 2.15;
  s7.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w:4.3, h:1.95, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
  s7.addShape(pres.shapes.RECTANGLE, { x, y, w:0.06, h:1.95, fill:{color:item.color} });
  s7.addText(item.title, { x:x+0.25, y:y+0.08, w:3.9, h:0.3, fontSize:15, fontFace:"Calibri", color:item.color, bold:true, margin:0 });
  s7.addText(item.items.map((t,j) => ({
    text:t, options:{ bullet:true, breakLine:j<item.items.length-1, fontSize:11, color:C.text }
  })), { x:x+0.25, y:y+0.42, w:3.85, h:1.4, fontFace:"Calibri", margin:0 });
});

// Save
const outPath = process.argv[2] || "G6_BCM_Summary_v2.pptx";
pres.writeFile({ fileName: outPath }).then(() => { console.log("Created: " + outPath); });
