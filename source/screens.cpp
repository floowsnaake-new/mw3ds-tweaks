#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.h"
#include "linear.h"
#include "log.h"
#include "pclink.h"
#include "renderer.h"
#include "screens.h"
#include "keymap.h"
#include "session.h"

// ---- names

static const char* kAttrKeys[8] = { "sattributestrength", "sattributeintelligence", "sattributewillpower",
	"sattributeagility", "sattributespeed", "sattributeendurance", "sattributepersonality", "sattributeluck" };

static const char* kAttrFallback[8] = { "Strength", "Intelligence", "Willpower", "Agility", "Speed",
	"Endurance", "Personality", "Luck" };

static const char* kSkillKeys[27] = { "sskillblock", "sskillarmorer", "sskillmediumarmor", "sskillheavyarmor",
	"sskillbluntweapon", "sskilllongblade", "sskillaxe", "sskillspear", "sskillathletics", "sskillenchant",
	"sskilldestruction", "sskillalteration", "sskillillusion", "sskillconjuration", "sskillmysticism",
	"sskillrestoration", "sskillalchemy", "sskillunarmored", "sskillsecurity", "sskillsneak", "sskillacrobatics",
	"sskilllightarmor", "sskillshortblade", "sskillmarksman", "sskillmercantile", "sskillspeechcraft",
	"sskillhandtohand" };

static const char* kSpecKeys[3] = { "sspecializationcombat", "sspecializationmagic", "sspecializationstealth" };

std::string attrName(const World& w, int i)
{
	return i >= 0 && i < 8 ? w.game.gmst(kAttrKeys[i], kAttrFallback[i]) : "?";
}

std::string skillName(const World& w, int i)
{
	return i >= 0 && i < 27 ? w.game.gmst(kSkillKeys[i], kSkillKeys[i] + 6) : "?";
}

std::string specName(const World& w, int i)
{
	return i >= 0 && i < 3 ? w.game.gmst(kSpecKeys[i]) : "?";
}

void header(const std::string& title, const std::string& note)
{
	uiCaption(title, note);
}

int buttonRow(const std::vector<std::string>& labels, int& focus, float y, float h)
{
	const UiInput& in = uiIn();
	int n = labels.size();
	if (n == 0)
		return -1;
	if (in.down & KEY_RIGHT) focus = (focus + 1) % n;
	if (in.down & KEY_LEFT) focus = (focus + n - 1) % n;
	if (focus >= n) focus = n - 1;
	float bw = (312.0f - (n - 1) * 4.0f) / n;
	int hit = -1;
	for (int i = 0; i < n; i++)
		if (uiButton(4 + i * (bw + 4), y, bw, h, labels[i], i == focus))
			hit = i;
	return hit;
}

// ---- item grids (Morrowind's inventory look)

UiGridItem gridItem(const Object* o, const std::string& id, int count, bool equipped)
{
	u32 flags = (equipped ? UIGRID_EQUIPPED : 0) | (o && o->magic ? UIGRID_MAGIC : 0);
	return { o ? o->iconIx : -1, count, flags, o ? o->name.str() : id };
}

// Morrowind's inventory filters: All, Weapon, Apparel, Magic, Misc
const char* kTabs[5] = { "All", "Weapon", "Apparel", "Magic", "Misc" };

bool inTab(const Object* o, int tab)
{
	if (tab == 0 || !o)
		return true;
	bool weapon = o->type == "WEAP", apparel = o->type == "ARMO" || o->type == "CLOT";
	bool magic = o->magic || o->type == "ALCH" || o->scroll;
	switch (tab)
	{
	case 1: return weapon;
	case 2: return apparel;
	case 3: return magic;
	default: return !weapon && !apparel && !magic;
	}
}

// The selected item's name, and its weight and value on the right
void itemInfo(const Object* o, float y, const std::string& extra)
{
	if (!o)
		return;
	char buf[64];
	snprintf(buf, sizeof(buf), "Wt %.1f  Val %d", o->weight, o->value);
	std::string right = extra.empty() ? std::string(buf) : extra;
	float rw = uiTextWidth(right, 0.42f);
	std::string name = o->name;
	while (name.size() > 3 && uiTextWidth(name, 0.5f) > 300 - rw)
		name = name.substr(0, name.size() - 4) + "...";
	uiText(6, y, 0.5f, col::header, name);
	uiTextRight(314, y + 2, 0.42f, col::textDim, right);
}

// ---- top screen overlay

void Session::drawTop()
{
	chat.draw();
	// Multiplayer: name + HP strip for each live remote player
	float hudY = 4.0f;
	if ((mpMode == 1 || mpMode == 2) && net.isConnected() && !(g_hideHud || g_hudTemp))
	{
		for (int i = 0; i < NetEngine::kMaxPeers; i++)
		{
			RemotePlayer& p = remotePlayers[i];
			if (!p.active || p.updateAge > 5.0f) continue;
			std::string label = p.name.empty() ? ("Player " + std::to_string(i + 1)) : p.name;
			if (p.dead)
				label += " [fallen]";
			else if (p.health >= 0.0f)
			{
				char hp[24];
				snprintf(hp, sizeof(hp), "  %.0f HP", p.health);
				label += hp;
			}
			u32 col = p.dead ? C2D_Color32(200, 80, 80, 220) : C2D_Color32(200, 230, 255, 220);
			uiRect(4, hudY - 1, uiTextWidth(label, 0.4f) + 6, 12, C2D_Color32(0, 0, 0, 140));
			uiText(6, hudY, 0.4f, col, label);
			hudY += 13.0f;
		}
	}
	// Companion name + HP bar (full length when healthy, shortens as he takes damage)
	if (companionRef >= 0 && companionRef < (int)w.refs.size() && !(g_hideHud || g_hudTemp))
	{
		Ref& cr = w.refs[companionRef];
		if (cr.cell >= 0 && !cr.dead && cr.healthMax > 0.0f)
		{
			char label[64];
			snprintf(label, sizeof(label), "%s  %.0f/%.0f", companionName.c_str(), cr.health, cr.healthMax);
			const float barFull = 100.0f;
			float frac = fmaxf(0.0f, fminf(1.0f, cr.health / cr.healthMax));
			float barW = fmaxf(4.0f, barFull * frac);   // shortens when hurt
			uiRect(4, hudY - 1, fmaxf(barFull + 8, uiTextWidth(label, 0.4f) + 6), 22, C2D_Color32(0, 0, 0, 140));
			uiText(6, hudY, 0.4f, C2D_Color32(220, 230, 180, 220), label);
			u32 hpCol = frac > 0.4f ? col::health : (frac > 0.2f ? C2D_Color32(220, 160, 40, 220) : C2D_Color32(200, 60, 50, 220));
			uiRect(6, hudY + 12, barW, 6, hpCol);
		}
	}
	const UiInput& in = uiIn();
	(void)in;
	// (aimShift: in 3D, the crosshair and the label sit at the depth of what's aimed at)
	float ax = aimShift;
	const bool hudOff = g_hideHud || g_hudTemp;      // Interface: HUD hidden (always, or for a screenshot)
	if (!indicators.empty())
	{
		// both eyes draw them; the time moves on once a frame (the left eye's pass)
		static u64 last = 0;
		u64 now = osGetTime();
		float dt = last && hudEye == 0 ? fminf(0.1f, (now - last) / 1000.0f) : 0.0f;
		if (hudEye == 0)
			last = now;
		drawIndicators(dt);
	}
	if (!menuOpen() && !g_hideCrosshair && !hudOff)
	{
		uiRect(199 + ax, 117, 3, 7, C2D_Color32(255, 255, 255, 150));
		uiRect(197 + ax, 119, 7, 3, C2D_Color32(255, 255, 255, 150));
	}
	if (target >= 0 && !menuOpen() && !hudOff)
	{
		// (a long name, "Hut Door - Ald-ruhn, Tiras Sadus: General Merchandise", wraps over lines inside the screen)
		std::vector<std::string> lines = targetPopupLines(target);
		float lh = uiLineHeight(0.55f), tw = 0.0f;
		for (auto& l : lines)
			tw = fmaxf(tw, uiTextWidth(l, 0.55f));
		float h = lines.size() * lh + 2;
		uiRect(200 + ax - tw / 2 - 6, 132, tw + 12, h + 2, col::border);
		uiRect(200 + ax - tw / 2 - 5, 133, tw + 10, h, col::panel);
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200 + ax, 134 + i * lh, 0.55f, w.ownedByOther(target) ? col::health : col::text, lines[i]);
	}
	// Notices, each wrapped to the screen's width ("Release Identification has been removed from your inventory."
	// ran off it)
	float y = 4;
	notesRight = 0.0f;
	// (while a menu pauses the world, short of the "Paused" label in the top right corner)
	float notesW = worldPaused() ? 384 - uiTextWidth("Paused", 0.5f) - 16 : 384;
	for (auto& n : notes)
		for (auto& line : uiWrap(n.text, notesW, 0.5f))
		{
			float tw = uiTextWidth(line, 0.5f);
			uiRect(4, y, tw + 8, 16, C2D_Color32(0, 0, 0, 150));
			uiText(8, y, 0.5f, col::header, line);
			notesRight = fmaxf(notesRight, 8 + tw);
			y += 18;
		}
	if (g_subtitles && w.time < subtitleUntil && !subtitle.empty())
	{
		std::vector<std::string> lines = uiWrap(subtitle, 380, 0.5f);
		float lh = uiLineHeight(0.5f);
		float top = 236 - lines.size() * lh;
		uiRect(4, top - 2, 392, lines.size() * lh + 4, C2D_Color32(0, 0, 0, 220));   // the world barely shows through
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200, top + i * lh, 0.5f, col::header, lines[i]);
	}
	// Sneaking and nobody has noticed: Morrowind's sneak eye by the crosshair
	if (hidden && !menuOpen() && !hudOff)
	{
		auto eye = w.game.art.hud.find("sneak");
		if (eye == w.game.art.hud.end() || !uiArt(eye->second.file, eye->second.w, eye->second.h, 170, 158, 20, 20))
			uiTextCentered(200, 158, 0.45f, col::header, "(hidden)");
	}
	// Wind-up of a blow under the crosshair
	if (attackCharge >= 0.0f && !menuOpen() && !hudOff)
	{
		uiRect(170, 150, 60, 5, C2D_Color32(0, 0, 0, 160));
		uiRect(170, 150, 60 * attackCharge, 5, col::header);
	}
	// Health of whoever was hit last
	if (enemyRef >= 0 && w.time < enemyUntil && w.active(enemyRef) && !hudOff)
	{
		const Ref& e = w.refs[enemyRef];
		uiRect(150, 222, 100, 10, C2D_Color32(0, 0, 0, 170));
		uiRect(151, 223, 98 * fmaxf(0.0f, e.health) / fmaxf(1.0f, e.healthMax), 8, col::health);
	}
	if (hurtFlash > 0.0f)
		uiRect(0, 0, 400, 240, C2D_Color32(200, 0, 0, (u8)(hurtFlash * 90)));
	// The selected item's or spell's tooltip
	if (menuOpen() && !tip.empty())
	{
		const float scale = 0.45f, lh = uiLineHeight(scale);
		float tw = 0.0f;
		for (auto& l : tip)
			tw = fmaxf(tw, uiTextWidth(l.first, l.second == col::header ? 0.52f : scale));
		tw = fminf(tw, 380.0f);
		float th = tip.size() * lh + 8, x = 200 - tw / 2 - 8, y = 120 - th / 2;
		uiRect(x - 1, y - 1, tw + 18, th + 2, col::border);
		uiRect(x, y, tw + 16, th, C2D_Color32(0, 0, 0, 215));
		for (size_t i = 0; i < tip.size(); i++)
			uiTextCentered(200, y + 4 + i * lh, tip[i].second == col::header ? 0.52f : scale, tip[i].second, tip[i].first);
	}
	// set by the bottom screen each frame; kept until both eyes have drawn it (clearing it after the first
	// pass left the tooltip in one eye only)
	if (hudEye == hudEyes - 1)
		tip.clear();
	drawPrecipitation();
	// A menu has stopped the world (both eyes draw it: it comes from the screen, not from a timer)
	if (worldPaused() && !hudOff)
	{
		float pw = uiTextWidth("Paused", 0.5f);
		uiRect(396 - pw - 12, 4, pw + 10, 16, C2D_Color32(0, 0, 0, 150));
		uiTextRight(392, 4, 0.5f, col::header, "Paused");
	}
	// Breath under water: a bar that empties, then drowning
	if (breath >= 0.0f)
	{
		float full = w.game.gmstf("fholdbreathtime", 20.0f);
		uiRect(150, 8, 100, 8, C2D_Color32(0, 0, 0, 150));
		uiRect(151, 9, 98 * fmaxf(0.0f, breath) / fmaxf(1.0f, full), 6, C2D_Color32(120, 170, 230, 220));
		uiTextCentered(200, 18, 0.4f, breath > 0.0f ? col::text : col::health, breath > 0.0f ? "Breath" : "Drowning!");
	}
	if (fade > 0.0f)
		uiRect(0, 0, 400, 240, C2D_Color32(0, 0, 0, (u8)(fminf(fade, 1.0f) * 255)));
	if (wantName)
		uiTextCentered(200, 110, 0.6f, col::header, "Enter your name");
}

