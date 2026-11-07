// Arrows, bolts, thrown weapons and spell bolts in flight. They fly straight, and hit the first
// actor (an upright cylinder) or wall along their way each frame.
//   marksman hit chance  as melee with the Marksman skill (unaware targets are always hit)
//   damage  (launcher chop + ammunition chop, by how far the shot was drawn; thrown weapons count
//           twice) x (0.5 + strength / 100), then armor as for blows
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "log.h"
#include "renderer.h"
#include "session.h"

static const float kActorRadius = 34.0f, kActorHeight = 135.0f;
static const float kPlayerRadius = 26.0f, kPlayerHeight = 128.0f;
static const int SKILL_MARKSMAN = 23;


// Segment a + t (b - a), t in 0..1, against an upright cylinder: earliest t or -1
static float hitCylinder(const float a[3], const float b[3], const float base[3], float radius, float height)
{
	float dx = b[0] - a[0], dy = b[1] - a[1];
	float fx = a[0] - base[0], fy = a[1] - base[1];
	float A = dx * dx + dy * dy, B = 2.0f * (fx * dx + fy * dy), C = fx * fx + fy * fy - radius * radius;
	float t;
	if (C <= 0.0f)
		t = 0.0f;                                  // starts inside
	else
	{
		if (A < 1e-6f)
			return -1.0f;
		float disc = B * B - 4.0f * A * C;
		if (disc < 0.0f)
			return -1.0f;
		t = (-B - sqrtf(disc)) / (2.0f * A);
		if (t < 0.0f || t > 1.0f)
			return -1.0f;
	}
	float z = a[2] + (b[2] - a[2]) * t;
	return z >= base[2] && z <= base[2] + height ? t : -1.0f;
}

static u32 spellGlow(const SpellDef& sp)
{
	for (auto& e : sp.effects)
		switch (e.effect)
		{
		case 14: return 0xFF2070FF;                // fire
		case 15: return 0xFFFF60C0;                // shock
		case 16: return 0xFFFFD090;                // frost
		case 27: return 0xFF40E060;                // poison
		default: break;
		}
	return 0xFFE0C0FF;
}

void Session::fireProjectile(int owner, const float from[3], const float dir[3], float speed, const std::string& item,
	const std::string& weapon, float charge, const std::string& spell)
{
	Projectile p;
	for (int k = 0; k < 3; k++)
	{
		p.pos[k] = from[k];
		p.vel[k] = dir[k] * speed;
	}
	p.owner = owner;
	p.item = item;
	p.weapon = weapon;
	p.charge = charge;
	p.spell = spell;
	p.life = 4.0f;
	if (!spell.empty())
	{
		auto it = w.game.spells.find(spell);
		p.glow = it != w.game.spells.end() ? spellGlow(it->second) : 0xFFFFFFFF;
		if (it != w.game.spells.end())
			p.bolt = boltPiece(it->second);
		p.life = 3.0f;
	}
	// Client in MP: local bolts are predictive visuals; host owns hits after the spawn packet.
	if (mpMode == 2 && net.isConnected())
		p.mpVisualOnly = true;
	projectiles.push_back(p);
	// Multiplayer: tell peers so they can draw the same bolt
	if ((mpMode == 1 || mpMode == 2) && net.isConnected())
		mpBroadcastProjectile(p);
}

void Session::mpBroadcastProjectile(const Projectile& p)
{
	std::vector<u8> buf;
	// owner: -1 player, >=0 NPC host-ref (client maps via cell if needed later)
	int owner = p.owner;
	buf.insert(buf.end(), (u8*)&owner, (u8*)&owner + 4);
	buf.insert(buf.end(), (u8*)p.pos, (u8*)p.pos + 12);
	buf.insert(buf.end(), (u8*)p.vel, (u8*)p.vel + 12);
	buf.insert(buf.end(), (u8*)&p.charge, (u8*)&p.charge + 4);
	buf.insert(buf.end(), (u8*)&p.life, (u8*)&p.life + 4);
	buf.insert(buf.end(), (u8*)&p.glow, (u8*)&p.glow + 4);
	auto putStr = [&](const std::string& s) {
		u8 n = (u8)std::min((size_t)255, s.size());
		buf.push_back(n);
		buf.insert(buf.end(), s.begin(), s.begin() + n);
	};
	putStr(p.item);
	putStr(p.weapon);
	putStr(p.spell);
	putStr(p.bolt);
	if (mpMode == 1)
		net.broadcastMsg(NM_PROJECTILE, buf.data(), (u32)buf.size());
	else
		net.sendMsg(NM_PROJECTILE, buf.data(), (u32)buf.size());
}

