const pptxgen = require("pptxgenjs");

let pres = new pptxgen();
pres.layout = "LAYOUT_16x9";
pres.author = "Reiser Lab";
pres.title = "G6 LED Panel — Timing Characterization Summary";

// Color palette — Midnight Executive
const C = {
  navy: "1E2761",
  darkNavy: "141B3D",
  ice: "CADCFC",
  white: "FFFFFF",
  accent: "00D4FF",
  green: "00CC88",
  red: "FF4455",
  gray: "8899AA",
  dimText: "6677AA",
  cardBg: "1A2050",
};

// =========================================================================
// SLIDE 1: Title
// =========================================================================
let s1 = pres.addSlide();
s1.background = { color: C.darkNavy };

// Top accent bar
s1.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.08, fill: { color: C.accent },
});

s1.addText("G6 20×20 LED Panel", {
  x: 0.8, y: 1.0, w: 8.4, h: 0.8,
  fontSize: 40, fontFace: "Arial Black", color: C.white, bold: true,
  margin: 0,
});

s1.addText("Timing Characterization & BCM Grayscale", {
  x: 0.8, y: 1.8, w: 8.4, h: 0.6,
  fontSize: 24, fontFace: "Calibri", color: C.accent,
  margin: 0,
});

s1.addText("Zero-jitter BCM scanning for 2-photon microscope synchronization", {
  x: 0.8, y: 2.6, w: 8.4, h: 0.5,
  fontSize: 16, fontFace: "Calibri", color: C.gray, italic: true,
  margin: 0,
});

// Key stats row
const stats = [
  { val: "0.000 µs", label: "Jitter" },
  { val: "9.5 µs", label: "Burst time" },
  { val: "16", label: "Gray levels" },
  { val: "400 Hz", label: "Frame rate" },
];

stats.forEach((s, i) => {
  const x = 0.8 + i * 2.3;
  s1.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: x, y: 3.6, w: 2.0, h: 1.2,
    fill: { color: C.cardBg }, rectRadius: 0.1,
  });
  s1.addText(s.val, {
    x: x, y: 3.7, w: 2.0, h: 0.6,
    fontSize: 22, fontFace: "Consolas", color: C.accent, bold: true,
    align: "center", margin: 0,
  });
  s1.addText(s.label, {
    x: x, y: 4.3, w: 2.0, h: 0.4,
    fontSize: 13, fontFace: "Calibri", color: C.gray,
    align: "center", margin: 0,
  });
});

s1.addText("Reiser Lab — RP2354 MCU, PIO-driven BCM, External 8 kHz Trigger", {
  x: 0.8, y: 5.1, w: 8.4, h: 0.3,
  fontSize: 11, fontFace: "Calibri", color: C.dimText,
  margin: 0,
});

// =========================================================================
// SLIDE 2: Architecture & Zero-Jitter Recipe
// =========================================================================
let s2 = pres.addSlide();
s2.background = { color: C.white };

s2.addText("Zero-Jitter Architecture", {
  x: 0.6, y: 0.3, w: 9, h: 0.7,
  fontSize: 32, fontFace: "Arial Black", color: C.navy, bold: true,
  margin: 0,
});

s2.addText("How we achieve deterministic LED timing for 2P microscope sync", {
  x: 0.6, y: 0.9, w: 9, h: 0.4,
  fontSize: 14, fontFace: "Calibri", color: C.gray, italic: true,
  margin: 0,
});

// Left column: timing diagram
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 0.5, y: 1.5, w: 4.4, h: 3.5,
  fill: { color: C.darkNavy }, rectRadius: 0.1,
});

s2.addText("8 kHz Trigger Cycle (125 µs)", {
  x: 0.7, y: 1.6, w: 4.0, h: 0.4,
  fontSize: 14, fontFace: "Consolas", color: C.accent, bold: true, margin: 0,
});

const timeline = [
  { text: "TRIGGER EDGE", color: C.red, desc: "External GPIO (GP45)" },
  { text: "noInterrupts()", color: C.accent, desc: "~10 ns" },
  { text: "BCM BURST", color: C.green, desc: "9.5 µs — 4 bit-planes via PIO" },
  { text: "interrupts()", color: C.accent, desc: "~10 ns" },
  { text: "IDLE WINDOW", color: C.gray, desc: "115 µs — SPI, precompute, etc." },
];

timeline.forEach((t, i) => {
  const y = 2.2 + i * 0.55;
  s2.addText(t.text, {
    x: 0.8, y: y, w: 2.0, h: 0.35,
    fontSize: 11, fontFace: "Consolas", color: t.color, bold: true, margin: 0,
  });
  s2.addText(t.desc, {
    x: 2.9, y: y, w: 1.8, h: 0.35,
    fontSize: 11, fontFace: "Calibri", color: C.ice, margin: 0,
  });
});

