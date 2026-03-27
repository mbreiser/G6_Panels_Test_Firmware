const pptxgen = require("pptxgenjs");

let pres = new pptxgen();
pres.layout = "LAYOUT_16x9";
pres.author = "Reiser Lab";
pres.title = "G6 Panel Hardware — Issues & Next Steps";

const C = {
  bg: "0D1117", bgCard: "161B22", accent: "58A6FF", accent2: "3FB950",
  accent3: "F78166", text: "E6EDF3", textMuted: "8B949E", border: "30363D", white: "FFFFFF",
};

// ═══════════════════════════════════════════════════════════════
// SLIDE 1: Flipped LEDs in the Janelia Batch
// ═══════════════════════════════════════════════════════════════
let s1 = pres.addSlide();
s1.background = { color: C.bg };
s1.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent3} });
s1.addText("Flipped LED Polarity — Janelia Batch", { x:0.6, y:0.2, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Left: the issue
s1.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:0.9, w:4.4, h:2.2, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s1.addText("What's Different", { x:0.7, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent3, bold:true, margin:0 });
s1.addText([
  { text:"iorodeo panels: ", options:{bold:true, fontSize:12} },
  { text:"Col HIGH + Row LOW = ON\n", options:{breakLine:true, fontSize:12} },
  { text:"Janelia batch: ", options:{bold:true, fontSize:12, color:C.accent3} },
  { text:"Col LOW + Row HIGH = ON\n\n", options:{breakLine:true, fontSize:12, color:C.accent3} },
  { text:"Anode/cathode are swapped → current\nflows in the opposite direction through\nthe LED matrix", options:{fontSize:11} },
], { x:0.7, y:1.35, w:4.0, h:1.6, fontFace:"Calibri", color:C.text, margin:0 });

// Right: driver analysis
s1.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.2, y:0.9, w:4.4, h:2.2, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s1.addText("UCC27517 Driver Impedance", { x:5.4, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
s1.addText([
  { text:"Push-pull output: can source AND sink\n", options:{breakLine:true, fontSize:12} },
  { text:"R_OH (source) ≠ R_OL (sink)\n", options:{breakLine:true, fontSize:12, fontFace:"Consolas"} },
  { text:"→ slight asymmetry in drive strength\n\n", options:{breakLine:true, fontSize:11} },
  { text:"40 drivers total (20 col + 20 row)\n", options:{breakLine:true, fontSize:12, bold:true} },
  { text:"Fully symmetric placement — neither\npolarity is disadvantaged", options:{fontSize:11} },
], { x:5.4, y:1.35, w:4.0, h:1.6, fontFace:"Calibri", color:C.text, margin:0 });

// Bottom: impact summary
s1.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:3.35, w:9.1, h:1.6, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s1.addText("Impact Assessment", { x:0.7, y:3.45, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s1.addText([
  { text:"Software: ", options:{bold:true, fontSize:12} },
  { text:"Trivial — one XOR per column word + row sense flip. Already implemented.\n", options:{breakLine:true, fontSize:11} },
  { text:"Voltage droop: ", options:{bold:true, fontSize:12} },
  { text:"Active row carries 20× single-LED current. With reversed polarity, row pin SOURCES\n", options:{breakLine:true, fontSize:11} },
  { text:"(vs sinks in normal). UCC27517 R_OH may be slightly higher than R_OL → marginally more droop.\n", options:{breakLine:true, fontSize:11} },
  { text:"Practical effect expected to be small given UCC27517's low output impedance (~1Ω typ).", options:{fontSize:11, color:C.textMuted} },
], { x:0.7, y:3.8, w:8.7, h:1.0, fontFace:"Calibri", color:C.text, margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 2: Route EINT to MCU
// ═══════════════════════════════════════════════════════════════
let s2 = pres.addSlide();
s2.background = { color: C.bg };
s2.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
s2.addText("Route EINT to MCU", { x:0.6, y:0.2, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Current state
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:0.9, w:4.4, h:1.5, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s2.addText("Current State", { x:0.7, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent3, bold:true, margin:0 });
s2.addText([
  { text:"EINT on headers J3-1 and J5-1\n", options:{breakLine:true, fontSize:12} },
  { text:"NOT routed to any MCU GPIO\n", options:{breakLine:true, fontSize:12, bold:true, color:C.accent3} },
  { text:"GP45, GP46, GP47 are unconnected", options:{fontSize:12} },
], { x:0.7, y:1.35, w:4.0, h:0.9, fontFace:"Calibri", color:C.text, margin:0 });

// Fix
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.2, y:0.9, w:4.4, h:1.5, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s2.addText("Fix: Add One PCB Trace", { x:5.4, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s2.addText([
  { text:"Route EINT header → GP45\n", options:{breakLine:true, fontSize:12, bold:true} },
  { text:"No rerouting of existing signals\n", options:{breakLine:true, fontSize:12} },
  { text:"Break out GP46-47 to test points\n", options:{breakLine:true, fontSize:11} },
  { text:"Arena controller: zero changes", options:{fontSize:11, color:C.textMuted} },
], { x:5.4, y:1.35, w:4.0, h:0.9, fontFace:"Calibri", color:C.text, margin:0 });

// What it enables
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:2.65, w:9.1, h:2.3, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s2.addText("What This Enables", { x:0.7, y:2.75, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
s2.addText([
  { text:"GP45 — EINT (external trigger input)\n", options:{breakLine:true, fontSize:13, bold:true, fontFace:"Consolas"} },
  { text:"  2P microscope sync: 8 kHz trigger from scan mirror\n", options:{breakLine:true, fontSize:11} },
  { text:"  Already tested with bodge wire: zero jitter confirmed\n\n", options:{breakLine:true, fontSize:11, color:C.accent2} },
  { text:"GP46 — Sync output (burst active)\n", options:{breakLine:true, fontSize:13, bold:true, fontFace:"Consolas"} },
  { text:"  HIGH during ~10 µs scan burst → oscilloscope/Saleae timing verification\n\n", options:{breakLine:true, fontSize:11} },
  { text:"GP47 — Frame-sync / spare\n", options:{breakLine:true, fontSize:13, bold:true, fontFace:"Consolas"} },
  { text:"  Pulse per frame (400 Hz) or general debug", options:{fontSize:11} },
], { x:0.7, y:3.1, w:8.7, h:1.7, fontFace:"Calibri", color:C.text, margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 3: Board Size
// ═══════════════════════════════════════════════════════════════
let s3 = pres.addSlide();
s3.background = { color: C.bg };
s3.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent2} });
s3.addText("Board Size — 45.0 × 45.0 mm", { x:0.6, y:0.2, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Board outline from KiCad
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:0.9, w:4.4, h:2.0, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s3.addText("From KiCad PCB", { x:0.7, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s3.addText([
  { text:"Edge.Cuts: (50,50) → (95,95)\n", options:{breakLine:true, fontSize:12, fontFace:"Consolas"} },
  { text:"Width:  45.0 mm\n", options:{breakLine:true, fontSize:14, bold:true} },
  { text:"Height: 45.0 mm\n", options:{breakLine:true, fontSize:14, bold:true} },
  { text:"Square outline", options:{fontSize:12, color:C.textMuted} },
], { x:0.7, y:1.35, w:4.0, h:1.4, fontFace:"Calibri", color:C.text, margin:0 });

// What's on it
s3.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.2, y:0.9, w:4.4, h:2.0, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s3.addText("Components", { x:5.4, y:1.0, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
s3.addText([
  { text:"RP2354 MCU (RP2350 + 8MB PSRAM)\n", options:{breakLine:true, fontSize:12} },
  { text:"40× UCC27517 gate drivers\n", options:{breakLine:true, fontSize:12} },
  { text:"20×20 passive LED matrix\n", options:{breakLine:true, fontSize:12} },
  { text:"SPI + EINT headers\n", options:{breakLine:true, fontSize:12} },
  { text:"2MB flash", options:{fontSize:12} },
], { x:5.4, y:1.35, w:4.0, h:1.4, fontFace:"Calibri", color:C.text, margin:0 });

// ═══════════════════════════════════════════════════════════════
// SLIDE 4: Optimize GPIO Usage for PIO
// ═══════════════════════════════════════════════════════════════
let s4 = pres.addSlide();
s4.background = { color: C.bg };
s4.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
s4.addText("Optimize GPIO for PIO — Contiguous Rows", { x:0.6, y:0.2, w:9, h:0.5, fontSize:28, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Current layout
s4.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:0.85, w:4.4, h:2.2, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s4.addText("Current Layout", { x:0.7, y:0.95, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent3, bold:true, margin:0 });
s4.addText([
  { text:"GP1-20   Columns (contiguous) ✓\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP21-31  Rows lower (11 pins)\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP32-35  SPI0 ← GAP!\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas", color:C.accent3, bold:true} },
  { text:"GP36-44  Rows upper (9 pins)\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP45-47  Unconnected\n\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas", color:C.textMuted} },
  { text:"⚠ SPI pins break row range\n", options:{breakLine:true, fontSize:11, color:C.accent3} },
  { text:"  Cannot use PIO for rows", options:{fontSize:11, color:C.accent3} },
], { x:0.7, y:1.3, w:4.0, h:1.6, fontFace:"Calibri", color:C.text, margin:0 });

// Proposed layout
s4.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.2, y:0.85, w:4.4, h:2.2, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s4.addText("Proposed (Option C)", { x:5.4, y:0.95, w:4.0, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s4.addText([
  { text:"GP1-20   Columns  → PIO0\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP21-40  Rows     → PIO1 ✓\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas", color:C.accent2, bold:true} },
  { text:"GP41     EINT trigger\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP42-43  Sync + spare\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"GP44-47  SPI1 (hardware)\n\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas"} },
  { text:"✓ Both ranges contiguous\n", options:{breakLine:true, fontSize:11, color:C.accent2} },
  { text:"  Enables fully autonomous PIO scan", options:{fontSize:11, color:C.accent2} },
], { x:5.4, y:1.3, w:4.0, h:1.6, fontFace:"Calibri", color:C.text, margin:0 });

// What it enables
s4.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:3.3, w:9.1, h:1.65, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s4.addText("What Contiguous Rows Enable", { x:0.7, y:3.4, w:8.7, h:0.3, fontSize:15, fontFace:"Calibri", color:C.accent, bold:true, margin:0 });
s4.addText([
  { text:"PIO1 out pins, 20", options:{fontSize:12, fontFace:"Consolas", bold:true} },
  { text:" for rows — entire scan burst can be PIO-only (zero CPU in loop)\n", options:{breakLine:true, fontSize:11} },
  { text:"PIO1 wait pin", options:{fontSize:12, fontFace:"Consolas", bold:true} },
  { text:" on GP41 — hardware trigger with zero CPU latency\n", options:{breakLine:true, fontSize:11} },
  { text:"No funcsel conflicts — SPI1 (GP44-47) completely outside PIO row range\n", options:{breakLine:true, fontSize:11} },
  { text:"Firmware: ", options:{bold:true, fontSize:11} },
  { text:"spi_init(spi0,...) → spi_init(spi1,...) + update pin constants. One-line change.", options:{fontSize:11, color:C.textMuted} },
], { x:0.7, y:3.75, w:8.7, h:1.1, fontFace:"Calibri", color:C.text, margin:0 });

// Save
const outPath = process.argv[2] || "G6_Hardware_Issues.pptx";
pres.writeFile({ fileName: outPath }).then(() => {
  console.log("Created: " + outPath);
});
