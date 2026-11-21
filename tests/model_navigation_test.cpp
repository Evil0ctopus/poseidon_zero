#include "base_model.h"

#include <iostream>

int main() {
    model::BaseModel model;
    if (model.screensaver_due(model::kScreensaverIdleMs)) {
        std::cerr << "Screensaver should be disabled by default\n";
        return 1;
    }
    model.set_screensaver_enabled(true);
    if (model.screensaver_due(model::kScreensaverIdleMs - 1) ||
        !model.screensaver_due(model::kScreensaverIdleMs)) {
        std::cerr << "Screensaver should start only after the inactivity threshold\n";
        return 1;
    }
    model.set_screensaver_palette(2);
    model.set_screensaver_palette(model::kScreensaverPaletteCount);
    if (model.screensaver_palette() != 2) {
        std::cerr << "Invalid screensaver palettes should not replace the selection\n";
        return 1;
    }
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

    model.set_current_page(model::AppPage::Tools);
    model.set_tool_selection(model::kToolsPageItemCount - 1);
    if (model.tool_selection() != model::kToolsPageItemCount - 1) {
        std::cerr << "Tools selection should reach the final utility\n";
        return 1;
    }

    model.move_tool_selection(1);
    if (model.tool_selection() != model::kToolsPageItemCount - 1) {
        std::cerr << "Tools selection should clamp at the final utility\n";
        return 1;
    }

    model.set_theme_id(model::ThemeId::Matrix);
    if (model.theme_id() != model::ThemeId::Matrix || !model.dark_mode()) {
        std::cerr << "Dark theme selection should update theme state\n";
        return 1;
    }

    model.set_theme_id(model::ThemeId::EInk);
    if (model.theme_id() != model::ThemeId::EInk || model.dark_mode()) {
        std::cerr << "E-ink theme should use the light appearance\n";
        return 1;
    }

    model.set_theme_id(static_cast<model::ThemeId>(model::kThemeCount));
    if (model.theme_id() != model::ThemeId::ZeroLight) {
        std::cerr << "Invalid theme selection should return to the default palette\n";
        return 1;
    }

    model.set_current_page(model::AppPage::Network);
    if (model.tool_selection() != model::kStandardToolItemCount - 1) {
        std::cerr << "Switching to a standard page should clamp its selection\n";
        return 1;
    }

    model.set_current_page(model::AppPage::Butter);
    model.set_tool_selection(model::kSystemToolItemCount - 1);
    if (model.tool_selection() != model::kSystemToolItemCount - 1) {
        std::cerr << "System selection should reach the final settings item\n";
        return 1;
    }

    model.move_tool_selection(1);
    if (model.tool_selection() != model::kSystemToolItemCount - 1) {
        std::cerr << "System selection should clamp at the final settings item\n";
        return 1;
    }

    model.set_current_page(model::AppPage::Network);
    if (model.tool_selection() != model::kStandardToolItemCount - 1) {
        std::cerr << "Switching from System should clamp to the standard page range\n";
        return 1;
    }

    return 0;
}
