#include "session.h"
#include "consolecmd.h"
#include "ui.h"
#include "audio.h"
#include "log.h"
#include "pclink.h"

#include <algorithm>
#include <string>
#include <vector>

// These are defined in screens.cpp. Declared here to avoid depending on screens.h's
// (possibly stale or differing) declarations.
void header(const std::string& title, const std::string& note);
int buttonRow(const std::vector<std::string>& labels, int& focus, float y, float h);

void Session::openConsole()
{
    if (screen != SCR_CONSOLE)
        openScreen(SCR_CONSOLE);
    if (consoleLog.empty())
    {
        consolePrint("MW3DS console. Type a command (A), run (Y), close (B). Try 'help'.");
        consolePrint("Selected object (Lock/Unlock/Enable/Disable/etc.) is under the crosshair.");
    }
    consoleSelected = target;
}

void Session::drawConsole()
{
    header("Console", std::string("A: type  Y: run  B: close"));
    const UiInput& in = uiIn();

    uiPanel(4, 24, 312, 150);
    float lh = uiLineHeight(0.40f);
    int rows = (int)(146.0f / lh);
    int total = (int)consoleLog.size();
    int first = std::max(0, total - rows);
    if (in.down & KEY_UP)   consoleScroll = std::max(0, consoleScroll - 1);
    if (in.down & KEY_DOWN) consoleScroll = std::min(std::max(0, total - rows), consoleScroll + 1);
    first = std::max(0, std::min(first + consoleScroll, std::max(0, total - rows)));
    for (int i = 0; i < rows && first + i < total; i++)
    {
        const ConsoleLine& l = consoleLog[first + i];
        uiText(8, 26 + i * lh, 0.40f, l.color, l.text);
    }

    uiRect(4, 178, 312, 24, col::panel);
    uiFrame(4, 178, 312, 24);
    uiText(8, 182, 0.45f, col::text, "> " + consoleInput + "_");

    if (target >= 0)
    {
        const Ref& r = w.refs[target];
        uiText(8, 162, 0.36f, col::textDim,
               "under crosshair: " + r.id.str() +
               (r.lockLevel ? " (lock " + std::to_string(r.lockLevel) + ")" : ""));
    }

    int b = buttonRow({ "Type", "Run", "Clear", "Close" }, focus, 208.0f, 28.0f);
    if (b == 0 || (in.down & KEY_A))
    {
        wantText = TEXT_CONSOLE;
        consoleScroll = 0;
        playSound(-1, "Menu Click");
    }
    else if (b == 1 || (in.down & KEY_Y))
        consoleSubmit();
    else if (b == 2)
    {
        consoleLog.clear();
        consoleScroll = 0;
    }
    else if (b == 3 || (in.down & KEY_B))
        closeScreen();
}

void Session::consolePrint(const std::string& text, u32 color)
{
    consoleLog.push_back({ text, color });
    if (consoleLog.size() > 128)
        consoleLog.erase(consoleLog.begin());
    logf("console: %s", text.c_str());
    pclink_log("%s", text.c_str());
}

void Session::consoleSubmit()
{
    std::string line = consoleInput;
    consoleInput.clear();
    if (line.empty()) return;
    consolePrint("> " + line, 0xFFE0E0E0);
    if (consoleCommandHook)
        consoleCommandHook(line);
}
