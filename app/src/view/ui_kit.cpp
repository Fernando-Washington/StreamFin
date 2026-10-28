#include "view/ui_kit.hpp"
#include "utils/theme.hpp"

#include <fmt/format.h>

namespace ui {

brls::Label* sectionHeader(const std::string& text) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(20);
    label->setMarginTop(16);
    label->setMarginBottom(8);
    label->setTextColor(theme::TEXT_MUTED);
    return label;
}

brls::Label* textRow(const std::string& text) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(18);
    label->setMarginBottom(4);
    return label;
}

Row clickableRow(const std::string& text, std::function<void()> onClick) {
    auto* row = new brls::Box();
    row->setFocusable(true);
    row->setHeight(64);
    row->setPadding(12, 20, 12, 20);
    row->setCornerRadius(6);
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(22);
    row->addView(label);
    row->registerClickAction([onClick](brls::View*) {
        onClick();
        return true;
    });
    return Row{row, label};
}

brls::Box* pillButton(const std::string& text, std::function<void()> onClick) {
    auto* box = new brls::Box();
    box->setFocusable(true);
    box->setHeight(56);
    box->setWidth(180);
    box->setMarginLeft(8);
    box->setMarginRight(8);
    box->setCornerRadius(6);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setAlignItems(brls::AlignItems::CENTER);
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(20);
    box->addView(label);
    box->registerClickAction([onClick](brls::View*) {
        onClick();
        return true;
    });
    return box;
}

brls::Box* storageBar(int64_t usedByApp, int64_t usedByOther, int64_t free) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setMarginBottom(20);

    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setWidthPercentage(100);
    bar->setHeight(20);
    content->addView(bar);

    const int64_t values[] = {usedByApp, usedByOther, free};
    const NVGcolor colors[] = {theme::STORAGE_APP, theme::STORAGE_OTHER, theme::STORAGE_FREE};
    const char* names[] = {"StreamFin", "Other data", "Free"};
    const double total = static_cast<double>(usedByApp) + usedByOther + free;
    for (size_t i = 0; i < 3; ++i) {
        if (values[i] <= 0) continue;
        auto* segment = new brls::Box();
        segment->setGrow(static_cast<float>(values[i] / total));
        segment->setHeight(20);
        segment->setBackgroundColor(colors[i]);
        bar->addView(segment);
    }

    auto* legend = new brls::Box();
    legend->setAxis(brls::Axis::ROW);
    legend->setMarginTop(10);
    content->addView(legend);
    for (size_t i = 0; i < 3; ++i) {
        auto* entry = new brls::Box();
        entry->setAxis(brls::Axis::ROW);
        entry->setAlignItems(brls::AlignItems::CENTER);
        entry->setGrow(1);

        auto* swatch = new brls::Box();
        swatch->setDimensions(14, 14);
        swatch->setBackgroundColor(colors[i]);
        swatch->setMarginRight(8);
        entry->addView(swatch);

        double size = static_cast<double>(values[i]);
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        size_t unit = 0;
        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            ++unit;
        }
        auto* label = new brls::Label();
        label->setText(fmt::format("{} - {:.1f} {}", names[i], size, units[unit]));
        label->setFontSize(18);
        label->setTextColor(theme::TEXT_PRIMARY);
        entry->addView(label);
        legend->addView(entry);
    }
    return content;
}

}  // namespace ui
