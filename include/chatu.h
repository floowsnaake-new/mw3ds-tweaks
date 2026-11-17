#pragma once
#include <3ds.h>
#include <string>
#include <vector>

class ChatOverlay {
public:
    void addMessage(const std::string& from, const std::string& text);
    void openCompose();
    void closeCompose();
    bool composing() const { return composing_; }
    void submit(const std::string& text);
    void draw();
    void update(float dt);

private:
    struct Message {
        std::string from;
        std::string text;
        float age;
    };
    std::vector<Message> messages_;
    bool composing_ = false;
    std::string composeText_;
    float displayUntil_ = 0.0f;

    static constexpr float kFadeAfter = 3.0f;
    static constexpr float kFadeTime = 2.0f;
    static constexpr float kMaxAge = 30.0f;
    static constexpr int kMaxMessages = 8;
};
