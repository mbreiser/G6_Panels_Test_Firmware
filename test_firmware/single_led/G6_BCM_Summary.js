const pptxgen = require("pptxgenjs");

let pres = new pptxgen();
pres.layout = "LAYOUT_16x9";
pres.author = "Reiser Lab";
pres.title = "G6 20x20 LED Panel - BCM Timing Characterization";

// Color palette: Midnight Executive
const C = {
  bg: "0D1117",
  bgCard: "161B22",
  accent: "58A6FF",
  accent2: "3FB950",
  accent3: "F78166",
  text: "E6EDF3",
  textMuted: "8B949E",
  border: "30363D",
  white: "FFFFFF",
};

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 1: Title
// ═══════════════════════════════════════════════════════════════════════════
let s1 = pres.addSlide();
s1.background = { color: C.bg };

// Accent bar at top
s1.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent },
});

// Main title
s1.addText("G6 20×20 LED Panel", {
  x: 0.8, y: 1.2, w: 8.4, h: 1.0,
  fontSize: 42, fontFace: "Arial Black", color: C.white, bold: true,
  margin: 0,
});

s1.addText("Timing & Grayscale Characterization", {
  x: 0.8, y: 2.1, w: 8.4, h: 0.7,
  fontSize: 28, fontFace: "Calibri", color: C.accent,
  margin: 0,
});

// Subtitle chips
const chips = [
  "RP2354 MCU (150 MHz)",
  "PIO-driven BCM",
  "Zero-jitter architecture",
];
chips.forEach((txt, i) => {
  s1.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 0.8 + i * 2.9, y: 3.2, w: 2.7, h: 0.45,
    fill: { color: C.bgCard },
    line: { color: C.border, width: 1 },
    rectRadius: 0.1,
  });
  s1.addText(txt, {
    x: 0.8 + i * 2.9, y: 3.2, w: 2.7, h: 0.45,
    fontSize: 11, fontFace: "Calibri", color: C.textMuted,
    align: "center", valign: "middle", margin: 0,
  });
});

// Bottom attribution
s1.addText("Reiser Lab — March 2026", {
  x: 0.8, y: 4.8, w: 8.4, h: 0.4,
  fontSize: 14, fontFace: "Calibri", color: C.textMuted,
  margin: 0,
});

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 2: Zero-Jitter BCM Architecture
// ═══════════════════════════════════════════════════════════════════════════
let s2 = pres.addSlide();
s2.background = { color: C.bg };
s2.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent },
});

s2.addText("Zero-Jitter BCM Architecture", {
  x: 0.6, y: 0.25, w: 9, h: 0.55,
  fontSize: 28, fontFace: "Arial Black", color: C.white, bold: true, margin: 0,
});

// Big stat callouts (left side)
const stats = [
  { val: "0.000 µs", label: "Jitter", sub: "640k measurements" },
  { val: "9.5 µs", label: "Burst time", sub: "5.5 µs margin in 15 µs window" },
  { val: "400 Hz", label: "Frame rate", sub: "8 kHz trigger / 20 rows" },
];
stats.forEach((st, i) => {
  const y = 1.1 + i * 1.35;
  s2.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 0.6, y: y, w: 4.2, h: 1.15,
    fill: { color: C.bgCard },
    line: { color: C.border, width: 1 },
    rectRadius: 0.1,
  });
  s2.addText(st.val, {
    x: 0.8, y: y + 0.08, w: 3.8, h: 0.55,
    fontSize: 32, fontFace: "Consolas", color: C.accent, bold: true, margin: 0,
  });
  s2.addText(st.label, {
    x: 0.8, y: y + 0.6, w: 1.8, h: 0.3,
    fontSize: 14, fontFace: "Calibri", color: C.white, bold: true, margin: 0,
  });
  s2.addText(st.sub, {
    x: 2.6, y: y + 0.6, w: 2.2, h: 0.3,
    fontSize: 11, fontFace: "Calibri", color: C.textMuted, margin: 0,
  });
});

// Architecture details (right side)
s2.addText("Architecture", {
  x: 5.2, y: 1.05, w: 4.4, h: 0.35,
  fontSize: 16, fontFace: "Calibri", color: C.accent2, bold: true, margin: 0,
});

const archItems = [
  "4-bit BCM: 16 intensity levels",
  "PIO drives 20 columns simultaneously",
  "CPU manages rows via gpio_set_mask64",
  "Single row per 8 kHz trigger",
  "T = 0.5 µs base time (configurable)",
];
s2.addText(
  archItems.map((t, i) => ({
    text: t,
    options: {
      bullet: true, breakLine: i < archItems.length - 1,
      fontSize: 13, color: C.text,
    },
  })),
  { x: 5.2, y: 1.5, w: 4.4, h: 1.5, fontFace: "Calibri", margin: 0 }
);