// Right column: recipe ingredients
s2.addText("Zero-Jitter Recipe (all required)", {
  x: 5.3, y: 1.5, w: 4.2, h: 0.4,
  fontSize: 16, fontFace: "Calibri", color: C.navy, bold: true, margin: 0,
});

const recipe = [
  { num: "1", title: "__not_in_flash_func + noinline", desc: "All timing code in SRAM — prevents XIP cache jitter" },
  { num: "2", title: "noInterrupts() per burst", desc: "Disable Core 0 interrupts during ~10 µs scan" },
  { num: "3", title: "Core 1 multicore lockout", desc: "Pause USB stack — eliminates bus contention" },
  { num: "4", title: "100-trigger warm-up", desc: "Stabilize CPU pipeline before measurement" },
];

recipe.forEach((r, i) => {
  const y = 2.0 + i * 0.85;
  s2.addShape(pres.shapes.OVAL, {
    x: 5.3, y: y + 0.05, w: 0.35, h: 0.35,
    fill: { color: C.accent },
  });
  s2.addText(r.num, {
    x: 5.3, y: y + 0.05, w: 0.35, h: 0.35,
    fontSize: 14, fontFace: "Calibri", color: C.darkNavy, bold: true,
    align: "center", valign: "middle", margin: 0,
  });
  s2.addText(r.title, {
    x: 5.8, y: y, w: 3.7, h: 0.35,
    fontSize: 13, fontFace: "Calibri", color: C.navy, bold: true, margin: 0,
  });
  s2.addText(r.desc, {
    x: 5.8, y: y + 0.35, w: 3.7, h: 0.35,
    fontSize: 11, fontFace: "Calibri", color: C.gray, margin: 0,
  });
});