// ---- bottom screen

void Session::drawBottom()
{
	uiRect(0, 0, 320, 240, C2D_Color32(10, 8, 6, 255));
	if (!messages.empty())
	{
		drawMessage();
		return;
	}
	switch (screen)
	{
	case SCR_NONE: drawHud(); drawVirtualSticks(); break;
	case SCR_DIALOGUE: drawDialogue(); break;
	case SCR_BOOK: drawBook(); break;
	case SCR_CONTAINER: drawContainer(); break;
	case SCR_INVENTORY: drawInventory(); break;
	case SCR_JOURNAL: drawJournal(); break;
	case SCR_STATS: drawStats(); break;
	case SCR_RACE: drawRace(); break;
	case SCR_CLASS_METHOD: drawClassMethod(); break;
	case SCR_CLASS_LIST: drawClassList(); break;
	case SCR_CLASS_QUIZ: drawClassQuiz(); break;
	case SCR_CLASS_RESULT: drawClassResult(); break;
	case SCR_BIRTH: drawBirth(); break;
	case SCR_REVIEW: drawReview(); break;
	case SCR_COMPANION: drawCompanion(); break;
	case SCR_COMPANION_ORDERS: drawCompanionOrders(); break;
	case SCR_REST: drawRest(); break;
	case SCR_LEVELUP: drawLevelUp(); break;
	case SCR_END: drawEnd(); break;
	case SCR_BARTER: drawBarter(); break;
	case SCR_DEATH: drawDeath(); break;
	case SCR_TRAVEL: drawTravel(); break;
	case SCR_MAP: drawMap(); break;
	case SCR_MAGIC: drawMagic(); break;
	case SCR_SPELLS: drawSpells(); break;
	case SCR_ARREST: drawArrest(); break;
	case SCR_TRAINING: drawTraining(); break;
	case SCR_PERSUADE: drawPersuade(); break;
	case SCR_ALCHEMY: drawAlchemy(); break;
	case SCR_SPELLMAKE: drawSpellmaking(); break;
	case SCR_ENCHANT: drawEnchanting(); break;
	case SCR_RECHARGE: drawRecharge(); break;
	case SCR_OPTIONS: drawOptions(); break;
	case SCR_CONTROLS: drawControls(); break;
	case SCR_MOREOPTIONS: drawMoreOptions(); break;
	case SCR_CHEATS: drawCheats(); break;
	case SCR_GRAPHICS: drawGraphics(); break;
	case SCR_PERFORMANCE: drawPerformance(); break;
	case SCR_CTRLSET: drawCtrlSet(); break;
	case SCR_INTERFACE: drawInterface(); break;
	case SCR_TELEPORT: drawTeleport(); break;
	case SCR_PCLINK: drawPcLink(); break;
	case SCR_CONSOLE: drawConsole(); break;
	case SCR_MULTIPLAYER: drawMultiplayer(); break;
	case SCR_HELP: drawHelp(); break;
	case SCR_KEYREMAP: drawKeyRemap(); break;
	case SCR_REPAIR: drawRepair(); break;
	case SCR_SAVES: drawSaves(); break;
	case SCR_GAMEMENU: drawGameMenu(); break;
	default: break;
	}
}

// The four quick keys as slots under the minimap (Up, Right, Down, Left), big enough to tap: the item's icon or the
// spell's first letters; a tap uses the slot as its D-pad press does (GoldenEye's D-pad moves: the taps are its quick keys)
void Session::drawQuickStrip()
{
	static const char* dirs[4] = { "U", "R", "D", "L" };
	const float kSlot = 26.0f;
	for (int k = 0; k < 4; k++)
	{
		float x = 190 + k * 30, y = 179;
		const std::string& q = w.stats.quickKeys[k];
		bool isSpell = q.compare(0, 6, "magic:") == 0 && q.compare(0, 11, "magic:item:") != 0;
		std::string id = q.compare(0, 4, "inv:") == 0 ? q.substr(4) : q.compare(0, 11, "magic:item:") == 0 ? q.substr(11)
			: q.compare(0, 6, "magic:") == 0 ? q.substr(6) : q;
		const Object* o = q.empty() || isSpell ? nullptr : w.game.object(id);
		uiRect(x, y, kSlot, kSlot, col::panel);
		if (!q.empty() && !(o && uiIcon(o->iconIx, x + 1, y + 1, kSlot - 2.0f)))
		{
			std::string name = isSpell ? (w.game.spells.count(id) ? w.game.spells[id].name : id) : o ? o->name.str() : id;
			uiTextCentered(x + kSlot / 2, y + kSlot / 2 - uiLineHeight(0.38f) / 2, 0.38f, isSpell ? col::link : col::textDim, name.substr(0, 3));
		}
		uiFrame(x, y, kSlot, kSlot);
		uiText(x + 2, y, 0.3f, col::textOver, dirs[k]);
		if (uiHit(x, y, kSlot, kSlot) && w.controlsEnabled && !playerDead)
			useQuickKey(k);
	}
}

void Session::drawHud()
{
	const PlayerStats& s = w.stats;
	std::string sub;
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(s.race))
			sub = r.name;
	if (const ClassDef* c = w.playerClass())
		sub += " " + c->name;
	header(s.name, sub);

	// Health, magicka, fatigue (Morrowind's colours), the numbers inside the bars
	float cur[3] = { s.health, s.magicka, s.fatigue }, max[3] = { s.healthMax, s.magickaMax, s.fatigueMax };
	u32 colors[3] = { col::health, col::magicka, col::fatigue };
	for (int i = 0; i < 3; i++)
	{
		float y = 27 + i * 15;
		uiBar(6, y, 174, 12, max[i] > 0 ? cur[i] / max[i] : 0, colors[i]);
		char buf[32];
		snprintf(buf, sizeof(buf), "%d/%d", (int)cur[i], (int)max[i]);
		uiTextCentered(93, y + 6 - uiLineHeight(0.36f) / 2, 0.36f, col::textPressed, buf);
	}

	// Weapon and spell, as the boxes by Morrowind's bars
	const InventoryItem* weapon = nullptr;
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "WEAP")
					weapon = &it;
	const Object* wo = weapon ? w.game.object(weapon->id) : nullptr;
	float by = 75;
	uiRect(6, by, 36, 36, col::panel);
	if (!(wo && uiIcon(wo->iconIx, 8, by + 2)))
		uiTextCentered(24, by + 18 - uiLineHeight(0.36f) / 2, 0.36f, col::textDim, wo ? wo->name.str().substr(0, 3) : "Fist");
	uiFrame(6, by, 36, 36);
	uiText(48, by + 1, 0.42f, col::text, wo ? wo->name : "Hand-to-hand");
	std::string spell = s.selectedSpell.empty() ? "" : w.game.spells[s.selectedSpell].name;
	uiText(48, by + 18, 0.42f, spell.empty() ? col::textDim : col::link, spell.empty() ? "No spell" : spell);

	// Messages
	float y = 116;
	for (auto& n : notes)
	{
		for (auto& line : uiWrap(n.text, 174, 0.42f))
		{
			if (y > 164)
				break;
			uiText(6, y, 0.42f, col::text, line);
			y += uiLineHeight(0.42f);
		}
	}

	// Minimap: tap it for the map
	bool mapOk = !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen);
	if (!g_hideMinimap)
	{
		drawMapView(186, 26, 128, 128, 1.0f, false, w.current >= 0 && w.cells[w.current].interior);
		// (the Chat button sits on the minimap's lower left corner while multiplayer is on: a tap there is not a map tap)
		if (mapOk && uiHit(186, 26, 128, 128) && !(mpMode != 0 && uiHit(186, 138, 38, 16)))
			openScreen(SCR_MAP);
	}
	uiTextCentered(250, 155, 0.38f, col::textDim, w.cellName().size() > 26 ? w.cellName().substr(0, 25) + "..." : w.cellName());

	if (target >= 0)
	{
		// two lines for a long name, a little smaller and higher, and the attack hint below them moves down
		float s = 0.45f;
		std::vector<std::string> use = targetPromptLines(target, &s);
		promptRows = (int)use.size();
		float lh = uiLineHeight(s);
		for (size_t i = 0; i < use.size(); i++)
			uiText(6, (use.size() > 1 ? 166 : 172) + i * lh, s, col::link, use[i]);
	}
	else
		promptRows = 1;
	if (w.fightingEnabled)
	{
		bool xbox = g_controlLayout == CONTROLS_XBOX || g_controlLayout == CONTROLS_GOLDENEYE;   // (GoldenEye: the Xbox buttons)
		std::string hint = w.stats.selectedSpell.empty() ? (xbox ? "R: attack (hold to wind up)" : "X: attack (hold to wind up)")
			: std::string(xbox ? "R: attack   L: cast " : "X: attack   Y: cast ") + w.game.spells[w.stats.selectedSpell].name;
		// Leave the quick key strip (from x 190) clear, and GoldenEye's 1st / 3rd button (from x 132)
		float room = g_controlLayout == CONTROLS_GOLDENEYE ? 122.0f : 180.0f;
		while (hint.size() > 3 && uiTextWidth(hint, 0.38f) > room)
			hint = hint.substr(0, hint.size() - 4) + "...";
		uiText(6, promptRows > 1 ? 192 : 190, 0.38f, col::textDim, hint);
	}
	drawQuickStrip();
	uiTextCentered(250, 166, 0.36f, col::textDim, dateText());
	// the bounty on the minimap's foot, the frame rate in its top corner (the strip took their place)
	if (w.bounty > 0)
	{
		std::string b = "Bounty " + std::to_string(w.bounty);
		uiRect(312 - uiTextWidth(b, 0.4f) - 4, 140, uiTextWidth(b, 0.4f) + 4, 13, C2D_Color32(0, 0, 0, 170));
		uiTextRight(311, 140, 0.4f, col::health, b);
	}
	if (g_showFps)
	{
		char perf[64];
		snprintf(perf, sizeof(perf), "%.0f fps", fps);
		uiTextRight(312, 27, 0.34f, C2D_Color32(80, 170, 255, 255), perf);   // bright blue, easy to read
	}
	// Multiplayer: a small Chat button (opens the keyboard; main.cpp sends the message), in place of the old SELECT shortcut
	if (mpMode != 0 && uiButton(188, 139, 34, 14, "Chat"))
		g_mpAsk = 3;
	// GoldenEye's SELECT sneaks: first / third person is a button here, beside the attack hint
	if (g_controlLayout == CONTROLS_GOLDENEYE && uiButton(132, 188, 50, 17, thirdPerson ? "1st" : "3rd"))
		povTapped = true;

	bool inv = w.menusEnabled & MENU_INVENTORY, stats = w.menusEnabled & MENU_STATS;
	if (uiButton(2, 208, 51, 28, "Items", false, inv))
		openScreen(SCR_INVENTORY);
	if (uiButton(55, 208, 51, 28, "Magic", false, (w.menusEnabled & MENU_MAGIC) || !w.game.chargen))
		openScreen(SCR_MAGIC);
	if (uiButton(108, 208, 51, 28, "Journal", false, !w.journal.empty()))
		openScreen(SCR_JOURNAL);
	if (uiButton(161, 208, 51, 28, "Map", false, !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen)))
		openScreen(SCR_MAP);
	if (uiButton(214, 208, 51, 28, "Stats", false, stats))
		openScreen(SCR_STATS);
	if (uiButton(267, 208, 51, 28, "Saves", false, !wantName))
	{
		saveList = savesList(dataDir);
		openScreen(SCR_SAVES);
	}
}

