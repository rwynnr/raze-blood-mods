//---------------------------------------------------------------------------
//
// blood2d shared mod base: event hooks, modifiers and the effect library.
// See bloodmod.h. Included at the end of all.cpp (unity build), so every
// Blood declaration is visible here.
//
//---------------------------------------------------------------------------

#include "ns.h"
#include "build.h"
#include "blood.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "i_time.h"
#include "hwrenderer/postprocessing/hw_postprocessshader.h"

CVARD(Bool, mod_eventlog, false, 0, "print each mod-base game event to the console")
CVARD(Float, mod_drunk_strength, 1.f, CVAR_ARCHIVE, "drunk camera effect strength multiplier")
// Read by the full status bar (sbar.zs DrawAmmoAmount), so the HUD keeps up when an effect takes weapons away.
CVARD(Bool, mod_hudweapons, true, CVAR_ARCHIVE, "status bar: grey out the ammo of weapons you don't have (0 = stock look)")

// Core-side hooks (gameinput.cpp, savegamehelp.cpp).
extern bool gModInvertControls;
extern void (*gModSaveHook)(bool before);
extern bool gModTexWarpOn;      // hw_walls.cpp: acid texture tessellation
extern float gModTexWarp[4];

BEGIN_BLD_NS

namespace bmod
{

Modifiers mods;

static TArray<Listener>& listeners() { static TArray<Listener> a; return a; }
static TArray<TickFn>& tickers() { static TArray<TickFn> a; return a; }
static TArray<DrawFn>& drawers() { static TArray<DrawFn> a; return a; }

void AddListener(Listener fn) { listeners().Push(fn); }
void AddTicker(TickFn fn) { tickers().Push(fn); }
void AddDrawer(DrawFn fn) { drawers().Push(fn); }

static double Rand01() { return rand() / double(RAND_MAX); }   // not the game RNG: never shifts game randomness
static double RandSym() { return Rand01() * 2. - 1.; }

//---------------------------------------------------------------------------
//
// events
//
//---------------------------------------------------------------------------

static const char* EventName(Event e)
{
	switch (e)
	{
	case Event::LevelStart: return "level start";
	case Event::LevelEnd: return "level end";
	case Event::SaveLoaded: return "save loaded";
	case Event::PlayerDeath: return "player death";
	case Event::Kill: return "kill";
	case Event::Pickup: return "pickup";
	}
	return "?";
}

static void Send(const EventInfo& ev)
{
	if (mod_eventlog)
	{
		switch (ev.type)
		{
		case Event::Kill:
			Printf("mod event: kill (victim type %d, killer type %d)\n", ev.actor ? ev.actor->GetType() : -1, ev.other ? ev.other->GetType() : -1);
			break;
		case Event::Pickup:
			Printf("mod event: pickup (item type %d)\n", ev.value);
			break;
		default:
			Printf("mod event: %s\n", EventName(ev.type));
			break;
		}
	}
	for (auto fn : listeners()) fn(ev);
}

void OnLevelStart() { Send({ Event::LevelStart, nullptr, nullptr, nullptr, 0 }); }
void OnLevelEnd() { Send({ Event::LevelEnd, nullptr, nullptr, nullptr, 0 }); }
void OnSaveLoaded() { Send({ Event::SaveLoaded, nullptr, nullptr, nullptr, 0 }); }
void OnPlayerDeath(DBloodPlayer* pPlayer) { Send({ Event::PlayerDeath, pPlayer, nullptr, nullptr, 0 }); }
void OnKill(DBloodActor* killer, DBloodActor* killed) { Send({ Event::Kill, nullptr, killed, killer, 0 }); }
void OnPickup(DBloodPlayer* pPlayer, int itemType) { Send({ Event::Pickup, pPlayer, nullptr, nullptr, itemType }); }

void OnFreeLevelData()
{
	// Safety net: nothing an effect changed may outlive the level (this also runs at shutdown,
	// before the config is written, so effects that flip a saved CVAR are undone in time).
	StopAllEffects();
}

//---------------------------------------------------------------------------
//
// actor bookkeeping: raw actor pointers are only touched after checking they're
// still in the sprite lists
//
//---------------------------------------------------------------------------

static void LiveActors(TMap<DBloodActor*, bool>& live)
{
	live.Clear();
	BloodSpriteIterator it;
	while (auto a = it.Next()) live.Insert(a, true);
}

//---------------------------------------------------------------------------
//
// real enemy size (giant / tiny): sprite scale = hitbox and height, xspr.scale so
// SEQ frames keep it, clipdist (tiny only, so giants can't wedge in doorways),
// plus melee reach / decision distances via MeleeRange and ScaleRange
//
//---------------------------------------------------------------------------

struct SizedActor { DBloodActor* actor; DVector2 scale; double clipdist; int xscale; };
static TArray<SizedActor> sized;
static TMap<DBloodActor*, double> sizeOf;
static double sizeFactor = 1;
static bool sizeClip;

static void SizeApplyAll(bool scaled)
{
	TMap<DBloodActor*, bool> live;
	LiveActors(live);
	for (auto& s : sized)
	{
		if (!live.CheckKey(s.actor)) continue;
		if (!scaled)
		{
			s.actor->spr.scale = s.scale;
			s.actor->clipdist = s.clipdist;
			s.actor->xspr.scale = s.xscale;
		}
		else
		{
			s.actor->spr.scale = s.scale * sizeFactor;
			if (sizeClip) s.actor->clipdist = s.clipdist * sizeFactor;
			s.actor->xspr.scale = std::max(int((s.xscale ? s.xscale : 256) * sizeFactor), 1);
		}
	}
}

static void SizeStart(double f, bool clip)
{
	sized.Clear();
	sizeOf.Clear();
	sizeFactor = f;
	sizeClip = clip;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (a->IsPlayerActor() || !a->hasX()) continue;
		sized.Push({ a, a->spr.scale, a->clipdist, a->xspr.scale });
		sizeOf.Insert(a, f);
	}
	SizeApplyAll(true);
}

