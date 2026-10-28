#include "view/search_header.hpp"
#include "view/svg_image.hpp"
#include "utils/theme.hpp"

using namespace brls::literals;

SearchHeader::SearchHeader() {
    this->setAxis(brls::Axis::ROW);
    this->setHeight(HEIGHT);
    this->setWidth(WIDTH);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setPadding(0, 20, 0, 20);
    this->setCornerRadius(HEIGHT / 2.0f);  // pill shape
    this->setBackgroundColor(theme::BG_RAISED);
    this->setFocusable(true);

    auto* icon = new SVGImage();
    icon->setImageFromSVGRes("icon/ico-search.svg");
    icon->setWidth(17);
    icon->setHeight(17);
    icon->setMarginRight(10);
    this->addView(icon);

    this->label = new brls::Label();
    this->label->setText("Search movies & series");
    this->label->setFontSize(16);
    this->label->setTextColor(theme::TEXT_SECONDARY);
    this->addView(this->label);

    this->registerClickAction([this](brls::View*) {
        this->activate();
        return true;
    });
    this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
}

void SearchHeader::activate() {
    brls::Application::getImeManager()->openForText(
        [this](const std::string& text) {
            if (text.empty()) return;
            this->setQuery(text);
            if (this->onSearch) this->onSearch(text);
        },
        "Search movies & series", "", 64, this->query, 0);
}

void SearchHeader::setQuery(const std::string& text) {
    this->query = text;
    this->label->setText(text);
    this->label->setTextColor(theme::TEXT_PRIMARY);
}

void SearchHeader::setCollapsed(bool value) {
    if (this->collapsed == value) return;
    this->collapsed = value;

    this->heightAnim.stop();
    this->heightAnim.reset(value ? HEIGHT : 0.0f);
    this->heightAnim.addStep(value ? 0.0f : HEIGHT, 180, brls::EasingFunction::quadraticOut);
    this->heightAnim.setTickCallback([this] {
        this->setHeight(this->heightAnim.getValue());
        // Fade with it so the collapse reads as a dismissal, not a squish.
        this->setAlpha(this->heightAnim.getValue() / HEIGHT);
    });
    this->heightAnim.start();
}
