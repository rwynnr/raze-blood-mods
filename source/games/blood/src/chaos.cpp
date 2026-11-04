//---------------------------------------------------------------------------
//
// blood2d chaos mode: every so often, a random temporary effect from the mod
// base's effect library (bloodmod.h), announced by a banner. A new effect
// starts every chaos_interval seconds; up to chaos_maxactive run at once (when
// a new one starts, the one closest to ending makes room). Settings menu:
// Options > Chaos mode (wadsrc/static/menudef.txt). Design in the blood2d
// workspace, docs/chaos.md.
//
//---------------------------------------------------------------------------

#include "ns.h"
#include <random>
#include "build.h"
#include "blood.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_draw.h"
#include "v_font.h"
#include "printf.h"

static bool chaosReplan;  // the interval changed: move the next effect to match
static bool chaosRecheck; // effect switches changed: running effects that were switched off end

CVARD(Bool, chaos, false, 0, "chaos mode: random temporary effects (toggle with chaos_toggle, bound to N)")
CUSTOM_CVARD(Int, chaos_interval, 45, CVAR_ARCHIVE, "chaos: a new effect starts every this many seconds")
{
	if (self < 2) self = 2;
	else if (self > 600) self = 600;
	chaosReplan = true;
}
CUSTOM_CVARD(Int, chaos_duration, 20, CVAR_ARCHIVE, "chaos: how long each effect lasts, seconds")
{
	if (self < 2) self = 2;
	else if (self > 300) self = 300;
}
CUSTOM_CVARD(Int, chaos_maxactive, 1, CVAR_ARCHIVE, "chaos: most effects running at once (when a new one starts, the one closest to ending makes room)")
{
	if (self < 1) self = 1;
	else if (self > 4) self = 4;
}
CVARD(Bool, chaos_random, true, CVAR_ARCHIVE, "chaos: vary each wait and each effect's length by up to a quarter")
CUSTOM_CVARD(Int, chaos_warning, 3, CVAR_ARCHIVE, "chaos: seconds of warning before an effect starts")
{
	if (self < 0) self = 0;
	else if (self > 10) self = 10;
}
CUSTOM_CVARD(Int, chaos_firstwait, 10, CVAR_ARCHIVE, "chaos: wait before the first effect after turning chaos on, seconds")
{
	if (self < 1) self = 1;
}
CUSTOM_CVARD(String, chaos_exclude, "", CVAR_ARCHIVE, "chaos: effects never to pick, separated by spaces (names from mod_effects)")
{
	chaosRecheck = true;
}

// Per-effect switches (Options > Chaos mode > Choose effects). An effect without one is always allowed.
#define CHAOS_FX(fx) CUSTOM_CVARD(Bool, chaos_fx_##fx, true, CVAR_ARCHIVE, "chaos: allow the " #fx " effect") { chaosRecheck = true; }
CHAOS_FX(giant)
CHAOS_FX(tiny)
CHAOS_FX(drunk)
CHAOS_FX(birdseye)
CHAOS_FX(acid)
CHAOS_FX(lowgrav)
CHAOS_FX(infammo)
CHAOS_FX(glasscannon)
CHAOS_FX(weaponswap)
CHAOS_FX(spread)
CHAOS_FX(bigboom)
CHAOS_FX(slowmo)
CHAOS_FX(fast)
CHAOS_FX(enemyshuffle)
CHAOS_FX(cultistshuffle)
CHAOS_FX(bomber)
CHAOS_FX(bulletshuffle)
CHAOS_FX(nohud)
CHAOS_FX(invert)
CHAOS_FX(upsidedown)
CHAOS_FX(mirror)
#undef CHAOS_FX

BEGIN_BLD_NS

