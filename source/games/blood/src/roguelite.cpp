//---------------------------------------------------------------------------
//
// blood2d roguelite mode, spike 1: a run on Blood's own maps.
// Design, status and verdict: blood2d workspace, docs/roguelite.md.
//
// A run is one episode, or the gauntlet (every episode in a row). Its regular
// maps (the chain of normal exits from the episode's first map, so secret maps
// are skipped) are shuffled with the last one (the boss) kept last. Everything
// is seeded per run. Weapons, ammo, armor and the pack carry over as usual.
// An episode's opening movie plays when the run enters the episode, whichever
// map comes first, and not again when the episode's first map comes up later.
//
// Each level start remixes the map:
// - eligible enemies (the base's shuffle rules: no bosses, critters, statues...;
//   nor an enemy wired into the map's script, such as Cheogh on E1M7) reroll to
//   random types from a threat budget that grows with depth (the stock map's
//   total on the first map, 1.5x on the last); key carriers keep their kind;
// - a chance growing from 5% to 25% makes each a champion: 2.5x health, 1.25x
//   size, tinted, 1.5x damage;
// - health, armor, ammo, explosive, weapon and powerup pickups reroll within
//   their category (keys, the diving suit, jump boots and other quest-like
//   items stay, as do items wired into the map's script);
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
//   rl_unlock <count|all>                set how many of this game's episodes are unlocked (testing)
//   rl_resetprogress                     lock this game's episodes again; empty the ichor bank and the wins
//   openmenu RogueliteMenu               the Roguelite menu (it is also on the main menu)
// CVARs: rl_skill (0-4, set by the menu), rl_elite_pal (the champions' tint, a palette lookup number:
// 0-14 in Blood, 0 = none), and the meta progress, kept in the config: rl_progress (per game, since
// Blood and Cryptic Passage share a config: episodes unlocked, the gauntlet won, the run whose saves can
// still be continued) and, shared by both, rl_ichor, rl_runs, rl_wins.
//
// Hooks into stock code (one-liners): levelEndLevel (the next map), playerProcess (Use while dead),
// GameInterface::NewGame, SerializeGameState (the save block), DBloodActor::modFlags (champions, saved
// with the actor), actDamageSprite and viewProcessSprites (champion damage and tint),
// GameInterface::MenuOpened (the menu's run list), the core main loop's return to the main menu
// (the run's result and writing the config), and the core intermission's choice of a map's intro movie.
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
#include "types.h"
#include "vm.h"
#include "printf.h"

extern TArray<VolumeRecord> volumes; // mapinfo.cpp: the loaded game's episodes
extern void (*gModMainMenuHook)();   // core main loop: the game has just gone back to the main menu
extern CutsceneDef* (*gModMapIntroHook)(MapRecord* map, CutsceneDef* stock); // core intermission: the movie entering a map plays

CUSTOM_CVARD(Int, rl_skill, 2, CVAR_ARCHIVE, "roguelite: skill of new runs, 0-4 (Blood's five skills)")
{
	if (self < 0) self = 0;
	else if (self > 4) self = 4;
}
// Of Blood's 15 palette lookups only 2 (BEAST, red), 5 (GRAY), 6 (GRAYISH), 9 (FLAME, orange) and 10 (COLD, blue)
// recolour a whole sprite; the others recolour a few colour ranges (robes, spiders, players). Blue stands out most
// against Blood's warm colours, and no stock enemy wears it (stone gargoyles wear GRAY).
CVARD(Int, rl_elite_pal, 10, CVAR_ARCHIVE, "roguelite: palette lookup that tints champions (0 = none; 10 blue, 2 red, 9 orange)")

// Meta progress, kept across runs. Blood and Cryptic Passage share a config, so what belongs to one game's episodes
// is kept per game in rl_progress; the ichor bank and the run counts are shared.
CVARD(String, rl_progress, "", CVAR_ARCHIVE, "roguelite: per game, '<game>=<episodes unlocked>,<gauntlet won>,<run that can be continued>;...'")
CVARD(Int, rl_ichor, 0, CVAR_ARCHIVE, "roguelite: ichor banked by the runs that have ended")
CVARD(Int, rl_runs, 0, CVAR_ARCHIVE, "roguelite: runs started (also numbers them)")
CVARD(Int, rl_wins, 0, CVAR_ARCHIVE, "roguelite: runs won")

// The Armory (a starting kit bought with points) and the Covenant (opt-in difficulty conditions, after Hades'
// Pact of Punishment) live in the config, so both are remembered from one run to the next. Their menus are in
// menudef ("RogueliteArmory", "RogueliteCovenant"); a run takes a copy of both when it starts.
BEGIN_BLD_NS
namespace rogue { void KitChanged(); void CovenantChanged(); }
END_BLD_NS
#define RL_KIT(name, maxUnits) CUSTOM_CVARD(Int, name, 0, CVAR_ARCHIVE, "roguelite Armory: how many of this the kit buys") \
	{ if (self < 0) self = 0; else if (self > maxUnits) self = maxUnits; else BLD_NS::rogue::KitChanged(); }
RL_KIT(rl_kit_flare, 1)
RL_KIT(rl_kit_shotgun, 1)
RL_KIT(rl_kit_spray, 1)
RL_KIT(rl_kit_tommy, 1)
RL_KIT(rl_kit_voodoo, 1)
RL_KIT(rl_kit_napalm, 1)
RL_KIT(rl_kit_tesla, 1)
RL_KIT(rl_kit_leech, 1)
RL_KIT(rl_kit_tnt, 3)
RL_KIT(rl_kit_prox, 3)
RL_KIT(rl_kit_remote, 3)
RL_KIT(rl_kit_shells, 3)
RL_KIT(rl_kit_bullets, 3)
RL_KIT(rl_kit_flares, 3)
RL_KIT(rl_kit_gas, 3)
RL_KIT(rl_kit_charge, 3)
RL_KIT(rl_kit_souls, 3)
RL_KIT(rl_kit_body, 1)
RL_KIT(rl_kit_fire, 1)
RL_KIT(rl_kit_spirit, 1)
RL_KIT(rl_kit_super, 1)
RL_KIT(rl_kit_docbag, 1)
RL_KIT(rl_kit_beast, 1)
RL_KIT(rl_kit_boots, 1)
RL_KIT(rl_kit_diving, 1)
#define RL_COV(name, maxRanks) CUSTOM_CVARD(Int, name, 0, CVAR_ARCHIVE, "roguelite Covenant: ranks of this condition") \
	{ if (self < 0) self = 0; else if (self > maxRanks) self = maxRanks; else BLD_NS::rogue::CovenantChanged(); }
RL_COV(rl_cov_wrath, 5)
RL_COV(rl_cov_flesh, 4)
RL_COV(rl_cov_haste, 2)
RL_COV(rl_cov_horde, 3)
RL_COV(rl_cov_champions, 2)
RL_COV(rl_cov_curses, 3)
RL_COV(rl_cov_famine, 4)
RL_COV(rl_cov_austerity, 3)
RL_COV(rl_cov_hourglass, 1)
RL_COV(rl_cov_wardens, 1)

BEGIN_BLD_NS