static int s_gfxTop = 0;      // the Graphics list's first row shown

// START: the game menu (Morrowind's Esc menu)
void Session::toggleGameMenu()
{
	if (screen == SCR_GAMEMENU)
		closeScreen();
	else if (screen == SCR_NONE && messages.empty() && !dlg.open && !wantName)
	{
		openScreen(SCR_GAMEMENU);
		playSound(-1, "Menu Click");
	}
}

void Session::drawGameMenu()
{
	// Soft backdrop so the pause menu reads as a panel over the world
	uiRect(0, 0, 320, 240, C2D_Color32(8, 10, 14, 220));
	header("Paused", w.cellName());
	// Save and quit: "Saving..." on screen first, then the save (a long campaign's takes seconds)
	if (quitSaving > 0)
	{
		uiTextCentered(160, 110, 0.6f, col::text, "Saving...");
		if (++quitSaving > 2)
		{
			save();
			wantQuit = true;
		}
		return;
	}
	const UiInput& in = uiIn();
	// OpenMW's getRestEnabled: EnableRest has run, or character creation has finished (CharGenState -1)
	auto cg = w.globals.find("chargenstate");
	bool rest = (w.menusEnabled & MENU_REST) || !w.game.chargen || (cg != w.globals.end() && cg->second == -1.0f);

	// Left = Game, right = Settings
	//  0 Resume          6 Options
	//  1 Save / Load     7 Graphics
	//  2 Rest / Wait     8 Performance
	//  3 Help            9 Controls
	//  4 Multiplayer    10 Interface
	//  5 Save and quit  11 Cheats
	const char* labels[12] = {
		"Resume", "Save / Load", "Rest / Wait", "Help", "Multiplayer", "Save and quit",
		"Options", "Graphics", "Performance", "Controls", "Interface", "Cheats"
	};
	bool enabled[12] = {
		true, !wantName, rest, true, true, pendingMenus.empty(),
		true, true, true, true, true, true
	};

	// Section panels
	uiRect(6, 26, 152, 178, C2D_Color32(20, 24, 30, 200));
	uiFrame(6, 26, 152, 178);
	uiRect(162, 26, 152, 178, C2D_Color32(20, 24, 30, 200));
	uiFrame(162, 26, 152, 178);
	uiText(14, 30, 0.38f, col::header, "GAME");
	uiText(170, 30, 0.38f, col::header, "SETTINGS");

	if (focus < 0 || focus > 11) focus = 0;
	int column = focus / 6, row = focus % 6;
	if (in.down & KEY_DOWN) row = (row + 1) % 6;
	if (in.down & KEY_UP) row = (row + 5) % 6;
	if (in.down & (KEY_LEFT | KEY_RIGHT)) column ^= 1;
	focus = column * 6 + row;

	int hit = -1;
	for (int i = 0; i < 12; i++)
	{
		int ccol = i / 6, r = i % 6;
		float x = 12.0f + ccol * 156.0f;
		float y = 46.0f + r * 25.0f;
		if (uiButton(x, y, 140, 22, labels[i], i == focus, enabled[i]))
			hit = i;
	}

	uiTextCentered(160, 214, 0.36f, col::textDim, "A: select   B: resume");

	if (hit == 0 || (in.down & KEY_B))
		closeScreen();
	else if (hit == 1)
	{
		saveList = savesList(dataDir);
		closeScreen();
		openScreen(SCR_SAVES);
	}
	else if (hit == 2)
	{
		closeScreen();
		openRest(false);
	}
	else if (hit == 3)
	{
		closeScreen();
		helpPage = 0;
		focus = 0;
		openScreen(SCR_HELP);
	}
	else if (hit == 4)
	{
		closeScreen();
		focus = 0;
		s_gfxTop = 0;
		openScreen(SCR_MULTIPLAYER);
	}
	else if (hit == 5)
		quitSaving = 1;
	else if (hit >= 6)
	{
		static const Screen kScreens[6] = {
			SCR_OPTIONS, SCR_GRAPHICS, SCR_PERFORMANCE, SCR_CTRLSET, SCR_INTERFACE, SCR_CHEATS
		};
		closeScreen();
		focus = 0;
		s_gfxTop = 0;
		openScreen(kScreens[hit - 6]);
	}
}

// Options: 3D, difficulty, effects and music volume (D-pad up / down picks, left / right or a tap on the
// bar sets); kept in settings.txt
void Session::drawOptions()
{
	header(w.game.gmst("soptions", "Options"));
	const UiInput& in = uiIn();
	// focus: 0..2 the bars, 3..5 the toggles (3D, combat rules, hit numbers), 6 gyro aim, 7..9 the buttons along the bottom
	if (in.down & KEY_DOWN) focus = focus >= 7 ? 0 : focus == 6 ? 7 : focus >= 3 ? 6 : focus + 1;
	else if (in.down & KEY_UP) focus = focus == 0 ? 7 : focus >= 7 ? 6 : focus == 6 ? 3 : focus >= 3 ? 2 : focus - 1;
	else if (focus >= 3 && focus <= 5 && (in.down & KEY_RIGHT)) focus = focus == 5 ? 3 : focus + 1;
	else if (focus >= 3 && focus <= 5 && (in.down & KEY_LEFT)) focus = focus == 3 ? 5 : focus - 1;
	bool changed = false;
	const char* names[3] = { "Difficulty", "Effects volume", "Music volume" };
	int* vals[3] = { &difficulty, &effectsVolume, &musicVolume };
	const int lo[3] = { -100, 0, 0 }, hi[3] = { 100, 100, 100 }, step[3] = { 10, 10, 10 };
	for (int i = 0; i < 3; i++)
	{
		float y = 28 + i * 38;
		uiText(20, y, 0.45f, i == focus ? col::header : col::text, names[i] + std::string(": ") + std::to_string(*vals[i]));
		float bx = 20, bw = 280, by = y + 18;
		uiRect(bx, by, bw, 8, col::panel);
		uiFrame(bx, by, bw, 8);
		float f = (float)(*vals[i] - lo[i]) / (hi[i] - lo[i]);
		uiRect(bx + f * bw - 4, by - 5, 8, 18, col::header);
		int v = *vals[i];
		if (i == focus && (in.down & KEY_LEFT)) v -= step[i];
		if (i == focus && (in.down & KEY_RIGHT)) v += step[i];
		if (in.touching && in.touchY >= by - 8 && in.touchY <= by + 16 && in.touchX >= bx - 6 && in.touchX <= bx + bw + 6)
		{
			focus = i;
			v = lo[i] + (int)roundf(fmaxf(0.0f, fminf(1.0f, (in.touchX - bx) / bw)) * (hi[i] - lo[i]) / step[i]) * step[i];
		}
		v = v < lo[i] ? lo[i] : v > hi[i] ? hi[i] : v;
		if (v != *vals[i])
		{
			*vals[i] = v;
			changed = true;
		}
	}
	const char* stereo = stereoMode == STEREO_OFF ? "3D: Off" : "3D: On";
	if (uiButton(4, 142, 100, 26, stereo, focus == 3) || (focus == 3 && (in.down & KEY_A)))
	{
		stereoMode = stereoMode == STEREO_OFF ? STEREO_ON : STEREO_OFF;
		changed = true;
	}
	// Combat: vanilla, or Glancing Blows and Crits (misses glance, crits; the same average damage)
	const char* rules = combatMode == COMBAT_GBAC ? "Glance+Crit" : "Vanilla hits";
	if (uiButton(110, 142, 100, 26, rules, focus == 4) || (focus == 4 && (in.down & KEY_A)))
	{
		combatMode = combatMode == COMBAT_GBAC ? COMBAT_VANILLA : COMBAT_GBAC;
		notify(combatMode == COMBAT_GBAC ? "Glancing blows and crits: misses glance for half damage, strong hits can crit."
			: "Vanilla combat.");
		changed = true;
	}
	// Hit & Miss Indicators: damage numbers and misses by the crosshair
	if (uiButton(216, 142, 100, 26, hitIndicators ? "Numbers: On" : "Numbers: Off", focus == 5)
		|| (focus == 5 && (in.down & KEY_A)))
	{
		hitIndicators = !hitIndicators;
		notify(hitIndicators ? "Hit and miss numbers on." : "Hit and miss numbers off.");
		changed = true;
	}
	// Gyro aim: turning the console turns the view (off, half, as far, twice as far as it turned)
	static const int kGyroSteps[4] = { 0, 50, 100, 200 };
	char gyro[40];
	snprintf(gyro, sizeof(gyro), g_gyroAim > 0 ? "Gyro aim: %.1fx" : "Gyro aim: Off", g_gyroAim / 100.0f);
	if (uiButton(4, 174, 312, 26, gyro, focus == 6) || (focus == 6 && (in.down & KEY_A)))
	{
		int k = 0;
		while (k < 3 && kGyroSteps[k] != g_gyroAim)
			k++;
		g_gyroAim = kGyroSteps[(k + 1) % 4];
		notify(g_gyroAim > 0 ? "Gyro aim on: turn the 3DS to aim." : "Gyro aim off.");
		changed = true;
	}
	if (changed)
	{
		audioSetScales(effectsVolume / 100.0f, musicVolume / 100.0f);
		saveSettings();
	}
	// Controls, Help, More..., Close (focus 7..10: D-pad left / right between them)
	int b = -1;
	const char* row[4] = { "Controls", "Help", "More...", "Close" };
	for (int i = 0; i < 4; i++)
		if (uiButton(4 + i * 80, 208, 76, 28, i == 3 ? w.game.gmst("sclose", "Close") : row[i], focus == 7 + i)
			|| (focus == 7 + i && (in.down & KEY_A)))
			b = i;
	if (focus >= 7 && (in.down & KEY_RIGHT)) focus = focus == 10 ? 7 : focus + 1;
	else if (focus >= 7 && (in.down & KEY_LEFT)) focus = focus == 7 ? 10 : focus - 1;
	if (b == 0)
	{
		closeScreen();
		openScreen(SCR_CONTROLS);
	}
	else if (b == 1)
	{
		closeScreen();
		helpPage = 0;
		focus = 0;
		openScreen(SCR_HELP);
	}
	else if (b == 2)
	{
		closeScreen();
		openScreen(SCR_MOREOPTIONS);
	}
	else if (b == 3 || (in.down & KEY_B))
		closeScreen();
}

