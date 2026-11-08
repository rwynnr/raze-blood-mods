//---------------------------------------------------------------------------
//
// blood2d roguelite mode, spike 1: a run on Blood's own maps.
// Design, status and verdict: blood2d workspace, docs/roguelite.md.
//
// A run is one episode, or the gauntlet (every episode in a row). Its regular
// maps (the chain of normal exits from the episode's first map, so secret maps
// are skipped) are shuffled with the last one (the boss) kept last. Everything
// is seeded per run. Weapons, ammo, armor and the pack carry over as usual.
//
// Each level start remixes the map:
// - eligible enemies (the base's shuffle rules: no bosses, critters, statues...;
//   nor an enemy whose death ends the level) reroll to random types from a
//   threat budget that grows with depth (the stock map's total on the first
//   map, 1.5x on the last);
// - a chance growing from 5% to 25% makes each a champion: 2.5x health, 1.25x
//   size, tinted, 1.5x damage;
// - health, armor, ammo, weapon and powerup pickups reroll within their category
//   (keys, the diving suit, jump boots and other quest-like items stay);
// - from the second map on, a 25% chance that one of the base's timed effects
//   curses the whole level (announced under the map's name).
//
// Dying ends the run: a "Run over" screen, then the main menu (no reload).
// Finishing the last map wins it: "Run complete" at the menu, and the next
// episode unlocks (all unlocked: the gauntlet opens). Ichor, earned per kill
// (more for champions) and per map cleared, is banked when the run ends.
// Saving and loading within a run work (the run is a save block). A save of a
// run that has ended (died, won, or replaced by a newer run) doesn't load.
// Nothing changes outside a run.
//
// Console:
//   rl_newrun <episode|gauntlet> [seed]  start a run (episode = 1..N in the game's own list) at rl_skill
//   rl_status                            the run in progress and the meta progress
//   rl_clearmap                          end the run's current map as cleared, as if its exit was used
//   rl_resetprogress                     lock the episodes again and empty the ichor bank
//   openmenu RogueliteMenu               the Roguelite menu (it is also on the main menu)
// CVARs: rl_skill (0-4, set by the menu), rl_elite_pal (the champions' tint, a palette lookup number:
// 0-14 in Blood, 0 = none), and the meta progress, kept in the config: rl_unlocked, rl_ichor, rl_runs,
// rl_wins, rl_gauntletwon, rl_activerun (the run whose saves can still be continued, 0 = none).
//
// Hooks into stock code (one-liners): levelEndLevel (the next map), playerProcess (Use while dead),
// GameInterface::NewGame, SerializeGameState (the save block), DBloodActor::modFlags (champions, saved
// with the actor), actDamageSprite and viewProcessSprites (champion damage and tint),
// GameInterface::MenuOpened (the menu's run list), and the core main loop's return to the main menu
// (the run's result and writing the config).
//
//---------------------------------------------------------------------------

#include "ns.h"
#include <algorithm>
#include <climits>
#include <random>
#include "build.h"
#include "blood.h"
#include "c_bind.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamecontrol.h"
#include "gameconfigfile.h"
#include "gstrings.h"
#include "mapinfo.h"
#include "palette.h"
#include "razemenu.h"
#include "serializer.h"
#include "v_draw.h"
#include "v_font.h"
#include "printf.h"

extern TArray<VolumeRecord> volumes; // mapinfo.cpp: the loaded game's episodes
extern void (*gModMainMenuHook)();   // core main loop: the game has just gone back to the main menu

CUSTOM_CVARD(Int, rl_skill, 2, CVAR_ARCHIVE, "roguelite: skill of new runs, 0-4 (Blood's five skills)")
{
	if (self < 0) self = 0;
	else if (self > 4) self = 4;
}
CVARD(Int, rl_elite_pal, 10, CVAR_ARCHIVE, "roguelite: palette lookup that tints champions (0 = none; 10 cold blue, 2 or 7 red)")

// Meta progress, kept across runs.
CVARD(Int, rl_unlocked, 1, CVAR_ARCHIVE, "roguelite: episodes unlocked, in the game's order (winning an episode run unlocks the next)")
CVARD(Int, rl_ichor, 0, CVAR_ARCHIVE, "roguelite: ichor banked by the runs that have ended")
CVARD(Int, rl_runs, 0, CVAR_ARCHIVE, "roguelite: runs started (also numbers them)")
CVARD(Int, rl_wins, 0, CVAR_ARCHIVE, "roguelite: runs won")
CVARD(Bool, rl_gauntletwon, false, CVAR_ARCHIVE, "roguelite: the gauntlet has been won")
CVARD(Int, rl_activerun, 0, CVAR_ARCHIVE, "roguelite: the run whose saves can still be continued (0 = none)")

BEGIN_BLD_NS

