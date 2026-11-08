#pragma once

//---------------------------------------------------------------------------
//
// blood2d shared mod base: game event hooks + a registry of temporary effects.
//
// Stock Blood code calls the On* functions and the helpers below through
// one-line hooks. Effects change behavior through the Modifiers (read by
// those hooks) or by changing actor data that they restore on stop - and also
// while a savegame is being written (suspend/resume), so saves never contain
// an effect. Modules (chaos, ...) register listeners, tickers and drawers.
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
		double gravityScale = 1;     // MoveThing / MoveDude (gravity and the buoyancy at water surfaces)
		double damageDealtScale = 1; // player -> anything else
		double damageTakenScale = 1; // anything -> player
		double drunk = 0;            // 0..1 camera sway
		bool hideHud = false;        // status bar
		double spreadDegrees = 0;    // extra random spread on every hitscan shot
		double splashScale = 1;      // explosion radius/damage
		double enemyTimeScale = 1;   // enemy AI, movement and animation speed
		bool bomberZombies = false;  // zombie melee attacks become explosions
	};
	extern Modifiers mods;

	// A game mode's lasting factor on enemy speed (the roguelite's Haste), on top of mods.enemyTimeScale.
	// Effects never touch it and stopping them doesn't reset it: the mode sets it and puts it back to 1.
	extern double modeEnemyTimeScale;
	double EnemyTimeScale(); // mods.enemyTimeScale * modeEnemyTimeScale

	int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage);
	void ModifyView(DVector3& pos, DRotator& angles, double interpfrac); // also drives shader effects
	void ModifyShot(DBloodActor* shooter, DVector3& dv);                // actFireVector, before normalizing
	double ScaleRange(DBloodActor* shooter, double range);               // actFireVector reach
	double MeleeRange(DBloodActor* actor, double dist);                  // AI melee distance checks
	void ScaleSplash(int& radius, int& baseDmg, int& distDmg);           // actRadiusDamage
	int ScaleBlastRadius(int radius);                                    // actCheckExplosion (TNT, bombs, barrels)
	int ScaleBlast(int damage);                                          // actCheckExplosion damage and concussion
	void BeginEnemyTic();                                                // actProcessSprites, first thing
	int EnemyPasses();                                                   // how many AI passes this tic
	void NextAiPass();
	int AiFrame();                                                       // frame counter the AI paces on
	int DudeMoveSteps(DBloodActor* actor);                               // MoveDude calls this tic
	int SeqTicks(DBloodActor* owner, int nTicks);                        // seqProcess per instance
	bool BomberAttack(DBloodActor* actor);                               // zombie hack callbacks: true = exploded instead
	// Bullet shuffle: true = the shot was replaced (the caller must not fire the original).
	bool RemapVector(DBloodActor* shooter, double offset, double zoffset, const DVector3& dv, int vectorType);
	bool RemapMissile(DBloodActor* shooter, double xyoff, double zoff, const DVector3& dv, int nType);

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
		void (*suspend)();           // optional; undo saved-data changes while a save is written
		void (*resume)();            // optional; redo them afterwards
		const char* group;           // optional; effects in the same group change the same state and never run together
	};

	int NumEffects();
	const Effect& GetEffect(int i);
	const Effect* FindEffect(const char* name);
	const char* EffectTitle(const Effect* e);
	bool Rivals(const Effect* a, const Effect* b); // different effects of the same group
	bool CanStart(const Effect* e);
	bool StartEffect(const Effect* e);           // ends any active rival first
	void StopEffect(const Effect* e);
	void StopAllEffects();
	bool IsActive(const Effect* e);
}

END_BLD_NS