// ---- The settings screens (Graphics, Performance, Controls, Interface, Cheats, Teleport): one list of rows each.
// Six rows show at once; D-pad up / down (or L / R for a page) moves through the list, left / right adjusts a slider row,
// A or a tap changes the row, and the bar on the right can be dragged. focus 0..n-1 are the rows, n the Close button.
struct RowListResult { int hit; int adjust; bool close; };

static RowListResult drawRowList(const std::vector<std::string>& rows, int& focus, int& top, const std::string& closeLabel)
{
	const UiInput& in = uiIn();
	const int n = (int)rows.size(), kVisible = 6;
	const float kListY = 30.0f, kListH = 168.0f;
	const bool bar = n > kVisible;
	const int maxTop = bar ? n - kVisible : 0;
	RowListResult r = { -1, 0, false };
	if (focus < 0 || focus > n) focus = 0;
	if (top < 0) top = 0;
	if (top > maxTop) top = maxTop;
	int before = focus;
	if (in.down & KEY_DOWN) focus = focus == n ? 0 : focus + 1;
	else if (in.down & KEY_UP) focus = focus == 0 ? n : focus - 1;
	else if (in.down & KEY_R) focus = focus >= n ? n : (focus + kVisible > n - 1 ? n - 1 : focus + kVisible);
	else if (in.down & KEY_L) focus = focus >= n ? n - 1 : (focus - kVisible < 0 ? 0 : focus - kVisible);
	if (focus != before && focus < n)
	{
		if (focus < top) top = focus;
		if (focus >= top + kVisible) top = focus - kVisible + 1;
	}
	if (bar && in.touching && in.touchX >= 300 && in.touchY >= kListY && in.touchY < kListY + kListH)
		top = (int)roundf(fmaxf(0.0f, fminf(1.0f, (in.touchY - kListY) / kListH)) * maxTop);
	if (focus < n && (in.down & KEY_LEFT)) r.adjust = -1;
	else if (focus < n && (in.down & KEY_RIGHT)) r.adjust = 1;
	for (int v = 0; v < kVisible && top + v < n; v++)
	{
		int i = top + v;
		bool tapped = uiButton(4, kListY + v * 28.0f, bar ? 292 : 312, 25, rows[i], focus == i);
		if (tapped)
			focus = i;
		if (tapped || (focus == i && (in.down & KEY_A)))
			r.hit = i;
	}
	if (bar)
	{
		uiRect(302, kListY, 10, kListH, col::panel);
		float th = kListH * kVisible / n;
		uiRect(302, kListY + (kListH - th) * top / (float)maxTop, 10, th, col::header);
	}
	if (uiButton(104, 208, 112, 28, closeLabel, focus == n) || (focus == n && (in.down & KEY_A)) || (in.down & KEY_B))
		r.close = true;
	return r;
}

// A slider row: A / a tap steps up (and wraps), left / right step it down / up
static bool slideRow(const RowListResult& r, int focus, int idx, int& v, int lo, int hi, int step)
{
	if (r.hit == idx)
	{
		v = v >= hi ? lo : v + step;
		return true;
	}
	if (r.adjust && focus == idx)
	{
		int nv = v + r.adjust * step;
		nv = nv < lo ? lo : nv > hi ? hi : nv;
		if (nv != v)
		{
			v = nv;
			return true;
		}
	}
	return false;
}

static bool toggleRow(const RowListResult& r, int idx, bool& v)
{
	if (r.hit != idx)
		return false;
	v = !v;
	return true;
}

// Graphics: what is drawn and how it looks
void Session::drawGraphics()
{
	header("Graphics");
	std::vector<std::string> rows = {
		"Draw distance: " + std::to_string(g_drawDistance) + "%",
		g_fogVisible ? "Fog: On" : "Fog: Off",
		g_weatherOff ? "Weather: Off" : "Weather: On",
		"Brightness: " + std::to_string(g_brightness) + "%" };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	bool changed = false;
	changed |= slideRow(r, focus, 0, g_drawDistance, 25, 100, 5);
	if (toggleRow(r, 1, g_fogVisible))
	{
		notify(g_fogVisible ? "Fog on." : "Fog off: the view ends sharply at the draw distance.");
		changed = true;
	}
	if (toggleRow(r, 2, g_weatherOff))
	{
		notify(g_weatherOff ? "Weather off: always clear." : "Weather on.");
		changed = true;
	}
	changed |= slideRow(r, focus, 3, g_brightness, 70, 130, 10);
	if (changed)
		saveSettings();
	if (r.close)
	{
		closeScreen();
		focus = 6;
		openScreen(SCR_GAMEMENU);
	}
}

// Performance presets: draw distance, the Performance switches, character and clutter distance
struct PerfPreset { int dist; unsigned off; int actor, clutter; };
static const char* const kPresetNames[3] = { "Low", "Balanced", "High" };
static const PerfPreset kPresets[3] = {
	{ 50, PERF_LAYERS | PERF_SHIMMER | PERF_DISTANT, 50, 25 },
	{ 75, PERF_SHIMMER, 75, 50 },
	{ 100, 0, 100, 100 } };

static int currentPreset()
{
	for (int i = 0; i < 3; i++)
		if (g_drawDistance == kPresets[i].dist && g_renderOff == kPresets[i].off && g_actorDist == kPresets[i].actor
			&& g_clutterDist == kPresets[i].clutter)
			return i;
	return -1;
}

// Performance: everything that costs frames
void Session::drawPerformance()
{
	header("Performance");
	static const char* const kNames[6] = { "Sky & clouds", "Terrain layers", "Transparency", "Characters", "Distant land", "Enchant shimmer" };
	static const unsigned kBits[6] = { PERF_SKY, PERF_LAYERS, PERF_BLENDED, PERF_ACTORS, PERF_DISTANT, PERF_SHIMMER };
	int preset = currentPreset();
	std::vector<std::string> rows;
	rows.push_back(std::string("Preset: ") + (preset >= 0 ? kPresetNames[preset] : "Custom"));
	rows.push_back(g_frameCapUser == 0 ? std::string("Frame cap: Auto") : "Frame cap: " + std::to_string(g_frameCapUser));
	rows.push_back("Character distance: " + std::to_string(g_actorDist) + "%");
	rows.push_back("Clutter distance: " + std::to_string(g_clutterDist) + "%");
	for (int i = 0; i < 6; i++)
		rows.push_back(std::string(kNames[i]) + ((g_renderOff & kBits[i]) ? ": Off" : ": On"));
	rows.push_back(g_showFps ? "FPS counter: On" : "FPS counter: Off");
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	bool changed = false;
	if (r.hit == 0 || (r.adjust && focus == 0))
	{
		int next = ((preset < 0 ? -1 : preset) + (r.adjust < 0 ? 2 : 1) + 3) % 3;
		g_drawDistance = kPresets[next].dist;
		g_renderOff = kPresets[next].off;
		g_actorDist = kPresets[next].actor;
		g_clutterDist = kPresets[next].clutter;
		notify(std::string("Performance preset: ") + kPresetNames[next] + ".");
		changed = true;
	}
	if (r.hit == 1 || (r.adjust && focus == 1))
	{
		g_frameCapUser = g_frameCapUser == 0 ? 30 : g_frameCapUser == 30 ? 60 : 0;
		notify(g_frameCapUser == 0 ? "Frame cap: automatic." : g_frameCapUser == 30 ? "Frame cap: 30 fps." : "Frame cap: 60 fps.");
		changed = true;
	}
	changed |= slideRow(r, focus, 2, g_actorDist, 25, 200, 25);
	changed |= slideRow(r, focus, 3, g_clutterDist, 25, 200, 25);
	for (int i = 0; i < 6; i++)
		if (r.hit == 4 + i)
		{
			g_renderOff ^= kBits[i];
			changed = true;
		}
	if (toggleRow(r, 10, g_showFps))
		changed = true;
	if (changed)
		saveSettings();
	if (r.close)
	{
		closeScreen();
		focus = 7;
		openScreen(SCR_GAMEMENU);
	}
}

// Controls: sticks, running, button layout, gyro
// Controls: sticks, running, button layout, gyro, footsteps
void Session::drawCtrlSet()
{
	header("Controls");
	static const int kGyroSteps[4] = { 0, 50, 100, 200 };
	char gyro[40];
	snprintf(gyro, sizeof(gyro), g_gyroAim > 0 ? "Gyro aim: %.1fx" : "Gyro aim: Off", g_gyroAim / 100.0f);
	std::vector<std::string> rows = {
		"Look sens: " + std::to_string(g_cstickSens) + "%",
		g_invertY ? "Invert Y look: On" : "Invert Y look: Off",
		g_swapSticks ? "Sticks: Swapped" : "Sticks: Default",
		g_autoRun ? "Always run: On" : "Always run: Off",
		std::string("Buttons: ") + (g_controlLayout == CONTROLS_CLASSIC ? "Classic" : g_controlLayout == CONTROLS_GOLDENEYE ? "GoldenEye" : "Xbox"),
		gyro,
		g_footsteps ? "Footstep sounds: On" : "Footstep sounds: Off",
		virtualSticks ? "Virtual Sticks: On" : "Virtual Sticks: Off",
		"Custom Bindings...",
		"Button help..." };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	bool changed = false;
	changed |= slideRow(r, focus, 0, g_cstickSens, 25, 200, 25);
	changed |= toggleRow(r, 1, g_invertY);
	if (toggleRow(r, 2, g_swapSticks))
	{
		notify(g_swapSticks ? "Sticks swapped: C-stick walks, Circle Pad looks." : "Sticks default: Circle Pad walks, C-stick looks.");
		changed = true;
	}
	if (toggleRow(r, 3, g_autoRun))
	{
		notify(g_autoRun ? "Always run: hold the run button to walk." : "Hold the run button to run.");
		changed = true;
	}
	if (r.hit == 4 || (r.adjust && focus == 4))
	{
		g_controlLayout = (g_controlLayout + (r.adjust < 0 ? 2 : 1)) % 3;
		notify(g_controlLayout == CONTROLS_CLASSIC ? "Classic buttons." : g_controlLayout == CONTROLS_GOLDENEYE ? "GoldenEye buttons." : "Xbox buttons.");
		changed = true;
	}
	if (r.hit == 5)
	{
		int k = 0;
		while (k < 3 && kGyroSteps[k] != g_gyroAim) k++;
		g_gyroAim = kGyroSteps[(k + 1) % 4];
		notify(g_gyroAim > 0 ? "Gyro aim on: turn the 3DS to aim." : "Gyro aim off.");
		changed = true;
	}
	if (toggleRow(r, 6, g_footsteps))
	{
		notify(g_footsteps ? "Footstep sounds on." : "Footstep sounds off.");
		changed = true;
	}
	if (changed) saveSettings();
	if (toggleRow(r, 7, virtualSticks))
	{
		notify(virtualSticks ? "Virtual sticks enabled on the bottom screen." : "Virtual sticks disabled.");
	}
	if (r.hit == 8)
	{
		closeScreen();
		openScreen(SCR_KEYREMAP);
	}
	else if (r.hit == 9)
	{
		closeScreen();
		openScreen(SCR_CONTROLS);
	}
	else if (r.close)
	{
		closeScreen();
		focus = 8;
		openScreen(SCR_GAMEMENU);
	}
}