namespace rogue
{

namespace
{

constexpr int kClock = 120;                                   // PlayClock units per second
constexpr int kIchorKill = 1, kIchorChampion = 5, kIchorMap = 10;
// Spike 2's gentler base: the threat budget stays at each map's stock total, champions are rarer and softer, and
// curses come only with the Covenant's Curses. The Covenant's conditions make it harder from there.
constexpr double kChampionFirst = 0.04, kChampionLast = 0.12; // champion chance on a run's first and last map
constexpr double kChampionHealth = 2.0, kChampionSize = 1.25; // and 1.25x damage (ScaleDamage)
constexpr double kCurseChance = 0.25;                         // per map, from the second on, with the Covenant's Curses
constexpr int kCurseTitle = 5 * kClock;                       // how long a curse is announced
constexpr int kSkipIchor = 10;                                // the reward choice's "nothing" pays this

//---------------------------------------------------------------------------
//
// the Armory: a starting kit bought with points before a run
//
//---------------------------------------------------------------------------

struct KitItem
{
	FIntCVarRef* cvar; // how many are bought
	const char* name;
	int item;         // the item handed over, as if picked up (its stock pickup amounts)
	int count;        // how many of that item one purchase is
	int cost;         // points per purchase
};

const KitItem kitItems[] =
{
	{ &rl_kit_flare, "Flare gun", kItemWeaponFlarePistol, 1, 10 },
	{ &rl_kit_shotgun, "Sawed-off shotgun", kItemWeaponSawedoff, 1, 20 },
	{ &rl_kit_spray, "Spray can", kItemAmmoSprayCan, 1, 15 },
	{ &rl_kit_tommy, "Tommy gun", kItemWeaponTommygun, 1, 30 },
	{ &rl_kit_voodoo, "Voodoo doll", kItemAmmoVoodooDoll, 1, 35 },
	{ &rl_kit_napalm, "Napalm launcher", kItemWeaponNapalmLauncher, 1, 40 },
	{ &rl_kit_tesla, "Tesla cannon", kItemWeaponTeslaCannon, 1, 45 },
	{ &rl_kit_leech, "Life leech", kItemWeaponLifeLeech, 1, 60 },
	{ &rl_kit_tnt, "TNT (5 sticks)", kItemAmmoTNTBox, 1, 15 },
	{ &rl_kit_prox, "Proximity bombs (3)", kItemAmmoProxBombBundle, 3, 15 },
	{ &rl_kit_remote, "Remote bombs (3)", kItemAmmoRemoteBombBundle, 3, 15 },
	{ &rl_kit_shells, "Shells (15)", kItemAmmoSawedoffBox, 1, 5 },
	{ &rl_kit_bullets, "Bullets (100)", kItemAmmoTommygunDrum, 1, 8 },
	{ &rl_kit_flares, "Flares (8)", kItemAmmoFlares, 1, 5 },
	{ &rl_kit_gas, "Gasoline (6)", kItemAmmoGasolineCan, 1, 10 },
	{ &rl_kit_charge, "Tesla charge (32)", kItemAmmoTeslaCharge, 1, 10 },
	{ &rl_kit_souls, "Trapped souls (10)", kItemAmmoTrappedSoul, 1, 10 },
	{ &rl_kit_body, "Body armour", kItemArmorBody, 1, 20 },
	{ &rl_kit_fire, "Fire armour", kItemArmorFire, 1, 15 },
	{ &rl_kit_spirit, "Spirit armour", kItemArmorSpirit, 1, 15 },
	{ &rl_kit_super, "Super armour", kItemArmorSuper, 1, 50 },
	{ &rl_kit_docbag, "Doctor's bag", kItemHealthDoctorBag, 1, 25 },
	{ &rl_kit_beast, "Beast vision", kItemBeastVision, 1, 10 },
	{ &rl_kit_boots, "Jump boots", kItemJumpBoots, 1, 10 },
	{ &rl_kit_diving, "Diving suit", kItemDivingSuit, 1, 10 },
};
constexpr int kNumKit = int(countof(kitItems));

//---------------------------------------------------------------------------
//
// the Covenant: opt-in conditions with ranks (Hades' Pact of Punishment).
// Each rank adds its Dread; each point of Dread pays +10% ichor.
//
//---------------------------------------------------------------------------

enum { kWrath, kFlesh, kHaste, kHorde, kChampions, kCurses, kFamine, kAusterity, kHourglass, kWardens, kNumConditions };

struct Condition
{
	FIntCVarRef* cvar;
	const char* name;
	int dread;        // per rank
};

const Condition conditions[kNumConditions] =
{
	{ &rl_cov_wrath, "Wrath", 1 },         // enemies deal +20% damage per rank
	{ &rl_cov_flesh, "Flesh", 1 },         // +15% enemy health per rank
	{ &rl_cov_haste, "Haste", 2 },         // enemies 15% faster per rank
	{ &rl_cov_horde, "Horde", 2 },         // +20% enemies per rank, arriving in waves
	{ &rl_cov_champions, "Champions", 2 }, // +100% champions per rank
	{ &rl_cov_curses, "Curses", 1 },       // curses on some maps; then nasty ones too; then on every map
	{ &rl_cov_famine, "Famine", 1 },       // healing heals 25% less per rank
	{ &rl_cov_austerity, "Austerity", 1 }, // 20 fewer kit points per rank
	{ &rl_cov_hourglass, "Hourglass", 3 }, // each map has a par time; past it, waves come every 30 s
	{ &rl_cov_wardens, "Wardens", 3 },     // on an episode's boss map, the boss and everything with it are champions
};
constexpr double kDreadIchor = 0.10;

struct Run
{
	bool active = false;
	bool over = false;            // the player died: the "Run over" screen shows until Use
	int id = 0;                   // the run's number; only the game's active run can be continued from a save
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
	int cov[kNumConditions] = {}; // the Covenant's ranks, as they were when the run started
	int kit[kNumKit] = {};        // the Armory's kit, as it was when the run started
	bool kitGiven = false;        // the kit is handed over on the run's first map
	int offers[3] = { -1, -1, -1 }; // the reward choice waiting on this map (reward ids), -1 = none
	bool offerPending = false;
	int levelClock = 0;           // PlayClock when this map started
	int extraEnemies = 0;         // Horde: enemies still to arrive on this map
	int nextWave = 0;             // Horde: PlayClock of the next wave
	int parTime = 0;              // Hourglass: this map's par time, in seconds (0 = none)
	int nextOvertime = 0;         // Hourglass: PlayClock of the next wave past the par time
	int unspent = 0;              // kit points not spent: ichor once the first map is cleared
};

Run run;
bool startingRun;    // rl_newrun asked for the game that is about to start
bool refusedLoad;    // a save of a run that has ended was loaded: back to the menu
FString menuMessage; // shown when the game is next back at the main menu
bool saveConfig;     // the meta progress changed: write the config at the main menu
int nextCurseTry;
bool backToCrypt;    // a run was won: back to the Crypt after the stock end of the episode

int RandInt(std::mt19937& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
double Rand01(std::mt19937& rng) { return std::uniform_real_distribution<double>(0., 1.)(rng); }

//---------------------------------------------------------------------------
//
// this game's progress: one "<game>=<unlocked>,<gauntlet won>,<active run>"
// entry per game in rl_progress (game: Raze's filter name, Blood.Blood or
// Blood.Cryptic), entries separated by ';'
//
//---------------------------------------------------------------------------

struct Progress
{
	int unlocked = 1;         // episodes unlocked, in the game's order
	bool gauntletWon = false;
	int activeRun = 0;        // the run whose saves can still be continued, 0 = none
};

Progress GetProgress()
{
	Progress p;
	auto entries = FString(*rl_progress).Split(";", FString::TOK_SKIPEMPTY);
	for (auto& entry : entries)
	{
		auto eq = entry.IndexOf('=');
		if (eq <= 0 || entry.Left(eq).CompareNoCase(LumpFilter) != 0) continue;
		auto values = entry.Mid(eq + 1).Split(",");
		if (values.Size() > 0) p.unlocked = std::max(int(values[0].ToLong()), 1);
		if (values.Size() > 1) p.gauntletWon = values[1].ToLong() != 0;
		if (values.Size() > 2) p.activeRun = int(values[2].ToLong());
	}
	return p;
}

void SetProgress(const Progress& p)
{
	FString out;
	auto entries = FString(*rl_progress).Split(";", FString::TOK_SKIPEMPTY);
	for (auto& entry : entries)
	{
		auto eq = entry.IndexOf('=');
		if (eq <= 0 || entry.Left(eq).CompareNoCase(LumpFilter) == 0) continue; // this game's old entry (or a broken one)
		out.AppendFormat("%s;", entry.GetChars());
	}
	out.AppendFormat("%s=%d,%d,%d", LumpFilter.GetChars(), p.unlocked, int(p.gauntletWon), p.activeRun);
	rl_progress = out.GetChars();
	saveConfig = true;
}

//---------------------------------------------------------------------------
//
// the Armory's kit, the Covenant's Dread, and handing items over
//
//---------------------------------------------------------------------------

int KitCost(const int* units = nullptr)
{
	int sum = 0;
	for (int i = 0; i < kNumKit; i++) sum += (units ? units[i] : int(*kitItems[i].cvar)) * kitItems[i].cost;
	return sum;
}

// The points a run gets for its kit. A stock game reaches later episodes with the earlier ones' guns, so they get
// more: 100 for the first, 50 more for each later one up to 250; Cryptic Passage 200; the gauntlet, which starts at
// the first episode, 100. The Covenant's Austerity takes 20 a rank.
int Budget(int episode, int austerity)
{
	int points = episode == 0 ? 100 : LumpFilter.CompareNoCase("Blood.Cryptic") == 0 ? 200 : std::min(100 + 50 * (episode - 1), 250);
	return std::max(points - 20 * austerity, 0);
}

int Dread(const int* ranks)
{
	int dread = 0;
	for (int c = 0; c < kNumConditions; c++) dread += ranks[c] * conditions[c].dread;
	return dread;
}

int ChosenDread()
{
	int ranks[kNumConditions];
	for (int c = 0; c < kNumConditions; c++) ranks[c] = *conditions[c].cvar;
	return Dread(ranks);
}

// The gun a weapon or weapon-giving ammo pickup gives (0 = none).
int WeaponOf(int item)
{
	if (item >= kItemWeaponBase && item < kItemWeaponMax) return gWeaponItemData[item - kItemWeaponBase].type;
	if (item >= kItemAmmoBase && item < kItemAmmoMax) return gAmmoItemData[item - kItemAmmoBase].weaponType;
	return 0;
}

// Hands an item over as if the player had picked it up: Blood's own pickup amounts and caps, no sound or message.
void GiveItem(DBloodPlayer* p, int type)
{
	auto raise = [&](int weapon)
	{
		int best = WeaponUpgrade(p, weapon);
		if (best != p->curWeapon)
		{
			p->weaponState = 0;
			p->nextWeapon = best;
		}
	};
	if (type >= kItemWeaponBase && type < kItemWeaponMax)
	{
		auto& d = gWeaponItemData[type - kItemWeaponBase];
		p->hasWeapon[d.type] = 1;
		if (d.ammoType >= 0) p->ammoCount[d.ammoType] = ClipHigh(p->ammoCount[d.ammoType] + d.count, gAmmoInfo[d.ammoType].max);
		raise(d.type);
	}
	else if (type >= kItemAmmoBase && type < kItemAmmoMax)
	{
		auto& d = gAmmoItemData[type - kItemAmmoBase];
		if (d.type >= countof(p->ammoCount)) return;
		p->ammoCount[d.type] = ClipHigh(p->ammoCount[d.type] + d.count, gAmmoInfo[d.type].max);
		if (d.weaponType)
		{
			p->hasWeapon[d.weaponType] = 1;
			raise(d.weaponType);
		}
	}
	else if (type >= kItemArmorBasic && type <= kItemArmorSuper)
	{
		auto& a = armorData[type - kItemArmorBasic];
		if (p->armor[0] < a.armor0max) p->armor[0] = ClipHigh(p->armor[0] + a.armor0, a.armor0max);
		if (p->armor[1] < a.armor1max) p->armor[1] = ClipHigh(p->armor[1] + a.armor1, a.armor1max);
		if (p->armor[2] < a.armor2max) p->armor[2] = ClipHigh(p->armor[2] + a.armor2, a.armor2max);
	}
	else if (type == kItemHealthDoctorBag || type == kItemJumpBoots || type == kItemDivingSuit || type == kItemBeastVision)
		packAddItem(p, gItemData[type - kItemBase].packSlot);
	else if (type == kItemHealthMedPouch || type == kItemHealthLifeEssense || type == kItemHealthLifeSeed || type == kItemHealthRedPotion)
		actHealDude(p->GetActor(), gPowerUpInfo[type - kItemBase].bonusTime, gPowerUpInfo[type - kItemBase].maxTime);
}

// The run's first map: the kit is handed over (after the remix, so none of it is rerolled).
void GiveKit()
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	FString list;
	for (int i = 0; i < kNumKit; i++)
	{
		if (run.kit[i] <= 0) continue;
		for (int n = 0; n < run.kit[i] * kitItems[i].count; n++) GiveItem(p, kitItems[i].item);
		list.AppendFormat("%s%s", list.IsEmpty() ? "" : ", ", kitItems[i].name);
		if (run.kit[i] > 1) list.AppendFormat(" x%d", run.kit[i]);
	}
	if (list.IsNotEmpty()) Printf(PRINT_NOTIFY, "Your kit: %s\n", list.GetChars());
}

//---------------------------------------------------------------------------
//
// the reward choice (Hades' boon pick): from the second map on, each map
// starts with a choice of three rewards, or nothing for a little ichor
//
//---------------------------------------------------------------------------

constexpr int kRewardAmmo = 1000; // a pickup's worth of ammo for every gun the player has

struct RewardPick { int id, weight; };

// The guns, with weights for a run's start and its end: the heavy ones get likelier deeper in.
struct RewardGun { int item, early, late; };
const RewardGun rewardGuns[] =
{
	{ kItemWeaponFlarePistol, 4, 1 }, { kItemWeaponSawedoff, 4, 2 }, { kItemAmmoSprayCan, 2, 2 }, { kItemWeaponTommygun, 3, 3 },
	{ kItemAmmoVoodooDoll, 1, 2 }, { kItemWeaponNapalmLauncher, 1, 3 }, { kItemWeaponTeslaCannon, 1, 3 }, { kItemWeaponLifeLeech, 0, 2 },
};

FString RewardName(int id)
{
	if (id == kRewardAmmo) return "Ammo for every gun you have";
	if (id == kItemHealthLifeEssense) return "Life essence";
	if (id == kItemHealthLifeSeed) return "Life seed (health to 200)";
	for (auto& k : kitItems)
		if (k.item == id) return WeaponOf(id) ? FStringf("%s, with ammo", k.name) : FString(k.name);
	return "Something";
}

int PickWeighted(std::mt19937& rng, const TArray<RewardPick>& list)
{
	int total = 0;
	for (auto& p : list) total += p.weight;
	if (total <= 0) return -1;
	int r = RandInt(rng, 0, total - 1);
	for (auto& p : list)
	{
		if (r < p.weight) return p.id;
		r -= p.weight;
	}
	return -1;
}

void RollOffers(std::mt19937& rng, double depth)
{
	for (auto& o : run.offers) o = -1;
	auto p = getPlayer(myconnectindex);
	if (!p) return;
	TArray<RewardPick> guns, armour, care;
	bool anyGun = false;
	for (auto& g : rewardGuns)
	{
		if (p->hasWeapon[WeaponOf(g.item)]) anyGun = true;
		else
		{
			int w = int(g.early + (g.late - g.early) * depth + 0.5);
			if (w > 0) guns.Push({ g.item, w });
		}
	}
	armour.Push({ kItemArmorBody, 3 });
	armour.Push({ kItemArmorFire, 2 });
	armour.Push({ kItemArmorSpirit, 2 });
	if (depth >= 0.5) armour.Push({ kItemArmorSuper, 1 });
	auto packFull = [&](int item) { return p->packSlots[gItemData[item - kItemBase].packSlot].curAmount >= 100; };
	if (!packFull(kItemHealthDoctorBag)) care.Push({ kItemHealthDoctorBag, 3 });
	care.Push({ kItemHealthLifeEssense, 2 });
	if (depth >= 0.3) care.Push({ kItemHealthLifeSeed, 1 });
	if (!packFull(kItemBeastVision)) care.Push({ kItemBeastVision, 1 });
	if (!packFull(kItemJumpBoots)) care.Push({ kItemJumpBoots, 1 });

	// Three kinds of reward, a gun among them while there are guns to find (the later maps expect them).
	TArray<int> kinds; // 0 gun, 1 armour, 2 care, 3 ammo
	if (guns.Size()) kinds.Push(0);
	TArray<int> rest;
	rest.Push(1);
	rest.Push(2);
	if (anyGun) rest.Push(3);
	for (int i = int(rest.Size()) - 1; i > 0; i--) std::swap(rest[i], rest[RandInt(rng, 0, i)]);
	for (int k : rest) if (kinds.Size() < 3) kinds.Push(k);
	for (unsigned i = 0; i < kinds.Size() && i < 3; i++)
	{
		switch (kinds[i])
		{
		case 0: run.offers[i] = PickWeighted(rng, guns); break;
		case 1: run.offers[i] = PickWeighted(rng, armour); break;
		case 2: run.offers[i] = PickWeighted(rng, care); break;
		default: run.offers[i] = kRewardAmmo; break;
		}
	}
	run.offerPending = run.offers[0] >= 0;
}

void GrantReward(int id)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor() || id < 0) return;
	if (id != kRewardAmmo)
	{
		GiveItem(p, id);
		return;
	}
	for (auto& g : rewardGuns)
	{
		if (!p->hasWeapon[WeaponOf(g.item)]) continue;
		if (g.item >= kItemWeaponBase && g.item < kItemWeaponMax)
		{
			auto& d = gWeaponItemData[g.item - kItemWeaponBase];
			if (d.ammoType >= 0) p->ammoCount[d.ammoType] = ClipHigh(p->ammoCount[d.ammoType] + d.count, gAmmoInfo[d.ammoType].max);
		}
		else GiveItem(p, g.item); // the spray can and the voodoo doll come as ammo
	}
	if (p->hasWeapon[kWeapDynamite]) GiveItem(p, kItemAmmoTNTBox);
}


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