namespace rogue
{

namespace
{

constexpr int kClock = 120;                                   // PlayClock units per second
constexpr int kIchorKill = 1, kIchorChampion = 5, kIchorMap = 10;
constexpr double kChampionFirst = 0.05, kChampionLast = 0.25; // champion chance on a run's first and last map
constexpr double kChampionHealth = 2.5, kChampionSize = 1.25;
constexpr double kBudgetGrowth = 0.5;                         // enemy threat budget: the stock map's x1 on the first map, x1.5 on the last
constexpr double kCurseChance = 0.25;                         // per map, from the second on
constexpr int kCurseTitle = 5 * kClock;                       // how long a curse is announced

struct Run
{
	bool active = false;
	bool over = false;            // the player died: the "Run over" screen shows until Use
	int id = 0;                   // the run's number; only rl_activerun can be continued from a save
	uint32_t seed = 0;
	int skill = 2;
	int episode = 0;              // 1..N in the game's episode list; 0 = the gauntlet
	TArray<FString> maps;         // the run's maps in order, by label
	int index = -1;               // the map being played
	int pending = -1;             // the map the next level start must be (a new run, an exit taken)
	bool exiting = false;         // this map's exit has been taken
	int ichor = 0, kills = 0, champions = 0, cleared = 0;
	FString curse;                // the effect cursing this map, "" = none
	int curseClock = INT_MIN / 2; // PlayClock when the curse was announced
};

Run run;
bool startingRun;    // rl_newrun asked for the game that is about to start
bool refusedLoad;    // a save of a run that has ended was loaded: back to the menu
FString menuMessage; // shown when the game is next back at the main menu
bool saveConfig;     // the meta progress changed: write the config at the main menu
int nextCurseTry;

int RandInt(std::mt19937& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
double Rand01(std::mt19937& rng) { return std::uniform_real_distribution<double>(0., 1.)(rng); }

//---------------------------------------------------------------------------
//
// episodes and the run's map order, from the loaded game's own map info
//
//---------------------------------------------------------------------------

struct Episode
{
	VolumeRecord* vol;
	FString name;
};

// The game's episodes, as the New Game menu lists them.
void GetEpisodes(TArray<Episode>& out)
{
	out.Clear();
	for (auto& vol : volumes)
	{
		if (vol.name.IsEmpty() || (vol.flags & VF_HIDEFROMSP) || !FindMapByName(vol.startmap.GetChars())) continue;
		out.Push({ &vol, FString(GStrings.localize(vol.name.GetChars())) });
	}
}

int Unlocked(int count) { return std::clamp((int)rl_unlocked, 1, std::max(count, 1)); }

// An episode's regular maps: its first map, then each map's normal exit until the episode ends.
// Maps that only a secret exit leads to are not on this chain.
void RegularMaps(const VolumeRecord* vol, TArray<MapRecord*>& out)
{
	out.Clear();
	auto map = FindMapByName(vol->startmap.GetChars());
	while (map && out.Find(map) == out.Size() && out.Size() < 64)
	{
		if (out.Size() > 0 && map->cluster != out[0]->cluster) break;
		out.Push(map);
		map = FindNextMap(map);
	}
}

// An episode run: that episode's regular maps, shuffled, the last one (the boss) kept last.
// The gauntlet: every episode in order, each shuffled the same way.
void BuildOrder(int episode, std::mt19937& rng, TArray<FString>& out)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	out.Clear();
	for (int e = 0; e < int(eps.Size()); e++)
	{
		if (episode != 0 && e + 1 != episode) continue;
		TArray<MapRecord*> maps;
		RegularMaps(eps[e].vol, maps);
		for (int i = int(maps.Size()) - 2; i > 0; i--) std::swap(maps[i], maps[RandInt(rng, 0, i)]);
		for (auto m : maps) out.Push(m->labelName);
	}
}

FString RunName(int episode)
{
	if (episode == 0) return "The gauntlet";
	TArray<Episode> eps;
	GetEpisodes(eps);
	if (episode >= 1 && episode <= int(eps.Size())) return eps[episode - 1].name;
	return FStringf("Episode %d", episode);
}

// The episode the current map belongs to (the gauntlet's HUD line).
FString CurrentEpisodeName()
{
	auto vol = currentLevel ? FindVolume(currentLevel->cluster) : nullptr;
	return vol ? FString(GStrings.localize(vol->name.GetChars())) : FString();
}

//---------------------------------------------------------------------------
//
// enemies: rerolled from a threat budget, some made champions
//
//---------------------------------------------------------------------------

struct Kind { int type, cost; };

// Every type the base's shuffle rules allow, with a rough threat cost, cheapest first.
const Kind kinds[] =
{
	{ kDudeSpiderBrown, 1 }, { kDudeTinyCaleb, 1 }, { kDudeZombieAxeNormal, 2 }, { kDudeSpiderRed, 2 },
	{ kDudeCultistTommy, 3 }, { kDudeCultistShotgun, 3 }, { kDudeZombieButcher, 3 }, { kDudeSpiderBlack, 3 },
	{ kDudeGillBeast, 3 }, { kDudeCultistTesla, 4 }, { kDudeCultistTNT, 4 }, { kDudeHellHound, 5 },
	{ kDudeGargoyleFlesh, 5 }, { kDudePhantasm, 5 }, { kDudeGargoyleStone, 8 },
};

int KindCost(int type)
{
	for (auto& k : kinds) if (k.type == type) return k.cost;
	return 3;
}

struct Look { int pal; DVector2 scale; };

void MakeChampion(DBloodActor* a)
{
	a->modFlags |= kModChampion;
	a->xspr.health = uint32_t(a->xspr.health * kChampionHealth);
	a->spr.scale *= kChampionSize;
	a->xspr.scale = int((a->xspr.scale ? a->xspr.scale : 256) * kChampionSize); // SEQ frames keep the size
}

void RemixEnemies(std::mt19937& rng, double depth)
{
	TArray<DBloodActor*> eligible, reroll;
	TMap<int, Look> looks; // how this map places each type (palette, size)
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (!bmod::ShuffleEligible(a, false)) continue;
		if (a->xspr.txID == kChannelLevelExitNormal || a->xspr.txID == kChannelLevelExitSecret) continue; // its death ends the level: a boss
		eligible.Push(a);
		if (!looks.CheckKey(a->spr.type)) looks.Insert(a->spr.type, { a->spr.pal, a->spr.scale });
		if (a->sector() && !IsUnderwaterSector(a->sector())) reroll.Push(a); // swimmers stay what they are
	}

