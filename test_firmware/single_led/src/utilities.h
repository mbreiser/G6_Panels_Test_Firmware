#ifndef UTILITIES_H
#define UTILITIES_H
#include <Arduino.h>
#include "constants.h"

// Lookup tables: layout_to_sch[row][col] = {sch_row, sch_col}
// Must be populated by calling init_index_maps() in setup().
extern uint8_t layout_to_sch_row[PANEL_SIZE][PANEL_SIZE];
extern uint8_t layout_to_sch_col[PANEL_SIZE][PANEL_SIZE];

// Call once from setup() to populate lookup tables
void init_index_maps();

#endif
