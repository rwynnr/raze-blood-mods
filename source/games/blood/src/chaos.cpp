//---------------------------------------------------------------------------
//
// blood2d chaos mode: every so often, a random temporary effect from the mod
// base's effect library (bloodmod.h). One effect at a time, announced by a
// banner; design in the blood2d workspace, docs/chaos.md.
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

CVARD(Bool, chaos, false, 0, "chaos mode: random temporary effects (toggle with chaos_toggle, bound to N)")
CVARD(Int, chaos_interval_min, 30, CVAR_ARCHIVE, "chaos: shortest wait between effects, seconds")
CVARD(Int, chaos_interval_max, 60, CVAR_ARCHIVE, "chaos: longest wait between effects, seconds")
CVARD(Int, chaos_duration_min, 15, CVAR_ARCHIVE, "chaos: shortest effect duration, seconds")
CVARD(Int, chaos_duration_max, 25, CVAR_ARCHIVE, "chaos: longest effect duration, seconds")
CVARD(Int, chaos_warning, 3, CVAR_ARCHIVE, "chaos: seconds of warning before an effect starts")
CVARD(Int, chaos_firstwait, 10, CVAR_ARCHIVE, "chaos: wait before the first effect after turning chaos on, seconds")
CVARD(String, chaos_exclude, "", CVAR_ARCHIVE, "chaos: effects never to pick, separated by spaces (names from mod_effects)")

BEGIN_BLD_NS