	double budget = 0;
	for (auto a : reroll) budget += KindCost(a->spr.type);
	budget *= 1. + kBudgetGrowth * depth;

	// Tiny Caleb only where the map has one: his size comes from how the map places him.
	TArray<Kind> pool;
	for (auto& k : kinds) if (k.type != kDudeTinyCaleb || looks.CheckKey(kDudeTinyCaleb)) pool.Push(k);

	// In random order, each enemy takes a random kind it can afford from an even share of what's left
	// (up to 1.5x the share). Cheap picks leave more for later ones, so the map's total stays near the budget.
	for (int i = int(reroll.Size()) - 1; i > 0; i--) std::swap(reroll[i], reroll[RandInt(rng, 0, i)]);
	TArray<int> afford;
	for (unsigned i = 0; i < reroll.Size(); i++)
	{
		double cap = std::max(1.5 * budget / double(reroll.Size() - i), 1.);
		afford.Clear();
		for (unsigned k = 0; k < pool.Size(); k++) if (pool[k].cost <= cap) afford.Push(int(k));
		const Kind& pick = afford.Size() ? pool[afford[RandInt(rng, 0, int(afford.Size()) - 1)]] : pool[0];
		budget -= pick.cost;
		auto look = looks.CheckKey(pick.type); // the map's own look for that type, else the one Blood spawns with
		bmod::RetypeDude(reroll[i], pick.type, look ? look->pal : 0, look ? look->scale : DVector2(1, 1));
	}