void Session::mpApplyProjectile(const u8* data, u32 len, int peer)
{
	if (len < 4 + 12 + 12 + 4 + 4 + 4 + 4) return;
	const u8* p = data;
	const u8* end = data + len;
	Projectile pr;
	memcpy(&pr.owner, p, 4); p += 4;
	memcpy(pr.pos, p, 12); p += 12;
	memcpy(pr.vel, p, 12); p += 12;
	memcpy(&pr.charge, p, 4); p += 4;
	memcpy(&pr.life, p, 4); p += 4;
	memcpy(&pr.glow, p, 4); p += 4;
	auto takeStr = [&](std::string& s) -> bool {
		if (p >= end) return false;
		u8 n = *p++;
		if (p + n > end) return false;
		s.assign((const char*)p, n);
		p += n;
		return true;
	};
	if (!takeStr(pr.item) || !takeStr(pr.weapon) || !takeStr(pr.spell) || !takeStr(pr.bolt))
		return;
	// From a client: host must own the sim (hit NPCs / players). From host: clients only draw.
	if (mpMode == 1) {
		// Client player shot → host simulates as a player-owned bolt
		if (pr.owner < 0) {
			pr.owner = -1;
			pr.mpFromPeer = peer;
			pr.mpVisualOnly = false;
		} else {
			// Should not happen often (client NPC AI is off); still spawn visual
			pr.mpVisualOnly = true;
		}
		projectiles.push_back(pr);
		// Rebroadcast to other clients only (originator already has a local bolt)
		std::vector<u8> out;
		int owner = pr.owner;
		out.insert(out.end(), (u8*)&owner, (u8*)&owner + 4);
		out.insert(out.end(), (u8*)pr.pos, (u8*)pr.pos + 12);
		out.insert(out.end(), (u8*)pr.vel, (u8*)pr.vel + 12);
		out.insert(out.end(), (u8*)&pr.charge, (u8*)&pr.charge + 4);
		out.insert(out.end(), (u8*)&pr.life, (u8*)&pr.life + 4);
		out.insert(out.end(), (u8*)&pr.glow, (u8*)&pr.glow + 4);
		auto putStr = [&](const std::string& s) {
			u8 n = (u8)std::min((size_t)255, s.size());
			out.push_back(n);
			out.insert(out.end(), s.begin(), s.begin() + n);
		};
		putStr(pr.item);
		putStr(pr.weapon);
		putStr(pr.spell);
		putStr(pr.bolt);
		for (int i = 0; i < NetEngine::kMaxPeers; i++)
			if (i != peer)
				net.sendMsgTo(i, NM_PROJECTILE, out.data(), (u32)out.size());
	} else {
		pr.mpVisualOnly = true;
		pr.mpFromPeer = -1;
		projectiles.push_back(pr);
	}
	logf("mp-proj: %s spawn owner=%d spell=%s item=%s",
	     mpMode == 1 ? "HOST" : "CLIENT", pr.owner, pr.spell.c_str(), pr.item.c_str());
}

// What happens when it reaches someone: victim is a ref index, or -1 for the player
// A shot that misses or strikes a wall still wears the player's bow or crossbow by 1 (OpenMW's reduceWeaponCondition
// with no hit); a thrown weapon is gone anyway
void Session::wearLauncher(const Projectile& p)
{
	InventoryItem* bow = playerWeaponItem();
	if (p.owner < 0 && bow && !(testGod && testGodBlows) && p.weapon != p.item && lower(bow->id) == p.weapon)
		wearItem(bow, 0.0f);
}