namespace
{

constexpr int kClock = 120; // PlayClock units per second

struct Running
{
	const bmod::Effect* effect;
	FString title;
	int start, end; // PlayClock; one-shots stay listed for a few seconds so their banner shows
};

bool on;                        // chaos was running last tic
TArray<Running> running;
int lastStart, nextStart;       // PlayClock: when the previous effect started, when the next one will
bool picked;                    // the next effect has been chosen (its warning is showing)...
const bmod::Effect* pending;    // ...and this is it (null: nothing could run this time)
FString pendingTitle;
const bmod::Effect* lastEffect; // never pick the same one twice in a row
int lastClock;

std::mt19937& Rng() { static std::mt19937 rng(std::random_device{}()); return rng; }

int RandRange(int lo, int hi)
{
	if (hi < lo) std::swap(lo, hi);
	return std::uniform_int_distribution<int>(lo, hi)(Rng());
}

// A setting in seconds as PlayClock units, varied by up to a quarter when chaos_random is on.
int Vary(int seconds)
{
	int c = std::max(seconds, 1) * kClock;
	if (chaos_random) c = RandRange(c * 3 / 4, c * 5 / 4);
	return std::max(c, kClock);
}

int WarningClocks()
{
	// Never as long as the wait itself.
	return std::clamp((int)chaos_warning, 0, std::max((int)chaos_interval - 1, 0)) * kClock;
}

int MaxActive() { return std::clamp((int)chaos_maxactive, 1, 4); }

bool Excluded(const char* name)
{
	const char* p = *chaos_exclude;
	size_t len = strlen(name);
	while (p && *p)
	{
		while (*p == ' ' || *p == ',' || *p == ';') p++;
		const char* start = p;
		while (*p && *p != ' ' && *p != ',' && *p != ';') p++;
		if (size_t(p - start) == len && !strnicmp(start, name, len)) return true;
	}
	return false;
}

bool Allowed(const bmod::Effect* e)
{
	if (Excluded(e->name)) return false;
	auto cv = FindCVar(FStringf("chaos_fx_%s", e->name).GetChars(), nullptr);
	return !cv || cv->GetGenericRep(CVAR_Bool).Bool;
}

int TimedCount()
{
	int n = 0;
	for (auto& r : running) if (!r.effect->oneShot) n++;
	return n;
}

int SoonestEnding()
{
	int best = -1;
	for (unsigned i = 0; i < running.Size(); i++)
		if (!running[i].effect->oneShot && (best < 0 || running[i].end < running[best].end)) best = int(i);
	return best;
}

void StopRunning(unsigned i)
{
	auto e = running[i].effect;
	running.Delete(i);
	if (!e->oneShot && bmod::IsActive(e)) bmod::StopEffect(e);
}

void EndAll()
{
	while (running.Size()) StopRunning(running.Size() - 1);
}

// The next effect starts `clocks` from now.
void PlanNext(int clocks)
{
	lastStart = PlayClock;
	nextStart = PlayClock + std::max(clocks, 1);
	picked = false;
	pending = nullptr;
}

void Restart(int clocks)
{
	EndAll();
	PlanNext(clocks);
	lastClock = PlayClock;
	chaosReplan = false;
}

// The interval setting changed: the next effect moves to match, unless it's already been announced.
void Replan()
{
	if (picked) return;
	nextStart = std::max(lastStart + (int)chaos_interval * kClock, PlayClock + WarningClocks());
}

// Would `e` clash with an effect that's still running when it starts?
bool Clashes(const bmod::Effect* e)
{
	for (auto& r : running)
		if (!r.effect->oneShot && r.end > nextStart && bmod::Rivals(r.effect, e)) return true;
	return false;
}

const bmod::Effect* PickEffect()
{
	// Pick a category by weight (silly 60 / twist 30 / nasty 10), then an effect within it.
	TArray<const bmod::Effect*> byCat[3];
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		if (e == lastEffect || !Allowed(e) || !bmod::CanStart(e) || Clashes(e)) continue;
		byCat[int(e->category)].Push(e);
	}
	static const int weights[3] = { 60, 30, 10 };
	int total = 0;
	for (int c = 0; c < 3; c++) if (byCat[c].Size()) total += weights[c];
	if (total == 0) return nullptr;
	int r = RandRange(0, total - 1);
	for (int c = 0; c < 3; c++)
	{
		if (!byCat[c].Size()) continue;
		if (r < weights[c]) return byCat[c][RandRange(0, int(byCat[c].Size()) - 1)];
		r -= weights[c];
	}
	return nullptr;
}

bool LocalPlayerAlive()
{
	auto pPlayer = getPlayer(myconnectindex);
	return pPlayer && pPlayer->GetActor() && pPlayer->GetActor()->xspr.health > 0;
}