	double chance = kChampionFirst + (kChampionLast - kChampionFirst) * depth;
	for (auto a : eligible) if (Rand01(rng) < chance) MakeChampion(a);
}

//---------------------------------------------------------------------------
//
// pickups: rerolled within their category, by weight (rare ones stay rare)
//
//---------------------------------------------------------------------------

struct ItemPick { int type, weight; };

const ItemPick healthPicks[] =
{
	{ kItemHealthRedPotion, 2 }, { kItemHealthLifeEssense, 4 }, { kItemHealthMedPouch, 3 },
	{ kItemHealthDoctorBag, 1 }, { kItemHealthLifeSeed, 1 },
};
const ItemPick armorPicks[] =
{
	{ kItemArmorBasic, 4 }, { kItemArmorBody, 2 }, { kItemArmorFire, 2 }, { kItemArmorSpirit, 2 }, { kItemArmorSuper, 1 },
};
const ItemPick ammoPicks[] =
{
	{ kItemAmmoSawedoffFew, 4 }, { kItemAmmoSawedoffBox, 2 }, { kItemAmmoTommygunFew, 4 }, { kItemAmmoTommygunDrum, 2 },
	{ kItemAmmoFlares, 4 }, { kItemAmmoTNTBundle, 3 }, { kItemAmmoTNTBox, 1 }, { kItemAmmoSprayCan, 2 },
	{ kItemAmmoProxBombBundle, 2 }, { kItemAmmoRemoteBombBundle, 2 }, { kItemAmmoTeslaCharge, 2 },
	{ kItemAmmoGasolineCan, 2 }, { kItemAmmoTrappedSoul, 1 }, { kItemAmmoVoodooDoll, 1 },
};
const ItemPick weaponPicks[] =
{
	{ kItemWeaponFlarePistol, 3 }, { kItemWeaponSawedoff, 3 }, { kItemWeaponTommygun, 3 },
	{ kItemWeaponNapalmLauncher, 2 }, { kItemWeaponTeslaCannon, 2 }, { kItemWeaponLifeLeech, 1 },
};
// Not the diving suit, jump boots, gas mask or asbestos armor (a level can need them to get through),
// nor the crystal ball (multiplayer only).
const ItemPick powerupPicks[] =
{
	{ kItemShadowCloak, 2 }, { kItemDeathMask, 1 }, { kItemTwoGuns, 2 }, { kItemReflectShots, 2 },
	{ kItemBeastVision, 2 }, { kItemShroomDelirium, 1 },
};

struct ItemCategory { const ItemPick* picks; int count; };

const ItemCategory itemCategories[] =
{
	{ healthPicks, int(countof(healthPicks)) },
	{ armorPicks, int(countof(armorPicks)) },
	{ ammoPicks, int(countof(ammoPicks)) },
	{ weaponPicks, int(countof(weaponPicks)) },
	{ powerupPicks, int(countof(powerupPicks)) },
};

const ItemCategory* CategoryOf(int type)
{
	for (auto& c : itemCategories)
		for (int i = 0; i < c.count; i++) if (c.picks[i].type == type) return &c;
	return nullptr;
}

// Changes a map's pickup in place (the base's rule: never delete or replace map sprites), with the new
// item's picture and size, resting where the old one did. Its X data (trigger IDs) stays.
void RetypeItem(DBloodActor* a, int type)
{
	if (a->spr.type == type) return;
	FTextureID tex;
	int xr, yr;
	if (type >= kItemWeaponBase && type < kItemWeaponMax)
	{
		auto& d = gWeaponItemData[type - kItemWeaponBase];
		tex = d.textureID(); xr = d.xrepeat; yr = d.yrepeat;
	}
	else if (type >= kItemAmmoBase && type < kItemAmmoMax)
	{
		auto& d = gAmmoItemData[type - kItemAmmoBase];
		tex = d.textureID(); xr = d.xrepeat; yr = d.yrepeat;
	}
	else if (type >= kItemBase && type < kItemMax)
	{
		auto& d = gItemData[type - kItemBase];
		tex = d.textureID(); xr = d.xrepeat; yr = d.yrepeat;
	}
	else return;
	if (!tex.isValid()) return;

	double top, bottom, newTop, newBottom;
	GetActorExtents(a, &top, &bottom);
	a->spr.type = type;
	a->spr.inittype = type;
	a->spr.setspritetexture(tex);
	a->spr.scale = DVector2(xr * REPEAT_SCALE, yr * REPEAT_SCALE);
	GetActorExtents(a, &newTop, &newBottom);
	a->spr.pos.Z += bottom - newBottom;
	a->interpolated = false;
	viewBackupSpriteLoc(a);
}

void RemixItems(std::mt19937& rng)
{
	TArray<DBloodActor*> items;
	BloodStatIterator it(kStatItem);
	while (auto a = it.Next()) items.Push(a);
	for (auto a : items)
	{
		auto cat = CategoryOf(a->spr.type);
		if (!cat) continue; // keys, flags, quest items and anything unknown stay
		int total = 0;
		for (int i = 0; i < cat->count; i++) total += cat->picks[i].weight;
		int r = RandInt(rng, 0, total - 1);
		for (int i = 0; i < cat->count; i++)
		{
			if (r < cat->picks[i].weight)
			{
				RetypeItem(a, cat->picks[i].type);
				break;
			}
			r -= cat->picks[i].weight;
		}
	}
}

//---------------------------------------------------------------------------
//
// curses: one of the base's timed effects for the whole level
//
//---------------------------------------------------------------------------

const bmod::Effect* CurseEffect() { return run.curse.IsEmpty() ? nullptr : bmod::FindEffect(run.curse.GetChars()); }

// The chaos defaults: silly effects come up most often, nasty ones least.
int CurseWeight(const bmod::Effect* e)
{
	return e->category == bmod::Category::Silly ? 6 : e->category == bmod::Category::Twist ? 4 : 2;
}

// Starts the curse unless it's on already, or a rival effect (from chaos mode) is on: then it comes back later.
bool StartCurse()
{
	auto e = CurseEffect();
	if (!e) return false;
	if (bmod::IsActive(e)) return true;
	for (int i = 0; i < bmod::NumEffects(); i++)
		if (bmod::IsActive(&bmod::GetEffect(i)) && bmod::Rivals(&bmod::GetEffect(i), e)) return false;
	return bmod::StartEffect(e);
}

void PickCurse(std::mt19937& rng)
{
	run.curse = "";
	if (run.index < 1 || Rand01(rng) >= kCurseChance) return;
	// Timed effects only: one-shots act once, and effects with a length of their own are meant to be brief.
	TArray<const bmod::Effect*> list;
	int total = 0;
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		if (e->oneShot || e->fixedSeconds > 0 || !bmod::CanStart(e)) continue;
		list.Push(e);
		total += CurseWeight(e);
	}
	if (total == 0) return;
	int r = RandInt(rng, 0, total - 1);
	for (auto e : list)
	{
		if (r < CurseWeight(e))
		{
			run.curse = e->name;
			break;
		}
		r -= CurseWeight(e);
	}
	StartCurse();
	run.curseClock = PlayClock;
}

//---------------------------------------------------------------------------
//
// the run
//
//---------------------------------------------------------------------------

void Remix()
{
	std::mt19937 rng(run.seed ^ (0x9E3779B9u * uint32_t(run.index + 1)));
	double depth = run.maps.Size() > 1 ? double(run.index) / double(run.maps.Size() - 1) : 0.;
	RemixEnemies(rng, depth);
	RemixItems(rng);
	PickCurse(rng);
}

void LevelStarted()
{
	nextCurseTry = 0;
	if (!run.active) return;
	if (run.pending < 0 || run.pending >= int(run.maps.Size()) || !currentLevel || run.maps[run.pending].CompareNoCase(currentLevel->labelName) != 0)
	{
		// Not the map the run expects (a map warp, a cheat): the run is left behind, as by a new game.
		Printf(PRINT_NOTIFY, "Roguelite: this map isn't the run's next one, the run is left\n");
		run = Run();
		return;
	}
	run.index = run.pending;
	run.pending = -1;
	run.exiting = false;
	Remix();
}