void Session::projectileHits(Projectile& p, int victim)
{
	if (!p.spell.empty())
	{
		auto it = w.game.spells.find(p.spell);
		if (it == w.game.spells.end())
			return;
		const SpellDef& sp = it->second;
		for (auto& e : sp.effects)
		{
			if (e.range != 2)
				continue;
			if (victim < 0)
				applyEffectToPlayer(e.effect == 86 ? SpellEffect{ 23, e.skill, e.attribute, e.min, e.max, e.duration, 2 } : e, sp.name, p.owner, (float)sp.cost);
			else
				applyEffectToActor(victim, e, p.owner < 0, (float)sp.cost, p.owner < 0 ? (float)castChance(sp) : 100.0f, sp.name);
		}
		playSound(victim, spellSound(sp, 2));
		{
			// the hit visual at the victim's feet (it is modelled around them), the burst around the chest
			float feet[3] = { victim < 0 ? w.player.feet[0] : w.refs[victim].pos[0], victim < 0 ? w.player.feet[1] : w.refs[victim].pos[1],
				victim < 0 ? w.player.feet[2] : w.refs[victim].pos[2] };
			float at[3] = { feet[0], feet[1], feet[2] + 60.0f };
			spellVfx(sp, 1, feet, victim < 0 ? -1 : victim, victim < 0 ? w.player.yaw : w.refs[victim].rot[2]);
			areaBurst(sp, 2, at, victim, p.owner);
		}
		logf("combat: %s hits %s", sp.name.c_str(), victim < 0 ? "the player" : w.refs[victim].id.c_str());
		return;
	}
	const Object* ammo = w.game.object(p.item);
	const Object* launcher = w.game.object(p.weapon);
	if (!ammo)
		return;
	float c = p.charge;
	float dmg = ammo->chop[0] + (ammo->chop[1] - ammo->chop[0]) * c;
	if (launcher && launcher != ammo)
		dmg += launcher->chop[0] + (launcher->chop[1] - launcher->chop[0]) * c;
	else
		dmg *= 2.0f;                                // thrown: weapon and ammunition at once
	if (p.owner < 0 && victim >= 0)
	{
		Ref& t = w.refs[victim];
		const ActorDef& def = w.game.actors[t.actor];
		const PlayerStats& s = w.stats;
		const bool fromPeer = p.mpFromPeer >= 0;
		// OpenMW's getHitChance with Marksman (evasion 0 when unaware, knocked down or paralyzed)
		// Peer shots: host has no remote Marksman skill — use a neutral attack term (~50)
		bool aware = npcAware(victim);
		float atkTerm = fromPeer ? 50.0f
			: attackTermOf(s.skills[SKILL_MARKSMAN], s.attributes[ATTR_AGILITY], s.attributes[ATTR_LUCK],
				s.fatigue, s.fatigueMax, w.effectTotal(117), w.effectTotal(47));
		float chance = roundf(atkTerm - npcDefense(victim, !aware || t.knockTimer > 0.0f));
		// Resist Normal Weapons (OpenMW): a thrown weapon, or a plain launcher, leaves it to the missile to get through
		bool launcherSpecial = launcher && launcher != ammo && ((launcher->flags & 3) || launcher->magic);
		bool missileSpecial = (ammo->flags & 3) || ammo->magic;
		float normalResist = normalWeaponResist(victim);
		bool resisted = normalResist != 0.0f && !launcherSpecial && !missileSpecial;
		bool immune = resisted && normalResist >= 100.0f;
		// GBAC (Options > Combat): glances and crits (x1.5 at range); sneak shots stay vanilla
		BlowRoll roll = rollBlow(chance, true, !aware || immune);
		bool hit = roll.lands;
		logf("combat: %s at %s, chance %d%%, %s%s", p.item.c_str(), t.id.c_str(), (int)chance,
			hit ? "hit" : "miss", fromPeer ? " (peer)" : "");
		if (!hit)
		{
			if (!fromPeer)
			{
				addIndicator("Miss (" + std::to_string((int)fmaxf(0.0f, chance)) + "%)", 0xffffff);
				wearLauncher(p);
			}
			else
				mpSendSwingResult(p.mpFromPeer, victim, 0 /*miss*/, chance, 0.0f);
			bool peaceful = t.ai != AI_COMBAT;
			makeHostile(victim);
			if (peaceful && !def.creature && !t.aggressor)
				crimeSeen(CRIME_ASSAULT, 0, victim), reportCrime(victim, kBountyAssault);
			return;
		}
		if (!fromPeer)
			useSkill(SKILL_MARKSMAN, 0, roll.skill);
		blowSound(victim, roll);
		float str = fromPeer ? 50.0f : (float)s.attributes[ATTR_STRENGTH];
		dmg *= (w.game.gmstf("fdamagestrengthbase", 0.5f) + 0.1f * w.game.gmstf("fdamagestrengthmult", 0.1f) * str)
			* roll.damage;
		if (!fromPeer)
		{
			InventoryItem* bow = playerWeaponItem();
			if (bow && launcher && lower(bow->id) == lower(launcher->id) && launcher->health > 0)
			{
				dmg *= (float)itemCondition(*bow) / launcher->health;
				wearItem(bow, dmg * w.game.gmstf("fweapondamagemult", 0.1f));
			}
		}
		if (resisted)
			dmg *= 1.0f - fminf(1.0f, normalResist / 100.0f);
		// On-strike enchant (host owns effects for peer shots)
		if (fromPeer)
			hostStrikeEnchantment(victim, p.weapon, ammo);
		else
			strikeEnchantment(victim, ammo);
		if (!npcShieldsBurn(victim))
		{
			if (fromPeer)
				mpSendSwingResult(p.mpFromPeer, victim, 2 /*blocked*/, chance, 0.0f);
			return;
		}
		u8 result = roll.kind == 0 ? 3 : (roll.kind == 2 ? 4 : 1);
		if (fromPeer)
			mpSendSwingResult(p.mpFromPeer, victim, result, chance, dmg);
		playerHitsNpc(victim, dmg, SKILL_MARKSMAN, false, true);
		// Some arrows can be taken back from the body: fProjectileThrownStoreChance percent, not enchanted ones
		if (!ammo->magic && rand() % 100 < (int)w.game.gmstf("fprojectilethrownstorechance", 25.0f))
			t.contents.emplace_back(1, lower(p.item));
	}
	else if (p.owner >= 0 && victim < 0)
	{
		Ref& r = w.refs[p.owner];
		const ActorDef& def = w.game.actors[r.actor];
		const int* pa = w.stats.attributes;
		bool helpless = w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f;
		float chance = roundf(attackTermOf(def.skills[SKILL_MARKSMAN], def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK],
			r.fatigue, r.fatigueMax, w.actorEffect(p.owner, 117), w.actorEffect(p.owner, 47)) - playerDefense(helpless));
		(void)pa;
		BlowRoll roll = rollBlow(chance, true, false);
		if (!roll.lands)
		{
			logf("combat: %s's %s misses", r.id.c_str(), p.item.c_str());
			return;
		}
		blowSound(-1, roll);
		npcStrikeEnchantment(p.owner, ammo);
		if (!playerShieldsBurn(p.owner))
			return;
		npcHitsPlayer(p.owner, dmg * (w.game.gmstf("fdamagestrengthbase", 0.5f) + 0.1f * w.game.gmstf("fdamagestrengthmult", 0.1f)
			* def.attributes[ATTR_STRENGTH]) * roll.damage, false, SKILL_MARKSMAN, false, true);
	}
}