// Bottom bar
s2.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 5.25, w: 10, h: 0.375,
  fill: { color: C.navy },
});
s2.addText("Result: 0.000 µs jitter across 640,000 measurements — confirmed with real external trigger", {
  x: 0.5, y: 5.27, w: 9, h: 0.35,
  fontSize: 12, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

// =========================================================================
// SLIDE 3: PIO Scanning & BCM Validation
// =========================================================================
let s2b = pres.addSlide();
s2b.background = { color: C.white };

s2b.addText("PIO-Driven BCM Scanning", {
  x: 0.6, y: 0.3, w: 9, h: 0.7,
  fontSize: 32, fontFace: "Arial Black", color: C.navy, bold: true, margin: 0,
});

s2b.addText("Hardware-timed column patterns via RP2350 Programmable I/O", {
  x: 0.6, y: 0.9, w: 9, h: 0.4,
  fontSize: 14, fontFace: "Calibri", color: C.gray, italic: true, margin: 0,
});

// Left: PIO explanation
s2b.addText("How It Works", {
  x: 0.6, y: 1.5, w: 4.4, h: 0.4,
  fontSize: 16, fontFace: "Calibri", color: C.navy, bold: true, margin: 0,
});

const pioSteps = [
  { step: "1", title: "Trigger arrives (GP45)", desc: "External 8 kHz pulse from microscope" },
  { step: "2", title: "CPU activates row GPIO", desc: "gpio_set_mask64() — one of 20 rows" },
  { step: "3", title: "PIO drives 4 bit-planes", desc: "out pins, 20 sets all columns in 1 cycle" },
  { step: "", title: "  B0: 0.5 µs  (weight 1)", desc: "" },
  { step: "", title: "  B1: 1.0 µs  (weight 2)", desc: "" },
  { step: "", title: "  B2: 2.0 µs  (weight 4)", desc: "" },
  { step: "", title: "  B3: 4.0 µs  (weight 8)", desc: "" },
  { step: "4", title: "CPU deactivates row", desc: "Total burst: 9.5 µs — fits 15 µs budget" },
  { step: "5", title: "Idle: 115 µs", desc: "SPI frame loading, precompute, USB" },
];

pioSteps.forEach((s, i) => {
  const y = 1.95 + i * 0.37;
  if (s.step) {
    s2b.addShape(pres.shapes.OVAL, {
      x: 0.6, y: y + 0.02, w: 0.28, h: 0.28,
      fill: { color: C.accent },
    });
    s2b.addText(s.step, {
      x: 0.6, y: y + 0.02, w: 0.28, h: 0.28,
      fontSize: 12, fontFace: "Calibri", color: C.darkNavy, bold: true,
      align: "center", valign: "middle", margin: 0,
    });
  }
  s2b.addText(s.title, {
    x: s.step ? 1.0 : 1.2, y: y, w: 2.4, h: 0.32,
    fontSize: s.step ? 12 : 11, fontFace: s.desc ? "Calibri" : "Consolas",
    color: s.step ? C.navy : C.gray, bold: !!s.step, margin: 0,
  });
  if (s.desc) {
    s2b.addText(s.desc, {
      x: 3.4, y: y, w: 1.8, h: 0.32,
      fontSize: 10, fontFace: "Calibri", color: C.gray, margin: 0,
    });
  }
});

// Right: Validation image
s2b.addText("Saleae Optical Validation", {
  x: 5.3, y: 1.5, w: 4.2, h: 0.4,
  fontSize: 16, fontFace: "Calibri", color: C.navy, bold: true, margin: 0,
});

s2b.addText("499 pulses averaged at 50 MHz — BCM bit-plane structure confirmed", {
  x: 5.3, y: 1.9, w: 4.2, h: 0.3,
  fontSize: 11, fontFace: "Calibri", color: C.gray, margin: 0,
});

// Embed the pulse average image
const fs = require("fs");
const b64Data = fs.readFileSync("bcm_pulse_b64.txt", "utf8").trim();
s2b.addImage({
  data: "image/png;base64," + b64Data,
  x: 5.2, y: 2.3, w: 4.4, h: 2.2,
});

// Labels for bit-planes on the image
s2b.addText("B0  B1   B2      B3", {
  x: 5.8, y: 4.5, w: 3.0, h: 0.3,
  fontSize: 11, fontFace: "Consolas", color: C.accent, margin: 0,
});

s2b.addText("← 9.5 µs burst →", {
  x: 6.0, y: 4.75, w: 2.5, h: 0.25,
  fontSize: 10, fontFace: "Consolas", color: C.gray, margin: 0,
});

// Bottom: key insight
s2b.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 5.25, w: 10, h: 0.375,
  fill: { color: C.navy },
});
s2b.addText("PIO out pins, 20 drives all columns in 1 clock cycle (6.67 ns) — CPU only manages row switching", {
  x: 0.5, y: 5.27, w: 9, h: 0.35,
  fontSize: 12, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

// =========================================================================
// SLIDE 4: BCM Results Table (was slide 3)
// =========================================================================
let s3 = pres.addSlide();
s3.background = { color: C.white };

s3.addText("BCM Burst Timing — All Configurations", {
  x: 0.6, y: 0.3, w: 9, h: 0.7,
  fontSize: 32, fontFace: "Arial Black", color: C.navy, bold: true,
  margin: 0,
});

s3.addText("4-bit BCM, 16 intensities × 4 base times × 10,000 frames at 8 kHz", {
  x: 0.6, y: 0.9, w: 9, h: 0.4,
  fontSize: 14, fontFace: "Calibri", color: C.gray, italic: true,
  margin: 0,
});

// Results table
const tableData = [
  [
    { text: "Base T (µs)", options: { bold: true, color: C.white, fill: { color: C.navy } } },
    { text: "Burst (µs)", options: { bold: true, color: C.white, fill: { color: C.navy } } },
    { text: "Jitter (µs)", options: { bold: true, color: C.white, fill: { color: C.navy } } },
    { text: "Outliers", options: { bold: true, color: C.white, fill: { color: C.navy } } },
    { text: "Fits 15 µs?", options: { bold: true, color: C.white, fill: { color: C.navy } } },
    { text: "Margin", options: { bold: true, color: C.white, fill: { color: C.navy } } },
  ],
  [
    { text: "0.25" }, { text: "5.73" }, { text: "0.000" },
    { text: "0 / 160k" }, { text: "✓ YES", options: { color: "007744" } }, { text: "9.3 µs" },
  ],
  [
    { text: "0.50", options: { bold: true } },
    { text: "9.46", options: { bold: true } },
    { text: "0.000", options: { bold: true, color: "007744" } },
    { text: "0 / 160k" },
    { text: "✓ YES", options: { bold: true, color: "007744" } },
    { text: "5.5 µs", options: { bold: true } },
  ],
  [
    { text: "0.75" }, { text: "13.23" }, { text: "0.000" },
    { text: "0 / 160k" }, { text: "~ YES", options: { color: "AA7700" } }, { text: "1.8 µs" },
  ],
  [
    { text: "1.00" }, { text: "16.96" }, { text: "0.000" },
    { text: "0 / 160k" }, { text: "✗ NO", options: { color: C.red } }, { text: "-2.0 µs" },
  ],
];

s3.addTable(tableData, {
  x: 0.6, y: 1.5, w: 8.8,
  fontSize: 14, fontFace: "Calibri",
  border: { type: "solid", color: "DDDDDD", pt: 0.5 },
  colW: [1.3, 1.3, 1.3, 1.5, 1.3, 1.3],
  rowH: [0.45, 0.45, 0.55, 0.45, 0.45],
  align: "center",
  valign: "middle",
});

// Recommendation callout
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, {
  x: 0.6, y: 4.0, w: 8.8, h: 1.2,
  fill: { color: "F0F7FF" }, rectRadius: 0.1,
  line: { color: C.accent, width: 1.5 },
});

s3.addText([
  { text: "Recommended: T = 0.50 µs\n", options: { fontSize: 16, bold: true, color: C.navy, breakLine: true } },
  { text: "16 intensity levels • 9.5 µs burst • 5.5 µs margin • 400 Hz frame rate • Zero jitter\n", options: { fontSize: 13, color: C.gray, breakLine: true } },
  { text: "Custom bit-plane weights (BCMWEIGHTS) available for linearity calibration — 6.67 ns resolution", options: { fontSize: 12, color: C.dimText } },
], {
  x: 0.9, y: 4.1, w: 8.2, h: 1.0,
  valign: "middle", margin: 0,
});

// =========================================================================
// SLIDE 4: External Validation & Next Steps
// =========================================================================
let s4 = pres.addSlide();
s4.background = { color: C.darkNavy };

// Top accent bar
s4.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 0, w: 10, h: 0.08, fill: { color: C.green },
});