int Unlocked(int count) { return std::clamp(GetProgress().unlocked, 1, std::max(count, 1)); }

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
		if (!looks.CheckKey(a->spr.type)) looks.Insert(a->spr.type, { a->spr.pal, a->spr.scale });
		// An enemy wired into the map's script stays as Blood placed it and is never a champion: one whose death
		// sends a message (Cheogh and his gargoyles on E1M7 feed the kill counter that opens the exit; the boss rush
		// on E4M8; a door that opens when its guard dies), or one that a trigger wakes or moves: 137 of the 4083
		// enemies of these types on Blood's and Cryptic Passage's maps.
		if (a->xspr.txID != 0 || a->xspr.rxID != 0) continue;
		eligible.Push(a);
		// A key carrier keeps its kind (it can still be a champion): a gargoyle or phantasm in its place could drop
		// the key over a pit or lava. 17 enemies on the stock maps carry one.
		if (a->xspr.key > 0 || (a->xspr.dropMsg >= kItemKeyBase && a->xspr.dropMsg < kItemKeyMax)) continue;
		if (a->sector() && !IsUnderwaterSector(a->sector())) reroll.Push(a); // swimmers stay what they are
	}

	double budget = 0; // the map's stock threat: the rerolled kinds add up to about the same
	for (auto a : reroll) budget += KindCost(a->spr.type);

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
		// The map's own look for that type, if it has one. Either way the type's SEQ sets its own palette and size as it
		// spawns (Tommy cultist 3, tesla 11, TNT 13, stone gargoyle 5...), so a type the map lacks still looks right.
		auto look = looks.CheckKey(pick.type);
		bmod::RetypeDude(reroll[i], pick.type, look ? look->pal : 0, look ? look->scale : DVector2(1, 1));
	}

	double chance = (kChampionFirst + (kChampionLast - kChampionFirst) * depth) * (1 + run.cov[kChampions]);
	for (auto a : eligible) if (Rand01(rng) < chance) MakeChampion(a);
}