namespace
{

enum class Phase { Off, Waiting, Warning, Active, OneShot };

constexpr int kClock = 120; // PlayClock units per second

Phase phase = Phase::Off;
int phaseStart, phaseEnd;        // PlayClock
const bmod::Effect* current;     // effect being warned about / running
const bmod::Effect* lastEffect;  // never pick the same one twice in a row
FString currentTitle;

std::mt19937& Rng() { static std::mt19937 rng(std::random_device{}()); return rng; }

int RandRange(int lo, int hi)
{
	if (hi < lo) std::swap(lo, hi);
	return std::uniform_int_distribution<int>(lo, hi)(Rng());
}

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

const bmod::Effect* PickEffect()
{
	// Pick a category by weight (silly 60 / twist 30 / nasty 10), then an effect within it.
	TArray<const bmod::Effect*> byCat[3];
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		if (e == lastEffect || Excluded(e->name) || !bmod::CanStart(e)) continue;
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

void EndCurrent()
{
	if (current && bmod::IsActive(current)) bmod::StopEffect(current);
	current = nullptr;
}

void ScheduleNext(int seconds)
{
	EndCurrent();
	phase = Phase::Waiting;
	phaseStart = PlayClock;
	phaseEnd = PlayClock + std::max(seconds, 1) * kClock;
}

void ScheduleNext() { ScheduleNext(RandRange(chaos_interval_min, chaos_interval_max)); }

bool LocalPlayerAlive()
{
	auto pPlayer = getPlayer(myconnectindex);
	return pPlayer && pPlayer->GetActor() && pPlayer->GetActor()->xspr.health > 0;
}

// Starts `e` now (no warning). Returns false if it can't run.
bool Launch(const bmod::Effect* e, const char* title)
{
	if (!bmod::StartEffect(e)) return false;
	current = e;
	lastEffect = e;
	currentTitle = title;
	phaseStart = PlayClock;
	if (e->oneShot)
	{
		phase = Phase::OneShot;
		phaseEnd = PlayClock + 3 * kClock; // just show the banner briefly
	}
	else
	{
		int secs = e->fixedSeconds > 0 ? e->fixedSeconds : RandRange(chaos_duration_min, chaos_duration_max);
		phase = Phase::Active;
		phaseEnd = PlayClock + std::max(secs, 1) * kClock;
	}
	auto pPlayer = getPlayer(myconnectindex);
	if (pPlayer && pPlayer->GetActor()) sfxPlay3DSound(pPlayer->GetActor(), 776, -1, 0); // powerup chime
	return true;
}

void ChaosTick()
{
	if (!chaos)
	{
		if (phase != Phase::Off) { EndCurrent(); phase = Phase::Off; }
		return;
	}
	if (phase == Phase::Off) ScheduleNext(chaos_firstwait);
	if (!LocalPlayerAlive()) return; // death ends effects (listener); wait for the reload/respawn

	const bool mapOpen = automapMode != am_off; // nothing starts while the automap is up
	switch (phase)
	{
	case Phase::Waiting:
		if (PlayClock >= phaseEnd && !mapOpen)
		{
			current = PickEffect();
			if (!current) { ScheduleNext(); break; }
			currentTitle = bmod::EffectTitle(current);
			phase = Phase::Warning;
			phaseStart = PlayClock;
			phaseEnd = PlayClock + std::max((int)chaos_warning, 0) * kClock;
		}
		break;

	case Phase::Warning:
		if (PlayClock >= phaseEnd && !mapOpen)
		{
			auto e = current;
			current = nullptr;
			FString title = currentTitle;
			if (!Launch(e, title.GetChars())) ScheduleNext(); // became unavailable during the warning
		}
		break;

	case Phase::Active:
	case Phase::OneShot:
		if (PlayClock >= phaseEnd) ScheduleNext();
		break;

	default:
		break;
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
		if (chaos && phase != Phase::Off) ScheduleNext();
		else { EndCurrent(); phase = Phase::Off; }
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
	if (!chaos || phase == Phase::Off) return;
	double sc = twod->GetHeight() / 480.;
	double top = 10 * sc;
	int remain = std::max(phaseEnd - PlayClock, 0);
	switch (phase)
	{
	case Phase::Waiting:
	{
		FStringf s("Chaos: next effect in %d s", (remain + kClock - 1) / kClock);
		DrawCentered(SmallFont, CR_WHITE, top, s.GetChars(), 1.5 * sc, 0.7);
		break;
	}
	case Phase::Warning:
	{
		// Gentle 1 Hz pulse; nothing on screen flashes faster than that.
		double pulse = 0.75 + 0.25 * sin(PlayClock * (2 * pi::pi() / kClock));
		FStringf s("Incoming: %s", currentTitle.GetChars());
		DrawCentered(BigFont, current ? CategoryColor(current->category) : CR_WHITE, top, s.GetChars(), 1.2 * sc, pulse);
		break;
	}
	case Phase::Active:
	case Phase::OneShot:
	{
		int color = lastEffect ? CategoryColor(lastEffect->category) : CR_WHITE;
		double bsc = 1.4 * sc;
		DrawCentered(BigFont, color, top, currentTitle.GetChars(), bsc);
		if (phase == Phase::Active)
		{
			double barW = 200 * sc, barH = 6 * sc;
			double x = (twod->GetWidth() - barW) * 0.5, y = top + BigFont->GetHeight() * bsc + 4 * sc;
			double frac = phaseEnd > phaseStart ? double(remain) / (phaseEnd - phaseStart) : 0.;
			Dim(twod, 0xff000000, 0.6f, int(x), int(y), int(barW), int(barH));
			int fw = int((barW - 2) * std::clamp(frac, 0., 1.));
			if (fw > 0) Dim(twod, 0xffe0e0e0, 0.9f, int(x + 1), int(y + 1), fw, int(barH - 2));
		}
		break;
	}
	default:
		break;
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

} // namespace

CCMD(chaos_toggle)
{
	chaos = !chaos;
	Printf(PRINT_NOTIFY, "Chaos %s\n", chaos ? "ON" : "OFF");
}

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
	EndCurrent();
	FString title = bmod::EffectTitle(e);
	if (!bmod::CanStart(e) || !Launch(e, title.GetChars()))
	{
		Printf("chaos_force: %s can't run right now\n", e->name);
		ScheduleNext();
	}
}

END_BLD_NS
