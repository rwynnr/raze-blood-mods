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

BEGIN_BLD_NS

class DBloodActor;
class DBloodPlayer;

namespace rogue
{
	// DBloodActor::modFlags: a champion (elite) enemy; a pickup holding its own amount of ammo (in xspr.data1)
	enum { kModChampion = 1, kModBigPickup = 2 };

	MapRecord* NextLevel(MapRecord* stockNext);                            // levelEndLevel: a run picks its own next map (null: the run is won)
	bool OnDeathRestart();                                                 // playerProcess, Use while dead in single player: true = handled
	void OnNewGame();                                                      // GameInterface::NewGame
	int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage); // actDamageSprite: champions and the Covenant's Wrath hit harder
	int ScaleHealing(DBloodActor* actor, int add);                         // actHealDude: the Covenant's Famine heals less
	bool OnUse(DBloodPlayer* pPlayer);                                     // playerProcess, Use: true = taken by the Crypt (a menu opened)
	int TakePickup(DBloodActor* item, int room);                           // PickupAmmo, PickupWeapon: the ammo a run's pickup gives (-1: stock)
	bool PickupTaken(DBloodActor* item, const char*& msg);                 // PickUp: names the amount; true = the rest stays on the map
	void TintSprite(DBloodActor* owner, tspritetype* tspr);               // viewProcessSprites: champions are tinted
	void Serialize(FSerializer& arc);                                      // SerializeGameState: the run's save block
	void OnMenuOpened();                                                   // GameInterface::MenuOpened: lists the runs in the Roguelite menu
}

END_BLD_NS