void Session::drawKeyRemap()
{
	header("Customize Controls");
	
	if (Keymap::waitingForKey) {
		uiTextCentered(160, 90, 0.6f, col::header, "Press any button to bind...");
		uiTextCentered(160, 120, 0.45f, col::textDim, "Press B to cancel");
		
		u32 down = hidKeysDown();
		if (down) {
			if (down & KEY_B) {
				Keymap::waitingForKey = false;
			} else {
				u32 newKey = 0;
				if (down & KEY_A) newKey = KEY_A;
				else if (down & KEY_B) newKey = KEY_B;
				else if (down & KEY_X) newKey = KEY_X;
				else if (down & KEY_Y) newKey = KEY_Y;
				else if (down & KEY_L) newKey = KEY_L;
				else if (down & KEY_R) newKey = KEY_R;
				else if (down & KEY_ZL) newKey = KEY_ZL;
				else if (down & KEY_ZR) newKey = KEY_ZR;
				else if (down & KEY_DUP) newKey = KEY_DUP;
				else if (down & KEY_DDOWN) newKey = KEY_DDOWN;
				else if (down & KEY_DLEFT) newKey = KEY_DLEFT;
				else if (down & KEY_DRIGHT) newKey = KEY_DRIGHT;
				else if (down & KEY_START) newKey = KEY_START;
				else if (down & KEY_SELECT) newKey = KEY_SELECT;
				
				if (newKey) {
					Keymap::setKey(Keymap::actionToRebind, newKey);
					Keymap::waitingForKey = false;
					Keymap::save();
					playSound(-1, "Menu Click");
				}
			}
		}
		return;
	}

	std::vector<std::string> rows;
	int total = (int)InputAction::Count;
	for (int i = 0; i < total; i++) {
		InputAction act = (InputAction)i;
		rows.push_back(std::string(Keymap::actionName(act)) + ": " + Keymap::keyName(Keymap::getKey(act)));
	}

	RowListResult r = drawRowList(rows, remapSelection, s_gfxTop, w.game.gmst("sclose", "Close"));
	
	if (r.hit >= 0 && r.hit < total) {
		Keymap::actionToRebind = (InputAction)r.hit;
		Keymap::waitingForKey = true;
		playSound(-1, "Menu Click");
	}
	
	// --- Reset to defaults button ---
	static bool s_resetConfirm = false;
	if (uiButton(104, 208, 112, 28, s_resetConfirm ? "Yes, reset" : "Reset Defaults"))
	{
		if (!s_resetConfirm)
		{
			s_resetConfirm = true;
			playSound(-1, "Menu Click");
		}
		else
		{
			Keymap::resetDefaults();
			Keymap::save();
			s_resetConfirm = false;
			playSound(-1, "Menu Click");
			notify("Controls reset to default.");
		}
	}
	else if (s_resetConfirm && uiHit(104, 208, 112, 28) == false)
	{
		// any tap elsewhere cancels the confirm state
	}
	// Also clear confirm if user presses B / selects another row
	if (r.hit >= 0)
		s_resetConfirm = false;

	if (r.close) {
		s_resetConfirm = false;
		closeScreen();
		openScreen(SCR_CONTROLS);
	}
}

// Readable UI: colour names for the map marker palette (must match screens_map.cpp)
static const char* mapColorName(int idx)
{
	static const char* names[] = { "Blue", "Red", "Green", "Yellow", "Cyan", "Magenta", "White", "Orange" };
	return names[(idx < 0 || idx > 7) ? 0 : idx];
}

// Interface: what the screens show
void Session::drawInterface()
{
	header("Interface");
	std::vector<std::string> rows = {
		g_subtitles ? "Subtitles: On" : "Subtitles: Off",
		g_hideCrosshair ? "Crosshair: Hidden" : "Crosshair: Shown",
		g_hideHud ? "Top screen HUD: Hidden" : "Top screen HUD: Shown",
		g_hideMinimap ? "Minimap: Hidden" : "Minimap: Shown",
		g_shotHideHud ? "Screenshot hides HUD: On" : "Screenshot hides HUD: Off",
		g_autosaveMin > 0 ? "Autosave: every " + std::to_string(g_autosaveMin) + " min" : std::string("Autosave: Off"),
		g_fontMode == 0 ? "Font: Morrowind" : g_fontMode == 1 ? "Font: Clear (system)" : "Font: Clear, larger",
		"Text size: " + std::to_string(g_textSize) + "%",
		g_mapBlue ? "Map marker: Clear (outlined)" : "Map marker: Original",
		g_mapBlue ? std::string("Map colour: ") + mapColorName(g_mapColorIdx) : "Map colour: (off)" };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	bool changed = false;
	changed |= toggleRow(r, 0, g_subtitles);
	changed |= toggleRow(r, 1, g_hideCrosshair);
	changed |= toggleRow(r, 2, g_hideHud);
	changed |= toggleRow(r, 3, g_hideMinimap);
	changed |= toggleRow(r, 4, g_shotHideHud);
	if (r.hit == 5 || (r.adjust && focus == 5))
	{
		static const int kMinutes[5] = { 0, 5, 10, 15, 30 };
		int k = 0;
		while (k < 4 && kMinutes[k] != g_autosaveMin)
			k++;
		g_autosaveMin = kMinutes[(k + (r.adjust < 0 ? 4 : 1)) % 5];
		notify(g_autosaveMin > 0 ? "Autosave on: it saves over your current save." : "Autosave off.");
		changed = true;
	}
	if (r.hit == 6 || (r.adjust && focus == 6))      // Font type
	{
		uiSetFontMode((g_fontMode + (r.adjust < 0 ? 2 : 1)) % 3);
		changed = true;
	}
	if (r.hit == 7 || (r.adjust && focus == 7))      // Text size (system font)
	{
		static const int kSizes[4] = { 90, 100, 115, 130 };
		int k = 0;
		while (k < 3 && kSizes[k] != g_textSize)
			k++;
		g_textSize = kSizes[(k + (r.adjust < 0 ? 3 : 1)) % 4];
		uiSetFontMode(g_fontMode);   // clears wrap cache so new size takes effect
		changed = true;
	}
	if (r.hit == 8 || (r.adjust && focus == 8))      // Map marker style
	{
		g_mapBlue = !g_mapBlue;
		changed = true;
	}
	if (r.hit == 9 || (r.adjust && focus == 9))      // Map colour (only when clear marker is on)
	{
		if (g_mapBlue)
		{
			g_mapColorIdx = (g_mapColorIdx + (r.adjust < 0 ? 7 : 1)) % 8;
			changed = true;
		}
	}
	if (changed)
		saveSettings();
	if (r.close)
	{
		closeScreen();
		focus = 9;
		openScreen(SCR_GAMEMENU);
	}
}

// More options: the extras that did not fit on the Options screen; kept in settings.txt
void Session::drawMoreOptions()
{
	header("More options");
	const UiInput& in = uiIn();
	// focus: 0 look sensitivity, 1 subtitles, 2 brightness, 3 cheats, 4 leveling, 5 leveling for new characters, 6 close
	if (focus < 0 || focus > 6) focus = 0;
	if (in.down & KEY_DOWN) focus = focus == 6 ? 0 : focus + 1;
	else if (in.down & KEY_UP) focus = focus == 0 ? 6 : focus - 1;
	bool changed = false;
	{
		std::string lab = "Look sens: " + std::to_string(g_cstickSens) + "%";
		if (uiButton(4, 30, 312, 26, lab, focus == 0) || (focus == 0 && (in.down & KEY_A)))
		{
			g_cstickSens = g_cstickSens >= 200 ? 25 : g_cstickSens + 25;
			changed = true;
		}
		if (focus == 0 && (in.down & KEY_LEFT) && g_cstickSens > 25) { g_cstickSens -= 25; changed = true; }
		if (focus == 0 && (in.down & KEY_RIGHT) && g_cstickSens < 200) { g_cstickSens += 25; changed = true; }
	}
	if (uiButton(4, 62, 312, 26, g_subtitles ? "Subtitles: On" : "Subtitles: Off", focus == 1)
		|| (focus == 1 && (in.down & KEY_A)))
	{
		g_subtitles = !g_subtitles;
		changed = true;
	}
	{
		std::string lab = "Brightness: " + std::to_string(g_brightness) + "%";
		if (uiButton(4, 94, 312, 26, lab, focus == 2) || (focus == 2 && (in.down & KEY_A)))
		{
			g_brightness = g_brightness >= 130 ? 70 : g_brightness + 10;
			changed = true;
		}
		if (focus == 2 && (in.down & KEY_LEFT) && g_brightness > 70) { g_brightness -= 10; changed = true; }
		if (focus == 2 && (in.down & KEY_RIGHT) && g_brightness < 130) { g_brightness += 10; changed = true; }
	}
	if (changed)
		saveSettings();
	if (uiButton(4, 126, 152, 26, "Cheats...", focus == 3) || (focus == 3 && (in.down & KEY_A)))
	{
		closeScreen();
		openScreen(SCR_CHEATS);
	}
	if (uiButton(164, 126, 152, 26, w.stats.xpLeveling ? "Leveling: XP" : "Leveling: Skills", focus == 4)
		|| (focus == 4 && (in.down & KEY_A)))
	{
		setXpLeveling(!w.stats.xpLeveling);
		notify(w.stats.xpLeveling ? "XP leveling: quests, kills and discoveries bring XP."
			: "Classic leveling: ten major or minor skill increases make a level.");
	}
	if (uiButton(4, 158, 312, 26, g_xpDefault ? "New characters: XP leveling" : "New characters: skill leveling", focus == 5)
		|| (focus == 5 && (in.down & KEY_A)))
	{
		g_xpDefault = !g_xpDefault;
		saveSettings();
	}
	if (uiButton(104, 208, 112, 28, w.game.gmst("sclose", "Close"), focus == 6)
		|| (focus == 6 && (in.down & KEY_A)) || (in.down & KEY_B))
	{
		closeScreen();
		openScreen(SCR_OPTIONS);
	}
}