static void SizeStop()
{
	SizeApplyAll(false);
	sized.Clear();
	sizeOf.Clear();
	sizeFactor = 1;
}

static double SizeOf(DBloodActor* actor)
{
	if (!actor || sizeOf.CountUsed() == 0) return 1;
	auto f = sizeOf.CheckKey(actor);
	return f ? *f : 1;
}

double MeleeRange(DBloodActor* actor, double dist) { return dist * SizeOf(actor); }

double ScaleRange(DBloodActor* shooter, double range)
{
	return range > 0 ? range * SizeOf(shooter) : range; // 0 = unlimited
}

//---------------------------------------------------------------------------
//
// other modifier helpers
//
//---------------------------------------------------------------------------

int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage)
{
	if (!target) return damage;
	// Damage from a thing, missile or explosion counts as its owner's: the blast of your TNT is your damage
	// (actCheckExplosion names the explosion sprite as the source of its concussion).
	if (source && !source->IsDudeActor())
	{
		auto owner = source->GetOwner();
		if (owner && owner->IsPlayerActor()) source = owner;
	}
	if (mods.damageDealtScale != 1 && source && source != target && source->IsPlayerActor() && !target->IsPlayerActor())
		damage = int(damage * mods.damageDealtScale);
	if (mods.damageTakenScale != 1 && target->IsPlayerActor())
		damage = int(damage * mods.damageTakenScale);
	return damage;
}

void ModifyShot(DBloodActor* shooter, DVector3& dv)
{
	if (mods.spreadDegrees <= 0) return;
	double len = dv.XY().Length();
	if (len <= 0) return;
	DVector2 xy = dv.XY().Rotated(DAngle::fromDeg(RandSym() * mods.spreadDegrees));
	dv.X = xy.X;
	dv.Y = xy.Y;
	dv.Z += tan(DAngle::fromDeg(RandSym() * mods.spreadDegrees * 0.6).Radians()) * len;
}

void ScaleSplash(int& radius, int& baseDmg, int& distDmg)
{
	if (mods.splashScale == 1) return;
	double s = mods.splashScale;
	radius = int(radius * (1. + (s - 1.) * 0.5));
	baseDmg = int(baseDmg * s);
	distDmg = int(distDmg * s);
}

// Explosion sprites (TNT, bombs, barrels, napalm) do their damage in actCheckExplosion, not actRadiusDamage, so
// they get the same scaling here: the radius grows by half the factor, the damage by the whole factor.
int ScaleBlastRadius(int radius)
{
	if (mods.splashScale == 1) return radius;
	return int(radius * (1. + (mods.splashScale - 1.) * 0.5));
}

int ScaleBlast(int damage)
{
	if (mods.splashScale == 1) return damage;
	return int(damage * mods.splashScale);
}

// Enemy time scale: run enemy AI / movement N times per tic (accumulated), and scale
// their animation ticks. 1.0 leaves stock behavior untouched.
static double enemyAcc;
static int enemyPassesThisTic = 1;
static int aiFrame;

void BeginEnemyTic()
{
	if (mods.enemyTimeScale == 1) { enemyAcc = 0; enemyPassesThisTic = 1; return; }
	enemyAcc += mods.enemyTimeScale;
	enemyPassesThisTic = int(enemyAcc);
	enemyAcc -= enemyPassesThisTic;
}

int EnemyPasses() { return enemyPassesThisTic; }
void NextAiPass() { aiFrame++; }
int AiFrame() { return mods.enemyTimeScale == 1 ? gFrameCount : aiFrame; }

int DudeMoveSteps(DBloodActor* actor)
{
	return (actor && actor->IsPlayerActor()) ? 1 : enemyPassesThisTic;
}

int SeqTicks(DBloodActor* owner, int nTicks)
{
	if (mods.enemyTimeScale == 1 || !owner || owner->spr.statnum != kStatDude || owner->IsPlayerActor()) return nTicks;
	return int(nTicks * mods.enemyTimeScale + 0.5);
}

//---------------------------------------------------------------------------
//
// post-process shader effects (GLDEFS in raze.pk3: filter/blood/gldefs.txt)
//
//---------------------------------------------------------------------------

static double acidAmount, acidTarget;
static bool flipX, flipY;
static uint64_t lastFrameMs;

static PostProcessShader* FindPPShader(const char* name)
{
	for (auto& s : PostProcessShaders) if (!s.Name.CompareNoCase(name)) return &s;
	return nullptr;
}

static void SetUniform(PostProcessShader* s, const char* name, double v)
{
	if (auto u = s->Uniforms.CheckKey(name)) u->Values[0] = v;
}

