// Multiplayer remote players, drawn as animated player bodies.
//
// Each connected peer owns one Session::RemotePlayer: a PlayerBody rebuilt
// from the peer's RemotePlayerDesc (race, sex, head, hair, worn items) and
// animated from the peer's reported BodyAct. The old NPC-clone "ghost"
// system is gone; spawnGhost / moveGhost / removeGhost are kept as thin
// wrappers over RemotePlayer for the console and the test menu.
//
// FIX (invisible remote player): drawRemotePlayers used to call
// rendererDrawActor directly. This engine does CPU skinning, so without
// PlayerBody::draw's actorDeform() call the skinned parts were never posed
// and the peer never appeared. The manual draw is replaced with
// p.body.draw(w, !secondEye), and the peer's real weaponOut flag is now
// passed to playerBodyUpdateRemote instead of guessing from worn items.

#include "session.h"
#include "log.h"
#include "playerbody.h"
#include "renderer.h"

#include <cmath>
#include <cstring>

static std::string mpLower(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return s;
}

// ---------------------------------------------------------------------------
// Applying a packet to a slot
// ---------------------------------------------------------------------------
void Session::applyRemotePlayerState(int slot, const RemotePlayerDesc& desc, const float feet[3],
                                     float yaw, float pitch, const BodyAct& act, float moveX, float moveY,
                                     float moveSpeed, bool weaponOut)
{
    if (slot < 0 || slot >= NetEngine::kMaxPeers)
        return;
    RemotePlayer& p = remotePlayers[slot];

    // Keep the weapon mesh visible through the whole swing / cast. A packet that
    // arrives with weaponOut=0 mid-WINDUP would otherwise strip the weapon.
    bool wantWeapon = weaponOut
        || act.action == BODY_WINDUP
        || act.action == BODY_STRIKE
        || act.action == BODY_CAST;

    bool rebuild =
        p.desc.race != desc.race ||
        p.desc.female != desc.female ||
        p.desc.head != desc.head ||
        p.desc.hair != desc.hair ||
        p.desc.worn != desc.worn ||
        p.weaponOut != wantWeapon ||
        !p.body.ready;

    p.desc = desc;
    p.weaponOut = wantWeapon;
    p.moveSpeed = moveSpeed;
    memcpy(p.feet, feet, sizeof(p.feet));
    p.yaw = yaw;
    p.pitch = pitch;
    p.act = act;
    p.moveX = moveX;
    p.moveY = moveY;
    p.updateAge = 0.0f;
    p.moveSpeed = moveSpeed;
    p.active = true;

    // Death pose: keep the body on Death1 held until they respawn / reconnect
    if (p.dead)
    {
        p.act.action = BODY_DEATH;
        if (p.act.group.empty() || p.act.group.find("Death") == std::string::npos)
            p.act.group = "Death1";
        p.moveX = p.moveY = 0.0f;
        p.moveSpeed = 0.0f;
        p.weaponOut = false;
        wantWeapon = false;
        rebuild = rebuild || p.body.ready; // force rebuild to drop the weapon
    }

    if (rebuild)
    {
        playerBodyRebuildRemote(p.body, w, desc, wantWeapon);
        logf("mp: remote slot %d rebuilt: %s%s, %zu items, weapon %d name=%s",
             slot, desc.race.c_str(), desc.female ? " F" : " M", desc.worn.size(), wantWeapon ? 1 : 0,
             p.name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Building the local broadcast
// ---------------------------------------------------------------------------
// Third-person attack group for the weapon (mirrors combat.cpp::attackGroup +
// the local-body Chop/Slash/Thrust suffix). Used so remote peers see the same
// swing the local third-person body plays, including follow-through.
static std::string mpBodyAttackGroup(const Object* wpn, bool tool, int attackKind)
{
    const char* base = "AttackHH";
    if (!wpn)
        base = tool ? "Attack1h" : "AttackHH";
    else switch (wpn->subtype)
    {
    case 2: case 4: case 8:          base = "Attack2c";    break;
    case 5: case 6:                  base = "Attack2w";    break;
    case WEAP_BOW:                   base = "AttackBow";   break;
    case WEAP_CROSSBOW:              base = "AttackXbow";  break;
    case WEAP_THROWN:                base = "AttackThrow"; break;
    default:                         base = "Attack1h";    break;
    }
    // Melee only: append Sl / Th when the wind-up chose slash or thrust
    static const char* kTag[] = { "", "Sl", "Th" };
    if (wpn && wpn->subtype < WEAP_BOW && attackKind >= 0 && attackKind < 3)
        return std::string(base) + kTag[attackKind];
    if (!wpn && tool && attackKind >= 0 && attackKind < 3)
        return std::string(base) + kTag[attackKind];
    return base;
}

Session::LocalPlayerState Session::buildLocalPlayerState() const
{
    LocalPlayerState s{};
    s.desc.race = w.stats.race;
    s.desc.female = w.stats.female;
    s.desc.head = w.stats.head;
    s.desc.hair = w.stats.hair;
    for (const InventoryItem& it : w.inventory)
        if (it.equipped && w.game.object(it.id))
            s.desc.worn.push_back(mpLower(it.id));

    memcpy(s.feet, w.player.feet, sizeof(s.feet));
    s.yaw = w.player.yaw;
    s.pitch = w.player.pitch;
    s.weaponOut = weaponDrawn;

    const Object* wpn = const_cast<Session*>(this)->playerWeapon();
    bool tool = const_cast<Session*>(this)->playerToolItem() != nullptr;
    // Always resolve the attack group (incl. Chop/Slash/Thrust) so wind-up and
    // strike packets carry the correct name — previously group was only set in
    // the idle branch and remote bodies played empty/wrong anims.
    std::string atkGroup = mpBodyAttackGroup(wpn, tool, attackKind);

    s.act.action = BODY_NONE;
    s.act.group = atkGroup;
    if (playerDead)
    {
        s.act.action = BODY_DEATH;
        s.act.group = "Death1";
        s.weaponOut = false;
        s.moveX = s.moveY = 0.0f;
        s.speed = 0.0f;
    }
    else if (vm.action == VM_CAST)
    {
        s.act.action = BODY_CAST;
        int range = castDef.effects.empty() ? 0 : castDef.effects[0].range;
        s.act.group = range == 2 ? "CastTarget" : range == 1 ? "CastTouch" : "CastSelf";
        // Keep weapon mesh visible while casting with weapon drawn
        s.weaponOut = weaponDrawn;
    }
    else if (weaponDrawn)
    {
        if (vm.action == VM_WINDUP)      s.act.action = BODY_WINDUP;
        else if (vm.action == VM_FOLLOW) s.act.action = BODY_STRIKE;
        // else BODY_NONE: ready stance with weapon out (Idle1h / WalkForward2c …)
        s.act.group = atkGroup;
        s.weaponOut = true;   // force weapon mesh on while drawn, even mid-swing
    }
    else
    {
        s.act.group = "AttackHH";
        s.weaponOut = false;
    }

    s.act.charge = vm.charge;
    s.act.speed = vm.speed > 0.0f ? vm.speed : 1.0f;
    // blowStrength is latched on release; during wind-up send current charge so
    // a late-joining peer still gets a sensible follow-through if they only
    // see the STRIKE packet.
    s.act.strength = (s.act.action == BODY_STRIKE) ? blowStrength
                   : (s.act.action == BODY_WINDUP) ? fmaxf(0.1f, vm.charge)
                   : blowStrength;
    s.act.stance = vm.group == "Bow" || vm.group == "Throw" ? "1h"
                 : vm.group == "Xbow" ? "2c"
                 : (vm.group.empty() ? (wpn ? (wpn->subtype == 2 || wpn->subtype == 4 ||
                                               wpn->subtype == 8 || wpn->subtype == WEAP_CROSSBOW) ? "2c"
                                             : (wpn->subtype == 5 || wpn->subtype == 6) ? "2w" : "1h"
                                           : "HH")
                                     : vm.group.c_str());

    // Movement intent: prefer the real ground velocity measured in netUpdate
    // (mpVel). Fall back to inertia when standing still / jumping.
    float speed2 = w.player.inertia[0] * w.player.inertia[0] + w.player.inertia[1] * w.player.inertia[1];
    s.speed = sqrtf(speed2);
    if (speed2 > 4.0f)
    {
        float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
        float ix = w.player.inertia[0], iy = w.player.inertia[1];
        float il = sqrtf(speed2);
        s.moveX = (ix * fy - iy * fx) / il;      // right (+)
        s.moveY = (ix * fx + iy * fy) / il;      // forward (+)
    }
    else
        s.moveX = s.moveY = 0.0f;

    return s;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void Session::drawRemotePlayers(bool secondEye)
{
    // PlayerBody::update reads w.player.feet / yaw. Save the local player,
    // drive w.player to each peer while its body updates, then restore.
    Player localSaved = w.player;

    for (int slot = 0; slot < NetEngine::kMaxPeers; slot++)
    {
        RemotePlayer& p = remotePlayers[slot];
        if (!p.active || !p.body.ready || p.updateAge > 5.0f)
            continue;
        float ddx = p.feet[0] - w.player.feet[0];
        float ddy = p.feet[1] - w.player.feet[1];
        if (ddx * ddx + ddy * ddy > 3000.0f * 3000.0f)
            continue;

        memcpy(w.player.feet, p.feet, sizeof(w.player.feet));
        w.player.yaw = p.yaw;

        BodyAct act = p.act;
        float mx = p.moveX, my = p.moveY, ms = p.moveSpeed;
        bool wout = p.weaponOut;
        if (p.dead)
        {
            act.action = BODY_DEATH;
            if (act.group.empty() || act.group.find("Death") == std::string::npos)
                act.group = "Death1";
            mx = my = 0.0f;
            ms = 0.0f;
            wout = false;
        }
        else if (p.knockTimer > 0.0f)
        {
            // Peer is on the ground — hold KnockOut until timer clears
            act.action = BODY_DEATH; // HOLD path in PlayerBody
            act.group = "KnockOut";
            mx = my = 0.0f;
            ms = 0.0f;
            if (!secondEye)
                p.knockTimer = fmaxf(0.0f, p.knockTimer - frameDtRemote);
        }

        playerBodyUpdateRemote(p.body, w, secondEye ? 0.0f : frameDtRemote, p.desc, act,
                               mx, my, ms, wout);

        p.body.draw(w, !secondEye);
    }

    w.player = localSaved;
}

// ---------------------------------------------------------------------------
// Legacy wrappers (console / menu use these)
// ---------------------------------------------------------------------------
int Session::spawnGhost(const std::string& name, const float pos[3], float yaw)
{
    RemotePlayerDesc desc;
    desc.race = w.stats.race;
    desc.female = w.stats.female;
    desc.head = w.stats.head;
    desc.hair = w.stats.hair;
    for (const InventoryItem& it : w.inventory)
        if (it.equipped && w.game.object(it.id))
            desc.worn.push_back(mpLower(it.id));
    BodyAct act; act.group = "AttackHH";
    for (int slot = 0; slot < NetEngine::kMaxPeers; slot++)
        if (!remotePlayers[slot].active) {
            applyRemotePlayerState(slot, desc, pos, yaw, 0.0f, act, 0.0f, 0.0f, 0.0f, !desc.worn.empty());
            logf("mp: spawned test ghost '%s' at slot %d", name.c_str(), slot);
            return slot;
        }
    return -1;
}

void Session::moveGhost(int ref, const float pos[3], float yaw)
{
    if (ref < 0 || ref >= NetEngine::kMaxPeers)
        return;
    memcpy(remotePlayers[ref].feet, pos, sizeof(remotePlayers[ref].feet));
    remotePlayers[ref].yaw = yaw;
}

void Session::removeGhost(int ref)
{
    if (ref < 0 || ref >= NetEngine::kMaxPeers)
        return;
    remotePlayers[ref] = RemotePlayer();
}

int Session::removeAllGhosts()
{
    int n = 0;
    for (int i = 0; i < NetEngine::kMaxPeers; i++) {
        if (remotePlayers[i].active)
            n++;
        remotePlayers[i] = RemotePlayer();
    }
    mpClientRef = mpHostRef = -1;
    return n;
}
