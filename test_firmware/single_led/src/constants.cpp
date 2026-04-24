#include "constants.h"

const uint8_t EINT_PIN = 45;  // External trigger, routed to header on both v0.2.1 and v0.3.1

#if PANEL_REV == 21
// G6 panel v0.2.1
//   Columns:  GP1-GP20 (20 contiguous, PIO0 base = GP1)
//   Rows:     GP21-GP31 + GP36-GP44 (split by SPI0 gap at GP32-35)
//   SPI:      SPI0 on GP32-35 (MOSI/CSn/SCK/MISO)
//   PSRAM CS: GP0 (via XIP_CS1n)
const uint8_t CS_PIN = 33;  // SPI0 CSn
const uint8_t COL_PIN[PANEL_SIZE] =
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
const uint8_t ROW_PIN[PANEL_SIZE] =
    {21,22,23,24,25,26,27,28,29,30,31,36,37,38,39,40,41,42,43,44};

#elif PANEL_REV == 31
// G6 panel v0.3.1
//   Columns:  GP0-GP19 (20 contiguous, PIO0 base = GP0)
//   Rows:     GP20-GP39 (20 contiguous, PIO1 w/ GPIOBASE=16 base = GP20)
//   SPI:      SPI1 on GP40-43 (MOSI/CSn/SCK/MISO)
//   PSRAM CS: GP47 (via XIP_CS1n)
const uint8_t CS_PIN = 41;  // SPI1 CSn
const uint8_t COL_PIN[PANEL_SIZE] =
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
const uint8_t ROW_PIN[PANEL_SIZE] =
    {20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};

#endif