static void UpdateShaders()
{
	uint64_t now = I_msTime();
	double dt = lastFrameMs ? std::min((now - lastFrameMs) / 1000., 0.1) : 0.;
	lastFrameMs = now;
	// 2 s fades in and out.
	if (acidAmount < acidTarget) acidAmount = std::min(acidTarget, acidAmount + dt * 0.5);
	else if (acidAmount > acidTarget) acidAmount = std::max(acidTarget, acidAmount - dt * 0.5);
	double secs = (now % 3600000) / 1000.;
	if (auto s = FindPPShader("bmod_acid"))
	{
		s->Enabled = acidAmount > 0.001;
		SetUniform(s, "amount", acidAmount);
		SetUniform(s, "timer", secs);
	}
	// World textures tessellate: tiles shrink to 2.5x repeats and bloom back out (~8 s cycle),
	// with a slow twist and crawl. Smooth, nothing flashes.
	gModTexWarpOn = acidAmount > 0.001;
	if (gModTexWarpOn)
	{
		double a = acidAmount;
		gModTexWarp[0] = float(1. + a * (0.75 + 0.75 * sin(secs * 0.8)));
		gModTexWarp[1] = float(a * 6. * sin(secs * 0.37));
		gModTexWarp[2] = float(a * 0.6 * sin(secs * 0.21)); // bounded drift: no racing during fades
		gModTexWarp[3] = float(a * 0.6 * cos(secs * 0.17));
	}
	if (auto s = FindPPShader("bmod_flip"))
	{
		s->Enabled = flipX || flipY;
		SetUniform(s, "flipx", flipX ? 1. : 0.);
		SetUniform(s, "flipy", flipY ? 1. : 0.);
	}
}

static void ResetShaders()
{
	acidAmount = acidTarget = 0;
	flipX = flipY = false;
	gModTexWarpOn = false;
	for (auto name : { "bmod_acid", "bmod_flip" })
		if (auto s = FindPPShader(name)) s->Enabled = false;
}

void ModifyView(DVector3& pos, DRotator& angles, double interpfrac)
{
	UpdateShaders();
	double k = mods.drunk * mod_drunk_strength;
	if (k <= 0) return;
	// Slow, smooth sway (every component well under 1 Hz), plus a sideways stagger.
	double t = (PlayClock + interpfrac * kTicsPerFrame) / 120.;
	angles.Roll += DAngle::fromDeg((sin(t * 1.7) * 16. + sin(t * 0.63) * 8.) * k);
	angles.Yaw += DAngle::fromDeg((sin(t * 0.71) * 9. + sin(t * 0.29) * 4.) * k);
	angles.Pitch += DAngle::fromDeg(sin(t * 1.13) * 7. * k);
	DVector2 side = angles.Yaw.ToVector().Rotated90CW();
	pos += DVector3(side * (sin(t * 0.9) * 4. * k), 0);
}

//---------------------------------------------------------------------------
//
// effect library
//
//---------------------------------------------------------------------------

static bool savedInfiniteAmmo;
static bool savedIso;

// The isometric camera lives on another branch; find its CVAR at runtime so this
// code doesn't depend on it.
static FBaseCVar* IsoCVar() { return FindCVar("r_isometric", nullptr); }
static bool IsoOn() { auto cv = IsoCVar(); return cv && cv->GetGenericRep(CVAR_Bool).Bool; }
static void SetIso(bool on)
{
	auto cv = IsoCVar();
	if (!cv) return;
	UCVarValue v;
	v.Bool = on;
	cv->SetGenericRep(v, CVAR_Bool);
}

static DBloodPlayer* LocalPlayer() { return getPlayer(myconnectindex); }

// Weapons the local player could switch to right now (owned, loaded, allowed underwater), other than the one in hand.
static int SwapCandidates(int* out)
{
	auto pPlayer = LocalPlayer();
	if (!pPlayer || !pPlayer->GetActor() || pPlayer->GetActor()->xspr.health <= 0) return 0;
	if (checkLitSprayOrTNT(pPlayer)) return 0; // never yank a lit explosive out of your hand
	int n = 0;
	for (int w = kWeapPitchFork; w < kWeapBeast; w++)
	{
		if (w == pPlayer->curWeapon || !pPlayer->hasWeapon[w]) continue;
		if (pPlayer->isUnderwater && BannedUnderwater(w)) continue;
		int ammo = weaponModes[w].ammoType;
		if (ammo >= 0 && pPlayer->ammoCount[ammo] <= 0) continue;
		if (out) out[n] = w;
		n++;
	}
	return n;
}

//---------------------------------------------------------------------------
//
// weapon roulette (one-shot, effect name "weaponswap"): every gun is a coin
// flip, whatever you had before. Ammo is left alone: a gun you lose keeps its
// ammo for when you find it again, a gun you win may have none yet. With few
// guns (early) that's mostly a gain, with a full arsenal (late) mostly a loss.
// Then you're handed a random weapon from the new set that has ammo. Like the
// shuffles, the result stays.
//
// Only the guns spin. Dynamite, the spray can, the bombs and the voodoo doll
// come with their own ammo pickups (picking up TNT is what gives you dynamite),
// and Blood picks slot 6 by ammo and then refuses a weapon you don't own: when
// the roulette took dynamite away, the sticks you still had couldn't be
// selected until you picked up more TNT. They stay as they are, like the ammo.
//
//---------------------------------------------------------------------------

static const int rouletteWeapons[] = { kWeapFlareGun, kWeapShotgun, kWeapTommyGun, kWeapNapalm, kWeapTeslaCannon, kWeapLifeLeech };

static const char* const rouletteNames[kWeapMax] = { "", "Pitchfork", "Flare gun", "Shotgun", "Tommy gun",
	"Napalm launcher", "Dynamite", "Spray can", "Tesla cannon", "Life leech", "Voodoo doll", "Proximity bombs",
	"Remote bombs", "" };