s4.addText("External Validation & Next Steps", {
  x: 0.6, y: 0.3, w: 9, h: 0.7,
  fontSize: 32, fontFace: "Arial Black", color: C.white, bold: true,
  margin: 0,
});

// Left: What's validated
s4.addText("Validated with Real Hardware", {
  x: 0.6, y: 1.2, w: 4.4, h: 0.4,
  fontSize: 16, fontFace: "Calibri", color: C.green, bold: true, margin: 0,
});

const validated = [
  "External 8 kHz trigger (waveform generator → GP45)",
  "Zero jitter with real trigger: 0.000 µs / 10k frames",
  "BCM bit-plane structure (1:2:4:8) resolved on Saleae",
  "16 intensity levels visually confirmed",
  "Saleae Logic Pro 8 automation via Python API",
  "Pulse-triggered averaging (499 pulses, 50 MHz analog)",
];

validated.forEach((v, i) => {
  s4.addText([
    { text: "✓  ", options: { color: C.green, bold: true } },
    { text: v, options: { color: C.ice } },
  ], {
    x: 0.6, y: 1.7 + i * 0.42, w: 4.6, h: 0.38,
    fontSize: 12, fontFace: "Calibri", margin: 0,
  });
});

// Right: Next steps
s4.addText("Next Steps", {
  x: 5.4, y: 1.2, w: 4.2, h: 0.4,
  fontSize: 16, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
});

const next = [
  { title: "Spectrometer linearity", desc: "Ocean Optics with integration averaging — sweep T × intensity for calibration LUT" },
  { title: "LED pin probing", desc: "GP1 on Saleae to measure true trigger-to-LED latency" },
  { title: "PCB redesign", desc: "Route EINT to GP45, add sync/debug pins (GP46-47)" },
  { title: "SPI frame loading", desc: "Arena controller sends pixel data via hardware SPI" },
];

next.forEach((n, i) => {
  const y = 1.7 + i * 0.85;
  s4.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 5.4, y: y, w: 4.2, h: 0.75,
    fill: { color: C.cardBg }, rectRadius: 0.08,
  });
  s4.addText(n.title, {
    x: 5.6, y: y + 0.05, w: 3.8, h: 0.3,
    fontSize: 13, fontFace: "Calibri", color: C.accent, bold: true, margin: 0,
  });
  s4.addText(n.desc, {
    x: 5.6, y: y + 0.35, w: 3.8, h: 0.35,
    fontSize: 11, fontFace: "Calibri", color: C.gray, margin: 0,
  });
});

// Bottom summary
s4.addShape(pres.shapes.RECTANGLE, {
  x: 0, y: 5.05, w: 10, h: 0.575,
  fill: { color: C.navy },
});
s4.addText("Production-ready: 4-bit BCM, T=0.5µs, zero jitter, 400 Hz, fits 15 µs budget with 5.5 µs margin", {
  x: 0.5, y: 5.1, w: 9, h: 0.5,
  fontSize: 14, fontFace: "Calibri", color: C.green, bold: true,
  align: "center", margin: 0,
});

// Write
const outPath = "/Users/reiserm/Documents/GitHub/G6_Panels_Test_Firmware/test_firmware/single_led/G6_Timing_Summary.pptx";
pres.writeFile({ fileName: outPath }).then(() => {
  console.log("Created: " + outPath);
});
