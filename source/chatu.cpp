#include "chatu.h"
#include "ui.h"

#include <algorithm>
#include <cmath>

void ChatOverlay::addMessage(const std::string& from, const std::string& text) {
    messages_.push_back({ from, text, 0.0f });
    if ((int)messages_.size() > kMaxMessages)
        messages_.erase(messages_.begin());
    displayUntil_ = kMaxAge;
}

void ChatOverlay::openCompose() {
    composing_ = true;
    composeText_.clear();
}

void ChatOverlay::closeCompose() {
    composing_ = false;
}

void ChatOverlay::submit(const std::string& text) {
    composing_ = false;
    if (text.empty()) return;
    composeText_ = text;
}

void ChatOverlay::update(float dt) {
    for (auto& m : messages_) m.age += dt;
    while (!messages_.empty() && messages_.front().age > kMaxAge)
        messages_.erase(messages_.begin());
    if (!messages_.empty())
        displayUntil_ = messages_.back().age + kMaxAge;
}

void ChatOverlay::draw() {
    if (messages_.empty() && !composing_) return;

    float alpha = 1.0f;
    if (!composing_ && !messages_.empty() && messages_.back().age > kMaxAge - kFadeAfter)
        alpha = std::max(0.0f, (kMaxAge - messages_.back().age) / kFadeTime);
    if (alpha <= 0.0f && !composing_) return;

    const float x = 8.0f;
    const float h = messages_.size() * 14.0f + (composing_ ? 16.0f : 0.0f) + 4.0f;
    const float y = 200.0f - h;
    const float w = 300.0f;

    uiRect(x - 2, y - 2, w + 4, h, C2D_Color32(0, 0, 0, (u8)(160 * alpha)));

    float cy = y;
    for (auto& m : messages_) {
        float fade = std::max(0.0f, std::min(1.0f, (kMaxAge - m.age) / kFadeTime));
        u32 col = C2D_Color32(255, 255, 255, (u8)(255 * alpha * fade));
        uiText(x + 2, cy, 0.4f, col, "<" + m.from + "> " + m.text);
        cy += 14.0f;
    }

    if (composing_) {
        u32 col = C2D_Color32(255, 220, 100, (u8)(255 * alpha));
        uiText(x + 2, cy, 0.4f, col, "> " + composeText_ + "_");
    }
}