s2.addText("Zero-Jitter Recipe (all required)", {
  x: 5.2, y: 3.2, w: 4.4, h: 0.35,
  fontSize: 16, fontFace: "Calibri", color: C.accent3, bold: true, margin: 0,
});

const recipe = [
  "__not_in_flash_func + noinline",
  "noInterrupts() during burst",
  "multicore_lockout (Core 1 paused)",
  "100-trigger warm-up",
];
s2.addText(
  recipe.map((t, i) => ({
    text: t,
    options: {
      bullet: { code: "2713" }, breakLine: i < recipe.length - 1,
      fontSize: 12, fontFace: "Consolas", color: C.text,
    },
  })),
  { x: 5.2, y: 3.6, w: 4.4, h: 1.4, margin: 0 }
);

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 3: External Trigger & Optical Validation
// ═══════════════════════════════════════════════════════════════════════════
let s3 = pres.addSlide();
s3.background = { color: C.bg };
s3.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent2 },
});

s3.addText("External Trigger & Optical Validation", {
  x: 0.6, y: 0.25, w: 9, h: 0.55,
  fontSize: 28, fontFace: "Arial Black", color: C.white, bold: true, margin: 0,
});

// BCM validation waveform description
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 0.6, y: 1.0, w: 4.3, h: 3.5,
  fill: { color: C.bgCard },
  line: { color: C.border, width: 1 },
  rectRadius: 0.1,
});

s3.addText("Saleae Pulse-Triggered Average", {
  x: 0.8, y: 1.1, w: 4.0, h: 0.35,
  fontSize: 15, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

s3.addText(
  [
    { text: "499 pulses averaged at 50 MHz\n", options: { breakLine: true, fontSize: 12 } },
    { text: "4 BCM bit-planes clearly resolved:\n", options: { breakLine: true, fontSize: 12, bold: true } },
    { text: "  B0 = 0.5 µs  (weight 1)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "  B1 = 1.0 µs  (weight 2)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "  B2 = 2.0 µs  (weight 4)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "  B3 = 4.0 µs  (weight 8)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "\n~0.4 µs PIO overhead gaps visible\nbetween each bit-plane", options: { fontSize: 11 } },
  ],
  { x: 0.8, y: 1.5, w: 3.9, h: 2.8, fontFace: "Calibri", color: C.text, margin: 0 }
);

// Right column: validation results
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 5.2, y: 1.0, w: 4.3, h: 1.5,
  fill: { color: C.bgCard },
  line: { color: C.border, width: 1 },
  rectRadius: 0.1,
});

s3.addText("External Trigger (GP45)", {
  x: 5.4, y: 1.1, w: 4.0, h: 0.3,
  fontSize: 15, fontFace: "Calibri", color: C.accent2, bold: true, margin: 0,
});

s3.addText(
  [
    { text: "Real 8 kHz waveform generator\n", options: { breakLine: true } },
    { text: "BCMBURST 10k: ", options: { bold: true } },
    { text: "0.000 µs jitter\n", options: { color: C.accent, bold: true, breakLine: true } },
    { text: "GPIO edge detection (polling)\n", options: { breakLine: true } },
    { text: "Per-burst noInterrupts()", options: {} },
  ],
  { x: 5.4, y: 1.45, w: 3.9, h: 1.0, fontSize: 12, fontFace: "Calibri", color: C.text, margin: 0 }
);

// Spectrometer
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 5.2, y: 2.7, w: 4.3, h: 1.8,
  fill: { color: C.bgCard },
  line: { color: C.border, width: 1 },
  rectRadius: 0.1,
});

s3.addText("Spectrometer + Linearity", {
  x: 5.4, y: 2.8, w: 4.0, h: 0.3,
  fontSize: 15, fontFace: "Calibri", color: C.accent3, bold: true, margin: 0,
});

s3.addText(
  [
    { text: "Ocean Insight Flame X\n", options: { breakLine: true, bold: true } },
    { text: "LED peak: 570.8 nm\n", options: { breakLine: true } },
    { text: "Integration-based averaging\n", options: { breakLine: true } },
    { text: "BCMWEIGHTS: 6.67 ns resolution\n", options: { breakLine: true, fontFace: "Consolas" } },
    { text: "Enables per-channel linearization", options: {} },
  ],
  { x: 5.4, y: 3.15, w: 3.9, h: 1.3, fontSize: 12, fontFace: "Calibri", color: C.text, margin: 0 }
);

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 4: Next Steps
// ═══════════════════════════════════════════════════════════════════════════
let s4 = pres.addSlide();
s4.background = { color: C.bg };
s4.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent3 },
});

