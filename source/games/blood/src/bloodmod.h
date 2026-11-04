#pragma once

//---------------------------------------------------------------------------
//
// blood2d shared mod base: game event hooks + a registry of temporary effects.
//
// Stock Blood code calls the On* functions through one-line hooks. Effects
// change behavior only through the Modifiers below (read by other one-line
// hooks) or through flags they restore on stop, so a save made while an effect
// is running never contains the effect. Modules (chaos, ...) register
// listeners, tickers and overlay drawers.
//
//---------------------------------------------------------------------------

BEGIN_BLD_NS

class DBloodActor;
class DBloodPlayer;

namespace bmod
{
	// ---- events ----
	enum class Event { LevelStart, LevelEnd, SaveLoaded, PlayerDeath, Kill, Pickup };

	struct EventInfo
	{
		Event type;
		DBloodPlayer* player; // PlayerDeath, Pickup
		DBloodActor* actor;   // Kill: the victim
		DBloodActor* other;   // Kill: the killer
		int value;            // Pickup: item type
	};

	using Listener = void (*)(const EventInfo&);
	using TickFn = void (*)();
	using DrawFn = void (*)(DBloodPlayer*);
	void AddListener(Listener fn);
	void AddTicker(TickFn fn);   // once per game tic, only while the game runs (not paused / in menus)
	void AddDrawer(DrawFn fn);   // 2D overlay, drawn after the status bar

	// Hooks called from stock code.
	void OnLevelStart();
	void OnLevelEnd();
	void OnSaveLoaded();
	void OnFreeLevelData();      // level teardown (level change, load, new game, shutdown)
	void OnTick();
	void OnPlayerDeath(DBloodPlayer* pPlayer);
	void OnKill(DBloodActor* killer, DBloodActor* killed);
	void OnPickup(DBloodPlayer* pPlayer, int itemType);
	void OnDrawOverlay(DBloodPlayer* pPlayer);

	// ---- modifiers read by stock code; effects set them, stopping resets them ----
	struct Modifiers
	{
		double gravityScale = 1;     // MoveThing / MoveDude
		double enemyDrawScale = 1;   // non-player dude sprites (visual only)
		double damageDealtScale = 1; // player -> anything else
		double damageTakenScale = 1; // anything -> player
		double drunk = 0;            // 0..1 camera sway
		bool hideHud = false;        // status bar
	};
	extern Modifiers mods;

	int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage);
	void ModifyView(DRotator& angles, double interpfrac);

	// ---- effects ----
	enum class Category { Silly, Twist, Nasty };

	struct Effect
	{
		const char* name;            // console name, e.g. "lowgrav"
		const char* title;           // shown to the player, e.g. "Low gravity"
		Category category;
		bool oneShot;                // acts once in start(); no duration, no stop
		int fixedSeconds;            // > 0 overrides the scheduler's duration
		void (*start)();
		void (*stop)();              // must undo everything start() did (null for one-shots)
		void (*tick)(double secs);   // optional; seconds since start
		bool (*available)();         // optional; false = can't run right now
		const char* (*titleNow)();   // optional; title decided at pick time
	};

	int NumEffects();
	const Effect& GetEffect(int i);
	const Effect* FindEffect(const char* name);
	const char* EffectTitle(const Effect* e);
	bool CanStart(const Effect* e);
	bool StartEffect(const Effect* e);
	void StopEffect(const Effect* e);
	void StopAllEffects();
	bool IsActive(const Effect* e);
}

END_BLD_NS
