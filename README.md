## Custom features (on top of upstream [3ds-conspiracy/mw3ds](https://github.com/3ds-conspiracy/mw3ds))

Working on a Mod Manager

![Working on a Mod Manager](/mod.png "Working on a Mod Manager")
 
Upstream provides a basic **Options** screen (3D, difficulty, volumes, gyro, combat rules, hit numbers) and a **Controls** help screen. The pause menu is a single column.

This fork expands the menus and adds quality-of-life, performance, and cheat options.

Added Official expansion packs: Tribunal and Bloodmoon

### Pause menu (two columns)

 ![Markdown menu](/menu.gif)

| Left | Right |
|------|--------|
| Return to game | Graphics |
| Save / Load | Performance |
| Rest / Wait | Controls |
| Options | Interface |
| Save and quit | Cheats |

### Graphics
- Draw distance (25–100%)
- Fog on/off
- Weather on/off (forces clear sky)
- Brightness (70–130%)

### Performance
- Presets: Low / Balanced / High
- Frame cap: Auto / 30 / 60
- Character distance & clutter distance
- Toggles: sky & clouds, terrain layers, transparency, characters, distant land, enchant shimmer
- FPS counter on/off

### Controls
- Look sensitivity (25–200%)
- Invert Y look
- Swap sticks (C-stick walks, Circle Pad looks)
- Always run g on/off
- Button layout: Classic / GoldenEye / Xbox
- 
### Interface
- Subtitles on/off
- Hide crosshair / top-screen HUD / minimap
- Screenshot hides HUD
- Autosave every 5 / 10 / 15 / 30 minutes (or off)

### Cheats *(not saved between sessions)*
- Restore health, magicka, fatigue
- +1000 gold
- God mode
- Infinite magicka & fatigue
- Weightless
- Walk speed & jump power multipliers
- All skills +10, all attributes +10
- Teleport to any town listed in the game data

### Other
- **L + R + Select** → screenshot saved to `sdmc:/3ds/mw3ds/screenshots/`
- Graphics / Performance / Controls / Interface settings are written to `settings.txt` and restored on next launch