void WriteConfig()
{
	if (!GameConfig) return;
	GameConfig->ArchiveGlobalData();
	GameConfig->ArchiveGameData(currentGame.GetChars());
	GameConfig->WriteConfigFile();
}

void Bank()
{
	rl_ichor = rl_ichor + run.ichor;
	if (rl_activerun == run.id) rl_activerun = 0;
	saveConfig = true;
}

FString RunStats()
{
	return FStringf("Maps cleared: %d of %d\nKills: %d (%d champions)\nIchor: +%d (banked: %d)",
		run.cleared, int(run.maps.Size()), run.kills, run.champions, run.ichor, (int)rl_ichor);
}

void RunOver()
{
	if (!run.active || run.over) return;
	run.over = true;
	if (auto e = CurseEffect()) bmod::StopEffect(e); // a clean view behind the "Run over" screen
	Bank();
}

// The last map's exit: the run is won. Its result shows at the main menu, after the stock end-of-episode screens.
void RunWon()
{
	Bank();
	rl_wins = rl_wins + 1;
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size());
	FString news;
	if (run.episode == 0)
	{
		rl_gauntletwon = true;
		news = "The gauntlet is won!";
	}
	else if (run.episode >= Unlocked(count) && run.episode < count)
	{
		rl_unlocked = run.episode + 1;
		news.Format("Unlocked: %s", eps[run.episode].name.GetChars());
		if (run.episode + 1 >= count) news += "\nThe gauntlet is open: every episode in one run.";
	}
	menuMessage.Format("RUN COMPLETE\n\n%s\n\n%s%s%s", RunName(run.episode).GetChars(), RunStats().GetChars(),
		news.IsEmpty() ? "" : "\n\n", news.GetChars());
	run = Run();
}

bool StartRun(int episode, uint32_t seed)
{
	std::mt19937 rng(seed);
	TArray<FString> order;
	BuildOrder(episode, rng, order);
	auto first = order.Size() ? FindMapByName(order[0].GetChars()) : nullptr;
	if (!first)
	{
		Printf("rl_newrun: no maps found for this run\n");
		return false;
	}
	if (run.active && !run.over) Printf(PRINT_NOTIFY, "Roguelite: the run in progress is abandoned\n");
	run = Run();
	run.active = true;
	run.id = std::max((int)rl_runs, 0) + 1;
	rl_runs = run.id;
	rl_activerun = run.id;
	run.seed = seed;
	run.skill = rl_skill;
	run.episode = episode;
	run.maps = std::move(order);
	run.pending = 0;
	startingRun = true;
	saveConfig = true;
	Printf(PRINT_NOTIFY, "Roguelite: %s, %d maps (run %d, seed %u)\n", RunName(episode).GetChars(), int(run.maps.Size()), run.id, seed);
	M_ClearMenus();
	DeferredStartGame(first, run.skill);
	return true;
}

//---------------------------------------------------------------------------
//
// events and the ticker
//
//---------------------------------------------------------------------------

void OnEvent(const bmod::EventInfo& ev)
{
	switch (ev.type)
	{
	case bmod::Event::LevelStart:
		LevelStarted();
		break;
	case bmod::Event::SaveLoaded:
		nextCurseTry = 0;
		if (run.active && !run.over) StartCurse(); // effects aren't saved: the curse comes back on
		break;
	case bmod::Event::Kill:
		if (run.active && !run.over && AllowedKillType(ev.actor))
		{
			bool champion = (ev.actor->modFlags & kModChampion) != 0;
			run.kills++;
			if (champion) run.champions++;
			run.ichor += champion ? kIchorChampion : kIchorKill;
		}
		break;
	case bmod::Event::PlayerDeath:
		if (ev.player == getPlayer(myconnectindex)) RunOver();
		break;
	default:
		break;
	}
}

void OnTick()
{
	if (refusedLoad)
	{
		refusedLoad = false;
		menuMessage = "THAT RUN IS OVER\n\nA run ends when you die, when you win it,\nor when you start another one.\n\nStart a new run from the Roguelite menu.";
		gameaction = ga_mainmenu;
		return;
	}
	if (!run.active || run.over || run.curse.IsEmpty()) return;
	if (PlayClock >= nextCurseTry || PlayClock < nextCurseTry - kClock)
	{
		nextCurseTry = PlayClock + kClock;
		StartCurse(); // back on once a rival effect from chaos mode has ended
	}
}

void OnMainMenu()
{
	if (run.over) run = Run();
	if (saveConfig)
	{
		saveConfig = false;
		WriteConfig(); // effects are off by now (level teardown), so nothing they flip gets saved
	}
	if (menuMessage.IsNotEmpty())
	{
		FString msg = menuMessage;
		menuMessage = "";
		M_StartMessage(msg.GetChars(), 1);
	}
}

//---------------------------------------------------------------------------
//
// drawing: the run line (top right), the curse's announcement (under the
// map's name), and the "Run over" screen
//
//---------------------------------------------------------------------------

