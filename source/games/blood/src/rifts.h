#pragma once

//---------------------------------------------------------------------------
//
// blood2d roguelite: rifts (rifts.cpp). A buried or laying axe zombie's wake
// opens a rift where it would have climbed out, and the foe the map's plan
// chose for that spot comes through. Outside a run, in the Crypt, or with
// rl_rifts 0: the stock rise. These are the hooks stock code and the
// roguelite call, and the run's side of the rifts (defined in roguelite.cpp).
//
//---------------------------------------------------------------------------

struct tspritetype;

BEGIN_BLD_NS

class DBloodActor;
struct AISTATE;

namespace rogue
{
	// Hooks called from stock code.
	bool RiftWake(DBloodActor* zombie);                                    // aiActivateDude, a buried/laying zombie waking: true = a rift opens (no rise)
	int RiftDamage(DBloodActor* target, DAMAGE_TYPE damageType, int damage); // actDamageSprite: a rift (and a zombie about to be one) keeps its last point
	bool RiftFireproof(DBloodActor* target);                               // actBurnSprite: true = it doesn't catch fire
	bool RiftSteady(DBloodActor* actor);                                   // RecoilDude: true = a rift, or an arrival in its first 0.5 s, doesn't flinch
	void RiftSprite(DBloodActor* owner, tspritetype* tspr);                // viewProcessSprites, a dude: an arrival fades in

	// Called by the roguelite (roguelite.cpp).
	void PlanRifts(uint32_t seed, int mapIndex, double depth);             // Remix: which zombies come through rifts, as what (its own random stream)
	void RiftLedger(DBloodActor* a, int& type, double& health);            // CountAmmo: a zombie with a plan counts as what comes through

	// For other code. A waiting rift is a zombie that woke and vanished, its foe not yet through: invisible, not solid,
	// not hittable by hitscan, still a kStatDude with health. Its arrival is the same actor, retyped in place, so its
	// xsprite and every DBloodActor::modFlags bit but the champion ones (set again if it arrives as a champion) carry over.
	bool IsRift(DBloodActor* a);                                           // a waiting rift (riftOpen, riftSpark or riftWait)
	extern AISTATE riftOpen, riftSpark, riftWait;                          // waiting: the first sparkle, the second, then until nothing holds it
	extern AISTATE riftHold, riftDone;                                     // arrived: 0.5 s with no attack, then it comes for you

	// The run's side, in roguelite.cpp: rifts.cpp reads the run only through these.
	struct RiftRules
	{
		double blueChance;     // a plain enemy's odds of being a (blue) champion at that depth, as the remix rolls them
		double flesh;          // the Covenant's Flesh on enemy health (1: none)
		double championHealth; // a champion's health factor
		double redHealth;      // a red champion's
		double championSize;   // a champion's size factor (it grows)
	};
	bool RiftRunLive();                                                    // a run in progress, outside the Crypt
	RiftRules RiftRunRules(double depth);
	void RiftMakeChampion(DBloodActor* a, bool red);                       // the remix's MakeChampion, grown: its flags, its health, its size
	int RiftThreatCost(int type);                                          // a kind's cost in the run's threat table (an axe zombie: 2)
}

END_BLD_NS
