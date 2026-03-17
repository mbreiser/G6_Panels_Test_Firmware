#include "utilities.h"

// Plain C lookup tables — NO std::map, NO Eigen, NO static-init objects.
// Populated at runtime by init_index_maps() called from setup().
uint8_t layout_to_sch_row[PANEL_SIZE][PANEL_SIZE];
uint8_t layout_to_sch_col[PANEL_SIZE][PANEL_SIZE];

// Will's original coordinate conversion logic (unchanged)
static void convert_schematic_to_layout(uint8_t si, uint8_t sj,
                                        uint8_t &li, uint8_t &lj) {
    if (sj % NUM_COLOR < NUM_COLOR / 2) {
        if (si < PANEL_SIZE / 2) {
            li = 2 * si;
            lj = sj;
        } else {
            li = 2 * (PANEL_SIZE - (si + 1));
            lj = NUM_COLOR / 2 + sj;
        }
    } else {
        if (si < PANEL_SIZE / 2) {
            li = 2 * si + 1;
            lj = sj - NUM_COLOR / 2;
        } else {
            li = 2 * (PANEL_SIZE - (si + 1)) + 1;
            lj = sj;
        }
    }
}

void init_index_maps() {
    // For every schematic (row,col) compute the layout (row,col),
    // then store the REVERSE mapping: layout → schematic.
    for (uint8_t si = 0; si < PANEL_SIZE; si++) {
        for (uint8_t sj = 0; sj < PANEL_SIZE; sj++) {
            uint8_t li, lj;
            convert_schematic_to_layout(si, sj, li, lj);
            layout_to_sch_row[li][lj] = si;
            layout_to_sch_col[li][lj] = sj;
        }
    }
}