// The run's last map of an episode: its boss map.
bool BossMap()
{
	if (run.index < 0 || run.index >= int(run.maps.Size())) return false;
	if (run.index + 1 >= int(run.maps.Size())) return true;
	auto cur = FindMapByName(run.maps[run.index].GetChars()), next = FindMapByName(run.maps[run.index + 1].GetChars());
	return cur && next && cur->cluster != next->cluster;
}

// The Covenant's Flesh and Wardens, on every enemy of the map (bosses and scripted ones too).
void CovenantEnemies()
{
	bool wardens = run.cov[kWardens] > 0 && BossMap();
	double flesh = 1. + 0.15 * run.cov[kFlesh];
	if (!wardens && run.cov[kFlesh] == 0) return;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0) continue;
		if (wardens && !(a->modFlags & kModChampion)) MakeChampion(a);
		a->xspr.health = uint32_t(a->xspr.health * flesh);
	}
}

// Horde and Hourglass, from the enemies the map starts with: how many more arrive in waves, and the par time.
void SetupPressure()
{
	run.levelClock = PlayClock;
	int enemies = 0;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
		if (!a->IsPlayerActor() && a->hasX() && a->xspr.health > 0 && AllowedKillType(a)) enemies++;
	run.extraEnemies = int(enemies * 0.2 * run.cov[kHorde] + 0.5);
	run.nextWave = PlayClock + 40 * kClock;
	run.parTime = run.cov[kHourglass] ? std::clamp(60 + 3 * enemies, 120, 600) : 0;
	run.nextOvertime = 0;
}

