#include "consolecmd.h"
#include "consolestate.h"
#include "session.h"
#include "log.h"
#include "pclink.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <sys/stat.h>
#include <vector>

// ---- toggles ----
bool g_showDebugText = false;
bool g_showCellBorders = false;
bool g_noCollision = false;
bool g_showBoxes = false;
bool g_showColGrid = false;
bool g_showCellGrid = false;
bool g_mapNoFog = false;
bool g_hoverFull = false;
bool g_aiOff = false;
bool g_noSky = false;
bool g_noWorld = false;
bool g_wireframe = false;
bool g_showPathGrid = false;
bool g_showTexNames = false;
bool g_combatStats = false;
bool g_magicStats = false;
bool g_dlgStats = false;
bool g_scriptsOff = false;
bool g_noLoadFade = false;

// ---- helpers ----
static std::string trimLower(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    std::string out = s.substr(a, b - a);
    for (auto& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

bool parseConsoleLine(const std::string& in, ParsedConsole& out)
{
    out = ParsedConsole();
    out.raw = in;
    std::string s = in;
    size_t arrow = std::string::npos;
    {
        bool inQuote = false;
        for (size_t k = 0; k + 1 < s.size(); k++)
        {
            if (s[k] == '"') inQuote = !inQuote;
            if (!inQuote && s[k] == '-' && s[k + 1] == '>') { arrow = k; break; }
        }
    }
    if (arrow != std::string::npos)
    {
        std::string pref = s.substr(0, arrow);
        size_t a = 0, b = pref.size();
        while (a < b && (isspace((unsigned char)pref[a]) || pref[a] == '"')) a++;
        while (b > a && (isspace((unsigned char)pref[b - 1]) || pref[b - 1] == '"')) b--;
        out.prefix = trimLower(pref.substr(a, b - a));
        s = s.substr(arrow + 2);
    }
    std::vector<std::string> toks;
    std::string cur;
    bool inQuote = false;
    for (char c : s)
    {
        if (c == '"') { inQuote = !inQuote; continue; }
        if (!inQuote && isspace((unsigned char)c))
        {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.empty()) return false;
    out.cmd = trimLower(toks[0]);
    for (size_t k = 1; k < toks.size(); k++) out.args.push_back(toks[k]);
    return true;
}

static int consoleRef(Session* s)
{
    if (!s) return -1;
    return s->consoleSelected >= 0 ? s->consoleSelected : s->target;
}

static float argf(const std::vector<std::string>& a, size_t i, float fb = 0.0f)
{ return i < a.size() ? (float)atof(a[i].c_str()) : fb; }
static int argi(const std::vector<std::string>& a, size_t i, int fb = 0)
{ return i < a.size() ? atoi(a[i].c_str()) : fb; }

static void sayf(Session* s, const char* fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (s) s->consolePrint(b);
    logf("console: %s", b);
}

static bool tryStatCommand(Session* s, const std::string& cmd, const std::vector<std::string>& args)
{
    if (!s) return false;
    int op = -1;
    std::string stat;
    if      (cmd.compare(0, 10, "modcurrent") == 0) { op = 3; stat = cmd.substr(10); }
    else if (cmd.compare(0, 3, "mod") == 0)         { op = 2; stat = cmd.substr(3); }
    else if (cmd.compare(0, 3, "set") == 0)         { op = 1; stat = cmd.substr(3); }
    else if (cmd.compare(0, 3, "get") == 0)         { op = 0; stat = cmd.substr(3); }
    if (op < 0 || stat.empty()) return false;
    int ref = consoleRef(s);
    float v = argf(args, 0, 0.0f);
    s->consoleStat(op, stat, v, ref);
    sayf(s, "%s %s = %.3f", op == 0 ? "get" : op == 1 ? "set" : op == 2 ? "mod" : "modcurrent", stat.c_str(), v);
    return true;
}

// ---- PC Link: save export (upload every save file to the PC) ----
static void exportSaves(Session* session)
{
    if (!session) return;
    if (!pclink_connected())
    {
        session->consolePrint("save export: not linked to the PC");
        logf("pclink: save export refused, not linked");
        return;
    }
    int n = 0;
    if (pclink_upload_file("sdmc:/3ds/mw3ds/save.json"))
        n++;
    for (int i = 1; i <= 999; i++)
    {
        char path[96];
        snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/saves/save_%03d.json", i);
        struct stat st;
        if (stat(path, &st) == 0 && pclink_upload_file(path))
            n++;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%d save(s) sent to the PC", n);
    session->consolePrint(msg);
    pclink_log("%s", msg);
    logf("pclink: %s", msg);
}

// ---- PC Link: player state 'M' message (for the browser's map tab) ----
static void sendPlayerState(Session* session)
{
    if (!session) return;
    if (!pclink_connected())
    {
        session->consolePrint("pclink state: not linked to the PC");
        return;
    }
    World& w = session->w;
    if (w.current < 0)
    {
        session->consolePrint("pclink state: no cell");
        return;
    }

    g_pclink_player_state.geom_count = 0;
    const float reach2 = 3000.0f * 3000.0f;  // 3 km radius
    Player& p = w.player;

    w.forLoadedActors([&](int i) {
        const Ref& r = w.refs[i];
        float dx = r.pos[0] - p.feet[0], dy = r.pos[1] - p.feet[1];
        if (dx * dx + dy * dy > reach2) return;
        pclink_add_geom(r.pos[0], r.pos[1], r.dead ? 5 : 2);
    });
    w.forLoadedRefs([&](int i) {
        const Ref& r = w.refs[i];
        if (!r.visible() || r.pickedUp) return;
        float dx = r.pos[0] - p.feet[0], dy = r.pos[1] - p.feet[1];
        if (dx * dx + dy * dy > reach2) return;
        if (r.type == "DOOR")      pclink_add_geom(r.pos[0], r.pos[1], 1);
        else if (r.type == "CONT") pclink_add_geom(r.pos[0], r.pos[1], 3);
    });

    const PlayerStats& st = w.stats;
    pclink_set_player_state(p.feet, p.yaw, p.pitch,
        w.current,
        w.cells[w.current].live ? 1 : 0,
        w.cellName().c_str(),
        st.health, st.healthMax,
        st.magicka, st.magickaMax,
        st.fatigue, st.fatigueMax,
        session->playerArmor());

    pclink_send_player_state();
    session->consolePrint("pclink state sent");
    pclink_log("Map state sent");
}

void runConsoleCommand(Session* session, const char* raw)
{
    ParsedConsole p;
    if (!parseConsoleLine(raw ? raw : "", p))
    {
        if (session) session->consolePrint("empty command");
        return;
    }
    if (p.prefix == "player" && session) session->consoleSelected = -1;

    const std::string& c = p.cmd;
    auto& a = p.args;

    if (c == "help")
    {
        if (!session) return;
        session->consolePrint("-- console commands --");
        session->consolePrint("pos|where [x|y|z]   cell   coe x y   fixme");
        session->consolePrint("coc <cell>   tp/goto <town>   positioncell x y z yaw cell");
        session->consolePrint("setpos x|y|z v   move x|y|z v   rotate x|y|z deg");
        session->consolePrint("sethealth v   setmagicka v   setfatigue v   setlevel v");
        session->consolePrint("setreputation v   setbounty v   setdisposition v");
        session->consolePrint("additem id n   addspell id   addsoulgem creature gem [n]");
        session->consolePrint("pcjoinfaction fac   pcraiserank fac   pclowerrank fac   pcclearexpelled fac");
        session->consolePrint("getpcfacrep fac   modpcfacrep v fac");
        session->consolePrint("modfactionreaction a b v   setfactionreaction a b v   getfactionreaction a b");
        session->consolePrint("addtopic topic   journal quest stage   setjournalindex quest stage");
        session->consolePrint("filljournal   fillmap   changeweather region type");
        session->consolePrint("set gamehour to v   set timescale to v   streammusic [file]");
        session->consolePrint("placeatpc id [n] [dist] [dir]   startscript name   stopscript name   resetactors");
        session->consolePrint("enablelevelupmenu   enablenamemenu   enableracemenu");
        session->consolePrint("xpleveling [on|off]   addxp amount");
        session->consolePrint("enableclassmenu   enablebirthmenu   enablestatreviewmenu   togglemenus");
        session->consolePrint("enable|disable   resurrect   lock [level]   unlock   sel   activate");
        session->consolePrint("setsuperjump v   setwaterbreathing 0|1   setwaterwalking 0|1   setflying 0|1");
        session->consolePrint("tgm   tim   tw   speed v   jump v   save export   pclink state");
        session->consolePrint("toggleai  togglesky  toggleworld  togglewireframe  togglepathgrid");
        session->consolePrint("togglecollision  toggleborders  togglecollisionboxes  togglecollisiongrid");
        session->consolePrint("togglefogofwar  togglefullhelp  toggledebugtext  togglegrid  togglescripts");
        session->consolePrint("toggleloadfade  togglecombatstats  togglemagicstats  toggledialoguestats");
        session->consolePrint("toggletexturestring  clear");
        return;
    }
    if (c == "clear") { if (session) session->consoleLog.clear(); return; }
    if (!session) { logf("console: no session (%s)", c.c_str()); return; }

    // -------- position
    if (c == "pos" || c == "where" || c == "getpos" || c == "coordinates")
    {
        std::string axis = a.empty() ? "" : trimLower(a[0]);
        const float* f = session->w.player.feet;
        if (axis == "x") sayf(session, "%.2f", f[0]);
        else if (axis == "y") sayf(session, "%.2f", f[1]);
        else if (axis == "z") sayf(session, "%.2f", f[2]);
        else
            sayf(session, "x %.1f  y %.1f  z %.1f  yaw %.1f  pitch %.1f  cell \"%s\"",
                 f[0], f[1], f[2],
                 session->w.player.yaw * 57.29578f, session->w.player.pitch * 57.29578f,
                 session->w.cellName().c_str());
        return;
    }
    if (c == "cell")
    {
        if (session->w.current < 0) { sayf(session, "no cell"); return; }
        const LevelCell& lc = session->w.cells[session->w.current];
        sayf(session, "cell \"%s\" (index %d, %s%s%s)",
             session->w.cellName().c_str(), session->w.current,
             lc.interior ? "interior" : "exterior",
             lc.region.empty() ? "" : ", region ", lc.region.c_str());
        return;
    }
    if (c == "coe" || c == "centeronexterior")
    {
        if (a.size() < 2) { sayf(session, "usage: coe x y"); return; }
        float x = argf(a, 0), y = argf(a, 1);
        int g = session->w.gridCell((int)floorf(x / 8192.0f), (int)floorf(y / 8192.0f));
        if (g < 0) { sayf(session, "no exterior cell at %.0f %.0f", x, y); return; }
        session->w.enterCell(g);
        session->w.player.feet[0] = x;
        session->w.player.feet[1] = y;
        session->w.player.feet[2] = 2000.0f;
        session->w.player.vz = 0.0f;
        session->w.player.fallTop = 2000.0f;
        sayf(session, "moved to exterior %.0f %.0f", x, y);
        return;
    }
    if (c == "fixme")
    {
        Player& pl = session->w.player;
        pl.feet[0] += sinf(pl.yaw) * 128.0f;
        pl.feet[1] += cosf(pl.yaw) * 128.0f;
        pl.feet[2] += 64.0f;
        pl.vz = 0.0f;
        pl.fallTop = pl.feet[2];
        sayf(session, "FixMe: moved 128 units");
        return;
    }
    if (c == "mpgoto")
    {
        session->mpGotoPeer();
        sayf(session, "mpgoto: moving to the other player's cell");
        return;
    }
    if (c == "coc" || c == "centeroncell")
    {
        if (a.empty()) { sayf(session, "usage: coc cell"); return; }
        session->testGoto(a[0]);
        sayf(session, "travelling to %s", a[0].c_str());
        return;
    }
    if (c == "tp" || c == "goto")
    {
        if (a.empty()) { sayf(session, "usage: tp town"); return; }
        std::string town = trimLower(a[0]);
        if (!session->w.game.townSpawns.count(town))
        { sayf(session, "unknown town '%s'", a[0].c_str()); return; }
        session->testGoto(a[0]);
        sayf(session, "travelling to %s", a[0].c_str());
        return;
    }
    if (c == "positioncell")
    {
        if (a.size() < 5) { sayf(session, "usage: positioncell x y z yaw cell"); return; }
        float x = argf(a, 0), y = argf(a, 1), z = argf(a, 2);
        float yaw = argf(a, 3) * 3.14159265f / 180.0f;
        int cell = session->w.cellIndex(a[4]);
        if (cell < 0) { sayf(session, "no cell \"%s\"", a[4].c_str()); return; }
        int ri = consoleRef(session);
        float pos[3] = { x, y, z };
        if (ri < 0)
        {
            session->teleportPlayer(cell, pos, yaw);
            sayf(session, "player -> %s", a[4].c_str());
        }
        else
        {
            session->w.relocate(ri, cell, pos, yaw);
            sayf(session, "%s -> %s", session->w.refs[ri].id.c_str(), a[4].c_str());
        }
        return;
    }
    if (c == "setpos")
    {
        if (a.size() < 2) { sayf(session, "usage: setpos x|y|z v"); return; }
        std::string ax = trimLower(a[0]);
        int k = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
        if (k < 0) { sayf(session, "axis?"); return; }
        int ri = consoleRef(session);
        float* pp = ri < 0 ? session->w.player.feet : session->w.refs[ri].pos;
        pp[k] = argf(a, 1);
        if (ri >= 0) { session->w.refs[ri].moved = true; session->w.syncActor(ri); }
        sayf(session, "set %c = %.2f", "xyz"[k], pp[k]);
        return;
    }
    if (c == "move")
    {
        if (a.size() < 2) { sayf(session, "usage: move x|y|z v"); return; }
        std::string ax = trimLower(a[0]);
        int k = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
        if (k < 0) { sayf(session, "axis?"); return; }
        float v = argf(a, 1);
        int ri = consoleRef(session);
        float* pp = ri < 0 ? session->w.player.feet : session->w.refs[ri].pos;
        float yaw = ri < 0 ? session->w.player.yaw : (session->w.refs[ri].actor >= 0 ? session->w.refs[ri].rot[2] : 0.0f);
        float v3[3] = { 0, 0, 0 }; v3[k] = v;
        float cc = cosf(yaw), ss = sinf(yaw);
        float wx = v3[0] * cc + v3[1] * ss, wy = -v3[0] * ss + v3[1] * cc;
        pp[0] += wx; pp[1] += wy; pp[2] += v3[2];
        if (ri >= 0) { session->w.refs[ri].moved = true; session->w.syncActor(ri); }
        sayf(session, "moved %c by %.1f", "xyz"[k], v);
        return;
    }
    if (c == "rotate")
    {
        if (a.size() < 2) { sayf(session, "usage: rotate x|y|z deg"); return; }
        std::string ax = trimLower(a[0]);
        int k = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
        if (k < 0) { sayf(session, "axis?"); return; }
        float deg = argf(a, 1);
        int ri = consoleRef(session);
        if (ri < 0)                                   session->w.player.yaw += deg * 3.14159265f / 180.0f;
        else if (session->w.refs[ri].actor >= 0)      { session->w.refs[ri].rot[2] += deg * 3.14159265f / 180.0f; session->w.syncActor(ri); }
        else                                          { session->w.refs[ri].rot[k] += deg * 3.14159265f / 180.0f; session->w.refs[ri].moved = true; }
        sayf(session, "rotated %c %.1f deg", "xyz"[k], deg);
        return;
    }

    // -------- stats
    if (c == "sethealth")
    {
        float v = argf(a, 0, session->w.stats.healthMax);
        session->w.stats.healthBonus += v - session->w.stats.healthMax;
        session->w.stats.healthMax = v;
        session->w.stats.health = v;
        sayf(session, "health = %.0f", v);
        return;
    }
    if (c == "setmagicka")
    {
        float v = argf(a, 0, 0.0f);
        session->w.stats.magickaMax = v;
        session->w.stats.magicka = v;
        sayf(session, "magicka = %.0f", v);
        return;
    }
    if (c == "setfatigue")
    {
        float v = argf(a, 0, 0.0f);
        session->w.stats.fatigueMax = v;
        session->w.stats.fatigue = v;
        sayf(session, "fatigue = %.0f", v);
        return;
    }
    if (c == "setlevel")
    {
        int v = argi(a, 0, 1);
        session->w.stats.level = v;
        session->w.recomputeStats();
        sayf(session, "level = %d", v);
        return;
    }
    if (c == "setreputation")
    {
        session->w.pcReputation = argi(a, 0, 0);
        sayf(session, "reputation = %d", session->w.pcReputation);
        return;
    }
    if (c == "setbounty" || c == "setpccrimelevel")
    {
        session->w.bounty = argi(a, 0, 0);
        session->w.globals["pccrimelevel"] = (float)session->w.bounty;
        sayf(session, "bounty = %d", session->w.bounty);
        return;
    }
    if (c == "setsuperjump")
    {
        float v = argf(a, 0, 0.0f);
        session->w.consoleSetPresenceEffect(9, v > 0.0f);
        for (auto& e : session->w.effects)
            if (e.effect == 9 && e.source == "console") e.magnitude = v;
        sayf(session, "superjump = %.0f", v);
        return;
    }
    if (c == "setwaterbreathing")
    {
        bool on = argi(a, 0, 1) != 0;
        session->w.consoleSetPresenceEffect(0, on);
        sayf(session, "water breathing %s", on ? "on" : "off");
        return;
    }
    if (c == "setwaterwalking")
    {
        bool on = argi(a, 0, 1) != 0;
        session->w.consoleSetPresenceEffect(2, on);
        sayf(session, "water walking %s", on ? "on" : "off");
        return;
    }
    if (c == "setflying")
    {
        bool on = argi(a, 0, 1) != 0;
        session->w.consoleSetPresenceEffect(10, on);
        sayf(session, "flying %s", on ? "on" : "off");
        return;
    }

    // -------- items
    if (c == "additem")
    {
        if (a.empty()) { sayf(session, "usage: additem id [n]"); return; }
        int n = argi(a, 1, 1);
        std::string id = a[0];
        if (lower(id) == "gold_001" || lower(id).rfind("gold_", 0) == 0) id = "gold_001";
        else if (!session->w.game.object(id)) { sayf(session, "no item \"%s\"", id.c_str()); return; }
        session->w.addItem(id, n);
        sayf(session, "added %d x %s", n, id.c_str());
        return;
    }
    if (c == "addspell")
    {
        if (a.empty()) { sayf(session, "usage: addspell id"); return; }
        std::string id = lower(a[0]);
        if (!session->w.game.spells.count(id)) { sayf(session, "no spell \"%s\"", id.c_str()); return; }
        if (std::find(session->w.stats.spells.begin(), session->w.stats.spells.end(), id) == session->w.stats.spells.end())
            session->w.stats.spells.push_back(id);
        session->w.recomputeStats();
        sayf(session, "learned %s", id.c_str());
        return;
    }
    if (c == "addsoulgem")
    {
        if (a.size() < 2) { sayf(session, "usage: addsoulgem creature gem [n]"); return; }
        std::string creature = lower(a[0]), gem = lower(a[1]);
        int n = argi(a, 2, 1);
        if (!session->w.game.object(gem)) { sayf(session, "no gem \"%s\"", gem.c_str()); return; }
        for (int i = 0; i < n; i++)
        {
            session->w.addItem(gem, 1);
            InventoryItem filled = { gem, 1, false };
            filled.soul = creature;
            for (size_t k = session->w.inventory.size(); k-- > 0;)
                if (session->w.inventory[k].id == gem && session->w.inventory[k].soul.empty())
                { session->w.inventory.erase(session->w.inventory.begin() + k); break; }
            session->w.addStack(filled);
        }
        sayf(session, "added %d x %s (%s)", n, gem.c_str(), creature.c_str());
        return;
    }
    if (c == "pcjoinfaction" || c == "pcraiserank" || c == "pclowerrank" ||
        c == "pcclearexpelled" || c == "getpcfacrep" || c == "modpcfacrep")
    {
        std::string fac;
        int v = 0;
        if (c == "modpcfacrep") { v = argi(a, 0, 0); fac = lower(a.size() >= 2 ? a[1] : ""); }
        else                    { fac = lower(a.empty() ? "" : a[0]); }
        if (fac.empty()) { sayf(session, "faction?"); return; }
        if (c == "pcjoinfaction")   { session->w.pcRank[fac] = 0; session->w.pcExpelled.erase(fac); }
        if (c == "pcraiserank")     { int r = session->w.pcRankIn(fac); session->w.pcRank[fac] = r < 0 ? 0 : std::min(9, r + 1); }
        if (c == "pclowerrank")     { int r = session->w.pcRankIn(fac); if (r <= 0) session->w.pcRank.erase(fac); else session->w.pcRank[fac] = r - 1; }
        if (c == "pcclearexpelled") session->w.pcExpelled.erase(fac);
        if (c == "getpcfacrep")     { sayf(session, "reputation %d in %s", session->w.pcFacRep[fac], fac.c_str()); return; }
        if (c == "modpcfacrep")     session->w.pcFacRep[fac] += v;
        sayf(session, "ok: %s %s", c.c_str(), fac.c_str());
        return;
    }
    if (c == "modfactionreaction" || c == "setfactionreaction" || c == "getfactionreaction")
    {
        if (a.size() < 2) { sayf(session, "usage: %s fac1 fac2 [v]", c.c_str()); return; }
        std::string f1 = lower(a[0]), f2 = lower(a[1]);
        int v = argi(a, 2, 0);
        std::string key = f1 + "|" + f2;
        if (c == "modfactionreaction")      session->w.factionReactions[key] += v;
        else if (c == "setfactionreaction") session->w.factionReactions[key] = v;
        sayf(session, "%s -> %s = %d", f1.c_str(), f2.c_str(), session->w.factionReaction(f1, f2));
        return;
    }

    // -------- journal / topics
    if (c == "addtopic")
    {
        if (a.empty()) { sayf(session, "usage: addtopic topic"); return; }
        std::string t = lower(a[0]);
        session->w.knownTopics.insert(t);
        sayf(session, "learned topic %s", t.c_str());
        return;
    }
    if (c == "journal" || c == "setjournalindex")
    {
        if (a.size() < 2) { sayf(session, "usage: %s quest stage", c.c_str()); return; }
        std::string q = lower(a[0]);
        int stage = argi(a, 1, 0);
        if (c == "journal") session->w.setJournal(q, stage);
        else                session->w.journalIndex[q] = stage;
        sayf(session, "%s %s = %d", c.c_str(), q.c_str(), stage);
        return;
    }
    if (c == "filljournal")
    {
        for (auto& kv : session->w.game.journals)
            for (auto& e : kv.second.entries)
                session->w.journalAdd(kv.first, e.index);
        sayf(session, "journal filled");
        return;
    }
    if (c == "fillmap")
    {
        for (auto& cell : session->w.cells)
            if (!cell.interior)
                session->w.mapCells.insert(World::mapCellKey(cell.gx, cell.gy));
        session->w.mapKnown.clear();
        for (auto& lbl : session->w.game.map.labels)
            session->w.mapKnown.insert(lower(lbl.text));
        sayf(session, "map filled");
        return;
    }

    // -------- weather / time
    if (c == "changeweather" || c == "setweather")
    {
        if (a.size() < 2) { sayf(session, "usage: changeweather region type"); return; }
        std::string region = lower(a[0]);
        int type = argi(a, 1, 0);
        session->w.changeWeather(region, type);
        sayf(session, "weather %s = %d", region.c_str(), type);
        return;
    }
    if (c == "set" && a.size() >= 3 && lower(a[1]) == "to")
    {
        std::string sub = lower(a[0]);
        float v = argf(a, 2);
        if (sub == "gamehour")
        {
            session->w.setGlobal("gamehour", v);
            sayf(session, "hour = %.2f", fmodf(session->w.gameHour, 24.0f));
            return;
        }
        if (sub == "timescale")
        {
            session->w.setGlobal("timescale", v);
            sayf(session, "timescale = %.1f", session->w.timescale());
            return;
        }
    }

    // -------- objects
    if (c == "enable" || c == "disable")
    {
        int ri = consoleRef(session);
        if (ri < 0) { sayf(session, "nothing selected"); return; }
        session->w.setEnabled(ri, c == "enable");
        sayf(session, "%s %s", c.c_str(), session->w.refs[ri].id.c_str());
        return;
    }
    if (c == "resurrect")
    {
        int ri = consoleRef(session);
        if (ri < 0 || session->w.refs[ri].actor < 0 || !session->w.refs[ri].dead)
        { sayf(session, "no dead actor selected"); return; }
        Ref& r = session->w.refs[ri];
        r.dead = r.died = false;
        r.statDelta.clear();
        r.spells.clear();
        r.noSpells.clear();
        r.health = r.healthMax;
        r.ai = AI_IDLE;
        session->w.syncActor(ri);
        sayf(session, "resurrected %s", r.id.c_str());
        return;
    }
    if (c == "lock" || c == "unlock")
    {
        int ri = consoleRef(session);
        if (ri < 0) { sayf(session, "nothing selected"); return; }
        if (session->w.refs[ri].type != "DOOR" && session->w.refs[ri].type != "CONT")
        { sayf(session, "no door/container selected"); return; }
        if (c == "unlock") { session->w.refs[ri].lockLevel = 0; sayf(session, "unlocked"); return; }
        int level = argi(a, 0, 100);
        session->w.refs[ri].lockLevel = level;
        sayf(session, "locked at %d", level);
        return;
    }
    if (c == "setdisposition")
    {
        int ri = consoleRef(session);
        if (ri < 0 || session->w.refs[ri].actor < 0 ||
            session->w.game.actors[session->w.refs[ri].actor].creature)
        { sayf(session, "no NPC selected"); return; }
        int v = argi(a, 0, 50);
        session->w.refs[ri].disposition = v;
        sayf(session, "disposition = %d", v);
        return;
    }
    if (c == "placeatpc")
    {
        if (a.empty()) { sayf(session, "usage: placeatpc id [n] [dist] [dir]"); return; }
        int n = argi(a, 1, 1);
        float d = argf(a, 2, 128.0f);
        int dir = argi(a, 3, 0);
        session->w.placeAt(a[0], n, d, dir, session->w.player.feet, session->w.player.yaw);
        sayf(session, "placed %d x %s", n, a[0].c_str());
        return;
    }
    if (c == "startscript" || c == "stopscript")
    {
        if (a.empty()) { sayf(session, "usage: %s name", c.c_str()); return; }
        if (c == "startscript") session->w.startGlobalScript(a[0]);
        else                    session->w.stopGlobalScript(a[0]);
        sayf(session, "%s %s", c.c_str(), a[0].c_str());
        return;
    }
    if (c == "resetactors" || c == "ra")
    {
        int cc = session->w.current;
        if (cc < 0) { sayf(session, "no cell"); return; }
        const LevelCell& lc = session->w.cells[cc];
        int moved = 0;
        for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
        {
            Ref& r = session->w.refs[i];
            if (r.actor < 0) continue;
            memcpy(r.pos, r.start, sizeof(r.pos));
            r.rot[2] = r.start[3];
            r.fitBox();
            r.moved = true;
            session->w.syncActor(i);
            moved++;
        }
        sayf(session, "reset %d actors", moved);
        return;
    }

    // -------- XP leveling
    if (c == "xpleveling")
    {
        PlayerStats& st = session->w.stats;
        if (!a.empty())
        {
            session->setXpLeveling(lower(a[0]) == "on" || a[0] == "1");
        }
        sayf(session, "xp leveling %s, xp %.1f / %.1f", st.xpLeveling ? "on" : "off", st.xp, session->xpNeeded());
        return;
    }
    if (c == "addxp")
    {
        if (a.empty()) { sayf(session, "usage: addxp amount"); return; }
        session->awardXp((float)atof(a[0].c_str()), "console");
        return;
    }

    // -------- menus
    if (c == "enablelevelupmenu")    { session->levelPicks.clear(); session->openScreen(SCR_LEVELUP);  sayf(session, "level up menu");  return; }
    if (c == "enablenamemenu")       { session->wantName = true;                                       sayf(session, "name menu");      return; }
    if (c == "enableracemenu")       { session->openScreen(SCR_RACE);                                  sayf(session, "race menu");      return; }
    if (c == "enableclassmenu")      { session->openScreen(SCR_CLASS_METHOD);                          sayf(session, "class menu");     return; }
    if (c == "enablebirthmenu")      { session->openScreen(SCR_BIRTH);                                 sayf(session, "birth menu");     return; }
    if (c == "enablestatreviewmenu") { session->openScreen(SCR_REVIEW);                                sayf(session, "stat review");    return; }
    if (c == "togglemenus" || c == "tm")
    {
        while (session->screen != SCR_NONE) session->closeScreen();
        session->messages.clear();
        sayf(session, "menus closed");
        return;
    }

    // -------- music
    if (c == "streammusic")
    {
        std::string file = a.empty() ? "" : a[0];
        session->streamMusic(file);
        sayf(session, "music: %s", file.empty() ? "(random)" : file.c_str());
        return;
    }

    // -------- interaction
		    // -------- ghost spawning (multiplayer prototype)
    if (c == "spawnghost") {
        float fx = sinf(session->w.player.yaw);
        float fy = cosf(session->w.player.yaw);
        float pos[3] = {
            session->w.player.feet[0] + fx * 100.0f,
            session->w.player.feet[1] + fy * 100.0f,
            session->w.player.feet[2]
        };
        int ref = session->spawnGhost("Ghost", pos, session->w.player.yaw + 3.14159f);
        if (ref >= 0)
            sayf(session, "spawned ghost ref %d", ref);
        else
            sayf(session, "failed to spawn ghost");
        return;
    }
    if (c == "removeghosts") {
        int n = 0;
        for (size_t k = 0; k < session->w.spawned.size(); ) {
            int i = session->w.spawned[k];
            if (session->w.refs[i].idLower.compare(0, 6, "ghost_") == 0) {
                session->w.despawn(i);
                n++;
            } else k++;
        }
        sayf(session, "removed %d ghosts", n);
        return;
    }
    if (c == "host") {
        if (session->net.host(50764)) {
            session->mpMode = 1;
            session->net.setHandler(Session::onNetMessage, session);
            sayf(session, "hosting on port 50764");
        } else {
            sayf(session, "host failed");
        }
        return;
    }
    if (c == "join") {
        if (a.empty()) { sayf(session, "usage: join <ip>"); return; }
        if (session->net.join(a[0].c_str(), 50764)) {
            session->mpMode = 2;
            session->net.setHandler(Session::onNetMessage, session);
            struct MsgHello { u32 version; char name[32]; } h;
            h.version = 1;
            strncpy(h.name, "client", sizeof(h.name));
            session->net.send(NM_HELLO, h);
            sayf(session, "connected to %s", a[0].c_str());
        } else {
            sayf(session, "join failed");
        }
        return;
    }
    if (c == "chat") {
        if (a.empty()) { sayf(session, "usage: chat <message>"); return; }
        std::string text;
        for (size_t k = 0; k < a.size(); k++) {
            if (k > 0) text += " ";
            text += a[k];
        }
        // Payload: [u8 nameLen][name][u16 textLen][text]
        u8 nameLen = (u8)session->mpHostName.size();
        u16 textLen = (u16)text.size();
        std::vector<u8> buf;
        buf.push_back(nameLen);
        buf.insert(buf.end(), session->mpHostName.begin(), session->mpHostName.end());
        buf.push_back(textLen & 0xFF);
        buf.push_back((textLen >> 8) & 0xFF);
        buf.insert(buf.end(), text.begin(), text.end());
        session->net.sendMsg(NM_CHAT, buf.data(), buf.size());
        session->chat.addMessage(session->mpHostName, text);
        sayf(session, "chat sent");
        return;
    }
    

    if (c == "activate")
    {
        int ri = consoleRef(session);
        if (ri < 0) { sayf(session, "nothing selected"); return; }
        session->playerActivate(ri);
        sayf(session, "activated %s", session->w.refs[ri].id.c_str());
        return;
    }
    if (c == "sel" || c == "select")
    {
        session->consoleSelected = session->target;
        if (session->consoleSelected < 0) { sayf(session, "nothing under crosshair"); return; }
        const Ref& r = session->w.refs[session->consoleSelected];
        sayf(session, "selected %s (lock %d%s%s)", r.id.c_str(), r.lockLevel,
             r.actor >= 0 ? ", actor" : "", r.type.empty() ? "" : r.type.c_str());
        return;
    }

    // -------- PC Link: save export / player state
    if (c == "save" && !a.empty() && lower(a[0]) == "export")
    {
        exportSaves(session);
        return;
    }
    if (c == "pclink" && !a.empty() && lower(a[0]) == "state")
    {
        sendPlayerState(session);
        return;
    }

    // -------- debug toggles
    #define TOGGLE(name, var, alias) \
        if (c == name || c == alias) { var = !var; sayf(session, #var " %s", var ? "on" : "off"); return; }
    TOGGLE("toggleai",            g_aiOff,          "tai")
    TOGGLE("togglesky",           g_noSky,          "ts")
    TOGGLE("toggleworld",         g_noWorld,        "tw")
    TOGGLE("togglewireframe",     g_wireframe,      "twf")
    TOGGLE("togglepathgrid",      g_showPathGrid,   "tpg")
    TOGGLE("togglecollision",     g_noCollision,    "tcl")
    TOGGLE("toggleborders",       g_showCellBorders,"tb")
    TOGGLE("togglecollisionboxes",g_showBoxes,      "tcb")
    TOGGLE("togglecollisiongrid", g_showColGrid,    "tcg")
    TOGGLE("togglefogofwar",      g_mapNoFog,       "tfow")
    TOGGLE("togglefullhelp",      g_hoverFull,      "tfh")
    TOGGLE("toggledebugtext",     g_showDebugText,  "tdt")
    TOGGLE("togglegrid",          g_showCellGrid,   "tg")
    TOGGLE("togglescripts",       g_scriptsOff,     "")
    TOGGLE("toggleloadfade",      g_noLoadFade,     "")
    TOGGLE("togglecombatstats",   g_combatStats,    "tcs")
    TOGGLE("togglemagicstats",    g_magicStats,     "tms")
    TOGGLE("toggledialoguestats", g_dlgStats,       "tds")
    TOGGLE("toggletexturestring", g_showTexNames,   "tts")
    #undef TOGGLE

    // -------- cheats
    if (c == "tgm" || c == "god")
    {
        g_godMode = !g_godMode;
        sayf(session, "god mode %s", g_godMode ? "on" : "off");
        return;
    }
    if (c == "tim" || c == "infmagic")
    {
        g_infMagic = !g_infMagic;
        sayf(session, "infinite magicka & fatigue %s", g_infMagic ? "on" : "off");
        return;
    }
    if (c == "tw")
    {
        g_noWeight = !g_noWeight;
        sayf(session, "weightless %s", g_noWeight ? "on" : "off");
        return;
    }
    if (c == "speed")
    {
        int v = argi(a, 0, 100);
        if (v < 100) v = 100;
        if (v > 400) v = 400;
        g_cheatSpeed = v;
        sayf(session, "walk speed %d%%", v);
        return;
    }
    if (c == "jump")
    {
        int v = argi(a, 0, 100);
        if (v < 100) v = 100;
        if (v > 300) v = 300;
        g_cheatJump = v;
        sayf(session, "jump %d%%", v);
        return;
    }

    // -------- generic Set/Mod/Get <stat>
    if (tryStatCommand(session, c, a))
        return;

    sayf(session, "unknown command '%s' (try: help)", c.c_str());
}