void DrawCentered(FFont* font, int color, double y, const char* text, double scale, double alpha = 1.)
{
	double w = font->StringWidth(text) * scale;
	DrawText(twod, font, color, (twod->GetWidth() - w) * 0.5, y, text, DTA_ScaleX, scale, DTA_ScaleY, scale, DTA_Alpha, alpha, TAG_DONE);
}

void DrawRunLine(double sc)
{
	double tsc = 1.2 * sc, x = twod->GetWidth() - 10 * sc, y = 8 * sc;
	double lh = SmallFont->GetHeight() * tsc + 2 * sc;
	auto line = [&](const char* text, int color)
	{
		DrawText(twod, SmallFont, color, x - SmallFont->StringWidth(text) * tsc, y, text, DTA_ScaleX, tsc, DTA_ScaleY, tsc, TAG_DONE);
		y += lh;
	};
	FString name = run.episode ? RunName(run.episode) : FString("The gauntlet: ") + CurrentEpisodeName();
	line(name.GetChars(), CR_GOLD);
	line(FStringf("Map %d/%d   Ichor %d", run.index + 1, int(run.maps.Size()), run.ichor).GetChars(), CR_WHITE);
	if (auto e = CurseEffect()) line(FStringf("Curse: %s", bmod::EffectTitle(e)).GetChars(), CR_RED);
}

// In the 320x200 space the map's name uses (Blood draws it at y 50), just under it.
void DrawCurseTitle()
{
	auto e = CurseEffect();
	int age = PlayClock - run.curseClock;
	if (!e || age < 0 || age >= kCurseTitle) return;
	double alpha = std::clamp((kCurseTitle - age) / (0.5 * kClock), 0., 1.);
	FStringf title("Curse: %s", bmod::EffectTitle(e));
	double w = BigFont->StringWidth(title.GetChars());
	double s = w > 300 ? 300 / w : 1.;
	double y = 50 + BigFont->GetHeight() + 6;
	DrawText(twod, BigFont, CR_RED, 160 - w * s / 2, y, title.GetChars(), DTA_FullscreenScale, FSMode_Fit320x200,
		DTA_ScaleX, s, DTA_ScaleY, s, DTA_Alpha, alpha, TAG_DONE);
	const char* sub = "for the whole level";
	DrawText(twod, SmallFont, CR_GRAY, 160 - SmallFont->StringWidth(sub) / 2., y + BigFont->GetHeight() * s + 3, sub,
		DTA_FullscreenScale, FSMode_Fit320x200, DTA_Alpha, alpha, TAG_DONE);
}

void DrawRunOver(DBloodPlayer* pPlayer, double sc)
{
	auto actor = pPlayer ? pPlayer->GetActor() : nullptr;
	double fade = pPlayer ? std::clamp(pPlayer->deathTime / 90., 0., 1.) : 1.;
	double H = twod->GetHeight();
	Dim(twod, 0xff000000, float(0.8 * fade), 0, 0, twod->GetWidth(), twod->GetHeight());

	double y = H * 0.22, bsc = 2 * sc;
	DrawCentered(BigFont, CR_RED, y, "RUN OVER", bsc, fade);
	y += BigFont->GetHeight() * bsc + 16 * sc;
	double ssc = 1.6 * sc, lh = SmallFont->GetHeight() * ssc + 6 * sc;
	DrawCentered(SmallFont, CR_GOLD, y, RunName(run.episode).GetChars(), ssc, fade);
	y += lh * 1.5;
	TArray<FString> lines;
	RunStats().Split(lines, "\n");
	for (auto& l : lines)
	{
		DrawCentered(SmallFont, CR_WHITE, y, l.GetChars(), ssc, fade);
		y += lh;
	}

	if (!actor || seqGetStatus(actor) >= 0) return; // Use does nothing until the death animation is over
	auto keys = Bindings.GetKeysForCommand("+open");
	FString keyName = keys.Size() ? C_NameKeys(keys.Data(), std::min<int>(keys.Size(), 2)) : FString("Use");
	FStringf prompt("Press %s to return to the main menu", keyName.GetChars());
	bool blink = (PlayClock / 30) & 1;
	DrawCentered(SmallFont, blink ? CR_WHITE : CR_GOLD, y + lh, prompt.GetChars(), ssc);
}

void OnDraw(DBloodPlayer* pPlayer)
{
	if (!run.active) return;
	double sc = twod->GetHeight() / 480.;
	if (run.over)
	{
		DrawRunOver(pPlayer, sc);
		return;
	}
	if (bmod::mods.hideHud) return; // the "No HUD" effect hides this too
	DrawRunLine(sc);
	DrawCurseTitle();
}

//---------------------------------------------------------------------------
//
// the Roguelite menu: menudef has the title and the skill; the runs are added
// here whenever a menu opens, so locks and the bank are current
//
//---------------------------------------------------------------------------

DMenuDescriptor* menuDesc;        // the menu the items below were added to (compared only)
TArray<DMenuItemBase*> menuItems; // the items added here