void Session::updateProjectiles(float dt)
{
	for (size_t i = 0; i < projectiles.size();)
	{
		Projectile& p = projectiles[i];
		p.life -= dt;
		float next[3] = { p.pos[0] + p.vel[0] * dt, p.pos[1] + p.vel[1] * dt, p.pos[2] + p.vel[2] * dt };
		float best = 2.0f;
		int victim = -2;                             // -2 nothing, -1 player, >= 0 ref
		// Visual-only bolts (seen from another peer) never apply damage
		const bool canHit = !p.mpVisualOnly;
		// Actors (the shooter's own arrows pass through them; NPC shots only hurt the player)
		if (canHit && p.owner < 0)
			w.forLoadedActors([&](int k) {
				const Ref& r = w.refs[k];
				if (r.actor < 0 || r.dead || !r.visible())
					return;
				float t = hitCylinder(p.pos, next, r.pos, kActorRadius, kActorHeight);
				if (t >= 0.0f && t < best)
				{
					best = t;
					victim = k;
				}
			});
		else if (canHit && p.owner >= 0)
		{
			// NPC shot: local player
			if (!playerDead)
			{
				float t = hitCylinder(p.pos, next, w.player.feet, kPlayerRadius, kPlayerHeight);
				if (t >= 0.0f && t < best)
				{
					best = t;
					victim = -1;
				}
			}
			// Host: also test remote player bodies
			if (mpMode == 1 && net.isConnected())
			{
				for (int s = 0; s < NetEngine::kMaxPeers; s++)
				{
					RemotePlayer& rp = remotePlayers[s];
					if (!rp.active || rp.dead) continue;
					float t = hitCylinder(p.pos, next, rp.feet, kPlayerRadius, kPlayerHeight);
					if (t >= 0.0f && t < best)
					{
						best = t;
						victim = -100 - s;  // encode remote slot
					}
				}
			}
		}
		// Walls and the ground
		bool wall = false;
		for (LoadedCell* l : w.loaded)
		{
			float t;
			if (collisionRaycast(l->cell.collision, p.pos, next, &t) && t < best)
			{
				best = t;
				wall = true;
				victim = -2;
			}
		}
		if (victim != -2)
		{
			Projectile hit = p;
			projectiles.erase(projectiles.begin() + i);
			// Remote player hit by NPC shot (host only): send damage, skip local projectileHits
			if (victim <= -100)
			{
				int slot = -100 - victim;
				if (slot >= 0 && slot < NetEngine::kMaxPeers && !hit.spell.empty())
				{
					// Spell bolt on remote: simplified flat damage
					mpSendPlayerDamage(slot, 15.0f * hit.charge, false);
				}
				else if (slot >= 0 && slot < NetEngine::kMaxPeers)
				{
					const Object* ammo = w.game.object(hit.item);
					float dmg = ammo ? (ammo->chop[0] + (ammo->chop[1] - ammo->chop[0]) * hit.charge) * 0.5f : 10.0f;
					dmg *= difficultyScale(true);
					mpSendPlayerDamage(slot, dmg, false);
				}
				continue;
			}
			projectileHits(hit, victim);
			continue;
		}
		if (wall || p.life <= 0.0f)
		{
			if (wall && !p.spell.empty())
			{
				auto it = w.game.spells.find(p.spell);
				playSound(-1, it != w.game.spells.end() ? spellSound(it->second, 2) : std::string("destruction hit"));
				// where it struck the wall: back off a little so the burst isn't inside it
				if (it != w.game.spells.end())
				{
					float at[3];
					for (int k = 0; k < 3; k++)
						at[k] = p.pos[0 + k] + p.vel[k] * dt * std::max(0.0f, best - 0.05f);
					areaBurst(it->second, 2, at, -2, p.owner);
				}
			}
			if (wall && p.spell.empty())
				wearLauncher(p);
			projectiles.erase(projectiles.begin() + i);
			continue;
		}
		for (int k = 0; k < 3; k++)
			p.pos[k] = next[k];
		i++;
	}
}