// Cheats: nothing here is saved as a setting (they are off again next time the game starts)
void Session::drawCheats()
{
	header("Cheats");
	std::vector<std::string> rows = {
		"Restore health, magicka, fatigue",
		"Add 1000 gold",
		g_godMode ? "God mode: On" : "God mode: Off",
		g_infMagic ? "Infinite magicka & fatigue: On" : "Infinite magicka & fatigue: Off",
		g_noWeight ? "Weightless: On" : "Weightless: Off",
		"Walk speed: " + std::to_string(g_cheatSpeed) + "%",
		"Jump power: " + std::to_string(g_cheatJump) + "%",
		"All skills +10",
		"All attributes +10",
		"Teleport to a town..." };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	if (r.hit == 0)
	{
		w.stats.health = w.stats.healthMax;
		w.stats.magicka = w.stats.magickaMax;
		w.stats.fatigue = w.stats.fatigueMax;
		notify("Health, magicka and fatigue restored.");
	}
	else if (r.hit == 1)
	{
		giveItems({ { 1000, "gold_001" } });
		notify("1000 gold added.");
	}
	if (toggleRow(r, 2, g_godMode))
		notify(g_godMode ? "God mode on (a single blow bigger than your health can still kill)." : "God mode off.");
	if (toggleRow(r, 3, g_infMagic))
		notify(g_infMagic ? "Infinite magicka and fatigue on." : "Infinite magicka and fatigue off.");
	if (toggleRow(r, 4, g_noWeight))
		notify(g_noWeight ? "Weightless: you can carry anything." : "Carry weight back to normal.");
	slideRow(r, focus, 5, g_cheatSpeed, 100, 400, 50);
	slideRow(r, focus, 6, g_cheatJump, 100, 300, 25);
	if (r.hit == 7)
	{
		for (int k = 0; k < 27; k++)
		{
			w.stats.skills[k] += 10;
			if (w.stats.skills[k] > 100)
				w.stats.skills[k] = 100;
		}
		w.recomputeStats();
		notify("All skills +10 (at most 100).");
	}
	else if (r.hit == 8)
	{
		for (int k = 0; k < 8; k++)
		{
			w.stats.attributes[k] += 10;
			if (w.stats.attributes[k] > 100)
				w.stats.attributes[k] = 100;
		}
		w.recomputeStats();
		notify("All attributes +10 (at most 100).");
	}
	else if (r.hit == 9)
	{
		closeScreen();
		focus = 0;
		s_gfxTop = 0;
		openScreen(SCR_TELEPORT);
	}
	else if (r.close)
	{
		closeScreen();
		focus = 10;
		openScreen(SCR_GAMEMENU);
	}
}

