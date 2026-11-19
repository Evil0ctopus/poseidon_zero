#include "base_model.h"

#include <iostream>

int main() {
    model::BaseModel model;
    model.set_tool_selection(7);
    if (model.tool_selection() != 7) {
        std::cerr << "Tool selection should reach the last grid item\n";
        return 1;
    }

    model.move_tool_selection(-2);
    if (model.tool_selection() != 5) {
        std::cerr << "Vertical navigation should preserve the grid column\n";
        return 1;
    }

    model.set_tool_selection(20);
    if (model.tool_selection() != 7) {
        std::cerr << "Tool selection should clamp to the final grid item\n";
        return 1;
    }

    model.set_tool_selection(-1);
    if (model.tool_selection() != 0) {
        std::cerr << "Tool selection should clamp to the first grid item\n";
        return 1;
    }

    return 0;
}