s4.addText("Next Steps", {
  x: 0.6, y: 0.25, w: 9, h: 0.55,
  fontSize: 28, fontFace: "Arial Black", color: C.white, bold: true, margin: 0,
});

// Two columns of cards
const nextItems = [
  {
    title: "Optical Linearity",
    color: C.accent,
    items: [
      "Spectrometer sweep: 16 BCM levels",
      "100 ms integration, full spectrum per level",
      "1s OFF gaps for clean segmentation",
      "Sweep T values for optimal linearity",
    ],
  },
  {
    title: "Calibration",
    color: C.accent2,
    items: [
      "BCMWEIGHTS: custom bit-plane durations",
      "6.67 ns resolution (1 CPU cycle)",
      "Measured LED response → correction LUT",
      "Non-power-of-2 weights (e.g. 1.2, 2.2, 4.1, 8)",
    ],
  },
  {
    title: "PCB Redesign",
    color: C.accent3,
    items: [
      "Option B: add EINT trace to GP45 (minimal)",
      "Option C: SPI1 on GP44-47, contiguous rows",
      "40 UCC27517 drivers (symmetric, both sides)",
      "GP45-47 for trigger + sync outputs",
    ],
  },
  {
    title: "Production Path",
    color: C.textMuted,
    items: [
      "Per-pixel pattern loading via SPI",
      "Frame double-buffering at 400 Hz",
      "Core 1 lockout (USB debug only)",
      "PIO wait-pin for hardware trigger",
    ],
  },
];

nextItems.forEach((item, i) => {
  const col = i % 2;
  const row = Math.floor(i / 2);
  const x = 0.6 + col * 4.6;
  const y = 1.0 + row * 2.15;

  s4.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: x, y: y, w: 4.3, h: 1.95,
    fill: { color: C.bgCard },
    line: { color: C.border, width: 1 },
    rectRadius: 0.1,
  });

  // Color accent bar on left
  s4.addShape(pres.shapes.RECTANGLE, {
    x: x, y: y, w: 0.06, h: 1.95,
    fill: { color: item.color },
  });

  s4.addText(item.title, {
    x: x + 0.25, y: y + 0.08, w: 3.9, h: 0.35,
    fontSize: 16, fontFace: "Calibri", color: item.color, bold: true, margin: 0,
  });

  s4.addText(
    item.items.map((t, j) => ({
      text: t,
      options: {
        bullet: true, breakLine: j < item.items.length - 1,
        fontSize: 11, color: C.text,
      },
    })),
    { x: x + 0.25, y: y + 0.45, w: 3.85, h: 1.4, fontFace: "Calibri", margin: 0 }
  );
});

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 5: BCM Waveform Validation
// ═══════════════════════════════════════════════════════════════════════════
let s5 = pres.addSlide();
s5.background = { color: C.bg };
s5.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent },
});

s5.addText("BCM Waveform — Pulse-Triggered Average", {
  x: 0.6, y: 0.25, w: 9, h: 0.55,
  fontSize: 28, fontFace: "Arial Black", color: C.white, bold: true, margin: 0,
});

// Embedded waveform image
s5.addImage({
  path: "bcm_pulse_average.png",
  x: 0.5, y: 1.0, w: 6.0, h: 3.5,
});

// Annotations (right side)
s5.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 6.8, y: 1.0, w: 2.8, h: 3.5,
  fill: { color: C.bgCard },
  line: { color: C.border, width: 1 },
  rectRadius: 0.1,
});