void Session::spawnVfx(const std::string& staticId, const float pos[3], float life, int follow, float yaw, float scale, bool loop)
{
	auto it = w.game.firstPerson.vfx.find(lower(staticId));
	if (it == w.game.firstPerson.vfx.end())
		return;
	// Many casters at once: only those near the player show, the nearest when there are too many
	auto dist2 = [&](const float* p) {
		float dx = p[0] - w.player.feet[0], dy = p[1] - w.player.feet[1], dz = p[2] - w.player.feet[2];
		return dx * dx + dy * dy + dz * dz;
	};
	float d2 = dist2(pos);
	if (d2 > kVfxRange * kVfxRange)
		return;
	if ((int)vfx.size() >= kMaxVfx)
	{
		size_t far = 0;
		for (size_t i = 1; i < vfx.size(); i++)
			if (dist2(vfx[i].pos) > dist2(vfx[far].pos))
				far = i;
		if (dist2(vfx[far].pos) <= d2)
			return;
		vfx.erase(vfx.begin() + far);
	}
	// (a continuous one cast again takes the old one's place: one bubble, not two)
	if (loop)
		for (size_t i = 0; i < vfx.size();)
			if (vfx[i].loop && vfx[i].piece == it->second && vfx[i].follow == follow)
				vfx.erase(vfx.begin() + i);
			else
				i++;
	// a visual with its own animation lasts as long as that (OpenMW drops a non-looping effect when it has played)
	FpPiece* piece = vm.piece(w, it->second);
	Vfx v;
	v.piece = it->second;
	v.what = lower(staticId);
	memcpy(v.pos, pos, sizeof(v.pos));
	v.age = 0.0f;
	v.life = piece && piece->length > 0.0f && !loop ? piece->length : life;
	v.loop = loop;
	v.follow = follow;
	v.yaw = yaw;
	v.scale = scale;
	vfx.push_back(v);
}

