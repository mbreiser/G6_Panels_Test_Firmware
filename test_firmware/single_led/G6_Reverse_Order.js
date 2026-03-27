const pptxgen = require("pptxgenjs");

let pres = new pptxgen();
pres.layout = "LAYOUT_16x9";
pres.author = "Reiser Lab";
pres.title = "BCM Scan Order: Forward vs Reverse";

const C = {
  bg: "0D1117", bgCard: "161B22", accent: "58A6FF", accent2: "3FB950",
  accent3: "F78166", text: "E6EDF3", textMuted: "8B949E", border: "30363D", white: "FFFFFF",
};

// ═══════════════════════════════════════════════════════════════
// SLIDE 1: Forward vs Reverse Linearity
// ═══════════════════════════════════════════════════════════════
let s1 = pres.addSlide();
s1.background = { color: C.bg };
s1.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent2} });
s1.addText("BCM Bit-Plane Scan Order: Forward vs Reverse", { x:0.5, y:0.2, w:9, h:0.5, fontSize:26, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Linearity plot
s1.addImage({ path:"slide_plots/fwd_vs_rev_linearity.png", x:0.3, y:0.8, w:9.4, h:3.9 });

// Bottom callout
s1.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:0.5, y:4.85, w:9.0, h:0.55, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.08 });
s1.addText([
  { text:"T = 0.7 µs  |  Default weights [1, 2, 4, 8]  |  All 400 LEDs  |  50 ms integration  |  Ocean Insight Flame X", options:{fontSize:10, color:C.textMuted} },
], { x:0.7, y:4.88, w:8.6, h:0.45, fontFace:"Calibri", margin:0, valign:"middle" });

// ═══════════════════════════════════════════════════════════════
// SLIDE 2: Per-Bit-Plane Analysis + Open Questions
// ═══════════════════════════════════════════════════════════════
let s2 = pres.addSlide();
s2.background = { color: C.bg };
s2.addShape(pres.shapes.RECTANGLE, { x:0, y:0, w:10, h:0.06, fill:{color:C.accent} });
s2.addText("Brightness Per Microsecond — Scan Order Effect", { x:0.5, y:0.2, w:9, h:0.5, fontSize:26, fontFace:"Arial Black", color:C.white, bold:true, margin:0 });

// Bit-plane plot
s2.addImage({ path:"slide_plots/fwd_vs_rev_bitplanes.png", x:0.3, y:0.8, w:5.0, h:3.6 });

// Right: key finding
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.5, y:0.8, w:4.2, h:1.8, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s2.addText("Key Finding", { x:5.7, y:0.9, w:3.8, h:0.25, fontSize:14, fontFace:"Calibri", color:C.accent2, bold:true, margin:0 });
s2.addText([
  { text:"First-scanned bit-plane always\nproduces more light per µs\n\n", options:{breakLine:true, fontSize:12, bold:true} },
  { text:"Reverse order (B3 first) gives\nB3 the brightness boost → fixes\nmonotonicity without weight correction\n\n", options:{breakLine:true, fontSize:11} },
  { text:"Forward:  NOT mono, 7.7% err\n", options:{breakLine:true, fontSize:11, fontFace:"Consolas", color:C.accent3} },
  { text:"Reverse:  MONOTONIC, 3.6% err", options:{fontSize:11, fontFace:"Consolas", color:C.accent2} },
], { x:5.7, y:1.2, w:3.8, h:1.3, fontFace:"Calibri", color:C.text, margin:0 });

// Open questions
s2.addShape(pres.shapes.ROUNDED_RECTANGLE, { x:5.5, y:2.8, w:4.2, h:2.05, fill:{color:C.bgCard}, line:{color:C.border,width:1}, rectRadius:0.1 });
s2.addText("Caveats & Open Questions", { x:5.7, y:2.9, w:3.8, h:0.25, fontSize:14, fontFace:"Calibri", color:C.accent3, bold:true, margin:0 });
s2.addText([
  { text:"Measured with all 400 LEDs ON\n", options:{breakLine:true, fontSize:11, bold:true} },
  { text:"→ Need to test single-LED and\n   partial-row patterns to confirm\n   effect doesn't depend on load\n\n", options:{breakLine:true, fontSize:10} },
  { text:"How dependent on LED type?\n", options:{breakLine:true, fontSize:11, bold:true} },
  { text:"→ Janelia batch (reversed polarity)\n   may behave differently than\n   standard iorodeo panels\n\n", options:{breakLine:true, fontSize:10} },
  { text:"→ UCC27517 source/sink asymmetry\n   could interact with scan order", options:{fontSize:10, color:C.textMuted} },
], { x:5.7, y:3.2, w:3.8, h:1.55, fontFace:"Calibri", color:C.text, margin:0 });

const outPath = process.argv[2] || "G6_Reverse_Order.pptx";
pres.writeFile({ fileName: outPath }).then(() => {
  console.log("Created: " + outPath);
});