s5.addText("Key Observations", {
  x: 7.0, y: 1.1, w: 2.5, h: 0.3,
  fontSize: 14, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

s5.addText(
  [
    { text: "499 pulses averaged\n", options: { breakLine: true, bold: true, fontSize: 12 } },
    { text: "50 MHz analog capture\n\n", options: { breakLine: true, fontSize: 11 } },
    { text: "4 bit-planes resolved:\n", options: { breakLine: true, bold: true, fontSize: 12 } },
    { text: "B0: 0.5 µs (1T)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "B1: 1.0 µs (2T)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "B2: 2.0 µs (4T)\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "B3: 4.0 µs (8T)\n\n", options: { breakLine: true, fontSize: 11, fontFace: "Consolas" } },
    { text: "~0.4 µs gaps between\nbit-planes (PIO overhead)\n\n", options: { breakLine: true, fontSize: 11 } },
    { text: "~1 µs trigger latency\n", options: { breakLine: true, fontSize: 11 } },
    { text: "(GPIO polling + driver)", options: { fontSize: 10, color: C.textMuted } },
  ],
  { x: 7.0, y: 1.45, w: 2.5, h: 2.8, fontFace: "Calibri", color: C.text, margin: 0 }
);

s5.addText("Photodiode signal (Saleae Logic Pro 8 analog input)", {
  x: 0.5, y: 4.6, w: 6.0, h: 0.3,
  fontSize: 10, fontFace: "Calibri", color: C.textMuted, italic: true, margin: 0,
});

// ═══════════════════════════════════════════════════════════════════════════
// SLIDE 6: Timing Budget
// ═══════════════════════════════════════════════════════════════════════════
let s6 = pres.addSlide();
s6.background = { color: C.bg };
s6.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.06, fill: { color: C.accent2 },
});

s6.addText("BCM Burst Timing Budget (4-bit, 8 kHz)", {
  x: 0.6, y: 0.25, w: 9, h: 0.55,
  fontSize: 28, fontFace: "Arial Black", color: C.white, bold: true, margin: 0,
});

// Timing table
const tableData = [
  ["T (µs)", "Burst (µs)", "Jitter (µs)", "Outliers", "Fits 15 µs?", "Margin"],
  ["0.25", "5.727", "0.000", "0 / 160k", "YES", "9.3 µs"],
  ["0.50", "9.460", "0.000", "0 / 160k", "YES", "5.5 µs"],
  ["0.75", "13.227", "0.000", "0 / 160k", "YES (barely)", "1.8 µs"],
  ["1.00", "16.960", "0.000", "0 / 160k", "NO", "-2.0 µs"],
];

s6.addTable(tableData, {
  x: 0.6, y: 1.1, w: 8.8,
  rowH: [0.4, 0.38, 0.38, 0.38, 0.38],
  colW: [1.0, 1.3, 1.3, 1.5, 1.8, 1.9],
  fontSize: 12,
  fontFace: "Consolas",
  color: C.text,
  border: { type: "solid", pt: 0.5, color: C.border },
  autoPage: false,
  rowOpts: [
    { fill: { color: C.accent }, color: C.bg, bold: true, fontSize: 11, fontFace: "Calibri" },
    {},
    { fill: { color: "1A2B1A" } },  // highlight recommended row
    {},
    {},
  ],
});

// Highlight T=0.50 row
s6.addShape(pres.shapes.RECTANGLE, {
  x: 0.6, y: 1.88, w: 8.8, h: 0.38,
  fill: { color: C.accent2, transparency: 85 },
});

s6.addText("Recommended: T = 0.50 µs", {
  x: 0.6, y: 1.88, w: 3, h: 0.38,
  fontSize: 11, fontFace: "Calibri", color: C.accent2, bold: true,
  valign: "middle", margin: [0, 0, 0, 10],
});

// Timeline diagram
s6.addText("8 kHz Trigger Period (125 µs)", {
  x: 0.6, y: 3.4, w: 8.8, h: 0.3,
  fontSize: 14, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

// Timeline bar
const tlY = 3.9;
const tlW = 8.8;
const burstFrac = 9.5 / 125;
const idleFrac = 1 - burstFrac;

// Full period bar
s6.addShape(pres.shapes.RECTANGLE, {
  x: 0.6, y: tlY, w: tlW, h: 0.5,
  fill: { color: C.bgCard },
  line: { color: C.border, width: 1 },
});

// Burst portion
s6.addShape(pres.shapes.RECTANGLE, {
  x: 0.6, y: tlY, w: tlW * burstFrac, h: 0.5,
  fill: { color: C.accent },
});
s6.addText("Burst\n9.5 µs", {
  x: 0.6, y: tlY, w: tlW * burstFrac, h: 0.5,
  fontSize: 9, fontFace: "Calibri", color: C.bg, bold: true,
  align: "center", valign: "middle", margin: 0,
});

// Idle portion
s6.addText("Idle: 115.5 µs — SPI, precompute, interrupts OK", {
  x: 0.6 + tlW * burstFrac + 0.1, y: tlY, w: tlW * idleFrac - 0.2, h: 0.5,
  fontSize: 10, fontFace: "Calibri", color: C.textMuted,
  align: "center", valign: "middle", margin: 0,
});

s6.addText("640,000 measurements (4 T values × 16 intensities × 10,000 frames) — zero jitter, zero outliers", {
  x: 0.6, y: 4.7, w: 8.8, h: 0.3,
  fontSize: 11, fontFace: "Calibri", color: C.textMuted, italic: true, margin: 0,
});

// Save
const outPath = process.argv[2] || "G6_BCM_Summary.pptx";
pres.writeFile({ fileName: outPath }).then(() => {
  console.log("Created: " + outPath);
});