// Horde's waves (the rest of its enemies, a few at a time) and Hourglass's (every 30 s past the par time).
void Pressure()
{
	if (run.extraEnemies > 0 && PlayClock >= run.nextWave)
	{
		int came = bmod::SpawnWave(std::min(run.extraEnemies, 3 + run.cov[kHorde]));
		run.extraEnemies -= came;
		run.nextWave = PlayClock + (came ? 50 : 5) * kClock; // no room behind the player: try again soon
	}
	if (run.parTime > 0 && PlayClock - run.levelClock > run.parTime * kClock && PlayClock >= run.nextOvertime)
	{
		int came = bmod::SpawnWave(3);
		run.nextOvertime = PlayClock + (came ? 30 : 5) * kClock;
	}
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
	{ kItemAmmoFlares, 4 }, { kItemAmmoSprayCan, 2 }, { kItemAmmoTeslaCharge, 2 },
	{ kItemAmmoGasolineCan, 2 }, { kItemAmmoTrappedSoul, 1 }, { kItemAmmoVoodooDoll, 1 },
};
// Explosives reroll only into explosives, so a map keeps the means to blast what it expects to be blasted.
const ItemPick explosivePicks[] =
{
	{ kItemAmmoTNTBundle, 3 }, { kItemAmmoTNTBox, 1 }, { kItemAmmoProxBombBundle, 2 }, { kItemAmmoRemoteBombBundle, 2 },
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
	{ explosivePicks, int(countof(explosivePicks)) },
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
		// So does an item wired into the map's script: picking it up sends a message (E1M6's fire armour opens a
		// door), or a trigger acts on it. Blood sends the message only once the item is taken, which a reroll into
		// something the player can't take yet would hold back. A "secret found" message only counts the secret.
		// 12 such items on the stock maps.
		if (a->hasX() && ((a->xspr.txID != 0 && a->xspr.txID != kChannelSecretFound) || a->xspr.rxID != 0)) continue;
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

// Only with the Covenant's Curses: rank 1, a quarter of the maps from the second on, silly and twist effects;
// rank 2, nasty ones too; rank 3, every map.
void PickCurse(std::mt19937& rng)
{
	run.curse = "";
	int rank = run.cov[kCurses];
	if (rank <= 0) return;
	if (rank < 3 && (run.index < 1 || Rand01(rng) >= kCurseChance)) return;
	// Timed effects only: one-shots act once, and effects with a length of their own are meant to be brief.
	TArray<const bmod::Effect*> list;
	int total = 0;
	for (int i = 0; i < bmod::NumEffects(); i++)
	{
		auto e = &bmod::GetEffect(i);
		if (e->oneShot || e->fixedSeconds > 0 || !bmod::CanStart(e)) continue;
		if (rank < 2 && e->category == bmod::Category::Nasty) continue;
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
	CovenantEnemies();
	RemixItems(rng);
	PickCurse(rng);
	SetupPressure();
	if (run.index == 0)
	{
		if (!run.kitGiven) GiveKit(); // after the remix, so none of it is rerolled
		run.kitGiven = true;
		run.offerPending = false;
	}
	else RollOffers(rng, depth); // the ticker opens the choice once the map is running
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

// The run's ichor with the Covenant's bonus: +10% per point of Dread.
int BankedIchor() { return int(run.ichor * (1. + kDreadIchor * Dread(run.cov)) + 0.5); }

void Bank()
{
	rl_ichor = rl_ichor + BankedIchor();
	auto p = GetProgress();
	if (p.activeRun == run.id)
	{
		p.activeRun = 0;
		SetProgress(p);
	}
	saveConfig = true;
}

FString RunStats()
{
	int dread = Dread(run.cov);
	FString bonus = dread ? FStringf(", +%d%% for Dread %d", int(kDreadIchor * 100 * dread + 0.5), dread) : FString();
	return FStringf("Maps cleared: %d of %d\nKills: %d (%d champions)\nIchor: +%d%s (banked: %d)",
		run.cleared, int(run.maps.Size()), run.kills, run.champions, BankedIchor(), bonus.GetChars(), (int)rl_ichor);
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
	auto p = GetProgress();
	if (run.episode == 0)
	{
		p.gauntletWon = true;
		SetProgress(p);
		news = "The gauntlet is won!";
	}
	else if (run.episode >= Unlocked(count) && run.episode < count)
	{
		p.unlocked = run.episode + 1;
		SetProgress(p);
		news.Format("Unlocked: %s", eps[run.episode].name.GetChars());
		if (run.episode + 1 >= count) news += "\nThe gauntlet is open: every episode in one run.";
	}
	menuMessage.Format("RUN COMPLETE\n\n%s\n\n%s%s%s", RunName(run.episode).GetChars(), RunStats().GetChars(),
		news.IsEmpty() ? "" : "\n\n", news.GetChars());
	backToCrypt = true;
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
	int budget = Budget(episode, rl_cov_austerity), cost = KitCost();
	if (cost > budget)
	{
		FStringf msg("Your kit costs %d points; %s allows %d.\nLighten it in the Armory.", cost, RunName(episode).GetChars(), budget);
		Printf(PRINT_NOTIFY, "%s\n", msg.GetChars());
		M_StartMessage(msg.GetChars(), 1);
		return false;
	}
	if (run.active && !run.over)
	{
		Printf(PRINT_NOTIFY, "Roguelite: the run in progress is abandoned\n");
		Bank(); // abandoning ends it: its ichor is banked, as when it ends any other way
	}
	run = Run();
	run.active = true;
	run.id = std::max((int)rl_runs, 0) + 1;
	rl_runs = run.id;
	auto p = GetProgress();
	p.activeRun = run.id;
	SetProgress(p);
	run.seed = seed;
	run.skill = rl_skill;
	run.episode = episode;
	run.maps = std::move(order);
	run.pending = 0;
	for (int i = 0; i < kNumKit; i++) run.kit[i] = *kitItems[i].cvar;
	for (int c = 0; c < kNumConditions; c++) run.cov[c] = *conditions[c].cvar;
	run.unspent = budget - cost;
	startingRun = true;
	saveConfig = true;
	Printf(PRINT_NOTIFY, "Roguelite: %s, %d maps (run %d, seed %u)\n", RunName(episode).GetChars(), int(run.maps.Size()), run.id, seed);
	M_ClearMenus();
	DeferredStartGame(first, run.skill);
	return true;
}

//---------------------------------------------------------------------------
//
// the Crypt: the hub between runs, like the House in Hades. rlcrypt.map is
// ours (tools/blood2d/rlcrypt.py writes it into raze.pk3) and is drawn with
// Blood's own art. It marks its features with invisible sprites (xsprite
// data4 = 'RL', data1 = the feature). Walking into a portal starts a run; Use
// at the weapon wall opens the Armory, at the altar the Covenant, at the
// tombstone the Roguelite menu. Dying, or winning, brings you back here.
//
//---------------------------------------------------------------------------

const char* const kCryptMap = "rlcrypt.map";
constexpr int kMarkerTag = 0x524C;
enum { kFeatArmory = 1, kFeatCovenant = 2, kFeatRecords = 3, kFeatPortal = 10, kNumPortals = 6 };
constexpr double kUseRange = 80;    // map units from a feature's marker
constexpr double kLabelRange = 192; // a portal's name shows within this
constexpr int kNoticeTime = 4 * kClock;

struct Crypt
{
	bool in = false;                       // the current level is the Crypt
	DVector2 feat[4];                      // the Armory, the Covenant and the Records markers (index = feature id)
	bool hasFeat[4] = {};
	sectortype* portal[kNumPortals] = {};  // each portal's alcove
	DVector2 portalPos[kNumPortals];
	int lastPortal = -1;                   // the portal the player stood in last tic: walking in starts a run
	int confirmPortal = -1;                // walked into once while a run can still be continued: again to abandon it
	int confirmClock = INT_MIN / 2;
	FString notice;                        // a short line under the crosshair
	int noticeClock = INT_MIN / 2;
};

Crypt crypt;
bool enteringCrypt;   // rl_crypt asked for the game that is about to start
FString cryptMessage; // shown once the Crypt is up (a run's result)

bool CryptAvailable() { return fileSystem.FileExists(kCryptMap); }
bool IsCryptLevel() { return currentLevel && currentLevel->fileName.CompareNoCase(kCryptMap) == 0; }

bool EnterCrypt()
{
	if (!isBlood() || numplayers > 1 || !CryptAvailable()) return false;
	auto map = SetupUserMap(kCryptMap);
	if (!map) return false;
	enteringCrypt = true;
	M_ClearMenus();
	DeferredStartGame(map, rl_skill);
	return true;
}

void CryptNotice(const FString& text)
{
	crypt.notice = text;
	crypt.noticeClock = PlayClock;
}

// The run a portal starts: portals 0-4 are the game's first five episodes, portal 5 the gauntlet
// (only when there's more than one episode). -1 = nothing behind it.
int PortalRun(int i, int count)
{
	if (i == kNumPortals - 1) return count > 1 ? 0 : -1;
	return i < count ? i + 1 : -1;
}

bool PortalOpen(int i, int count)
{
	int r = PortalRun(i, count);
	return r == 0 ? Unlocked(count) >= count : r > 0 && r <= Unlocked(count);
}

void SetupCrypt()
{
	crypt = Crypt();
	crypt.in = true;
	BloodStatIterator it(kStatDecoration);
	while (auto a = it.Next())
	{
		if (!a->hasX() || a->xspr.data4 != kMarkerTag) continue;
		int id = a->xspr.data1;
		if (id >= kFeatArmory && id <= kFeatRecords)
		{
			crypt.feat[id] = a->spr.pos.XY();
			crypt.hasFeat[id] = true;
		}
		else if (id >= kFeatPortal && id < kFeatPortal + kNumPortals)
		{
			crypt.portal[id - kFeatPortal] = a->sector();
			crypt.portalPos[id - kFeatPortal] = a->spr.pos.XY();
		}
	}
	// Locked and unused portals go dark.
	TArray<Episode> eps;
	GetEpisodes(eps);
	for (int i = 0; i < kNumPortals; i++)
	{
		auto sect = crypt.portal[i];
		if (!sect || PortalOpen(i, int(eps.Size()))) continue;
		sect->floorshade = sect->ceilingshade = 48;
		for (auto& wal : sect->walls) wal.shade = 48;
	}
	auto p = getPlayer(myconnectindex);
	if (p && p->GetActor()) // standing in a portal at the start doesn't count as walking in
		for (int i = 0; i < kNumPortals; i++) if (crypt.portal[i] && crypt.portal[i] == p->GetActor()->sector()) crypt.lastPortal = i;
}

void EnterPortal(int i)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size()), r = PortalRun(i, count);
	if (r < 0) CryptNotice("This portal is sealed");
	else if (!PortalOpen(i, count))
		CryptNotice(r == 0 ? FString("The gauntlet opens once every episode is unlocked") :
			FStringf("%s is locked: win a run of %s first", eps[r - 1].name.GetChars(), eps[r - 2].name.GetChars()));
	else if (GetProgress().activeRun != 0 && (crypt.confirmPortal != i || PlayClock - crypt.confirmClock > 10 * kClock))
	{
		crypt.confirmPortal = i;
		crypt.confirmClock = PlayClock;
		CryptNotice("A run can still be continued from its saves: walk in again to abandon it");
	}
	else StartRun(r, uint32_t(std::random_device{}())); // refuses (with a message) a kit over the run's points
}

void CryptTick()
{
	if (cryptMessage.IsNotEmpty())
	{
		FString msg = cryptMessage;
		cryptMessage = "";
		M_StartMessage(msg.GetChars(), 1);
		return;
	}
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	int in = -1;
	for (int i = 0; i < kNumPortals; i++) if (crypt.portal[i] && crypt.portal[i] == p->GetActor()->sector()) in = i;
	if (in == crypt.lastPortal) return;
	crypt.lastPortal = in;
	if (in >= 0) EnterPortal(in);
}

// The feature the player is close enough to use, or 0.
int NearFeature(DBloodPlayer* p)
{
	if (!crypt.in || !p || !p->GetActor()) return 0;
	auto pos = p->GetActor()->spr.pos.XY();
	for (int id = kFeatArmory; id <= kFeatRecords; id++)
		if (crypt.hasFeat[id] && (crypt.feat[id] - pos).Length() <= kUseRange) return id;
	return 0;
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
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
		LevelStarted();
		break;
	case bmod::Event::SaveLoaded:
		nextCurseTry = 0;
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
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

void OpenRewardChoice(); // the menu section below

void OnTick()
{
	if (refusedLoad)
	{
		refusedLoad = false;
		menuMessage = "THAT RUN IS OVER\n\nA run ends when you die, when you win it,\nor when you start another one.\n\nStart a new run from the Roguelite menu.";
		gameaction = ga_mainmenu;
		return;
	}
	bmod::modeEnemyTimeScale = run.active && !run.over ? 1. + 0.15 * run.cov[kHaste] : 1.; // the Covenant's Haste lasts the run
	if (crypt.in)
	{
		CryptTick();
		return;
	}
	if (!run.active || run.over) return;
	if (run.offerPending && gamestate == GS_LEVEL && !M_Active())
	{
		OpenRewardChoice(); // closing it without a choice opens it again: pick a reward, or nothing for a little ichor
		return;
	}
	Pressure();
	if (run.curse.IsEmpty()) return;
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
	if (backToCrypt)
	{
		// A run was won: after the stock end of the episode, back to the Crypt with the result.
		backToCrypt = false;
		cryptMessage = menuMessage;
		menuMessage = "";
		if (EnterCrypt()) return;
		menuMessage = cryptMessage;
		cryptMessage = "";
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
	int dread = Dread(run.cov);
	FString stats = FStringf("Map %d/%d   Ichor %d", run.index + 1, int(run.maps.Size()), run.ichor);
	if (dread) stats.AppendFormat("   Dread %d", dread);
	line(stats.GetChars(), CR_WHITE);
	if (auto e = CurseEffect()) line(FStringf("Curse: %s", bmod::EffectTitle(e)).GetChars(), CR_RED);
	if (run.parTime > 0)
	{
		int left = run.parTime - (PlayClock - run.levelClock) / kClock;
		if (left >= 0) line(FStringf("Par %d:%02d", left / 60, left % 60).GetChars(), left < 30 ? CR_ORANGE : CR_WHITE);
		else line(FStringf("Overtime %d:%02d", -left / 60, -left % 60).GetChars(), CR_RED);
	}
	if (run.extraEnemies > 0) line(FStringf("Horde: %d to come", run.extraEnemies).GetChars(), CR_GRAY);
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
	FStringf prompt("Press %s to return to %s", keyName.GetChars(), CryptAvailable() ? "the Crypt" : "the main menu");
	bool blink = (PlayClock / 30) & 1;
	DrawCentered(SmallFont, blink ? CR_WHITE : CR_GOLD, y + lh, prompt.GetChars(), ssc);
}

// The Crypt: what Use does where you stand, the nearest portal's run, and the latest notice.
void DrawCrypt(DBloodPlayer* pPlayer)
{
	if (bmod::mods.hideHud) return;
	double sc = twod->GetHeight() / 480., ssc = 1.5 * sc, y = twod->GetHeight() * 0.62;
	auto line = [&](const FString& text, int color)
	{
		DrawCentered(SmallFont, color, y, text.GetChars(), ssc);
		y += SmallFont->GetHeight() * ssc + 4 * sc;
	};
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size());
	switch (NearFeature(pPlayer))
	{
	case kFeatArmory:
		line(FStringf("Use: the Armory (your kit: %d points)", KitCost()), CR_GOLD);
		break;
	case kFeatCovenant:
	{
		int dread = ChosenDread();
		line(dread ? FStringf("Use: the Covenant (Dread %d)", dread) : FString("Use: the Covenant (no conditions)"), CR_GOLD);
		break;
	}
	case kFeatRecords:
		line(FStringf("Use: the Records (ichor %d, runs %d, won %d)", (int)rl_ichor, (int)rl_runs, (int)rl_wins), CR_GOLD);
		break;
	default:
		break;
	}
	auto actor = pPlayer ? pPlayer->GetActor() : nullptr;
	if (actor)
	{
		int best = -1;
		double bestDist = kLabelRange;
		for (int i = 0; i < kNumPortals; i++)
		{
			if (!crypt.portal[i]) continue;
			double d = (crypt.portalPos[i] - actor->spr.pos.XY()).Length();
			if (d < bestDist)
			{
				best = i;
				bestDist = d;
			}
		}
		if (best >= 0)
		{
			int r = PortalRun(best, count);
			if (r < 0) line("A sealed portal", CR_DARKGRAY);
			else if (!PortalOpen(best, count)) line(FStringf("%s (locked)", RunName(r).GetChars()), CR_DARKGRAY);
			else
			{
				int budget = Budget(r, rl_cov_austerity), cost = KitCost();
				line(FStringf("%s: %d kit points (your kit: %d)", RunName(r).GetChars(), budget, cost), cost > budget ? CR_RED : CR_WHITE);
			}
		}
	}
	int age = PlayClock - crypt.noticeClock;
	if (crypt.notice.IsNotEmpty() && age >= 0 && age < kNoticeTime) line(crypt.notice, CR_ORANGE);
}

void OnDraw(DBloodPlayer* pPlayer)
{
	if (crypt.in)
	{
		DrawCrypt(pPlayer);
		return;
	}
	if (!run.active) return;
	double sc = twod->GetHeight() / 480.;
	if (run.over)
	{
		DrawRunOver(pPlayer, sc);
		return;
	}
	DrawCurseTitle(); // even under the "No HUD" curse, which must announce itself too
	if (bmod::mods.hideHud) return; // the "No HUD" effect hides the run line
	DrawRunLine(sc);
}

//---------------------------------------------------------------------------
//
// the Roguelite menu: menudef has the title and the skill; the runs are added
// here whenever a menu opens, so locks and the bank are current
//
//---------------------------------------------------------------------------

DMenuDescriptor* menuDesc;        // the menu the items below were added to (compared only)
TArray<DMenuItemBase*> menuItems; // the items added here

// A menu entry that starts a run. While another run is in progress (being played, or continuable from its saves),
// it asks first with Blood's yes/no message (OptionMenuItemSafeCommand), since starting a run ends the other one.
DMenuItemBase* CreateRunItem(const char* label, const char* command, bool confirm)
{
	auto cls = confirm ? PClass::FindClass("OptionMenuItemSafeCommand") : nullptr;
	auto init = cls ? dyn_cast<PFunction>(cls->FindSymbol("Init", false)) : nullptr;
	if (!init) return CreateOptionMenuItemCommand(label, FName(command));
	auto item = cls->CreateNew();
	FString labelStr = label;
	FString prompt = "This abandons the run in progress: its saves stop loading.\nStart a new run?";
	VMValue params[] = { item, &labelStr, FName(command).GetIndex(), &prompt };
	VMCall(init->Variants[0].Implementation, params, countof(params), nullptr, 0);
	return (DMenuItemBase*)item;
}

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
	auto progress = GetProgress();
	bool inProgress = (run.active && !run.over) || progress.activeRun != 0; // starting a run would end it
	text(FStringf("Ichor banked: %d", (int)rl_ichor).GetChars(), CR_GOLD);
	text(FStringf("Runs: %d   Won: %d", (int)rl_runs, (int)rl_wins).GetChars(), -1);
	if (run.active && !run.over && gamestate == GS_LEVEL)
		text(FStringf("Playing: %s, map %d of %d", RunName(run.episode).GetChars(), run.index + 1, int(run.maps.Size())).GetChars(), CR_WHITE);
	else if (progress.activeRun != 0)
		text("A run is in progress: load one of its saves to go on", -1);
	text("", -1);

	int first = -1;
	if (CryptAvailable() && !crypt.in)
	{
		first = int(opt->mItems.Size());
		add(CreateOptionMenuItemCommand("Enter the Crypt: the Armory, the Covenant, the portals", FName("rl_crypt")));
		text("", -1);
	}
	header("Episode runs");
	for (int e = 0; e < count; e++)
	{
		if (e < unlocked)
		{
			if (first < 0) first = int(opt->mItems.Size());
			add(CreateRunItem(eps[e].name.GetChars(), FStringf("rl_newrun %d", e + 1).GetChars(), inProgress));
		}
		else text(FStringf("%s  (locked)", eps[e].name.GetChars()).GetChars(), CR_DARKGRAY);
	}
	if (count == 0) text("No episodes found", CR_DARKGRAY);
	text("", -1);
	header("The gauntlet");
	if (count > 0 && unlocked >= count)
		add(CreateRunItem(progress.gauntletWon ? "Every episode in one run (won)" : "Every episode in one run", "rl_newrun gauntlet", inProgress));
	else text("Unlocks once every episode is unlocked", CR_DARKGRAY);
	text("", -1);
	text("Winning an episode run unlocks the next one.", -1);
	text("Dying ends the run. Starting a new run abandons the old one.", -1);
	opt->mSelectedItem = inProgress ? -1 : first; // -1: the menu's first item (the skill), so Enter can't start a run by accident
	opt->mScrollPos = 0;
}

//---------------------------------------------------------------------------
//
// the Armory's and the Covenant's menus (menudef: CVAR options and sliders):
// their titles and first lines follow the choices as they change. And the
// reward choice's menu, filled in here.
//
//---------------------------------------------------------------------------

DOptionMenuDescriptor* FindOptionMenu(const char* name)
{
	auto desc = MenuDescriptors.CheckKey(FName(name));
	if (!desc || !*desc || !(*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return nullptr;
	return static_cast<DOptionMenuDescriptor*>(*desc);
}

// A menu item's text (OptionMenuItem.mLabel), which the open menu draws each frame.
void SetLabel(DMenuItemBase* item, const FString& text)
{
	if (!item) return;
	auto field = dyn_cast<PField>(item->GetClass()->FindSymbol("mLabel", true));
	if (field && field->Type == TypeString) *reinterpret_cast<FString*>(reinterpret_cast<uint8_t*>(item) + field->Offset) = text;
}

FString KitFitText()
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	int cost = KitCost(), austerity = rl_cov_austerity;
	FString over;
	auto check = [&](int episode, const FString& name)
	{
		int budget = Budget(episode, austerity);
		if (cost > budget) over.AppendFormat("%s%s (%d)", over.IsEmpty() ? "" : ", ", name.GetChars(), budget);
	};
	for (int e = 1; e <= int(eps.Size()); e++) check(e, FStringf("episode %d", e));
	check(0, "the gauntlet");
	return over.IsEmpty() ? FString("This kit fits every run.") : FStringf("Too many points for: %s", over.GetChars());
}

void UpdateMenus()
{
	if (auto m = FindOptionMenu("RogueliteArmory"))
	{
		m->mTitle.Format("The Armory: %d points", KitCost());
		if (m->mItems.Size()) SetLabel(m->mItems[0], KitFitText());
	}
	if (auto m = FindOptionMenu("RogueliteCovenant"))
	{
		int dread = ChosenDread();
		m->mTitle = dread ? FStringf("The Covenant: Dread %d", dread) : FString("The Covenant");
		if (m->mItems.Size())
			SetLabel(m->mItems[0], dread ? FStringf("Dread %d: +%d%% ichor", dread, int(kDreadIchor * 100 * dread + 0.5)) : FString("No conditions: Dread 0"));
	}
}

void BuildPickMenu()
{
	auto opt = FindOptionMenu("RoguelitePick");
	if (!opt) return;
	opt->mItems.Clear();
	auto add = [&](DMenuItemBase* item)
	{
		if (!item) return;
		GC::WriteBarrier(opt, item);
		opt->mItems.Push(item);
	};
	opt->mTitle.Format("Map %d of %d: choose a reward", run.index + 1, int(run.maps.Size()));
	add(CreateOptionMenuItemStaticText(" ", -1));
	for (int i = 0; i < 3; i++)
		if (run.offers[i] >= 0) add(CreateOptionMenuItemCommand(RewardName(run.offers[i]).GetChars(), FName(FStringf("rl_pick %d", i + 1).GetChars()), true));
	add(CreateOptionMenuItemStaticText(" ", -1));
	add(CreateOptionMenuItemCommand(FStringf("Nothing (+%d ichor)", kSkipIchor).GetChars(), FName("rl_pick 0"), true));
	opt->mSelectedItem = -1;
	opt->mScrollPos = 0;
}

void OpenRewardChoice()
{
	BuildPickMenu();
	if (!FindOptionMenu("RoguelitePick"))
	{
		run.offerPending = false; // no menu to choose in (an old raze.pk3): the reward is skipped
		return;
	}
	M_StartControlPanel(false);
	M_SetMenu(FName("RoguelitePick"));
}

//---------------------------------------------------------------------------
//
// the movie entering a map: Blood ties an episode's opening movie to the
// episode's first map (The Way of All Flesh: Caleb rising from his grave, on
// E1M1), and Dead Reckoning's other one to its boss map. In a run, the opening
// movie plays when the run enters the episode, on whichever map comes first,
// and not again when the episode's first map comes up later; a movie on any
// other map stays with its map (the boss map is the run's last anyway).
//
//---------------------------------------------------------------------------

CutsceneDef* MapIntro(MapRecord* map, CutsceneDef* stock)
{
	if (!map || !run.active || run.over || run.pending < 0 || run.pending >= int(run.maps.Size())
		|| run.maps[run.pending].CompareNoCase(map->labelName) != 0)
		return stock; // not the run going to its next map: stock
	auto vol = FindVolume(map->cluster);
	auto start = vol ? FindMapByName(vol->startmap.GetChars()) : nullptr;
	if (!start) return stock;
	auto prev = run.pending > 0 ? FindMapByName(run.maps[run.pending - 1].GetChars()) : nullptr;
	bool entering = !prev || prev->cluster != map->cluster;
	if (entering) return &start->intro;
	static CutsceneDef none = [] { CutsceneDef c; c.function = "none"; return c; }(); // plays nothing, and nothing in its place
	return start == map ? &none : stock;
}

struct Registrar
{
	Registrar()
	{
		bmod::AddListener(OnEvent);
		bmod::AddTicker(OnTick);
		bmod::AddDrawer(OnDraw);
		::gModMainMenuHook = [] { OnMainMenu(); };
		::gModMapIntroHook = [](MapRecord* map, CutsceneDef* stock) { return MapIntro(map, stock); };
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
	if (run.active && run.over)
	{
		// The player has died and an exit fires late (a delayed trigger): the level doesn't end, so the "Run over"
		// screen stays instead of the stock next map loading outside the run.
		gGameOptions.uGameFlags &= ~GF_AdvanceLevel;
		return stockNext;
	}
	if (!run.active || run.index < 0) return stockNext;
	if (run.exiting) // a second exit in the same tic: the same answer
		return run.pending >= 0 && run.pending < int(run.maps.Size()) ? FindMapByName(run.maps[run.pending].GetChars()) : nullptr;
	run.exiting = true;
	run.cleared++;
	run.ichor += kIchorMap;
	if (run.index == 0)
	{
		run.ichor += run.unspent; // the kit points left unspent, now that the run is under way
		run.unspent = 0;
	}
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
	if (!EnterCrypt()) gameaction = ga_mainmenu; // no reload: the run is over; back to the Crypt (or the menu)
	return true;
}

void OnNewGame()
{
	if (startingRun)
	{
		startingRun = false;
		// Write the run's number and this game's active run now, so a crash mid-run can't lose them (its saves
		// would be refused, and the next run would reuse its number). The last level's effects are off by now.
		saveConfig = false;
		WriteConfig();
		return;
	}
	run = Run(); // a game outside the run (or the Crypt); the run, if any, can still be continued from its saves
	enteringCrypt = false;
	bmod::modeEnemyTimeScale = 1.;
	if (saveConfig)
	{
		saveConfig = false;
		WriteConfig(); // a run that ended (back to the Crypt) changed the progress; the last level's effects are off
	}
}

bool OnUse(DBloodPlayer* pPlayer)
{
	int id = NearFeature(pPlayer);
	if (!id) return false;
	M_StartControlPanel(false);
	M_SetMenu(FName(id == kFeatArmory ? "RogueliteArmory" : id == kFeatCovenant ? "RogueliteCovenant" : "RogueliteMenu"));
	return true;
}

int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage)
{
	if (!source || !target || damage <= 0) return damage;
	auto s = source;
	for (int i = 0; i < 2 && s && !s->IsDudeActor(); i++) s = s->GetOwner(); // a missile's or explosion's owner
	if (!s || s == target || !s->IsDudeActor() || s->IsPlayerActor()) return damage;
	if (s->modFlags & kModChampion) damage += damage / 4; // champions hit 1.25x
	if (run.active && !run.over && target->IsPlayerActor() && run.cov[kWrath] > 0)
		damage = int(damage * (1. + 0.2 * run.cov[kWrath]) + 0.5); // the Covenant's Wrath
	return damage;
}

int ScaleHealing(DBloodActor* actor, int add)
{
	if (!run.active || run.over || !actor || !actor->IsPlayerActor() || add <= 0 || run.cov[kFamine] <= 0) return add;
	return std::max(1, int(add * (1. - 0.25 * run.cov[kFamine]) + 0.5)); // the Covenant's Famine
}

void KitChanged() { UpdateMenus(); }
void CovenantChanged() { UpdateMenus(); }

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
				("curse", run.curse)
				("kitgiven", run.kitGiven)
				("offerpending", run.offerPending)
				("levelclock", run.levelClock)
				("extraenemies", run.extraEnemies)
				("nextwave", run.nextWave)
				("partime", run.parTime)
				("nextovertime", run.nextOvertime)
				("unspent", run.unspent);
			arc.Array("cov", run.cov, kNumConditions);
			arc.Array("kit", run.kit, kNumKit);
			arc.Array("offers", run.offers, 3);
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
			("curse", loaded.curse)
			("kitgiven", loaded.kitGiven)
			("offerpending", loaded.offerPending)
			("levelclock", loaded.levelClock)
			("extraenemies", loaded.extraEnemies)
			("nextwave", loaded.nextWave)
			("partime", loaded.parTime)
			("nextovertime", loaded.nextOvertime)
			("unspent", loaded.unspent);
		arc.Array("cov", loaded.cov, kNumConditions);
		arc.Array("kit", loaded.kit, kNumKit);
		arc.Array("offers", loaded.offers, 3);
		arc.EndObject();
		has = true;
	}
	run = Run();
	if (!has) return; // a save outside any run: stock Blood
	if (loaded.id <= 0 || loaded.id != GetProgress().activeRun || loaded.index < 0 || loaded.index >= int(loaded.maps.Size()))
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
	UpdateMenus();
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
	auto p = GetProgress();
	Printf("Gauntlet: %s. Run that can be continued: %d (%s)\n", p.gauntletWon ? "won" : count > 0 && unlocked >= count ? "open" : "locked",
		p.activeRun, LumpFilter.GetChars());
	Printf("Ichor banked: %d. Runs: %d, won: %d (all games)\n", (int)rl_ichor, (int)rl_runs, (int)rl_wins);
}