// Starts `e` now (its warning, if any, has been shown) and plans the next one. Returns false if it can't run.
bool Launch(const bmod::Effect* e, const char* title)
{
	if (!bmod::CanStart(e)) return false;
	// A running rival ends first (the base would end it anyway), then the one closest to ending makes room.
	for (unsigned i = running.Size(); i-- > 0; )
		if (!running[i].effect->oneShot && bmod::Rivals(running[i].effect, e)) StopRunning(i);
	if (!e->oneShot)
		while (TimedCount() >= MaxActive()) StopRunning(SoonestEnding());
	if (!bmod::StartEffect(e)) return false;
	lastEffect = e;
	PlanNext(Vary(chaos_interval));
	int end;
	if (e->oneShot) end = PlayClock + 3 * kClock; // just show the banner briefly
	else
	{
		end = PlayClock + (e->fixedSeconds > 0 ? e->fixedSeconds * kClock : Vary(chaos_duration));
		if (MaxActive() == 1) end = std::min(end, nextStart); // one at a time: over before the next one lands
	}
	running.Push({ e, FString(title), PlayClock, std::max(end, PlayClock + kClock) });
	auto pPlayer = getPlayer(myconnectindex);
	if (pPlayer && pPlayer->GetActor()) sfxPlay3DSound(pPlayer->GetActor(), 776, -1, 0); // powerup chime
	return true;
}

void ChaosTick()
{
	if (!chaos)
	{
		if (on) { EndAll(); on = false; }
		return;
	}
	if (!on)
	{
		on = true;
		Restart(std::max((int)chaos_firstwait, 1) * kClock);
	}
	else if (PlayClock < lastClock) Restart(Vary(chaos_interval)); // the game clock was reset under us
	lastClock = PlayClock;
	if (chaosReplan) { chaosReplan = false; Replan(); }
	if (!LocalPlayerAlive()) return; // death ends effects (listener); wait for the reload/respawn

	// Effects whose time is up, that ended elsewhere, or that were switched off in the menu.
	bool recheck = chaosRecheck;
	chaosRecheck = false;
	for (unsigned i = running.Size(); i-- > 0; )
	{
		auto& r = running[i];
		bool timed = !r.effect->oneShot;
		if (PlayClock >= r.end || (timed && !bmod::IsActive(r.effect)) || (timed && recheck && !Allowed(r.effect)))
			StopRunning(i);
	}
	// More running than allowed (the setting was lowered): the ones closest to ending go.
	while (TimedCount() > MaxActive()) StopRunning(SoonestEnding());

	if (automapMode != am_off) return; // nothing starts while the automap is up
	if (!picked && PlayClock >= nextStart - WarningClocks())
	{
		picked = true;
		pending = PickEffect();
		if (pending)
		{
			pendingTitle = bmod::EffectTitle(pending);
			nextStart = std::max(nextStart, PlayClock + WarningClocks()); // always the full warning (e.g. after the automap)
		}
	}
	if (PlayClock >= nextStart)
	{
		auto e = pending;
		FString title = pendingTitle;
		if (!e || !Launch(e, title.GetChars())) PlanNext(Vary(chaos_interval)); // nothing could run: try again next time
	}
}

void ChaosEvent(const bmod::EventInfo& ev)
{
	switch (ev.type)
	{
	case bmod::Event::PlayerDeath:
		if (ev.player != getPlayer(myconnectindex)) break;
		[[fallthrough]];
	case bmod::Event::LevelStart:
	case bmod::Event::LevelEnd:
	case bmod::Event::SaveLoaded:
		// Effects end on death, level start/end and load (saving doesn't end them: saves stay clean).
		if (chaos && on) Restart(Vary(chaos_interval));
		else { EndAll(); on = false; }
		break;
	default:
		break;
	}
}

void DrawCentered(FFont* font, int color, double y, const char* text, double scale, double alpha = 1.)
{
	double w = font->StringWidth(text) * scale;
	DrawText(twod, font, color, (twod->GetWidth() - w) * 0.5, y, text, DTA_ScaleX, scale, DTA_ScaleY, scale, DTA_Alpha, alpha, TAG_DONE);
}

int CategoryColor(bmod::Category c)
{
	return c == bmod::Category::Silly ? CR_GREEN : c == bmod::Category::Twist ? CR_GOLD : CR_RED;
}