static bool RouletteAvailable()
{
	auto pPlayer = LocalPlayer();
	if (!pPlayer || !pPlayer->GetActor() || pPlayer->GetActor()->xspr.health <= 0) return false;
	// never yank a lit explosive out of your hand; leave the beast's claws alone
	return !checkLitSprayOrTNT(pPlayer) && pPlayer->curWeapon != kWeapBeast;
}

static void RouletteSpin()
{
	if (!RouletteAvailable()) return;
	auto pPlayer = LocalPlayer();
	bool had[kWeapMax];
	for (int w = 0; w < kWeapMax; w++) had[w] = pPlayer->hasWeapon[w];

	bool changed = false; // spin again in the 1-in-64 case where nothing changed
	for (int tries = 0; tries < 8 && !changed; tries++)
	{
		for (int w : rouletteWeapons)
		{
			pPlayer->hasWeapon[w] = (rand() & 1) != 0;
			if (pPlayer->hasWeapon[w] != had[w]) changed = true;
		}
	}
	if (!pPlayer->hasWeapon[pPlayer->nextWeapon]) pPlayer->nextWeapon = kWeapNone; // a switch queued by a pickup

	// A random gun from the new set that has ammo; the pitchfork only when nothing else is loaded and the
	// weapon in your hand is gone.
	int list[kWeapMax];
	int n = SwapCandidates(list), guns = 0;
	for (int i = 0; i < n; i++) if (list[i] != kWeapPitchFork) list[guns++] = list[i];
	if (guns > 0) pPlayer->newWeapon = list[rand() % guns];
	else if (pPlayer->curWeapon == kWeapNone || !pPlayer->hasWeapon[pPlayer->curWeapon]) pPlayer->newWeapon = kWeapPitchFork;

	FString won, lost;
	for (int w : rouletteWeapons)
	{
		if (pPlayer->hasWeapon[w] == had[w]) continue;
		FString& s = pPlayer->hasWeapon[w] ? won : lost;
		int ammo = weaponModes[w].ammoType;
		bool empty = pPlayer->hasWeapon[w] && ammo >= 0 && pPlayer->ammoCount[ammo] <= 0;
		s.AppendFormat("%s%s%s", s.IsEmpty() ? "" : ", ", rouletteNames[w], empty ? " (no ammo)" : "");
	}
	FString msg = "Weapon roulette:";
	if (won.IsNotEmpty()) msg.AppendFormat(" won %s", won.GetChars());
	if (lost.IsNotEmpty()) msg.AppendFormat("%s lost %s", won.IsNotEmpty() ? ";" : "", lost.GetChars());
	Printf(PRINT_NOTIFY, "%s\n", msg.GetChars());
}

static bool HasEnemies()
{
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next()) if (!a->IsPlayerActor() && a->hasX()) return true;
	return false;
}

//---------------------------------------------------------------------------
//
// enemy / cultist shuffle: change dude types in place. Each actor keeps its
// XSPRITE (trigger IDs, key, drop item, state), so level logic still works.
// Only regular, counted, mobile types take part (no bosses, statues, buried
// zombies, hands, eels, pods, bats, rats, innocents, prone or burning cultists),
// so kill totals stay right and nothing scripted gets lost.
//
//---------------------------------------------------------------------------

static bool IsCultistType(int t)
{
	return t == kDudeCultistTommy || t == kDudeCultistShotgun || t == kDudeCultistTesla || t == kDudeCultistTNT;
}

static bool IsFlyerType(int t)
{
	return t == kDudeGargoyleFlesh || t == kDudeGargoyleStone || t == kDudePhantasm;
}

static bool ShuffleEligible(DBloodActor* a, bool cultistsOnly)
{
	if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || (a->spr.flags & 32)) return false;
	if (a->xspr.aiState && a->xspr.aiState->stateType == kAiStateOther) return false; // mid-transformation
	int t = a->spr.type;
	if (IsCultistType(t)) return true;
	if (cultistsOnly) return false;
	switch (t)
	{
	case kDudeZombieAxeNormal: case kDudeZombieButcher: case kDudeGargoyleFlesh: case kDudeGargoyleStone:
	case kDudePhantasm: case kDudeHellHound: case kDudeSpiderBrown: case kDudeSpiderRed: case kDudeSpiderBlack:
	case kDudeGillBeast: case kDudeTinyCaleb:
		return true;
	default:
		return false;
	}
}

static void RetypeDude(DBloodActor* a, int newType, int newPal, DVector2 newScale)
{
	if (a->spr.type == newType) return;
	auto target = a->GetTarget();
	int oldMax = getDudeInfo(a->spr.type)->startHealth << 4;
	int newMax = getDudeInfo(newType)->startHealth << 4;
	int health = oldMax > 0 ? int(int64_t(a->xspr.health) * newMax / oldMax) : newMax; // keep the wounded fraction

	a->spr.type = newType;
	a->spr.inittype = newType;
	a->spr.pal = newPal;
	a->spr.scale = newScale;
	a->spr.cstat &= ~CSTAT_SPRITE_YFLIP;  // ceiling spiders land on their feet
	a->clipdist = getDudeInfo(newType)->fClipdist();
	a->xspr.health = std::clamp(health, 16, newMax);
	if (IsFlyerType(newType)) a->spr.flags &= ~(kPhysGravity | kPhysFalling); // aiInitSprite only ORs flyer flags in
	seqSpawn(getDudeInfo(newType)->seqStartID, a, -1);
	aiInitSprite(a);
	if (target)
	{
		aiSetTarget(a, target);
		aiActivateDude(a);
	}
}