void BuildMenu()
{
	auto desc = MenuDescriptors.CheckKey(FName("RogueliteMenu"));
	if (!desc || !*desc || !(*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return;
	auto opt = static_cast<DOptionMenuDescriptor*>(*desc);
	if (opt != menuDesc)
	{
		menuDesc = opt; // the menus were (re)loaded: nothing of ours is in this one
		menuItems.Clear();
	}
	for (unsigned i = opt->mItems.Size(); i-- > 0; )
		if (menuItems.Find(opt->mItems[i]) < menuItems.Size()) opt->mItems.Delete(i);
	menuItems.Clear();

	auto add = [&](DMenuItemBase* item)
	{
		if (!item) return;
		GC::WriteBarrier(opt, item);
		opt->mItems.Push(item);
		menuItems.Push(item);
	};
	auto text = [&](const char* s, int color) { add(CreateOptionMenuItemStaticText(s, color < 0 ? -1 : 0x12340000 | color)); };
	auto header = [&](const char* s) { add(CreateOptionMenuItemStaticText(s, 1)); };

	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size()), unlocked = Unlocked(count);
	text(FStringf("Ichor banked: %d", (int)rl_ichor).GetChars(), CR_GOLD);
	text(FStringf("Runs: %d   Won: %d", (int)rl_runs, (int)rl_wins).GetChars(), -1);
	if (run.active && !run.over && gamestate == GS_LEVEL)
		text(FStringf("Playing: %s, map %d of %d", RunName(run.episode).GetChars(), run.index + 1, int(run.maps.Size())).GetChars(), CR_WHITE);
	else if (rl_activerun != 0)
		text("A run is in progress: load one of its saves to go on", -1);
	text("", -1);

	header("Episode runs");
	int first = -1;
	for (int e = 0; e < count; e++)
	{
		if (e < unlocked)
		{
			if (first < 0) first = int(opt->mItems.Size());
			add(CreateOptionMenuItemCommand(eps[e].name.GetChars(), FName(FStringf("rl_newrun %d", e + 1).GetChars())));
		}
		else text(FStringf("%s  (locked)", eps[e].name.GetChars()).GetChars(), CR_DARKGRAY);
	}
	if (count == 0) text("No episodes found", CR_DARKGRAY);
	text("", -1);
	header("The gauntlet");
	if (count > 0 && unlocked >= count)
		add(CreateOptionMenuItemCommand(rl_gauntletwon ? "Every episode in one run (won)" : "Every episode in one run", FName("rl_newrun gauntlet")));
	else text("Unlocks once every episode is unlocked", CR_DARKGRAY);
	text("", -1);
	text("Winning an episode run unlocks the next one.", -1);
	text("Dying ends the run. Starting a new run abandons the old one.", -1);
	opt->mSelectedItem = first;
	opt->mScrollPos = 0;
}

struct Registrar
{
	Registrar()
	{
		bmod::AddListener(OnEvent);
		bmod::AddTicker(OnTick);
		bmod::AddDrawer(OnDraw);
		::gModMainMenuHook = [] { OnMainMenu(); };
	}
} registrar;

} // namespace

//---------------------------------------------------------------------------
//
// hooks called from stock code (roguelite.h)
//
//---------------------------------------------------------------------------

MapRecord* NextLevel(MapRecord* stockNext)
{
	if (!run.active || run.over || run.index < 0) return stockNext;
	if (run.exiting) // a second exit in the same tic: the same answer
		return run.pending >= 0 && run.pending < int(run.maps.Size()) ? FindMapByName(run.maps[run.pending].GetChars()) : nullptr;
	run.exiting = true;
	run.cleared++;
	run.ichor += kIchorMap;
	int next = run.index + 1; // a secret exit leads on to the next map too (secret maps aren't in v1)
	auto map = next < int(run.maps.Size()) ? FindMapByName(run.maps[next].GetChars()) : nullptr;
	if (!map)
	{
		RunWon();
		return nullptr; // the stock end of the episode (and the game) follows
	}
	run.pending = next;
	return map;
}

bool OnDeathRestart()
{
	if (!run.active || !run.over) return false;
	gameaction = ga_mainmenu; // no reload: the run is over
	return true;
}

void OnNewGame()
{
	if (startingRun)
	{
		startingRun = false;
		return;
	}
	run = Run(); // a game outside the run; the run, if any, can still be continued from its saves
}

int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage)
{
	if (!source || !target || damage <= 0) return damage;
	auto s = source;
	for (int i = 0; i < 2 && s && !s->IsDudeActor(); i++) s = s->GetOwner(); // a missile's or explosion's owner
	if (!s || s == target || !(s->modFlags & kModChampion)) return damage;
	return damage + damage / 2;
}

void TintSprite(DBloodActor* owner, tspritetype* tspr)
{
	if (!owner || !tspr || !(owner->modFlags & kModChampion)) return;
	int pal = rl_elite_pal;
	if (pal > 0 && pal < MAXREALPAL && lookups.checkTable(pal)) tspr->pal = pal;
	tspr->shade = int8_t(std::max(tspr->shade - 12, -128)); // and a little brighter
}

