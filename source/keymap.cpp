#include "keymap.h"
#include "session.h"
#include "log.h"
#include <fstream>
#include <sstream>

u32 Keymap::s_keys[(int)InputAction::Count] = {
    KEY_R, KEY_L, KEY_Y, KEY_B, KEY_ZL, KEY_X, KEY_A, KEY_SELECT,
    KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT, KEY_START
};
bool Keymap::s_loaded = false;
bool Keymap::waitingForKey = false;
InputAction Keymap::actionToRebind = InputAction::Attack;

const char* Keymap::actionName(InputAction action) {
    switch (action) {
        case InputAction::Attack: return "Attack";
        case InputAction::Cast: return "Cast Spell";
        case InputAction::Jump: return "Jump";
        case InputAction::Run: return "Run / Walk";
        case InputAction::Sneak: return "Sneak";
        case InputAction::ReadyWeapon: return "Ready Weapon";
        case InputAction::Activate: return "Activate / Interact";
        case InputAction::ViewPOV: return "Toggle 1st/3rd Person";
        case InputAction::Quick1: return "Quick Key 1 (Up)";
        case InputAction::Quick2: return "Quick Key 2 (Right)";
        case InputAction::Quick3: return "Quick Key 3 (Down)";
        case InputAction::Quick4: return "Quick Key 4 (Left)";
        case InputAction::Menu: return "Game Menu";
        default: return "Unknown";
    }
}

u32 Keymap::getKey(InputAction action) {
    if (!s_loaded) load();
    return s_keys[(int)action];
}

void Keymap::setKey(InputAction action, u32 key) {
    s_keys[(int)action] = key;
}

void Keymap::load() {
    s_loaded = true;
    std::ifstream f("sdmc:/3ds/mw3ds/keymap.txt");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string name = line.substr(0, eq);
            u32 key = (u32)std::stoul(line.substr(eq + 1));
            for (int i = 0; i < (int)InputAction::Count; i++) {
                if (name == actionName((InputAction)i)) { s_keys[i] = key; break; }
            }
        }
    }
}

void Keymap::save() {
    std::ofstream f("sdmc:/3ds/mw3ds/keymap.txt");
    if (!f.is_open()) return;
    for (int i = 0; i < (int)InputAction::Count; i++) {
        f << actionName((InputAction)i) << "=" << s_keys[i] << "\n";
    }
}

void Keymap::resetDefaults() {
    s_keys[(int)InputAction::Attack] = KEY_R;
    s_keys[(int)InputAction::Cast] = KEY_L;
    s_keys[(int)InputAction::Jump] = KEY_Y;
    s_keys[(int)InputAction::Run] = KEY_B;
    s_keys[(int)InputAction::Sneak] = KEY_ZL;
    s_keys[(int)InputAction::ReadyWeapon] = KEY_X;
    s_keys[(int)InputAction::Activate] = KEY_A;
    s_keys[(int)InputAction::ViewPOV] = KEY_SELECT;
    s_keys[(int)InputAction::Quick1] = KEY_DUP;
    s_keys[(int)InputAction::Quick2] = KEY_DRIGHT;
    s_keys[(int)InputAction::Quick3] = KEY_DDOWN;
    s_keys[(int)InputAction::Quick4] = KEY_DLEFT;
    s_keys[(int)InputAction::Menu] = KEY_START;
}

u32 Keymap::translate(u32 hw_held, u32 hw_down, u32 hw_up, InputAction action) {
    u32 key = getKey(action);
    u32 mask = 0;
    if (hw_held & key) mask |= 1;
    if (hw_down & key) mask |= 2;
    if (hw_up & key) mask |= 4;
    return mask;
}

std::string Keymap::keyName(u32 key) {
    if (key == KEY_A) return "A";
    if (key == KEY_B) return "B";
    if (key == KEY_X) return "X";
    if (key == KEY_Y) return "Y";
    if (key == KEY_L) return "L";
    if (key == KEY_R) return "R";
    if (key == KEY_ZL) return "ZL";
    if (key == KEY_ZR) return "ZR";
    if (key == KEY_UP) return "D-Pad Up";
    if (key == KEY_DOWN) return "D-Pad Down";
    if (key == KEY_LEFT) return "D-Pad Left";
    if (key == KEY_RIGHT) return "D-Pad Right";
    if (key == KEY_START) return "Start";
    if (key == KEY_SELECT) return "Select";
    return "Unknown";
}