// "balmora" -> "Balmora", "sadrith mora" -> "Sadrith Mora"
static std::string titleCase(std::string s)
{
	bool up = true;
	for (char& c : s)
	{
		if (up && c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		up = c == ' ';
	}
	return s;
}

// PC Link: connect this 3DS to pclink_server.py on the PC (screen mirror, PC controller, console commands)
void Session::drawPcLink()
{
	header("PC Link");
	const char* addr = pclink_get_address();
	std::vector<std::string> rows = {
		std::string("Status: ") + pclink_status(),
		pclink_connected() ? "Disconnect from PC" : "Connect to PC",
		std::string("PC address: ") + (addr[0] ? addr : "(tap to type it)"),
		std::string("3DS address: ") + pclink_local_address() };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	if (r.hit == 1)
	{
		if (pclink_connected())
			pclink_disconnect();
		else
			pclink_connect(nullptr, 0);
	}
	else if (r.hit == 2)
		g_pcAskAddress = true;   // main.cpp opens the keyboard between frames
	else if (r.close)
	{
		closeScreen();
		focus = 11;
		openScreen(SCR_GAMEMENU);
	}
}

// Multiplayer: host / join another MW3DS over Wi-Fi (peer to peer), chat, ghost players
void Session::drawMultiplayer()
{
	header("Multiplayer");
	static bool s_loaded = false;
	if (!s_loaded)
	{
		s_loaded = true;
		mpLoadSettings();
		if (!mpGetSavedName().empty()) mpHostName = mpGetSavedName();
	}
	bool on = mpMode != 0;
	std::string status = mpMode == 1 ? (net.isConnected() ? "Hosting: player connected" : "Hosting: waiting for a player")
		: mpMode == 2 ? (net.isConnected() ? "Connected to host" : "Join dropped") : "Not connected";
	std::vector<std::string> rows = {
		"Status: " + status,
		mpMode == 1 ? "Stop hosting" : "Host a game",
		mpMode == 2 ? "Disconnect" : "Join a game",
		"Host IP: " + (g_mpHostIp.empty() ? std::string("(tap to type it)") : g_mpHostIp),
		"Your name: " + mpHostName,
		"Send a chat message",
		"Spawn test ghost",
		"Remove all ghosts",
		"Open console",
		"Go to the other player's cell",
		std::string("Your 3DS IP: ") + pclink_local_address() };
	RowListResult r = drawRowList(rows, focus, s_gfxTop, w.game.gmst("sclose", "Close"));
	if (r.hit == 1) { if (mpMode == 1) mpDisconnect(); else mpHost(); }
	else if (r.hit == 2) { if (mpMode == 2) mpDisconnect(); else mpJoin(); }
	else if (r.hit == 3) g_mpAsk = 1;
	else if (r.hit == 4) g_mpAsk = 2;
	else if (r.hit == 5) { if (on) g_mpAsk = 3; else notify("Host or join first"); }
	else if (r.hit == 6)
	{
		float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
		float pos[3] = { w.player.feet[0] + fx * 100.0f, w.player.feet[1] + fy * 100.0f, w.player.feet[2] };
		notify(spawnGhost("Ghost", pos, w.player.yaw + 3.14159f) >= 0 ? "Ghost spawned in front of you" : "Ghost spawn failed");
	}
	else if (r.hit == 7) notify("Removed " + std::to_string(removeAllGhosts()) + " ghosts");
	else if (r.hit == 8) { closeScreen(); openConsole(); }
	else if (r.hit == 9)
	{
		if (on && net.isConnected()) { closeScreen(); mpGotoPeer(); }
		else notify("Host or join first");
	}
	else if (r.close)
	{
		closeScreen();
		focus = 5;
		openScreen(SCR_GAMEMENU);
	}
}

// Teleport: every town the converted game data names a street spot for (the way a new game starts there)
void Session::drawTeleport()
{
	header("Teleport");
	
	static std::vector<std::string> allCells;
	if (allCells.empty()) {
		for (auto& cell : w.game.cells) {
			std::string name = cell.name;
			if (name.empty() && !cell.region.empty()) {
				name = "Wilderness (" + cell.region + ")";
			}
			if (name.empty()) continue;
			allCells.push_back(name);
		}
		std::sort(allCells.begin(), allCells.end());
	}

	// Filter by search
	std::vector<std::string> filtered;
	std::string lowerSearch = lower(teleportSearch);
	for (auto& n : allCells) {
		if (lowerSearch.empty() || lower(n).find(lowerSearch) != std::string::npos)
			filtered.push_back(n);
	}

	if (filtered.empty()) {
		uiTextCentered(160, 100, 0.5f, col::textDim, "No results");
	} else {
		int pick = uiList(list2, 4, 24, 312, 156, filtered, 0.42f, true);
		if (pick >= 0 && pick < (int)filtered.size()) {
			closeScreen();
			notify("Travelling to " + filtered[pick] + "...");
			testGoto(filtered[pick]);
			return;
		}
	}

	// Search button
	if (uiButton(4, 184, 154, 20, teleportSearch.empty() ? "Search..." : "Search: " + teleportSearch)) {
		wantText = TEXT_TELEPORT;
	}
	// Clear button
	if (!teleportSearch.empty() && uiButton(162, 184, 154, 20, "Clear Search")) {
		teleportSearch.clear();
	}

	int noFocus = -1;
	if (uiButton(104, 208, 112, 28, w.game.gmst("sclose", "Close")) || (uiIn().down & KEY_B)) {
		closeScreen();
	}
}

// What each button does
void Session::drawControls()
{
	bool xbox = g_controlLayout == CONTROLS_XBOX, goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	header(xbox ? "Controls (Xbox layout)" : goldenEye ? "Controls (GoldenEye layout)" : "Controls (classic layout)");
	static const char* kXbox[][2] = {
		{ "Circle Pad / C-Stick", "Move / look" }, { "A", "Activate: talk, open, take" },
		{ "R (hold, release)", "Wind up, strike" }, { "X", "Ready / put away the weapon" }, { "L", "Cast the selected spell" },
		{ "Y", "Jump (swim up in water)" }, { "B (hold)", "Run / walk" }, { "ZL (hold)", "Sneak" },
		{ "D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	static const char* kClassic[][2] = {
		{ "Circle Pad", "Move" }, { "C-Stick / D-Pad", "Look" }, { "A", "Activate: talk, open, take" },
		{ "X (hold, release)", "Ready weapon, wind up, strike" }, { "ZL + X", "Put the weapon away" }, { "Y", "Cast the selected spell" },
		{ "B", "Jump (swim up in water)" }, { "ZR (hold)", "Run / walk" }, { "L (hold)", "Sneak" },
		{ "ZL + D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	// One stick (an Old 3DS without the C-stick) on the Xbox buttons: the pad walks and turns; an aim button held
	// stands still and the pad aims
	static const char* kGoldenEye[][2] = {
		{ "Circle Pad", "Walk (up, down), turn" }, { "X (hold) / ZL / ZR", "Aim: stand still, the pad aims" },
		{ "X (tap)", "Ready / put away the weapon" }, { "R (hold, release)", "Wind up (aiming), strike" },
		{ "L", "Cast the selected spell" }, { "A", "Activate: talk, open, take" },
		{ "Y", "Jump (swim up in water)" }, { "B (hold)", "Run" }, { "SELECT", "Sneak on / off" },
		{ "D-Pad", "Left / right: strafe; up / down: look" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Menus, quick keys; 1st / 3rd" } };
	float y = 30;
	int n = xbox ? sizeof(kXbox) / sizeof(kXbox[0]) : goldenEye ? sizeof(kGoldenEye) / sizeof(kGoldenEye[0])
		: sizeof(kClassic) / sizeof(kClassic[0]);
	for (int i = 0; i < n; i++)
	{
		const char* const* l = xbox ? kXbox[i] : goldenEye ? kGoldenEye[i] : kClassic[i];
		uiText(14, y, 0.42f, col::header, l[0]);
		uiText(140, y, 0.42f, col::text, l[1]);
		y += 13.5f;
	}
	// Y: the next layout (Xbox, classic, GoldenEye); A / B: back to Options
	if (uiButton(40, 208, 130, 28, xbox ? "Y: Use classic" : goldenEye ? "Y: Use Xbox" : "Y: Use GoldenEye") || (uiIn().down & KEY_Y))
	{
		g_controlLayout = xbox ? CONTROLS_CLASSIC : goldenEye ? CONTROLS_XBOX : CONTROLS_GOLDENEYE;
		saveSettings();
	}
	else if (uiButton(180, 208, 100, 28, w.game.gmst("sclose", "Close"), true) || (uiIn().down & (KEY_A | KEY_B)))
	{
		closeScreen();
		openScreen(SCR_OPTIONS);
	}
}

// Save a new game or load any save (autosave, your saves, bundled starting points)
void Session::drawSaves()
{
	header("Saves");
	int pick = uiSaveList(list, saveList, 4, 24, 312, 176, !deletingSave);
	if (saveList.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "No saves yet");
	// Saving mid-chargen would lose the menus the scripts queued, so wait until they're done
	bool canSave = pendingMenus.empty() && !wantName && !playerDead;     // (dead: load only)
	bool canLoad = list.selected >= 0 && list.selected < (int)saveList.size();
	// (the starting points that come with the game stay)
	bool canDelete = canLoad && !saveList[list.selected].bundled;
	if (deletingSave)
	{
		// "Delete <name>?" Yes / No
		if (!canDelete)
			deletingSave = false;
		else
		{
			// (a solid box over the list, and its two buttons inside it, not across the whole screen as a button row)
			uiRect(20, 70, 280, 100, C2D_Color32(0, 0, 0, 255));
			uiPanel(20, 70, 280, 100);
			uiTextBox(scroll, 26, 76, 268, 50, "Delete " + saveList[list.selected].title + "?", 0.5f, false);
			if (uiIn().down & (KEY_LEFT | KEY_RIGHT))
				focus = 1 - focus;
			int yn = -1;
			static const char* kAnswer[2] = { "Yes", "No" };
			for (int i = 0; i < 2; i++)
				if (uiButton(34 + i * 132, 132, 120, 28, kAnswer[i], i == focus))
					yn = i;
			if (yn == 0)
			{
				remove(saveList[list.selected].path.c_str());
				remove((saveList[list.selected].path + ".meta").c_str());
				logf("saves: deleted %s", saveList[list.selected].path.c_str());
				saveList = savesList(dataDir);
				if (list.selected >= (int)saveList.size())
					list.selected = (int)saveList.size() - 1;
				notify("Save deleted.");
			}
			if (yn >= 0 || (uiIn().down & KEY_B))
			{
				deletingSave = false;
				focus = 0;
			}
		}
		return;
	}
	int b = buttonRow({ canSave ? "New save" : playerDead ? "(dead)" : "(finish chargen)", "Load", "Delete", "Back" }, focus);
	if (b == 2 && canDelete)
	{
		deletingSave = true;
		focus = 1;               // (No)
	}
	else if (b == 0 && canSave)
	{
		std::string path = saveNewPath();
		if (w.saveGame(path.c_str()))
			notify("Game saved.");
		saveList = savesList(dataDir);
	}
	else if ((b == 1 || (pick >= 0 && (uiIn().down & KEY_A))) && canLoad)
	{
		reloadPath = saveList[list.selected].path;
		wantReload = true;
		closeScreen();
	}
	else if (b == 3 || (uiIn().down & KEY_B))
	{
		if (playerDead)
			openScreen(SCR_DEATH);     // back to the death screen: no way back into the game but a load
		else
			closeScreen();
	}
}

void Session::drawVirtualSticks()
{
	if (!virtualSticks) return;

	// Left stick (move)
	uiRect(20, 140, 60, 60, C2D_Color32(0, 0, 0, 100));
	uiFrame(20, 140, 60, 60, "menu_thin_border", col::textDim);
	float lx = 50 + virtualStickLX * 20;
	float ly = 170 - virtualStickLY * 20;
	uiRect(lx - 10, ly - 10, 20, 20, col::select);
	uiFrame(lx - 10, ly - 10, 20, 20, "menu_button_frame", col::text);

	// Right stick (look)
	uiRect(240, 140, 60, 60, C2D_Color32(0, 0, 0, 100));
	uiFrame(240, 140, 60, 60, "menu_thin_border", col::textDim);
	float rx = 270 + virtualStickRX * 20;
	float ry = 170 - virtualStickRY * 20;
	uiRect(rx - 10, ry - 10, 20, 20, col::select);
	uiFrame(rx - 10, ry - 10, 20, 20, "menu_button_frame", col::text);
}

void Session::drawMessage()
{
	MessageState& m = messages.front();
	uiPanel(8, 8, 304, 224);
	const UiInput& in = uiIn();
	int n = m.buttons.size();
	if (m.text != messageShown)
	{
		messageShown = m.text;
		messageScroll = UiScroll();
	}
	// The text above a row of buttons when it fits there; else the buttons go to a column on the right and the
	// text gets the whole height on the left, scrolling a line at a time (drag it, or the circle pad) with a bar
	// that shows how much more there is
	float lh = uiLineHeight(0.5f);
	std::vector<std::string> lines = uiWrap(m.text, 284, 0.5f);
	float bh = 26.0f;
	messageSidebar = lines.size() * lh > 224 - n * (bh + 4) - 4 - 12;
	float tx = 14, tw = 292, ty = 12, th = 224 - n * (bh + 4) - 4 - 12;
	// (a column button's label wraps onto a second line rather than being cut short)
	const float bx = 188, bw = 118, bs = 0.42f;
	std::vector<std::vector<std::string>> labels(n);
	std::vector<float> by(n), bhs(n);
	if (messageSidebar)
	{
		tw = 166;
		th = 216;
		lines = uiWrap(m.text, tw - 8, 0.5f);
		float y = 16, blh = uiLineHeight(bs);
		for (int i = 0; i < n; i++)
		{
			labels[i] = uiWrap(m.buttons[i], bw - 10, bs);
			if (labels[i].size() > 2)
				labels[i].resize(2);
			by[i] = y;
			bhs[i] = fmaxf(24.0f, labels[i].size() * blh + 8);
			y += bhs[i] + 4;
		}
	}
	int rows = std::max(1, (int)(th / lh)), total = (int)lines.size();
	int maxTop = std::max(0, total - rows);
	if (in.touching && in.touchX >= tx && in.touchX < tx + tw && in.touchY >= ty && in.touchY < ty + th)
		messageScroll.scroll -= in.dragDY;
	messageScroll.scroll += uiStickScroll();
	messageScroll.scroll = fmaxf(0.0f, fminf(messageScroll.scroll, maxTop * lh));
	int first = std::min(maxTop, (int)(messageScroll.scroll / lh + 0.5f));
	for (int i = 0; i < rows && first + i < total; i++)
		uiText(tx + 4, ty + i * lh, 0.5f, col::text, lines[first + i]);
	if (maxTop > 0)
	{
		float bx = tx + tw + 2, trackH = rows * lh;
		uiRect(bx, ty, 3, trackH, col::panelLight);
		uiRect(bx, ty + trackH * first / total, 3, fmaxf(6.0f, trackH * rows / total), col::header);
	}
	messageTop = first;
	messageLeft = std::max(0, total - first - rows);
	if (in.down & (KEY_DOWN | KEY_RIGHT)) focus = (focus + 1) % n;
	if (in.down & (KEY_UP | KEY_LEFT)) focus = (focus + n - 1) % n;
	if (focus >= n) focus = 0;
	int hit = -1;
	for (int i = 0; i < n; i++)
	{
		bool pick;
		if (messageSidebar)
		{
			pick = uiButton(bx, by[i], bw, bhs[i], "", i == focus);
			float blh = uiLineHeight(bs), top = by[i] + (bhs[i] - labels[i].size() * blh) / 2;
			for (size_t k = 0; k < labels[i].size(); k++)
				uiTextCentered(bx + bw / 2, top + k * blh, bs, i == focus ? col::textOver : col::text, labels[i][k]);
		}
		else
			pick = uiButton(20, 224 - n * (bh + 4) + i * (bh + 4), 280, bh, m.buttons[i], i == focus);
		if (pick)
			hit = i;
	}
	if (hit >= 0)
	{
		playSound(-1, "Menu Click");
		if (m.fromScript)
			buttonPressed = hit;
		messages.erase(messages.begin());
		focus = 0;
	}
}

// ---- dialogue

// ---- tooltips

std::string Session::effectLine(const SpellEffect& e, bool showRange)
{
	std::string s = effectLabel(e.effect, e.skill, e.attribute);
	auto me = w.game.magicEffects.find(e.effect);
	int flags = me != w.game.magicEffects.end() ? me->second.flags : 0;
	char buf[64];
	if (!(flags & 0x8) && (e.min || e.max))
	{
		const char* unit = e.max == 1 ? " pt" : " pts";
		if (e.min == e.max)
			snprintf(buf, sizeof(buf), " %d%s", e.min, unit);
		else
			snprintf(buf, sizeof(buf), " %d to %d%s", e.min, e.max, unit);
		s += buf;
	}
	if (!(flags & 0x4) && e.duration > 1)
	{
		snprintf(buf, sizeof(buf), " for %d secs", e.duration);
		s += buf;
	}
	if (e.area > 0)
	{
		snprintf(buf, sizeof(buf), " in %d ft", e.area);
		s += buf;
	}
	if (showRange)
		s += e.range == 0 ? " on Self" : e.range == 1 ? " on Touch" : " on Target";
	return s;
}

void Session::spellTip(const SpellDef& sp)
{
	tip.clear();
	tip.emplace_back(sp.name, col::header);
	static const char* kSchool[6] = { "Alteration", "Conjuration", "Destruction", "Illusion", "Mysticism", "Restoration" };
	int school = spellSchool(sp);
	if (sp.type == 0 && school >= 0 && school < 6)
		tip.emplace_back(std::string("School: ") + kSchool[school], col::textDim);
	for (auto& e : sp.effects)
		tip.emplace_back(effectLine(e, true), col::text);
}

void Session::itemTip(const Object* o, const InventoryItem* it)
{
	tip.clear();
	if (!o)
		return;
	tip.emplace_back(o->name, col::header);
	char buf[96];
	std::string typeLabel = w.game.gmst("stype", "Type");
	if (typeLabel.empty() || typeLabel.back() != ':')
		typeLabel += ":";
	// Its kind, as OpenMW's tooltips name it: a weapon's skill, one or two handed for the melee ones ("Type: Long
	// Blade, Two Handed"); armor's weight class (Light / Medium / Heavy Armor)
	if (o->type == "WEAP" && o->subtype >= 0 && o->subtype <= WEAP_BOLT)
	{
		static const int kSkill[14] = { 22, 5, 5, 4, 4, 4, 7, 6, 6, 23, 23, 23, 23, 23 };
		std::string kind = typeLabel + " " + skillName(w, kSkill[o->subtype]);
		if (o->subtype < WEAP_BOW)
			kind += ", " + (o->subtype == 0 || o->subtype == 1 || o->subtype == 3 || o->subtype == 7
				? w.game.gmst("sonehanded", "One Handed") : w.game.gmst("stwohanded", "Two Handed"));
		tip.emplace_back(kind, col::textDim);
	}
	else if (o->type == "ARMO" && o->subtype >= 0 && o->subtype < 11)
	{
		tip.emplace_back(typeLabel + " " + skillName(w, armorSkill(o)), col::textDim);    // Light / Medium / Heavy Armor
	}
	if (o->type == "WEAP")
	{
		bool ranged = o->subtype >= WEAP_BOW && o->subtype <= WEAP_BOLT;
		if (ranged)
			snprintf(buf, sizeof(buf), "Attack: %d - %d", o->chop[0], o->chop[1]), tip.emplace_back(buf, col::text);
		else
		{
			snprintf(buf, sizeof(buf), "Chop: %d - %d", o->chop[0], o->chop[1]), tip.emplace_back(buf, col::text);
			snprintf(buf, sizeof(buf), "Slash: %d - %d", o->slash[0], o->slash[1]), tip.emplace_back(buf, col::text);
			snprintf(buf, sizeof(buf), "Thrust: %d - %d", o->thrust[0], o->thrust[1]), tip.emplace_back(buf, col::text);
		}
	}
	else if (o->type == "ARMO")
	{
		snprintf(buf, sizeof(buf), "Armor Rating: %d", o->armor);
		tip.emplace_back(buf, col::text);
	}
	if ((o->type == "WEAP" || o->type == "ARMO") && o->health > 0)
	{
		int cond = it && it->condition >= 0 ? it->condition : o->health;
		snprintf(buf, sizeof(buf), "Condition: %d/%d", cond, o->health);
		tip.emplace_back(buf, col::text);
	}
	if (o->type == "LOCK" || o->type == "PROB" || o->type == "REPA")
	{
		int uses = it && it->condition >= 0 ? it->condition : o->uses;
		snprintf(buf, sizeof(buf), "Quality: %.2f   Uses: %d", o->quality, uses);
		tip.emplace_back(buf, col::text);
	}
	if (o->type == "APPA")
	{
		snprintf(buf, sizeof(buf), "Quality: %.2f", o->quality);
		tip.emplace_back(buf, col::text);
	}
	snprintf(buf, sizeof(buf), "Weight: %.1f   Value: %d", o->weight, o->value);
	tip.emplace_back(buf, col::text);
	if (it && !it->soul.empty())
	{
		std::string soul = it->soul;
		for (auto& a : w.game.actors)
			if (lower(a.id) == it->soul)
				soul = a.name;
		tip.emplace_back("Soul: " + soul, col::text);
	}
	// potions: their effects; ingredients: as many as Alchemy reveals (fWortChanceValue each)
	if (o->type == "ALCH")
		for (auto& e : o->effects)
			tip.emplace_back(effectLine(e, false), col::text);
	if (o->type == "INGR")
	{
		int known = (int)(w.stats.skills[16] / w.game.gmstf("fwortchancevalue", 15.0f));
		for (size_t k = 0; k < o->ingredient.size(); k++)
			tip.emplace_back((int)k < known ? effectLabel(o->ingredient[k].effect, o->ingredient[k].skill, o->ingredient[k].attribute)
				: std::string("?"), (int)k < known ? col::text : col::textDim);
	}
	if (o->type == "BOOK" && o->bookSkill >= 0)
		tip.emplace_back(o->scroll ? "Scroll" : "Book", col::textDim);
	// enchantment: how it's cast, its charge and effects
	auto en = o->ench.empty() ? w.game.spells.end() : w.game.spells.find(o->ench);
	if (en != w.game.spells.end())
	{
		const SpellDef& sp = en->second;
		static const char* kHow[4] = { "Cast Once", "Cast When Strikes", "Cast When Used", "Constant Effect" };
		int t = sp.type - ENCH_ONCE;
		tip.emplace_back(t >= 0 && t < 4 ? kHow[t] : "Enchanted", col::header);
		for (auto& e : sp.effects)
			tip.emplace_back(effectLine(e, t != 3), col::text);
		if (t == 1 || t == 2)
		{
			float charge = it ? w.chargeOf(*it) : (float)sp.charge;
			snprintf(buf, sizeof(buf), "Charge: %d/%d", (int)charge, sp.charge);
			tip.emplace_back(buf, col::text);
		}
	}
}

// The crosshair name for the top screen: wrapped within 340 pixels, so "Hut Door - Ald-ruhn, Tiras Sadus: General
// Merchandise" shows whole (it ran off both edges of the 400-pixel screen)
std::vector<std::string> Session::targetPopupLines(int ref)
{
	return uiWrap(targetLabel(ref), 340.0f, 0.55f);
}

// What A does to what is aimed at, for the bottom screen: short of the date under the map (from x 190), so within 180
// pixels, over two lines at most, the second cut with ".." when even that is not enough
std::vector<std::string> Session::targetPromptLines(int ref, float* scale)
{
	const Ref& t = w.refs[ref];
	// (a creature alive: "Talk to" one with words of its own, else only its name, as OpenMW shows it)
	bool talks = t.type == "CREA" && t.actor >= 0 && ((w.game.actors[t.actor].services & 0x3FFFF) || w.game.speakers.count(t.idLower));
	std::string verb = t.dead ? "Search " : t.type == "NPC_" || talks ? "Talk to " : t.type == "CREA" ? "" : t.type == "DOOR" ? "Open " : "Use ";
	std::string use = "A: " + verb + targetLabel(ref);
	// one line at the usual size; else two, a little smaller, and smaller still before anything is cut
	float s = 0.45f;
	std::vector<std::string> lines = uiWrap(use, 180.0f, s);
	for (float step : { 0.42f, 0.36f })
		if (lines.size() > 1)
		{
			s = step;
			lines = uiWrap(use, 180.0f, s);
			if (lines.size() <= 2)
				break;
		}
	if (scale)
		*scale = s;
	if (lines.size() > 2)
	{
		std::string rest;
		for (size_t i = 1; i < lines.size(); i++)
			rest += (i > 1 ? " " : "") + lines[i];
		lines.resize(2);
		lines[1] = rest;
	}
	while (lines.size() == 2 && lines[1].size() > 3 && uiTextWidth(lines[1], s) > 180.0f)
		lines[1] = lines[1].substr(0, lines[1].size() - 4) + "...";
	return lines;
}

// Multi-page guide: Graphics, Performance, Controls, Interface, Cheats, Multiplayer
void Session::drawHelp()
{
	struct Page { const char* title; const char* body; };
	static const Page pages[] = {
		{ "Welcome",
		  "This is a short guide to the 3DS port of Morrowind.\n\n"
		  "Use Next / Prev to flip pages, or Close when you are done.\n\n"
		  "You can open this guide any time from the Menu (Help) or from Options." },
		{ "Graphics",
		  "Menu → Graphics\n\n"
		  "• Draw distance (25–100%) — how far the world is drawn. Lower = faster.\n"
		  "• Fog on/off — softens the horizon.\n"
		  "• Weather on/off — off forces a clear sky.\n"
		  "• Brightness (70–130%) — overall screen brightness." },
		{ "Performance",
		  "Menu → Performance\n\n"
		  "• Presets: Low / Balanced / High — quick quality vs speed.\n"
		  "• Frame cap: Auto / 30 / 60.\n"
		  "• Character distance & clutter distance — how far people and small props draw.\n"
		  "• Toggles: sky & clouds, terrain layers, transparency, characters, distant land, enchant shimmer.\n"
		  "• FPS counter on/off — shows frames per second." },
		{ "Controls",
		  "Menu → Controls\n\n"
		  "• Look sensitivity (25–200%).\n"
		  "• Invert Y look.\n"
		  "• Swap sticks — C-stick walks, Circle Pad looks.\n"
		  "• Always run on/off.\n"
		  "• Button layout: Classic / GoldenEye / Xbox.\n\n"
		  "Check the on-screen prompts for Activate, Attack, and Ready Weapon." },
		{ "Interface",
		  "Menu → Interface\n\n"
		  "• Subtitles on/off.\n"
		  "• Hide crosshair / top-screen HUD / minimap.\n"
		  "• Screenshot hides HUD — clean pictures.\n"
		  "• Font and text size; minimap marker color.\n"
		  "• Autosave every 5 / 10 / 15 / 30 minutes, or off." },
		{ "Cheats",
		  "Menu → Cheats\n\n"
		  "Cheats are not saved between sessions.\n\n"
		  "• Restore health, magicka, fatigue.\n"
		  "• +1000 gold.\n"
		  "• God mode; infinite magicka & fatigue; weightless.\n"
		  "• Walk speed & jump power multipliers.\n"
		  "• All skills +10, all attributes +10.\n"
		  "• Teleport to any town listed in the game data." },
		{ "Multiplayer",
		  "Menu → Multiplayer\n\n"
		  "Host a game or join with a code / address.\n"
		  "Peers appear as player bodies with name and HP on the top screen.\n"
		  "The host is authoritative for the world; stay near each other for the best experience.\n\n"
		  "Use the Chat button on the bottom screen when connected." },
		{ "Companion",
		  "After character creation you can recruit an Imperial Guard companion.\n\n"
		  "Talk to him for orders: Follow me, Stay back, Attack, Share items, Dismiss.\n"
		  "Share items lets you give potions and gear; taking gear removes it from his body.\n"
		  "His HP is on the top-left of the top screen." },
		{ "Harvesting",
		  "Plants, mushrooms and ore nodes are harvested directly: press A and the ingredients go straight into your inventory. There is no container screen. The plant disappears once harvested, as in the original game. Harvesting someone else's plants counts as theft if a guard or the owner sees you." },
		{ "Tips",
		  "• Start: leave the Census Office, talk to people, check your journal.\n"
		  "• Save often (Menu → Save / Load).\n"
		  "• If the game is slow, try Performance → Low and lower draw distance.\n"
		  "• Open Help again any time from the Menu.\n"
		  "\n"
		  "Good luck in Morrowind." },
	};
	const int nPages = (int)(sizeof(pages) / sizeof(pages[0]));
	if (helpPage < 0) helpPage = 0;
	if (helpPage >= nPages) helpPage = nPages - 1;

	char head[64];
	snprintf(head, sizeof(head), "Help  (%d / %d)", helpPage + 1, nPages);
	header(head, pages[helpPage].title);

	// Body text, wrapped
	float y = 28.0f;
	const float scale = 0.42f;
	for (const std::string& line : uiWrap(pages[helpPage].body, 300.0f, scale))
	{
		if (y > 195.0f)
			break;
		uiText(12, y, scale, col::text, line);
		y += uiLineHeight(scale);
	}

	const UiInput& in = uiIn();
	int b = -1;
	const char* labels[3] = { "Prev", "Next", "Close" };
	bool en[3] = { helpPage > 0, helpPage < nPages - 1, true };
	for (int i = 0; i < 3; i++)
		if (uiButton(8 + i * 104, 208, 100, 28, labels[i], focus == i, en[i])
			|| (focus == i && (in.down & KEY_A) && en[i]))
			b = i;
	if (in.down & KEY_LEFT) focus = focus <= 0 ? 2 : focus - 1;
	if (in.down & KEY_RIGHT) focus = focus >= 2 ? 0 : focus + 1;
	if (in.down & KEY_L && helpPage > 0) { helpPage--; playSound(-1, "Menu Click"); }
	if (in.down & KEY_R && helpPage < nPages - 1) { helpPage++; playSound(-1, "Menu Click"); }
	if (b == 0 && helpPage > 0)
	{
		helpPage--;
		playSound(-1, "Menu Click");
	}
	else if (b == 1 && helpPage < nPages - 1)
	{
		helpPage++;
		playSound(-1, "Menu Click");
	}
	else if (b == 2 || (in.down & KEY_B))
		closeScreen();
}