void Session::spellVfx(const SpellDef& sp, int kind, const float pos[3], int follow, float yaw, int range)
{
	static const char* kDefault[3] = { "vfx_defaultcast", "vfx_defaulthit", "vfx_defaultarea" };
	std::vector<std::pair<std::string, float>> shown;
	for (auto& e : sp.effects)
	{
		if (kind == 2 && (e.range != range || e.area <= 0))
			continue;
		auto me = w.game.magicEffects.find(e.effect);
		if (me == w.game.magicEffects.end())
			continue;
		const std::string& id = kind == 0 ? me->second.cvfx : kind == 1 ? me->second.hvfx : me->second.avfx;
		std::pair<std::string, float> one(id.empty() ? std::string(kDefault[kind]) : id, kind == 2 ? e.area * 2.0f : 1.0f);
		if (std::find(shown.begin(), shown.end(), one) != shown.end())
			continue;
		shown.push_back(one);
		// a continuous effect's hit repeats for as long as the effect lasts (OpenMW's ContinuousVfx: a shield's bubble)
		bool loop = kind == 1 && (me->second.flags & 0x20) && e.duration > 1;
		spawnVfx(one.first, pos, loop ? (float)e.duration : 1.2f, follow, yaw, one.second, loop);
		if (kind == 1)
			return;                                    // one hit visual a spell (the first effect's)
	}
}

void Session::castVfx(const SpellDef& sp, int caster)
{
	const float* feet = caster < 0 ? w.player.feet : w.refs[caster].pos;
	spellVfx(sp, 0, feet, caster, caster < 0 ? w.player.yaw : w.refs[caster].rot[2]);
}

// A bolt flies as its first target effect's bolt model, VFX_DefaultBolt for an effect without one (OpenMW's
// ProjectileManager::launchMagicBolt); looked up once, when it leaves
std::string Session::boltPiece(const SpellDef& sp)
{
	for (auto& e : sp.effects)
	{
		if (e.range != 2)
			continue;
		auto me = w.game.magicEffects.find(e.effect);
		auto pc = w.game.firstPerson.vfx.find(me == w.game.magicEffects.end() || me->second.bvfx.empty() ? std::string("vfx_defaultbolt")
			: me->second.bvfx);
		if (pc != w.game.firstPerson.vfx.end())
			return pc->second;
	}
	return "";
}

