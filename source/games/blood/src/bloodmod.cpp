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

CVARD(Bool, mod_eventlog, false, 0, "print each mod-base game event to the console")

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
// modifier helpers
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

void ModifyView(DRotator& angles, double interpfrac)
{
	if (mods.drunk <= 0) return;
	// Slow, smooth sway (well under 1 Hz).
	double t = (PlayClock + interpfrac * kTicsPerFrame) / 120.;
	angles.Roll += DAngle::fromDeg(sin(t * 1.9) * 9. * mods.drunk);
	angles.Yaw += DAngle::fromDeg(sin(t * 0.8) * 5. * mods.drunk);
	angles.Pitch += DAngle::fromDeg(sin(t * 1.3) * 4. * mods.drunk);
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

static const Effect effects[] =
{
	{ "giant", "Giant enemies", Category::Silly, false, 0,
		[] { mods.enemyDrawScale = 2.; }, [] { mods.enemyDrawScale = 1.; }, nullptr, nullptr, nullptr },

	{ "tiny", "Tiny enemies", Category::Silly, false, 0,
		[] { mods.enemyDrawScale = 0.5; }, [] { mods.enemyDrawScale = 1.; }, nullptr, nullptr, nullptr },

	{ "drunk", "Drunk camera", Category::Silly, false, 0,
		[] { mods.drunk = 0.; }, [] { mods.drunk = 0.; },
		[](double secs) { mods.drunk = std::min(1., secs / 2.); }, nullptr, nullptr },

	{ "birdseye", "Bird's eye", Category::Silly, false, 15,
		[] { savedIso = IsoOn(); SetIso(!savedIso); }, [] { SetIso(savedIso); }, nullptr,
		[] { return IsoCVar() != nullptr; },
		[] { return IsoOn() ? "Ground level" : "Bird's eye"; } },

	{ "lowgrav", "Low gravity", Category::Twist, false, 0,
		[] { mods.gravityScale = 0.35; }, [] { mods.gravityScale = 1.; }, nullptr, nullptr, nullptr },

	{ "infammo", "Infinite ammo", Category::Twist, false, 0,
		[] { savedInfiniteAmmo = gInfiniteAmmo; gInfiniteAmmo = true; }, [] { gInfiniteAmmo = savedInfiniteAmmo; },
		nullptr, nullptr, nullptr },

	{ "glasscannon", "Glass cannon", Category::Twist, false, 0,
		[] { mods.damageDealtScale = 4.; mods.damageTakenScale = 2.; },
		[] { mods.damageDealtScale = 1.; mods.damageTakenScale = 1.; }, nullptr, nullptr, nullptr },

	{ "weaponswap", "Weapon swap", Category::Twist, true, 0,
		[]
		{
			int list[kWeapMax];
			int n = SwapCandidates(list);
			if (n > 0) LocalPlayer()->newWeapon = list[rand() % n];
		},
		nullptr, nullptr, [] { return SwapCandidates(nullptr) > 0; }, nullptr },

	{ "nohud", "No HUD", Category::Nasty, false, 0,
		[] { mods.hideHud = true; }, [] { mods.hideHud = false; }, nullptr, nullptr, nullptr },
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
}

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
		Printf("%-12s %-16s %s%s%s\n", e.name, e.title, CategoryName(e.category), e.oneShot ? ", one-shot" : "",
			bmod::IsActive(&e) ? " (active)" : "");
	}
}

END_BLD_NS
