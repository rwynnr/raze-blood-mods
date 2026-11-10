#pragma once

//---------------------------------------------------------------------------
//
// blood2d roguelite mode (roguelite.cpp): runs on Blood's own maps.
// These are the hooks stock code calls; each one leaves stock behavior
// untouched unless a run is in progress (or, for the champion hooks, the
// actor was made a champion by a run).
//
//---------------------------------------------------------------------------

class FSerializer;
struct MapRecord;
struct tspritetype;
class tspriteArray;
struct walltype;

BEGIN_BLD_NS

class DBloodActor;
class DBloodPlayer;

namespace rogue
{
	// DBloodActor::modFlags: a champion (elite) enemy; a pickup holding its own amount of ammo (in xspr.data1); a red
	// champion (the rarer tier; also a champion); a missile from the run's forged gun, with bits 16-32 its offspring code
	// (0 the gun's own shot, 1 a Litany's offspring or the starburst's ring, and a starburst carrier once it has thrown
	// its ring, 2 offspring flying back to you, 3 the gun's own shot, split) and 64
	// for one bought with blood (the Blood Price, and the Fetch's cleaver flying back from one); 128 on a forged blast
	// that has thrown you (Brimstone Leap).
	// Two bits mean something else on another kind of actor, where the meanings can't meet:
	// - kModSpareYou (64): on a forged blast (the TNT thing ForgeBlast spawns, then the explosion it becomes), one that
	//   doesn't hurt you (the Larder of Knives'). A forged missile's 64 is cleared when it strikes (ForgeImpact), so a
	//   blood-bought fireball's explosion never carries it.
	// - kModPyreMark (128): on a kStatFlare, the Pyre's flare lodged in a burning foe (never kModForged, which a load
	//   would take for a Corpse-Candle). Forged missiles never carry 128, and kModKicked is only set on explosions.
	// - kModOmen (8): on a kStatItem, an Omen's pickup or a cursed reliquary (its Omen in xspr.data1). Every reader of
	//   kModForged looks at projectiles, flares, decorations, things or explosions only, and no item is ever forged.
	// - kModWave (16): on a dude, a wave enemy (SpawnWave's; set in WaveSpawned, kept through RetypeDude, burning and
	//   death): it counts half toward the momentum. Every reader of kModOffspring (16, 32) checks kModForged first, which
	//   no dude ever carries.
	enum { kModChampion = 1, kModBigPickup = 2, kModRedChampion = 4, kModForged = 8, kModOffspring = 48, kModBloodBought = 64, kModKicked = 128 };
	enum { kModSpareYou = kModBloodBought, kModPyreMark = kModKicked, kModOmen = kModForged, kModWave = 16 };

	MapRecord* NextLevel(MapRecord* stockNext);                            // levelEndLevel: a run picks its own next map (null: the run is won)
	bool OnDeathRestart();                                                 // playerProcess, Use while dead in single player: true = handled
	void OnNewGame();                                                      // GameInterface::NewGame
	int ScaleDamage(DBloodActor* source, DBloodActor* target, DAMAGE_TYPE damageType, int damage); // actDamageSprite: champions, the Covenant's Wrath, the Forge
	int ScaleHealing(DBloodActor* actor, int add);                         // actHealDude: the Covenant's Famine heals less (0: no heal)
	bool OnUse(DBloodPlayer* pPlayer);                                     // playerProcess, Use: true = taken by the Crypt (a menu opened)
	bool OnUseNothing(DBloodPlayer* pPlayer);                              // playerProcess, Use with nothing to push (ActionScan -1): true = a cursed reliquary taken
	bool Unflinching(DBloodActor* actor);                                  // RecoilDude: red champions don't flinch, but for a heavy hit of yours
	// The Forge (roguelite.cpp): the run's forged gun fires its runes' payloads in place of its own primary fire, and its
	// alt fire is a forged mode of its own (rl_forge_alt).
	void WeaponCallback(int nCallback, int id, void* data);                // qavPlay: a weapon frame's callback (the stock one if no Forge)
	bool SwallowMissile(DBloodActor* shooter, double xyoff, double zoff, const DVector3& dv, int type); // actFireMissile
	bool SwallowVector(DBloodActor* shooter, double offset, double zoffset, const DVector3& dv, int type); // actFireVector
	bool ForgeLoaded(DBloodPlayer* pPlayer, int ammotype, int count);      // CheckAmmo: true = Blood Price lets it fire dry
	bool ForgeLoadedWeapon(DBloodPlayer* pPlayer, int weapon);             // CheckWeaponAmmo, next/prev: and lets you pick it dry
	struct ForgeImpact                                                     // actImpactMissile: a forged missile's Wound
	{
		ForgeImpact(DBloodActor* missile, DBloodActor* hit, int hitCode, walltype* wall);
		~ForgeImpact();
		bool active = false;
		DBloodActor* missile = nullptr;
		int type = 0;                                                      // the missile's type (the stock code changes it)
		int hitCode = -1;                                                  // what it struck: 0/4 a wall, 1 ceiling, 2 floor, 3 a sprite
		walltype* wall = nullptr;                                          // the wall it struck (codes 0 and 4)
		double flight = 0;                                                 // its heading in degrees, before the stock code stops it
		bool liveHit = false, sky = false, larder = false, fetch = false;
		bool bloodBought = false;                                          // paid for in blood (its mark is cleared before the Fetch reads it)
		bool biting = false;                                               // a cleaver: its stock heal is the Forge's (ScaleHealing)
		int healthBefore = 0;                                              // your health before it struck (sixteenths)
		DBloodActor* foe = nullptr;                                        // the live foe it struck (liveHit)
		bool flareStrike = false;                                          // the bare forged flare: its impact damage comes after the stock burn
	};
	double MissileSpeedScale(DBloodActor* missile);                        // actMoveMissile, homing: a forged cleaver flies faster (1: stock)
	bool ForgeBurst(DBloodActor* carrier);                                 // FlareBurst: true = a forged starburst threw its own ring
	struct ForgeExplosion                                                  // actCheckExplosion: a forged blast (Brimstone Leap, the Larder, half on you)
	{
		ForgeExplosion(DBloodActor* explosion);
		~ForgeExplosion();
	};
	int TakePickup(DBloodActor* item, int room);                           // PickupAmmo, PickupWeapon: the ammo a run's pickup gives (-1: stock)
	bool PickupTaken(DBloodActor* item, const char*& msg);                 // PickUp: names the amount; true = the rest stays on the map
	bool ClaimPickup(DBloodPlayer* pPlayer, DBloodActor* item);            // PickUp, first: true = the run's (an Omen taken, a reliquary left for Use)
	struct OmenWound                                                       // actDamageSprite: Bloodlust (an Omen) heals you from your hits on foes
	{
		OmenWound(DBloodActor* source, DBloodActor* target);
		~OmenWound();
		DBloodActor* foe = nullptr;                                        // the live foe your hit struck (null: none, or no Bloodlust)
		int healthBefore = 0;                                              // its health as the hit began (sixteenths)
	};
	void TintSprite(DBloodActor* owner, tspritetype* tspr);               // viewProcessSprites: champions are tinted
	void OmenItemSprite(tspriteArray& tsprites, tspritetype* tspr);        // viewProcessSprites, an item: an Omen keeps its tier's palette, a cursed reliquary's halo
	void Serialize(FSerializer& arc);                                      // SerializeGameState: the run's save block
	void OnMenuOpened();                                                   // GameInterface::MenuOpened: lists the runs in the Roguelite menu
}

END_BLD_NS
