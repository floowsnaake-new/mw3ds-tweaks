#pragma once
#include <3ds.h>
#include <string>

enum class InputAction {
    Attack,
    Cast,
    Jump,
    Run,
    Sneak,
    ReadyWeapon,
    Activate,
    ViewPOV,
    Quick1,
    Quick2,
    Quick3,
    Quick4,
    Menu,
    Count
};

class Keymap {
public:
    static const char* actionName(InputAction action);
    static u32 getKey(InputAction action);
    static void setKey(InputAction action, u32 key);
    static void load();
    static void save();
    static void resetDefaults();
    
    static u32 translate(u32 hw_held, u32 hw_down, u32 hw_up, InputAction action);
    static std::string keyName(u32 key);
    
    static bool waitingForKey;
    static InputAction actionToRebind;
private:
    static u32 s_keys[(int)InputAction::Count];
    static bool s_loaded;
};
