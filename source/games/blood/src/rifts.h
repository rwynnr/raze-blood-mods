#pragma once

//---------------------------------------------------------------------------
//
// blood2d roguelite: rifts (rifts.cpp). A buried or laying axe zombie's wake
// opens a rift where it would have climbed out, and the foe the map's plan
// chose for that spot comes through; Horde's and Hourglass's wave enemies come
// through rifts too. Outside a run, in the Crypt, or with rl_rifts 0: the
// stock rise and stock waves. These are the hooks stock code and the
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
	bool RiftFireproof(DBloodActor* target);                               // actBurnSprite, fxFlameLick: true = it doesn't catch fire, nor show flames
	bool RiftSteady(DBloodActor* actor);                                   // RecoilDude, aiDamageSprite: true = a rift, or an arrival in its first 0.5 s: a hit
	                                                                       // takes health and nothing more (no flinch, new target, wake, dodge or chase)
	void RiftSprite(DBloodActor* owner, tspritetype* tspr);                // viewProcessSprites, a dude: an arrival fades in
	// IsRift (below) is a hook too: aiActivateDude and aiSetTarget (a waiting rift doesn't wake, takes no foe and is
	// no one's), the proximity triggers (it sets off nothing), Tchernobog's aim (it never bends to one).

	// Called by the roguelite (roguelite.cpp).
	void PlanRifts(uint32_t seed, int mapIndex, double depth);             // Remix: which zombies come through rifts, as what (its own random stream)
	void RiftLedger(DBloodActor* a, int& type, double& health);            // CountAmmo: a zombie with a plan counts as what comes through
	struct RiftWaves                                                       // Pressure: the waves spawned while one lives (Horde's, Hourglass's) come
	{                                                                      // through rifts (rl_rifts); any other wave (chaos's) stays stock
		RiftWaves();
		~RiftWaves();
		RiftWaves(const RiftWaves&) = delete;
		RiftWaves& operator=(const RiftWaves&) = delete;
	};
	void RiftWave(DBloodActor* spawned);                                   // WaveSpawned: a wave enemy, placed; within a RiftWaves, it comes through a rift

	// For other code. A waiting rift is a zombie that woke and vanished, its foe not yet through, or a Horde or Hourglass
	// wave enemy not yet through: invisible, not solid, not hittable by hitscan, never targeting anyone, still a kStatDude
	// with health. Its arrival is the same actor: a zombie's retyped in place, so its xsprite and every
	// DBloodActor::modFlags bit but the champion ones (set again if it arrives as a champion) carry over; a wave enemy's
	// keeps its kind, its health and every modFlags bit. While it waits, DBloodActor::riftPlan says what comes (rifts.cpp:
	// the type - kDudeBase in bits 0-7, a blue champion 0x100, the stagger in bits 9-12, a wave enemy 0x2000); on arrival
	// riftPlan is 0.
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