CCMD(rl_unlock)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	int count = int(eps.Size());
	if (argv.argc() < 2)
	{
		Printf("usage: rl_unlock <count|all>   (%d of %d episodes unlocked)\n", Unlocked(count), count);
		return;
	}
	auto p = GetProgress();
	p.unlocked = !stricmp(argv[1], "all") ? std::max(count, 1) : std::clamp(atoi(argv[1]), 1, std::max(count, 1));
	SetProgress(p);
	Printf(PRINT_NOTIFY, "Roguelite: %d of %d episodes unlocked%s\n", p.unlocked, count, count > 0 && p.unlocked >= count ? ", the gauntlet is open" : "");
}

CCMD(rl_crypt)
{
	if (!EnterCrypt()) Printf("rl_crypt: Blood single player only, and the Crypt (rlcrypt.map) must be in raze.pk3\n");
}

CCMD(rl_pick)
{
	if (!run.active || run.over || !run.offerPending || argv.argc() < 2) return;
	int n = atoi(argv[1]);
	if (n >= 1 && n <= 3 && run.offers[n - 1] >= 0)
	{
		GrantReward(run.offers[n - 1]);
		Printf(PRINT_NOTIFY, "Reward: %s\n", RewardName(run.offers[n - 1]).GetChars());
	}
	else
	{
		run.ichor += kSkipIchor;
		Printf(PRINT_NOTIFY, "No reward: +%d ichor\n", kSkipIchor);
	}
	run.offerPending = false;
	M_ClearMenus();
}