static int CountShuffleable(bool cultistsOnly, int* distinctTypes)
{
	int n = 0, firstType = -1;
	bool mixed = false;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (!ShuffleEligible(a, cultistsOnly)) continue;
		if (firstType < 0) firstType = a->spr.type;
		else if (a->spr.type != firstType) mixed = true;
		n++;
	}
	if (distinctTypes) *distinctTypes = mixed ? 2 : (n ? 1 : 0);
	return n;
}

static void EnemyShuffle()
{
	// Permute (type, palette, scale) among the eligible enemies: same mix of monsters, new places.
	struct Look { int type, pal; DVector2 scale; };
	TArray<DBloodActor*> actors;
	TArray<Look> looks;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (!ShuffleEligible(a, false)) continue;
		actors.Push(a);
		looks.Push({ a->spr.type, a->spr.pal, a->spr.scale });
	}
	for (int i = int(looks.Size()) - 1; i > 0; i--) std::swap(looks[i], looks[rand() % (i + 1)]);
	for (unsigned i = 0; i < actors.Size(); i++) RetypeDude(actors[i], looks[i].type, looks[i].pal, looks[i].scale);
}

static void CultistShuffle()
{
	// Every cultist re-rolls to a different variant (and so a different weapon).
	static const int types[] = { kDudeCultistTommy, kDudeCultistShotgun, kDudeCultistTesla, kDudeCultistTNT };
	int palOf[4] = { -1, -1, -1, -1 };
	TArray<DBloodActor*> list;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (!ShuffleEligible(a, true)) continue;
		list.Push(a);
		for (int k = 0; k < 4; k++) if (a->spr.type == types[k] && palOf[k] < 0) palOf[k] = a->spr.pal;
	}
	for (auto a : list)
	{
		int k = rand() % 4;
		if (types[k] == a->spr.type) k = (k + 1 + rand() % 3) % 4;
		RetypeDude(a, types[k], palOf[k] >= 0 ? palOf[k] : a->spr.pal, a->spr.scale);
	}
}

//---------------------------------------------------------------------------
//
// enemy wave (one-shot): a few enemies teleport in behind the player, copied
// from the kinds still alive on the map (type, palette and size), so they look
// like the level's own. A spot must be in plain sight of the player (nothing
// spawns in a sealed room or behind a wall), on dry floor with headroom and
// some elbow room, not far above or below the player. They count toward the
// level's kill total and come straight for you. Only while eligible enemies
// are alive (the shuffles' rules: no bosses, statues, hands, eels...).
//
//---------------------------------------------------------------------------

struct WaveSpot { DVector3 pos; sectortype* sect; };

static bool WaveKinds(TArray<DBloodActor*>& out)
{
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next()) if (ShuffleEligible(a, false)) out.Push(a);
	return out.Size() > 0;
}

static bool FindWaveSpot(DBloodActor* pl, DBloodActor* kind, const TArray<WaveSpot>& taken, WaveSpot& out)
{
	auto plSect = pl->sector();
	if (!plSect) return false;
	double top, bottom;
	GetActorExtents(kind, &top, &bottom);
	double height = std::max(bottom - top, 8.);
	double room = std::max(kind->clipdist, 4.) * 1.5;
	DVector3 from(pl->spr.pos.XY(), pl->spr.pos.Z - height * 0.5);
	static const double dists[] = { 160, 128, 104, 80, 64 };
	for (double d : dists)
	{
		for (int k = 0; k < 8; k++)
		{
			// behind the player: within 75 degrees of straight back
			DAngle ang = pl->spr.Angles.Yaw + DAngle180 + DAngle::fromDeg(double(rand() % 151 - 75));
			DVector2 p = pl->spr.pos.XY() + ang.ToVector() * d;
			auto sect = plSect;
			updatesector(p, &sect);
			if (!sect || IsUnderwaterSector(sect)) continue;
			double cz, fz;
			calcSlope(sect, p.X, p.Y, &cz, &fz);
			if (fz - cz < height + 4 || std::abs(fz - pl->spr.pos.Z) > 64) continue;
			DVector3 mid(p, fz - height * 0.5);
			if (!cansee(from, plSect, mid, sect)) continue;
			bool ok = true;
			for (int q = 0; q < 4 && ok; q++)
			{
				DVector2 e = p + DAngle::fromDeg(q * 90.).ToVector() * room;
				auto es = sect;
				updatesector(e, &es);
				ok = es && cansee(mid, sect, DVector3(e, mid.Z), es);
			}
			for (auto& t : taken) if (ok && (t.pos.XY() - p).LengthSquared() < room * room * 4) ok = false;
			if (!ok) continue;
			out = { DVector3(p, fz), sect };
			return true;
		}
	}
	return false;
}

static bool WaveAvailable()
{
	auto pPlayer = LocalPlayer();
	if (!pPlayer || !pPlayer->GetActor() || pPlayer->GetActor()->xspr.health <= 0) return false;
	TArray<DBloodActor*> kinds;
	if (!WaveKinds(kinds)) return false;
	TArray<WaveSpot> none;
	WaveSpot spot;
	return FindWaveSpot(pPlayer->GetActor(), kinds[0], none, spot);
}