void ChaosDraw(DBloodPlayer*)
{
	if (!chaos || !on) return;
	double sc = twod->GetHeight() / 480.;
	double y = 10 * sc;

	// Top line: the countdown to the next effect, or the warning naming it.
	if (picked && pending)
	{
		// Gentle 1 Hz pulse; nothing on screen flashes faster than that.
		double pulse = 0.75 + 0.25 * sin(PlayClock * (2 * pi::pi() / kClock));
		FStringf s("Incoming: %s", pendingTitle.GetChars());
		double bsc = 1.2 * sc;
		DrawCentered(BigFont, CategoryColor(pending->category), y, s.GetChars(), bsc, pulse);
		y += BigFont->GetHeight() * bsc + 4 * sc;
	}
	else
	{
		int remain = std::max(nextStart - PlayClock, 0);
		FStringf s("Chaos: next effect in %d s", (remain + kClock - 1) / kClock);
		double ssc = 1.5 * sc;
		DrawCentered(SmallFont, CR_WHITE, y, s.GetChars(), ssc, 0.7);
		y += SmallFont->GetHeight() * ssc + 4 * sc;
	}

	// Running effects, oldest first, each with a bar for the time it has left.
	double tsc = (running.Size() > 1 ? 1.1 : 1.4) * sc;
	for (auto& r : running)
	{
		DrawCentered(BigFont, CategoryColor(r.effect->category), y, r.title.GetChars(), tsc);
		y += BigFont->GetHeight() * tsc + 3 * sc;
		if (r.effect->oneShot) continue;
		double barW = 200 * sc, barH = 6 * sc;
		double x = (twod->GetWidth() - barW) * 0.5;
		double frac = r.end > r.start ? double(std::max(r.end - PlayClock, 0)) / (r.end - r.start) : 0.;
		Dim(twod, 0xff000000, 0.6f, int(x), int(y), int(barW), int(barH));
		int fw = int((barW - 2) * std::clamp(frac, 0., 1.));
		if (fw > 0) Dim(twod, 0xffe0e0e0, 0.9f, int(x + 1), int(y + 1), fw, int(barH - 2));
		y += barH + 6 * sc;
	}
}

struct ChaosRegistrar
{
	ChaosRegistrar()
	{
		bmod::AddTicker(ChaosTick);
		bmod::AddListener(ChaosEvent);
		bmod::AddDrawer(ChaosDraw);
	}
} chaosRegistrar;

// Steps the interval through a few common paces.
void ChaosPace(int dir)
{
	static const int paces[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120 };
	int cur = chaos_interval, next = cur;
	if (dir < 0)
	{
		for (int i = int(countof(paces)) - 1; i >= 0; i--)
			if (paces[i] < cur) { next = paces[i]; break; }
	}
	else
	{
		for (int p : paces)
			if (p > cur) { next = p; break; }
	}
	chaos_interval = next;
	Printf(PRINT_NOTIFY, "Chaos: a new effect every %d s\n", next);
}

} // namespace

CCMD(chaos_toggle)
{
	chaos = !chaos;
	Printf(PRINT_NOTIFY, "Chaos %s\n", chaos ? "ON" : "OFF");
}

CCMD(chaos_faster) { ChaosPace(-1); }
CCMD(chaos_slower) { ChaosPace(1); }

CCMD(chaos_force)
{
	auto e = argv.argc() >= 2 ? bmod::FindEffect(argv[1]) : nullptr;
	if (!e)
	{
		Printf("usage: chaos_force <effect>   effects:");
		for (int i = 0; i < bmod::NumEffects(); i++) Printf(" %s", bmod::GetEffect(i).name);
		Printf("\n");
		return;
	}
	if (gamestate != GS_LEVEL) { Printf("chaos_force: not in a level\n"); return; }
	if (!chaos) { chaos = true; Printf(PRINT_NOTIFY, "Chaos ON\n"); }
	if (!on)
	{
		on = true;
		Restart(std::max((int)chaos_firstwait, 1) * kClock);
	}
	for (unsigned i = running.Size(); i-- > 0; )
		if (running[i].effect == e) StopRunning(i); // already running: start it over
	FString title = bmod::EffectTitle(e);
	if (!Launch(e, title.GetChars())) Printf("chaos_force: %s can't run right now\n", e->name);
}

END_BLD_NS