CCMD(rl_kit_clear)
{
	for (auto& k : kitItems) *k.cvar = 0;
	Printf(PRINT_NOTIFY, "The Armory's kit is empty\n");
}

CCMD(rl_cov_clear)
{
	for (auto& c : conditions) *c.cvar = 0;
	Printf(PRINT_NOTIFY, "The Covenant is clear: Dread 0\n");
}

CCMD(rl_kit)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	FString list;
	for (auto& k : kitItems)
		if (*k.cvar > 0) list.AppendFormat("%s%s%s", list.IsEmpty() ? "" : ", ", k.name, *k.cvar > 1 ? FStringf(" x%d", int(*k.cvar)).GetChars() : "");
	Printf("Kit (%d points): %s\n", KitCost(), list.IsEmpty() ? "nothing" : list.GetChars());
	FString budgets;
	for (int e = 1; e <= int(eps.Size()); e++) budgets.AppendFormat("episode %d %d, ", e, Budget(e, rl_cov_austerity));
	Printf("Points: %sthe gauntlet %d\n", budgets.GetChars(), Budget(0, rl_cov_austerity));
	FString cov;
	for (auto& c : conditions)
		if (*c.cvar > 0) cov.AppendFormat("%s%s %d", cov.IsEmpty() ? "" : ", ", c.name, int(*c.cvar));
	Printf("Covenant: %s (Dread %d)\n", cov.IsEmpty() ? "none" : cov.GetChars(), ChosenDread());
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
	SetProgress(Progress());
	rl_ichor = 0;
	rl_wins = 0;
	saveConfig = true;
	Printf(PRINT_NOTIFY, "Roguelite: progress reset (this game's episodes locked; ichor bank and wins emptied)\n");
}

} // namespace rogue

END_BLD_NS