void Serialize(FSerializer& arc)
{
	if (arc.isWriting())
	{
		if (!run.active || run.over) return;
		if (arc.BeginObject("roguelite"))
		{
			arc("id", run.id)
				("seed", run.seed)
				("skill", run.skill)
				("episode", run.episode)
				("maps", run.maps)
				("index", run.index)
				("ichor", run.ichor)
				("kills", run.kills)
				("champions", run.champions)
				("cleared", run.cleared)
				("curse", run.curse);
			arc.EndObject();
		}
		return;
	}

	// Reading: the save's run, if it has one that can still go on.
	startingRun = false;
	refusedLoad = false;
	Run loaded;
	bool has = false;
	if (arc.BeginObject("roguelite"))
	{
		arc("id", loaded.id)
			("seed", loaded.seed)
			("skill", loaded.skill)
			("episode", loaded.episode)
			("maps", loaded.maps)
			("index", loaded.index)
			("ichor", loaded.ichor)
			("kills", loaded.kills)
			("champions", loaded.champions)
			("cleared", loaded.cleared)
			("curse", loaded.curse);
		arc.EndObject();
		has = true;
	}
	run = Run();
	if (!has) return; // a save outside any run: stock Blood
	if (loaded.id <= 0 || loaded.id != rl_activerun || loaded.index < 0 || loaded.index >= int(loaded.maps.Size()))
	{
		refusedLoad = true; // that run has ended: the ticker sends the game back to the menu
		return;
	}
	run = std::move(loaded);
	run.active = true;
	run.curseClock = INT_MIN / 2;
}

void OnMenuOpened()
{
	BuildMenu();
}

//---------------------------------------------------------------------------
//
// console
//
//---------------------------------------------------------------------------

CCMD(rl_newrun)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size()), unlocked = Unlocked(count);
	if (argv.argc() < 2)
	{
		Printf("usage: rl_newrun <episode|gauntlet> [seed]   (skill: rl_skill)\n");
		for (int e = 0; e < count; e++) Printf("  %d  %s%s\n", e + 1, eps[e].name.GetChars(), e < unlocked ? "" : " (locked)");
		Printf("  gauntlet  every episode in one run%s\n", count > 0 && unlocked >= count ? "" : " (locked)");
		return;
	}
	if (!isBlood() || numplayers > 1)
	{
		Printf("rl_newrun: Blood single player only\n");
		return;
	}
	int episode = !stricmp(argv[1], "gauntlet") ? 0 : atoi(argv[1]);
	if (count == 0 || episode < 0 || episode > count || (episode == 0 && stricmp(argv[1], "gauntlet")))
	{
		Printf("rl_newrun: no episode '%s' (1-%d or gauntlet)\n", argv[1], count);
		return;
	}
	if (episode == 0 && unlocked < count)
	{
		Printf(PRINT_NOTIFY, "The gauntlet opens once every episode is unlocked\n");
		return;
	}
	if (episode > unlocked)
	{
		Printf(PRINT_NOTIFY, "%s is locked: win a run of %s first\n", eps[episode - 1].name.GetChars(), eps[episode - 2].name.GetChars());
		return;
	}
	uint32_t seed = argv.argc() >= 3 ? uint32_t(strtoul(argv[2], nullptr, 10)) : uint32_t(std::random_device{}());
	StartRun(episode, seed);
}

CCMD(rl_status)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size()), unlocked = Unlocked(count);
	if (run.active)
	{
		auto curse = CurseEffect();
		Printf("Run %d: %s, map %d of %d, skill %d, seed %u%s\n", run.id, RunName(run.episode).GetChars(), run.index + 1,
			int(run.maps.Size()), run.skill, run.seed, run.over ? " (over)" : "");
		Printf("  ichor %d, kills %d (%d champions), maps cleared %d, curse: %s\n", run.ichor, run.kills, run.champions,
			run.cleared, curse ? bmod::EffectTitle(curse) : "none");
		FString order;
		for (int i = 0; i < int(run.maps.Size()); i++)
			order.AppendFormat("%s%s%s%s", i ? " " : "", i == run.index ? "[" : "", run.maps[i].GetChars(), i == run.index ? "]" : "");
		Printf("  maps: %s\n", order.GetChars());
	}
	else Printf("No run in progress\n");
	FString list;
	for (int e = 0; e < count; e++) list.AppendFormat("%s%s%s", e ? ", " : "", eps[e].name.GetChars(), e < unlocked ? "" : " (locked)");
	Printf("Episodes: %s\n", count ? list.GetChars() : "none found");
	Printf("Gauntlet: %s. Ichor banked: %d. Runs: %d, won: %d. Run that can be continued: %d\n",
		rl_gauntletwon ? "won" : count > 0 && unlocked >= count ? "open" : "locked", (int)rl_ichor, (int)rl_runs, (int)rl_wins, (int)rl_activerun);
}

CCMD(rl_clearmap)
{
	if (!run.active || run.over || run.index < 0 || gamestate != GS_LEVEL)
	{
		Printf("rl_clearmap: no run in progress\n");
		return;
	}
	levelEndLevel(0); // as if the map's exit was used
}

CCMD(rl_resetprogress)
{
	// rl_runs keeps counting, so an old run's saves never match a new run's number.
	rl_unlocked = 1;
	rl_ichor = 0;
	rl_wins = 0;
	rl_gauntletwon = false;
	rl_activerun = 0;
	saveConfig = true;
	Printf(PRINT_NOTIFY, "Roguelite: progress reset (episodes locked, ichor bank empty)\n");
}

} // namespace rogue

END_BLD_NS
