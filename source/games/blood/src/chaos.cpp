//---------------------------------------------------------------------------
//
// blood2d chaos mode: every so often, a random effect from the mod base's
// effect library (bloodmod.h). A new effect starts every chaos_interval
// seconds, picked by each effect's chance (chaos_chance_<name>, 0-10); up to
// chaos_maxactive run at once (when a new one starts, the one closest to
// ending makes room). A new effect is announced with a big title at the top
// for two seconds, then shown as a chip in a row above the status bar until it
// ends. Settings menu: Options > Chaos mode (wadsrc/static/menudef.txt).
// Design in the blood2d workspace, docs/chaos.md.
//
//---------------------------------------------------------------------------

#include "ns.h"
#include <algorithm>
#include <climits>
#include <random>
#include <vector>
#include "build.h"
#include "blood.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamecvars.h"
#include "texturemanager.h"
#include "v_draw.h"
#include "v_font.h"
#include "printf.h"

static bool chaosReplan;  // the interval changed: move the next effect to match
static bool chaosRecheck; // effect chances changed: running effects set to 0 end

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

// Per-effect chance (Options > Chaos mode > Choose effects): 0 = never, otherwise a weight, so an effect at 6
// comes up three times as often as one at 2. Defaults by category: silly 6, twist 4, nasty 2. chaos_fx_<name>
// is the older on/off switch: switching it off (as an older config may) sets the chance to 0 and flips back on.
#define CHAOS_FX(fx, def) \
	CUSTOM_CVARD(Int, chaos_chance_##fx, def, CVAR_ARCHIVE, "chaos: how often the " #fx " effect comes up, 0 (never) to 10") \
	{ \
		if (self < 0) self = 0; \
		else if (self > 10) self = 10; \
		chaosRecheck = true; \
	} \
	CUSTOM_CVARD(Bool, chaos_fx_##fx, true, CVAR_ARCHIVE, "chaos: 0 sets the " #fx " effect's chance to 0") \
	{ \
		if (!self) { chaos_chance_##fx = 0; self = true; } \
		chaosRecheck = true; \
	}
CHAOS_FX(giant, 6)
CHAOS_FX(tiny, 6)
CHAOS_FX(drunk, 6)
CHAOS_FX(birdseye, 6)
CHAOS_FX(acid, 6)
CHAOS_FX(lowgrav, 4)
CHAOS_FX(infammo, 4)
CHAOS_FX(glasscannon, 4)
CHAOS_FX(weaponswap, 4)
CHAOS_FX(spread, 4)
CHAOS_FX(bigboom, 4)
CHAOS_FX(slowmo, 4)
CHAOS_FX(fast, 4)
CHAOS_FX(enemyshuffle, 4)
CHAOS_FX(cultistshuffle, 4)
CHAOS_FX(bomber, 4)
CHAOS_FX(bulletshuffle, 4)
CHAOS_FX(nohud, 2)
CHAOS_FX(invert, 2)
CHAOS_FX(upsidedown, 2)
CHAOS_FX(mirror, 2)
CHAOS_FX(wave, 2)
#undef CHAOS_FX

BEGIN_BLD_NS

namespace
{

constexpr int kClock = 120;               // PlayClock units per second
constexpr int kAnnounce = 2 * kClock;     // a new effect's big title, before it turns into a chip
constexpr int kOneShotTitle = 3 * kClock; // one-shots get the big title only, shown a little longer
constexpr int kFade = kClock / 2;         // an ending effect's chip fades out over this

struct Running
{
	const bmod::Effect* effect;
	FString title;
	int start, end; // PlayClock; one-shots stay listed while their title shows
	int seq;        // order of arrival: chips run oldest first, left to right
};

struct Fading // the chip of an effect that just ended, fading where it was
{
	FString title;
	bmod::Category category;
	int seq, from;
};

bool on;                        // chaos was running last tic
TArray<Running> running;
TArray<Fading> fading;
int nextSeq;
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

int DefaultChance(bmod::Category c)
{
	return c == bmod::Category::Silly ? 6 : c == bmod::Category::Twist ? 4 : 2;
}

int EffectChance(const bmod::Effect* e)
{
	auto cv = FindCVar(FStringf("chaos_chance_%s", e->name).GetChars(), nullptr);
	if (!cv) return DefaultChance(e->category); // an effect added without a setting
	return std::clamp(cv->GetGenericRep(CVAR_Int).Int, 0, 10);
}

bool Allowed(const bmod::Effect* e)
{
	return !Excluded(e->name) && EffectChance(e) > 0;
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

// `fade`: leave the effect's chip on screen for a moment, fading (not when everything is reset at once).
void StopRunning(unsigned i, bool fade = true)
{
	Running r = running[i];
	running.Delete(i);
	if (fade && !r.effect->oneShot && PlayClock >= r.start + kAnnounce)
		fading.Push({ r.title, r.effect->category, r.seq, PlayClock });
	if (!r.effect->oneShot && bmod::IsActive(r.effect)) bmod::StopEffect(r.effect);
}

void EndAll()
{
	while (running.Size()) StopRunning(running.Size() - 1, false);
	fading.Clear();
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
	// Each effect that can run, by its chance (a weight). Never the same one twice in a row, unless it's the
	// only one that can run.
	struct Candidate { const bmod::Effect* e; int weight; };
	TArray<Candidate> list;
	int total = 0;
	const bmod::Effect* repeat = nullptr;
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		if (!Allowed(e) || !bmod::CanStart(e) || Clashes(e)) continue;
		if (e == lastEffect) { repeat = e; continue; }
		int w = EffectChance(e);
		list.Push({ e, w });
		total += w;
	}
	if (total == 0) return repeat;
	int r = RandRange(0, total - 1);
	for (auto& c : list)
	{
		if (r < c.weight) return c.e;
		r -= c.weight;
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
	if (e->oneShot) end = PlayClock + kOneShotTitle; // just the big title
	else
	{
		end = PlayClock + (e->fixedSeconds > 0 ? e->fixedSeconds * kClock : Vary(chaos_duration));
		if (MaxActive() == 1) end = std::min(end, nextStart); // one at a time: over before the next one lands
	}
	running.Push({ e, FString(title), PlayClock, std::max(end, PlayClock + kClock), nextSeq++ });
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

	// Effects whose time is up, that ended elsewhere, or whose chance was set to 0 in the menu.
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

//---------------------------------------------------------------------------
//
// drawing: the warning and new effects' big titles at the top; a row of chips
// just above the status bar (oldest left), each in its category's colour with
// the seconds left and a fill that drains, then a dashed "next" chip with the
// countdown (hidden while the warning shows). Ending chips fade out where they
// were; the rest close the gap. With the status bar hidden (hud_size, No HUD)
// the row sits at the bottom edge; a row too wide for the screen wraps upward.
//
//---------------------------------------------------------------------------

void DrawCentered(FFont* font, int color, double y, const char* text, double scale, double alpha = 1.)
{
	double w = font->StringWidth(text) * scale;
	DrawText(twod, font, color, (twod->GetWidth() - w) * 0.5, y, text, DTA_ScaleX, scale, DTA_ScaleY, scale, DTA_Alpha, alpha, TAG_DONE);
}

int CategoryColor(bmod::Category c)
{
	return c == bmod::Category::Silly ? CR_GREEN : c == bmod::Category::Twist ? CR_GOLD : CR_RED;
}

PalEntry CategoryRGB(bmod::Category c)
{
	return c == bmod::Category::Silly ? PalEntry(0xff4fdc4f) : c == bmod::Category::Twist ? PalEntry(0xfff2c230) : PalEntry(0xffff4a3a);
}

// Screen y of the top of the status bar or HUD, found the way the level stats find it (statusbar.zs,
// PrintLevelStats, with the heights Blood's sbar.zs reports); the bottom edge when nothing is drawn there.
double ChipsFloor()
{
	double h = twod->GetHeight();
	if (bmod::mods.hideHud || hud_size == Hud_Nothing) return h;
	double space = 28; // the fullscreen HUDs
	if (hud_size <= Hud_StbarOverlay)
	{
		auto id = TexMan.CheckForTexture("Statusbar", ETextureType::Any);
		auto tex = id.isValid() ? TexMan.GetGameTexture(id) : nullptr;
		if (tex) space = tex->GetDisplayHeight();
	}
	return h * (200. - space * (double)hud_scalefactor) / 200.;
}

struct Chip
{
	FString text;
	PalEntry rgb;
	double frac;  // time left, 0..1 (the fill)
	double alpha;
	bool next;    // the dashed countdown chip
	double w;
};

void DrawChip(const Chip& c, double x, double y, double h, double tsc, double padX, double padY, double sc)
{
	int ix = int(x), iy = int(y), iw = int(c.w), ih = int(h);
	int bt = std::max(1, int(sc * 0.75 + 0.5)); // border thickness
	float ba = float(0.9 * c.alpha);
	Dim(twod, 0xff000000, float(0.55 * c.alpha), ix, iy, iw, ih);
	if (!c.next)
	{
		int fw = int(iw * std::clamp(c.frac, 0., 1.));
		if (fw > 0) Dim(twod, c.rgb, float(0.3 * c.alpha), ix, iy, fw, ih);
		Dim(twod, c.rgb, ba, ix, iy, iw, bt);
		Dim(twod, c.rgb, ba, ix, iy + ih - bt, iw, bt);
		Dim(twod, c.rgb, ba, ix, iy, bt, ih);
		Dim(twod, c.rgb, ba, ix + iw - bt, iy, bt, ih);
	}
	else
	{
		int dash = std::max(3, int(4 * sc)), space = std::max(2, int(3 * sc));
		for (int dx = 0; dx < iw; dx += dash + space)
		{
			int len = std::min(dash, iw - dx);
			Dim(twod, c.rgb, ba, ix + dx, iy, len, bt);
			Dim(twod, c.rgb, ba, ix + dx, iy + ih - bt, len, bt);
		}
		for (int dy = 0; dy < ih; dy += dash + space)
		{
			int len = std::min(dash, ih - dy);
			Dim(twod, c.rgb, ba, ix, iy + dy, bt, len);
			Dim(twod, c.rgb, ba, ix + iw - bt, iy + dy, bt, len);
		}
	}
	DrawText(twod, SmallFont, c.next ? CR_GRAY : CR_WHITE, x + padX, y + padY, c.text.GetChars(),
		DTA_ScaleX, tsc, DTA_ScaleY, tsc, DTA_Alpha, c.alpha, TAG_DONE);
}

int SecondsLeft(int clocks) { return (std::max(clocks, 0) + kClock - 1) / kClock; }

void ChaosDraw(DBloodPlayer*)
{
	if (!chaos || !on) return;
	double W = twod->GetWidth();
	double sc = twod->GetHeight() / 480.;
	for (unsigned i = fading.Size(); i-- > 0; )
		if (PlayClock - fading[i].from >= kFade || PlayClock < fading[i].from) fading.Delete(i);

	// Top: the warning naming the next effect, then the big titles of effects that just started.
	double y = 10 * sc;
	if (picked && pending)
	{
		// Gentle 1 Hz pulse; nothing on screen flashes faster than that.
		double pulse = 0.75 + 0.25 * sin(PlayClock * (2 * pi::pi() / kClock));
		FStringf s("Incoming: %s", pendingTitle.GetChars());
		double bsc = 1.2 * sc;
		DrawCentered(BigFont, CategoryColor(pending->category), y, s.GetChars(), bsc, pulse);
		y += BigFont->GetHeight() * bsc + 4 * sc;
	}
	for (auto& r : running)
	{
		int len = r.effect->oneShot ? kOneShotTitle : kAnnounce;
		int age = PlayClock - r.start;
		if (age < 0 || age >= len) continue;
		double alpha = std::clamp((len - age) / (0.4 * kClock), 0., 1.); // fades out over its last 0.4 s
		double tsc = 1.5 * sc;
		DrawCentered(BigFont, CategoryColor(r.effect->category), y, r.title.GetChars(), tsc, alpha);
		y += BigFont->GetHeight() * tsc + 3 * sc;
	}

	// Bottom: the chips, by order of arrival (fading ones keep their place), then the countdown.
	std::vector<std::pair<int, Chip>> list;
	for (auto& r : running)
	{
		int age = PlayClock - r.start;
		if (r.effect->oneShot || age < kAnnounce) continue;
		double frac = r.end > r.start ? double(r.end - PlayClock) / (r.end - r.start) : 0.;
		double alpha = std::clamp((age - kAnnounce) / (0.25 * kClock), 0., 1.); // fades in as the title goes
		list.push_back({ r.seq, { FStringf("%s %ds", r.title.GetChars(), SecondsLeft(r.end - PlayClock)),
			CategoryRGB(r.effect->category), frac, alpha, false, 0 } });
	}
	for (auto& f : fading)
	{
		double alpha = 1. - double(PlayClock - f.from) / kFade;
		list.push_back({ f.seq, { FStringf("%s 0s", f.title.GetChars()), CategoryRGB(f.category), 0., alpha, false, 0 } });
	}
	std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	if (!(picked && pending))
		list.push_back({ INT_MAX, { FStringf("next %ds", SecondsLeft(nextStart - PlayClock)), PalEntry(0xff9a9a9a), 0., 0.8, true, 0 } });
	if (list.empty()) return;

	double tsc = 1.2 * sc, padX = 7 * sc, padY = 3 * sc, gap = 6 * sc;
	double chipH = SmallFont->GetHeight() * tsc + 2 * padY;
	double maxW = W * 0.9;
	std::vector<std::vector<int>> lines;
	std::vector<double> lineW;
	for (int i = 0; i < int(list.size()); i++)
	{
		Chip& c = list[i].second;
		c.w = SmallFont->StringWidth(c.text.GetChars()) * tsc + 2 * padX;
		if (lines.empty() || lineW.back() + gap + c.w > maxW)
		{
			lines.push_back({});
			lineW.push_back(-gap);
		}
		lines.back().push_back(i);
		lineW.back() += gap + c.w;
	}
	double bottom = ChipsFloor() - 5 * sc;
	for (size_t l = 0; l < lines.size(); l++)
	{
		double ly = bottom - (l + 1) * chipH - l * gap * 0.5; // the first line sits lowest; more wrap upward
		double x = (W - lineW[l]) * 0.5;
		for (int i : lines[l])
		{
			DrawChip(list[i].second, x, ly, chipH, tsc, padX, padY, sc);
			x += list[i].second.w + gap;
		}
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
		if (running[i].effect == e) StopRunning(i, false); // already running: start it over
	FString title = bmod::EffectTitle(e);
	if (!Launch(e, title.GetChars())) Printf("chaos_force: %s can't run right now\n", e->name);
}

// Every effect's chance back to its category default (Choose effects > Reset).
CCMD(chaos_chance_reset)
{
	for (int i = 0; i < bmod::NumEffects(); i++)
		if (auto cv = FindCVar(FStringf("chaos_chance_%s", bmod::GetEffect(i).name).GetChars(), nullptr)) cv->ResetToDefault();
	Printf(PRINT_NOTIFY, "Chaos: effect chances back to their defaults\n");
}

// Lists each effect's chance and its share of the picks (when every allowed effect can run).
CCMD(chaos_chances)
{
	int total = 0;
	for (int i = 0; i < bmod::NumEffects(); i++)
		if (Allowed(&bmod::GetEffect(i))) total += EffectChance(&bmod::GetEffect(i));
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		int c = Allowed(e) ? EffectChance(e) : 0;
		Printf("%-16s %2d  %5.1f%%\n", e->name, c, total > 0 ? 100. * c / total : 0.);
	}
}

END_BLD_NS