// Projectiles, then the player's arms over everything (after rendererDrawWorld); arms false: a view held elsewhere for
// a side-by-side shot, no arms (the body still, in third person)
void Session::drawExtras(const float eye[3], bool secondEye, bool arms)
{
	for (auto& p : projectiles)
	{
		if (!p.spell.empty())
		{
			// the bolt's own visual (found when it left), else a glow of its school's colour
			FpPiece* bolt = p.bolt.empty() ? nullptr : vm.piece(w, p.bolt);
			if (!bolt)
			{
				rendererDrawGlow(p.pos, 9.0f, p.glow);
				continue;
			}
			float hb = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
			C3D_Mtx bm;
			Mtx_Identity(&bm);
			Mtx_Translate(&bm, p.pos[0], p.pos[1], p.pos[2], true);
			Mtx_RotateZ(&bm, -atan2f(p.vel[0], p.vel[1]), true);
			Mtx_RotateX(&bm, atan2f(p.vel[2], hb), true);
			float age = bolt->length > 0.0f ? fmodf(3.0f - p.life, bolt->length) : 0.0f;    // its animation over and over
			for (auto& m : bolt->meshes)
			{
				C3D_Mtx at;
				if (fpTrackMatrix(m, bm, age, at))
					rendererDrawMesh(m, &at, bolt->textures[m.tex]);
			}
			continue;
		}
		auto it = w.game.firstPerson.items.find(p.item);
		FpPiece* piece = it != w.game.firstPerson.items.end() ? vm.piece(w, it->second.model) : nullptr;
		if (!piece)
			continue;
		// The model's +Y points along the flight
		float h = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
		C3D_Mtx model;
		Mtx_Identity(&model);
		Mtx_Translate(&model, p.pos[0], p.pos[1], p.pos[2], true);
		Mtx_RotateZ(&model, -atan2f(p.vel[0], p.vel[1]), true);
		Mtx_RotateX(&model, atan2f(p.vel[2], h), true);
		u32 glow = w.game.enchantGlow(p.item);      // an enchanted arrow or dart shimmers in flight
		for (auto& m : piece->meshes)
			rendererDrawMeshGlow(m, &model, piece->textures[m.tex], glow);
	}
	// Spell visuals of the moment, facing as their actor faces (none past kVfxRange from the eye: many casters at once)
	for (auto& v : vfx)
	{
		float dx = v.pos[0] - eye[0], dy = v.pos[1] - eye[1];
		if (dx * dx + dy * dy > kVfxRange * kVfxRange)
			continue;
		FpPiece* piece = vm.piece(w, v.piece);
		if (!piece)
			continue;
		C3D_Mtx vm_;
		Mtx_Identity(&vm_);
		Mtx_Translate(&vm_, v.pos[0], v.pos[1], v.pos[2], true);
		Mtx_RotateZ(&vm_, -v.yaw, true);
		// (a piece with no animation of its own swells into place)
		float grow = v.scale * (piece->length > 0.0f ? 1.0f : 0.6f + 0.4f * fminf(1.0f, v.age / 0.25f));
		Mtx_Scale(&vm_, grow, grow, grow);
		float age = v.loop && piece->length > 0.0f ? fmodf(v.age, piece->length) : v.age;   // (a continuous one over and over)
		for (auto& m : piece->meshes)
		{
			C3D_Mtx at;
			if (fpTrackMatrix(m, vm_, age, at))
				rendererDrawMesh(m, &at, piece->textures[m.tex]);
		}
	}
	// Dropped items, with their own mesh where they lie
	for (int i : w.spawned)
	{
		const Ref& r = w.refs[i];
		if (!r.dropped || !r.visible() || r.cell < 0 || !w.cells[w.placeOf(i)].live)
			continue;
		float dx = r.pos[0] - eye[0], dy = r.pos[1] - eye[1];
		if (dx * dx + dy * dy > 3000.0f * 3000.0f)
			continue;
		auto it = w.game.firstPerson.items.find(r.idLower);
		FpPiece* piece = it != w.game.firstPerson.items.end() ? vm.piece(w, it->second.model) : nullptr;
		if (!piece)
			continue;
		C3D_Mtx model;
		Mtx_Identity(&model);
		Mtx_Translate(&model, r.pos[0], r.pos[1], r.pos[2], true);
		Mtx_RotateZ(&model, -r.rot[2], true);
		u32 glow = w.game.enchantGlow(r.idLower);   // an enchanted one shimmers where it lies
		for (auto& m : piece->meshes)
			rendererDrawMeshGlow(m, &model, piece->textures[m.tex], glow);
	}
	// Multiplayer: draw remote peers as animated player bodies.
	drawRemotePlayers(secondEye);
	// Companion: dressable PlayerBody (clothes/armor come off when taken from the pack)
	drawCompanionBody(secondEye);
	// Client view of the host's dressable companion
	drawRemoteCompanionBody(secondEye);

	if (!arms && !thirdPerson)
		return;
	if (thirdPerson || previewFace)
		body.draw(w, !secondEye);
	else
		vm.draw(eye, w.player.yaw, w.player.pitch, !secondEye);
}