static void EnemyWave()
{
	auto pPlayer = LocalPlayer();
	if (!pPlayer || !pPlayer->GetActor() || pPlayer->GetActor()->xspr.health <= 0) return;
	auto pl = pPlayer->GetActor();
	TArray<DBloodActor*> kinds;
	if (!WaveKinds(kinds)) return;
	int count = 3 + gGameOptions.nDifficulty / 2; // 3, 3, 4, 4, 5 by skill
	TArray<WaveSpot> taken;
	for (int i = 0; i < count; i++)
	{
		auto kind = kinds[rand() % kinds.Size()];
		WaveSpot spot;
		if (!FindWaveSpot(pl, kind, taken, spot)) continue;
		taken.Push(spot);
		auto a = actSpawnDude(pl, kind->spr.type, -1);
		if (!a) continue;
		a->spr.pal = kind->spr.pal;
		a->spr.scale = kind->spr.scale;
		a->vel.Zero();
		a->spr.pos = spot.pos;
		ChangeActorSect(a, spot.sect);
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		if (bottom > spot.pos.Z) a->spr.pos.Z -= bottom - spot.pos.Z;
		a->spr.Angles.Yaw = (pl->spr.pos.XY() - a->spr.pos.XY()).Angle(); // facing you
		a->interpolated = false;
		viewBackupSpriteLoc(a);
		if (AllowedKillType(a)) Level.addKillCount();
		aiSetTarget(a, pl);
		aiActivateDude(a);
		sfxPlay3DSound(a, 201, -1, 0); // teleporter
	}
	if (mod_eventlog) Printf("mod effect: wave spawned %d of %d\n", int(taken.Size()), count);
}

//---------------------------------------------------------------------------
//
// bomber zombies
//
//---------------------------------------------------------------------------

static bool HasZombies()
{
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		int t = a->spr.type;
		if (t == kDudeZombieAxeNormal || t == kDudeZombieAxeBuried || t == kDudeZombieAxeLaying || t == kDudeZombieButcher) return true;
	}
	return false;
}

bool BomberAttack(DBloodActor* actor)
{
	if (!mods.bomberZombies || !actor || !actor->hasX() || actor->xspr.health <= 0) return false;
	actor->vel.Zero();
	// Same pattern as the modern kamikaze dude: blow up, then die in the blast (gib death).
	if (doExplosion(actor, kExplosionStandard) && actor->xspr.health > 0)
		actDamageSprite(actor, actor, kDamageExplode, 65535);
	return true;
}

//---------------------------------------------------------------------------
//
// bullet shuffle: what the player's guns fire is remapped for the duration.
// Sources are gun hitscans and gun missiles; TNT can be a result but isn't
// remapped as a source (thrown things are configured by their callers).
//
//---------------------------------------------------------------------------

enum ShotKind { SK_None = -1, SK_Bullet, SK_Pellets, SK_Flare, SK_Napalm, SK_Tesla, SK_TNT, SK_Count };
static const char* const shotSource[] = { "Tommy gun", "Shotgun", "Flare gun", "Napalm launcher", "Tesla cannon" };
static const char* const shotResult[] = { "bullets", "buckshot", "flares", "fireballs", "tesla bolts", "dynamite" };
static bool bulletShuffleOn, inRemap;
static int shotMap[SK_TNT];          // source kind (Bullet..Tesla) -> result kind
static int budgetClock, missileBudget, burstBudget, bulletBudget, nextTNTClock;

static int VectorKind(int vectorType)
{
	switch (vectorType)
	{
	case kVectorTommyRegular: case kVectorTommyAP: return SK_Bullet;
	case kVectorShell: case kVectorShellAP: return SK_Pellets;
	default: return SK_None; // pitchfork, beast claws, enemy-only vectors
	}
}

static int MissileKind(int nType)
{
	switch (nType)
	{
	case kMissileFlareRegular: case kMissileFlareAlt: return SK_Flare;
	case kMissileFireballNapalm: return SK_Napalm;
	case kMissileTeslaRegular: case kMissileTeslaAlt: return SK_Tesla;
	default: return SK_None; // spray flames, life leech
	}
}

static void BulletShuffleStart()
{
	// Random mapping of the 5 gun kinds onto the 6 results with no gun keeping its own ammo.
	int pool[SK_Count];
	for (;;)
	{
		for (int i = 0; i < SK_Count; i++) pool[i] = i;
		for (int i = SK_Count - 1; i > 0; i--) std::swap(pool[i], pool[rand() % (i + 1)]);
		bool fixed = false;
		for (int s = 0; s < SK_TNT; s++) if (pool[s] == s) fixed = true;
		if (!fixed) break;
	}
	FString msg;
	for (int s = 0; s < SK_TNT; s++)
	{
		shotMap[s] = pool[s];
		msg.AppendFormat("%s%s fires %s", s ? ", " : "", shotSource[s], shotResult[pool[s]]);
	}
	Printf(PRINT_NOTIFY, "%s\n", msg.GetChars());
	bulletShuffleOn = true;
}

// Per-tic budgets so a 16-pellet shotgun blast can't turn into 16 rockets.
static bool Spend(int& budget)
{
	if (budgetClock != PlayClock) { budgetClock = PlayClock; missileBudget = 2; burstBudget = 1; bulletBudget = 4; }
	if (budget <= 0) return false;
	budget--;
	return true;
}

