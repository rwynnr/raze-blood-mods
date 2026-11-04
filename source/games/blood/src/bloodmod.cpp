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

// Core-side hooks (gameinput.cpp, savegamehelp.cpp).
extern bool gModInvertControls;
extern void (*gModSaveHook)(bool before);

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
	if (auto s = FindPPShader("bmod_acid"))
	{
		s->Enabled = acidAmount > 0.001;
		SetUniform(s, "amount", acidAmount);
		SetUniform(s, "timer", (now % 3600000) / 1000.);
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

// Weapons the local player could be swapped to right now.
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

static bool HasEnemies()
{
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next()) if (!a->IsPlayerActor() && a->hasX()) return true;
	return false;
}

static const Effect effects[] =
{
	// name, title, category, oneShot, fixedSeconds, start, stop, tick, available, titleNow, suspend, resume

	{ "giant", "Giant enemies", Category::Silly, false, 0,
		[] { SizeStart(2., false); }, SizeStop, nullptr, HasEnemies, nullptr,
		[] { SizeApplyAll(false); }, [] { SizeApplyAll(true); } },

	{ "tiny", "Tiny enemies", Category::Silly, false, 0,
		[] { SizeStart(0.5, true); }, SizeStop, nullptr, HasEnemies, nullptr,
		[] { SizeApplyAll(false); }, [] { SizeApplyAll(true); } },

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

	{ "weaponswap", "Weapon swap", Category::Twist, true, 0,
		[]
		{
			int list[kWeapMax];
			int n = SwapCandidates(list);
			if (n > 0) LocalPlayer()->newWeapon = list[rand() % n];
		},
		nullptr, nullptr, [] { return SwapCandidates(nullptr) > 0; }, nullptr, nullptr, nullptr },

	{ "spread", "Spray and pray", Category::Twist, false, 0,
		[] { mods.spreadDegrees = 7.; }, [] { mods.spreadDegrees = 0.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "bigboom", "Big explosions", Category::Twist, false, 0,
		[] { mods.splashScale = 2.; }, [] { mods.splashScale = 1.; }, nullptr, nullptr, nullptr, nullptr, nullptr },

	{ "slowmo", "Slow-motion enemies", Category::Twist, false, 0,
		[] { mods.enemyTimeScale = 0.5; }, [] { mods.enemyTimeScale = 1.; }, nullptr, HasEnemies, nullptr, nullptr, nullptr },

	{ "fast", "Fast enemies", Category::Twist, false, 0,
		[] { mods.enemyTimeScale = 2.; }, [] { mods.enemyTimeScale = 1.; }, nullptr, HasEnemies, nullptr, nullptr, nullptr },

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

bool IsActive(const Effect* e)
{
	for (auto& a : active) if (a.effect == e) return true;
	return false;
}

bool StartEffect(const Effect* e)
{
	if (!CanStart(e)) return false;
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
