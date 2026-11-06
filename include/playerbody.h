#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "actors.h"
#include "viewmodel.h"

struct World;

// What the player's arms do, taken from the first-person action (in OpenMW one character controller drives both
// views): a blow drawn back, let go, or a spell cast
enum BodyAction { BODY_NONE, BODY_WINDUP, BODY_STRIKE, BODY_CAST, BODY_DEATH };   // death: its group played once, held

struct BodyAct
{
	BodyAction action = BODY_NONE;
    std::string group;   // the NPC group: Attack1h ... AttackHH, CastSelf / CastTouch / CastTarget
	float charge = 0.0f;                          // wind-up: 0..1 between the min and max attack marks
	float speed = 1.0f;                           // the weapon's speed: blows play at this rate
	float strength = 1.0f;                        // a blow let go: small / medium / large follow-through by it
	std::string stance;                      // the weapon's family for its ready stance: 1h 2c 2w HH
	float moveX = 0.0f, moveY = 0.0f;             // the stick against the facing (+y forward): which way they walk
	bool airborne = false;                        // off the ground (jumping, falling): the jump
	bool flying = false;                          // levitating: walks, never runs or sneaks (OpenMW)
};

// The player seen from outside (third-person view, character creation's face preview): the NPC skeleton
// of their race and sex, dressed from the "third" pieces (tools/convert/firstperson.py third_person): the bare
// body, the chosen head and hair, then clothes, armor, skirts and robes as an NPC's are put together
// (tools/convert/npc.py NpcBuilder.parts), the weapon while it is out.
struct PlayerBody
{
	bool ready = false;
	int skel = -1;                                // index into the shared NPC skeletons
	ActorSet set;                                 // actors[0]: the player
	std::vector<C3D_Tex*> textures;               // indexed by the meshes' tex
	std::unordered_map<std::string, FpPiece*> pieces;   // bones resolved against this skeleton
	std::string builtFor, forRace;
	bool forFemale = false;
	std::string group;                            // the walk / idle group asked for last ("" after a blow or cast)
	BodyAction acting = BODY_NONE;                // the arms' action being played
	std::string actGroup;
	bool followCut = false;                       // a blow's follow-through was chosen at its hit
	bool stanceOut = false;                       // the walk / idle was chosen with the weapon out
	bool swimOut = false;                         // ... and in the water (actorPlay's swimming groups)

	void rebuild(World& w, bool weaponOut);      // (re)dress when race, head, hair or equipment changed
	// Animation from what the player does; place at feet, facing yaw
	void update(World& w, float dt, float speed, bool swimming, bool sneaking, bool weaponOut, const BodyAct& act);
	const char* playing() const;                  // the group being played ("" when none)
	void draw(World& w, bool deform);
	void free(World& w);
};

// The heads / hairs a race and sex can choose from (body ids), for character creation
const std::vector<std::pair<std::string, std::string>>* playerHeadChoices(const World& w, bool hair);

// ---------------------------------------------------------------------------
// Remote player bodies (multiplayer): a PlayerBody built from a descriptor
// that is not the local player. Rebuild picks race, sex, head, hair and
// worn items from `desc` (never from the local World::stats / inventory).
// ---------------------------------------------------------------------------
struct RemotePlayerDesc
{
    std::string race = "imperial";
    bool female = false;
    std::string head, hair;
    std::vector<std::string> worn;   // lowercase item ids, all treated as equipped
};

void playerBodyRebuildRemote(PlayerBody& body, World& w, const RemotePlayerDesc& desc, bool weaponOut);
void playerBodyUpdateRemote(PlayerBody& body, World& w, float dt, const RemotePlayerDesc& desc,
                            const BodyAct& act, float moveX, float moveY, float moveSpeed, bool weaponOut);