static void FireKind(DBloodActor* shooter, double xyoff, double zoff, DVector3 dv, int kind)
{
	double len = dv.XY().Length();
	if (len <= 0) return;
	dv /= len;
	inRemap = true;
	switch (kind)
	{
	case SK_Bullet:
		if (Spend(bulletBudget)) actFireVector(shooter, xyoff, zoff, dv, kVectorTommyRegular);
		break;
	case SK_Pellets:
		if (Spend(burstBudget))
			for (int i = 0; i < 8; i++)
				actFireVector(shooter, xyoff, zoff, DVector3(dv.X + RandSym() * 0.09, dv.Y + RandSym() * 0.09, dv.Z + RandSym() * 0.05), kVectorShell);
		break;
	case SK_Flare:
		if (Spend(missileBudget)) actFireMissile(shooter, xyoff, zoff, dv, kMissileFlareRegular);
		break;
	case SK_Napalm:
		if (Spend(missileBudget)) actFireMissile(shooter, xyoff, zoff, dv, kMissileFireballNapalm);
		break;
	case SK_Tesla:
		if (Spend(missileBudget)) actFireMissile(shooter, xyoff, zoff, dv, kMissileTeslaRegular);
		break;
	case SK_TNT:
		if (PlayClock >= nextTNTClock)
		{
			nextTNTClock = PlayClock + 36; // at most ~3 bundles per second
			auto t = actFireThing(shooter, xyoff, zoff, dv.Z * 0.25 - 0.1, kThingArmedTNTBundle, 24.);
			if (t && t->hasX()) t->xspr.Impact = 1; // goes off on contact
		}
		break;
	}
	inRemap = false;
}

bool RemapVector(DBloodActor* shooter, double offset, double zoffset, const DVector3& dv, int vectorType)
{
	if (!bulletShuffleOn || inRemap || !shooter || !shooter->IsPlayerActor()) return false;
	int src = VectorKind(vectorType);
	if (src == SK_None) return false;
	FireKind(shooter, offset, zoffset, dv, shotMap[src]);
	return true;
}

bool RemapMissile(DBloodActor* shooter, double xyoff, double zoff, const DVector3& dv, int nType)
{
	if (!bulletShuffleOn || inRemap || !shooter || !shooter->IsPlayerActor()) return false;
	int src = MissileKind(nType);
	if (src == SK_None) return false;
	FireKind(shooter, xyoff, zoff, dv, shotMap[src]);
	return true;
}

