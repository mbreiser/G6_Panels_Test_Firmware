#ifndef CONSTANTS_H
#define CONSTANTS_H
#include <Arduino.h>

constexpr uint8_t  PANEL_SIZE = 20;
constexpr uint8_t  NUM_COLOR = 4;

extern const uint8_t CS_PIN;
extern const uint8_t COL_PIN[PANEL_SIZE];
extern const uint8_t ROW_PIN[PANEL_SIZE];

#endif
