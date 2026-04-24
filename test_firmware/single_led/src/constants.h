#ifndef CONSTANTS_H
#define CONSTANTS_H
#include <Arduino.h>

#ifndef PANEL_REV
#error "PANEL_REV not defined. Build with -DPANEL_REV=21 or -DPANEL_REV=31 (set in platformio.ini)."
#endif

#if (PANEL_REV != 21) && (PANEL_REV != 31)
#error "Unsupported PANEL_REV. Only 21 (v0.2.1) and 31 (v0.3.1) are valid."
#endif

constexpr uint8_t  PANEL_SIZE = 20;
constexpr uint8_t  NUM_COLOR = 4;

// LED polarity: both v0.2.1 and v0.3.1 are NORMAL polarity (col HIGH + row LOW = ON).
// Idle state is columns LOW + rows HIGH. This differs from v0.1 Janelia which
// was reversed polarity (col LOW + row HIGH = ON).
constexpr bool COL_ON_LEVEL = true;   // column HIGH = ON
constexpr bool ROW_ON_LEVEL = false;  // row LOW = ON

extern const uint8_t CS_PIN;
extern const uint8_t EINT_PIN;     // External trigger input (GP45, same on both revs)
extern const uint8_t COL_PIN[PANEL_SIZE];
extern const uint8_t ROW_PIN[PANEL_SIZE];

#endif