static const Effect effects[] =
{
	// name, title, category, oneShot, fixedSeconds, start, stop, tick, available, titleNow, suspend, resume, group
	// Groups: "size" = giant, tiny and the shuffles (a shuffle retypes enemies, which would clash with the
	// sizes giant/tiny restore); "enemyspeed" = slowmo, fast.

	{ "giant", "Giant enemies", Category::Silly, false, 0,
		[] { SizeStart(2., false); }, SizeStop, nullptr, HasEnemies, nullptr,
		[] { SizeApplyAll(false); }, [] { SizeApplyAll(true); }, "size" },

	{ "tiny", "Tiny enemies", Category::Silly, false, 0,
		[] { SizeStart(0.5, true); }, SizeStop, nullptr, HasEnemies, nullptr,
		[] { SizeApplyAll(false); }, [] { SizeApplyAll(true); }, "size" },

	{ "drunk", "Drunk camera", Category::Silly, false, 0,
		[] { mods.drunk = 0.; }, [] { mods.drunk = 0.; },
		[](double secs) { mods.drunk = std::min(1., secs / 2.); }, nullptr, nullptr, nullptr, nullptr },

	{ "birdseye", "Bird's eye", Category::Silly, false, 15,
		[] { savedIso = IsoOn(); SetIso(!savedIso); }, [] { SetIso(savedIso); }, nullptr,
		[] { return IsoCVar() != nullptr; },
		[] { return IsoOn() ? "Ground level" : "Bird's eye"; }, nullptr, nullptr },

	{ "acid", "Acid trip", Category::Silly, false, 0,
		[] { acidTarget = 1.; }, [] { acidTarget = 0.; }, nullptr,
		[] { return FindPPShader("bmod_acid") != nullptr; }, nullptr, nullptr, nullptr },

	{ "lowgrav", "Low gravity", Category::Twist, false, 0,
		[] { mods.gravityScale = 0.35; }, [] { mods.gravityScale = 1.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "infammo", "Infinite ammo", Category::Twist, false, 0,
		[] { savedInfiniteAmmo = gInfiniteAmmo; gInfiniteAmmo = true; }, [] { gInfiniteAmmo = savedInfiniteAmmo; },
		nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "glasscannon", "Glass cannon", Category::Twist, false, 0,
		[] { mods.damageDealtScale = 4.; mods.damageTakenScale = 2.; },
		[] { mods.damageDealtScale = 1.; mods.damageTakenScale = 1.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "weaponswap", "Weapon roulette", Category::Twist, true, 0,
		RouletteSpin, nullptr, nullptr, RouletteAvailable, nullptr, nullptr, nullptr },

	{ "spread", "Spray and pray", Category::Twist, false, 0,
		[] { mods.spreadDegrees = 7.; }, [] { mods.spreadDegrees = 0.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "bigboom", "Big explosions", Category::Twist, false, 0,
		[] { mods.splashScale = 2.; }, [] { mods.splashScale = 1.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "slowmo", "Slow-motion enemies", Category::Twist, false, 0,
		[] { mods.enemyTimeScale = 0.5; }, [] { mods.enemyTimeScale = 1.; }, nullptr, HasEnemies, nullptr, nullptr, nullptr,
		"enemyspeed" },

	{ "fast", "Fast enemies", Category::Twist, false, 0,
		[] { mods.enemyTimeScale = 2.; }, [] { mods.enemyTimeScale = 1.; }, nullptr, HasEnemies, nullptr, nullptr, nullptr,
		"enemyspeed" },

	{ "nohud", "No HUD", Category::Nasty, false, 0,
		[] { mods.hideHud = true; }, [] { mods.hideHud = false; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "invert", "Inverted controls", Category::Nasty, false, 0,
		[] { gModInvertControls = true; }, [] { gModInvertControls = false; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "upsidedown", "Upside down", Category::Nasty, false, 0,
		[] { flipY = true; }, [] { flipY = false; }, nullptr,
		[] { return FindPPShader("bmod_flip") != nullptr; }, nullptr, nullptr, nullptr },

	{ "mirror", "Mirror world", Category::Nasty, false, 0,
		[] { flipX = true; }, [] { flipX = false; }, nullptr,
		[] { return FindPPShader("bmod_flip") != nullptr; }, nullptr, nullptr, nullptr },

	// one-shots: the shuffled enemies stay shuffled (like any normal game event)
	{ "enemyshuffle", "Enemy shuffle", Category::Twist, true, 0,
		EnemyShuffle, nullptr, nullptr,
		[] { int types; return CountShuffleable(false, &types) >= 2 && types >= 2; }, nullptr, nullptr, nullptr, "size" },

	{ "cultistshuffle", "Cultist shuffle", Category::Twist, true, 0,
		CultistShuffle, nullptr, nullptr,
		[] { return CountShuffleable(true, nullptr) >= 1; }, nullptr, nullptr, nullptr, "size" },

	{ "bomber", "Bomber zombies", Category::Twist, false, 0,
		[] { mods.bomberZombies = true; }, [] { mods.bomberZombies = false; }, nullptr, HasZombies, nullptr, nullptr, nullptr },

	{ "bulletshuffle", "Bullet shuffle", Category::Twist, false, 0,
		BulletShuffleStart, [] { bulletShuffleOn = false; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	// one-shot: the new enemies stay, like any others
	{ "wave", "Enemy wave", Category::Nasty, true, 0,
		EnemyWave, nullptr, nullptr, WaveAvailable, nullptr, nullptr, nullptr },
};

struct ActiveEffect { const Effect* effect; int startClock; };
static TArray<ActiveEffect> active;

int NumEffects() { return int(countof(effects)); }
const Effect& GetEffect(int i) { return effects[i]; }

const Effect* FindEffect(const char* name)
{
	for (auto& e : effects) if (!stricmp(e.name, name)) return &e;
	return nullptr;
}

const char* EffectTitle(const Effect* e) { return e->titleNow ? e->titleNow() : e->title; }
bool CanStart(const Effect* e) { return e && !IsActive(e) && (!e->available || e->available()); }

bool Rivals(const Effect* a, const Effect* b)
{
	return a && b && a != b && a->group && b->group && !strcmp(a->group, b->group);
}

bool IsActive(const Effect* e)
{
	for (auto& a : active) if (a.effect == e) return true;
	return false;
}

bool StartEffect(const Effect* e)
{
	if (!CanStart(e)) return false;
	// Effects of one group change the same state: the newcomer replaces a running rival.
	for (unsigned i = active.Size(); i-- > 0; )
		if (Rivals(active[i].effect, e)) StopEffect(active[i].effect);
	if (e->start) e->start();
	if (!e->oneShot) active.Push({ e, PlayClock });
	if (mod_eventlog) Printf("mod effect: start %s\n", e->name);
	return true;
}

void StopEffect(const Effect* e)
{
	for (unsigned i = 0; i < active.Size(); i++)
	{
		if (active[i].effect != e) continue;
		active.Delete(i);
		if (e->stop) e->stop();
		if (mod_eventlog) Printf("mod effect: stop %s\n", e->name);
		return;
	}
}

void StopAllEffects()
{
	while (active.Size()) StopEffect(active.Last().effect);
	mods = Modifiers(); // belt and braces: back to stock values
	gModInvertControls = false;
	bulletShuffleOn = false;
	ResetShaders();
}

static void OnSaveWrite(bool before)
{
	for (auto& a : active)
	{
		if (before && a.effect->suspend) a.effect->suspend();
		if (!before && a.effect->resume) a.effect->resume();
	}
}

static struct SaveHookRegistrar
{
	SaveHookRegistrar() { ::gModSaveHook = [](bool before) { OnSaveWrite(before); }; }
} saveHookRegistrar;

void OnTick()
{
	for (auto& a : active)
		if (a.effect->tick) a.effect->tick((PlayClock - a.startClock) / 120.);
	for (auto fn : tickers()) fn();
}

void OnDrawOverlay(DBloodPlayer* pPlayer)
{
	for (auto fn : drawers()) fn(pPlayer);
}

} // namespace bmod

static const char* CategoryName(bmod::Category c)
{
	return c == bmod::Category::Silly ? "silly" : c == bmod::Category::Twist ? "twist" : "nasty";
}

CCMD(mod_effects)
{
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto& e = bmod::GetEffect(i);
		Printf("%-12s %-20s %s%s%s\n", e.name, e.title, CategoryName(e.category), e.oneShot ? ", one-shot" : "",
			bmod::IsActive(&e) ? " (active)" : "");
	}
}

END_BLD_NS
