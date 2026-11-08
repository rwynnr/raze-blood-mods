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
// - a chance growing from 6% to 18% makes each a champion (2.5x health,
//   1.25x size and damage, tinted blue), and a rarer one, from 4% to 12%, a
//   red champion: 5x health, 1.5x damage, 1.4x size, moving and animating
//   1.35x as fast, and it doesn't flinch;
// - health, armor, ammo, explosive, weapon and powerup pickups reroll within
//   their category (keys, the diving suit, jump boots and other quest-like
//   items stay, as do items wired into the map's script); ammo leans to the
//   guns the player has or can find there;
// - the map's own ammo, its pickups and what its enemies drop, is raised so
//   that the player's guns could kill its enemies rl_ammo_target percent of
//   the way (150: 1.5 times): the pickups of their ammo grow (up to twice
//   their stock amount, drawn bigger), then more pickups come next to the
//   map's own pickups and enemies; each secret area gets caches on top. What
//   the player carries isn't counted. A pickup the pocket can't hold all of
//   leaves the rest on the floor;
// - from the second map on, a 25% chance that one of the base's timed effects
//   curses the whole level (announced under the map's name).
//
// The Forge (docs/roguelite-forge.md): one gun of the kit, forged with runes in
// the Armory, fires the runes' payload in place of its own primary fire: the
// stock fire function still runs (hands, sounds, ammo, timing), its shots are
// swallowed and their aims recorded, and the payload goes out along them.
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
//   rl_ammo                              this map's ammo: what its enemies take, what you have, the supplies
//   rl_forge [vessel mouth path wound penance]  the forged gun (numbers as in the menu); sets it, and in a run
//                                        also forges the run's gun at once (testing)
//   rl_forge_clear                       melt the forged gun down
//   rl_unlock <count|all>                set how many of this game's episodes are unlocked (testing)
//   rl_resetprogress                     lock this game's episodes again; empty the ichor bank and the wins
//   openmenu RogueliteMenu               the Roguelite menu (it is also on the main menu)
// CVARs: rl_skill (0-4, set by the menu), rl_elite_pal (the champions' tint, a palette lookup number:
// 0-14 in Blood, 0 = none), rl_ammo_target (the supplies' target, percent; 0 = no supplies), and the meta progress, kept in the config: rl_progress (per game, since
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
#include <cmath>
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
CVARD(Int, rl_elite_red_pal, 2, CVAR_ARCHIVE, "roguelite: palette lookup that tints red champions (0 = none; 2 red)")
CVARD(Int, rl_ammo_target, 150, CVAR_ARCHIVE, "roguelite: each map's pickups hold this percent of the ammo your guns need for its enemies (150 = 1.5x; 0 = stock amounts)")

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
namespace rogue { void KitChanged(); void CovenantChanged(); void ForgeChanged(); }
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
// The Forge's recipe: one choice per slot (0 = none), checked together by ForgeChanged.
#define RL_FORGE(name, maxValue) CUSTOM_CVARD(Int, name, 0, CVAR_ARCHIVE, "roguelite Forge: this slot's rune (0 = none)") \
	{ if (self < 0) self = 0; else if (self > maxValue) self = maxValue; else BLD_NS::rogue::ForgeChanged(); }
RL_FORGE(rl_forge_vessel, 3)
RL_FORGE(rl_forge_mouth, 2)
RL_FORGE(rl_forge_path, 2)
RL_FORGE(rl_forge_wound, 2)
RL_FORGE(rl_forge_penance, 1)
CVARD(Int, rl_forge_added, 0, CVAR_ARCHIVE, "roguelite Forge: the Vessel the Forge itself put in the kit (taken out again with it)")

BEGIN_BLD_NS

namespace rogue
{

namespace
{

constexpr int kClock = 120;                                   // PlayClock units per second
constexpr int kIchorKill = 1, kIchorChampion = 5, kIchorRed = 15, kIchorMap = 10;
// Spike 2's gentler base: the threat budget stays at each map's stock total, and curses come only with the Covenant's
// Curses. The Covenant's conditions make it harder from there. Champions came back up after the second playtest
// ("the champions could be upped a bit in both frequency and health, and an additional level to them, like a red
// champion that is as rare as the current ones but holds a lot more HP, speed, etc."): the old 4-12% and 2x went to
// 6-18% and 2.5x, and red champions took the old rate.
constexpr double kChampionFirst = 0.06, kChampionLast = 0.18; // champion chance on a run's first and last map
constexpr double kChampionHealth = 2.5, kChampionSize = 1.25; // and 1.25x damage (ScaleDamage)
constexpr double kRedFirst = 0.04, kRedLast = 0.12;           // red champions: the rarer tier
constexpr double kRedHealth = 5.0, kRedSize = 1.4, kRedSpeed = 1.35; // and 1.5x damage; they don't flinch
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

//---------------------------------------------------------------------------
//
// the Forge's parts (docs/roguelite-forge.md): a Vessel (the gun), and one
// rune in each of three circles, Mouth (what it fires), Path (how it
// travels), Wound (what a hit does), and an optional Penance
//
//---------------------------------------------------------------------------

enum { kForgeVessel, kForgeMouth, kForgePath, kForgeWound, kForgePenance, kForgeSlots };
enum { kVesselFlare = 1, kVesselShotgun, kVesselTommy };
enum { kMouthHellmouth = 1, kMouthCleaver };
enum { kPathHound = 1, kPathChoir };
enum { kWoundPyre = 1, kWoundGrave };
enum { kPenanceBloodPrice = 1 };

// A Vessel's worth per round of its own ammo against a plain 50-health foe (ammoProfiles' damage, capped at the foe's
// health, times the hit rate), so a Mouth can be weighed against it: a heavier payload drinks more rounds a pull, a
// lighter one fires more projectiles a round.
struct ForgeVesselInfo
{
	const char* name;
	int weapon, ammo;
	FIntCVarRef* kit;  // its kit line: forging a gun puts it in the kit
	int kitCost;
	double worth;      // per round
	int stockMissile;  // its own round, if a missile (the flare), else 0
	bool slow;         // at least 0.3 s between pulls: may take a heavy Mouth
	const char* round; // its own round, in a sentence
};
const ForgeVesselInfo forgeVessels[] =
{
	{ "", 0, 0, nullptr, 0, 1, 0, false, "" },
	{ "Flare Gun", kWeapFlareGun, 1, &rl_kit_flare, 10, 40, kMissileFlareRegular, true, "a flare" },
	{ "Sawed-off", kWeapShotgun, 2, &rl_kit_shotgun, 20, 30, 0, true, "its pellets" },
	{ "Tommy Gun", kWeapTommyGun, 3, &rl_kit_tommy, 30, 7.2, 0, false, "a bullet" },
};

struct ForgeRuneInfo
{
	const char* name;
	const char* word; // in the gun's name: an adjective, or for a Wound "of the ..."
	int cost;         // a Penance's is a refund
	const char* says; // in the gun's sentence
};
const ForgeRuneInfo forgeMouths[] =
{
	{ "", "", 0, "" },
	{ "Hellmouth", "Hellmouth", 20, "napalm fireball" },
	{ "Cleaver's Psalm", "Cleaving", 15, "cleaver" },
};
const char* forgeMouthPlural[] = { "", "napalm fireballs", "cleavers" };
const int forgeMouthMissile[] = { 0, kMissileFireballNapalm, kMissileButcherKnife };
const double forgeMouthWorth[] = { 1, 52.5, 8.7 }; // napalm: 50 x 0.7 hit x 1.5 foes; cleaver: 14.5 spirit x 0.6
const bool forgeMouthHeavy[] = { false, true, false };
const bool forgeMouthBurns[] = { false, true, false };
const ForgeRuneInfo forgePaths[] =
{
	{ "", "", 0, "" },
	{ "Hound's Nose", "Hunting", 15, "hunting the foe ahead" },
	{ "Choir of Three", "Threefold", 20, "in a fan of three" },
};
const ForgeRuneInfo forgeWounds[] =
{
	{ "", "", 0, "" },
	{ "Pyre", "of the Pyre", 10, "Sets foes alight." },
	{ "Gravedigger's Blast", "of the Grave", 20, "Bursts where it lands; the blast reaches you too." },
};
const ForgeRuneInfo forgePenances[] =
{
	{ "", "", 0, "" },
	{ "Blood Price", "Blood-Priced", 15, "Dry, a pull costs 4 health." },
};
constexpr int kForgeMissiles = 8, kForgeVectors = 32; // most a pull fires
constexpr int kPyreBurn = 240;                       // burn time: 30 health over about 4 s
constexpr int kBloodPrice = 4;                       // health a dry pull costs
constexpr int kForgeHealPerSecond = 10;              // most the forge heals you a second
constexpr double kHoundCone = 25, kHoundRange = 1024; // degrees, world units
const int forgeSlotMax[kForgeSlots] = { 3, 2, 2, 2, 1 };

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
	int forge[kForgeSlots] = {};  // the Forge's recipe, as it was when the run started
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

// The Forge's runes, less a Penance's refund (which can't take the runes below 0: the Vessel's own kit price stands).
int ForgeCost(const int* f)
{
	if (!f[kForgeVessel]) return 0;
	int runes = forgeMouths[f[kForgeMouth]].cost + forgePaths[f[kForgePath]].cost + forgeWounds[f[kForgeWound]].cost;
	return std::max(runes - forgePenances[f[kForgePenance]].cost, 0);
}

// The recipe from the CVARs, each slot clamped to its table (a config from another build may hold more).
int ForgeClamp(int slot, int value) { return std::clamp(value, 0, forgeSlotMax[slot]); }

void ForgeRecipe(int* f)
{
	f[kForgeVessel] = ForgeClamp(kForgeVessel, rl_forge_vessel);
	f[kForgeMouth] = ForgeClamp(kForgeMouth, rl_forge_mouth);
	f[kForgePath] = ForgeClamp(kForgePath, rl_forge_path);
	f[kForgeWound] = ForgeClamp(kForgeWound, rl_forge_wound);
	f[kForgePenance] = ForgeClamp(kForgePenance, rl_forge_penance);
}

// The kit's points, the Forge's included.
int KitCost(const int* units = nullptr)
{
	int sum = 0;
	for (int i = 0; i < kNumKit; i++) sum += (units ? units[i] : int(*kitItems[i].cvar)) * kitItems[i].cost;
	if (!units)
	{
		int f[kForgeSlots];
		ForgeRecipe(f);
		sum += ForgeCost(f);
	}
	return sum;
}

// The points a run gets for its kit. A stock game reaches later episodes with the earlier ones' guns, so they get
// more: 100 for the first, 50 more for each later one up to 250; Cryptic Passage 200; the gauntlet, which starts at
// the first episode, 100. The Covenant's Austerity takes 20 a rank, but never the last 30, so a forged gun stays in
// reach at any Dread.
int Budget(int episode, int austerity)
{
	int points = episode == 0 ? 100 : LumpFilter.CompareNoCase("Blood.Cryptic") == 0 ? 200 : std::min(100 + 50 * (episode - 1), 250);
	return std::max(points - 20 * austerity, std::min(points, 30));
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

//---------------------------------------------------------------------------
//
// the Forge: what a recipe fires, its name, and which runes go together
//
//---------------------------------------------------------------------------

struct ForgeRates
{
	int perRound = 0; // payload projectiles a round of ammo (0: the gun's own round, as many as it fired)
	int drink = 1;    // rounds of ammo a pull's round costs
	bool missile = false;
};

ForgeRates Rates(const int* f)
{
	ForgeRates r;
	auto& v = forgeVessels[f[kForgeVessel]];
	int m = f[kForgeMouth];
	if (m)
	{
		double worth = forgeMouthWorth[m];
		r.perRound = std::clamp(int(v.worth / worth + 0.5), 1, kForgeMissiles);
		r.drink = std::max(int(worth / v.worth + 0.5), 1);
		r.missile = true;
	}
	else r.missile = v.stockMissile != 0;
	if (f[kForgePath] == kPathChoir)
	{
		r.drink *= 2;
		if (r.perRound) r.perRound = std::max(1, std::min(r.perRound, kForgeMissiles / 3)); // three of each still fit a pull
	}
	return r;
}

FString ForgeName(const int* f)
{
	if (!f[kForgeVessel]) return "no forged gun";
	FString s = "The ";
	if (f[kForgePenance]) s.AppendFormat("%s ", forgePenances[f[kForgePenance]].word);
	if (f[kForgePath]) s.AppendFormat("%s ", forgePaths[f[kForgePath]].word);
	if (f[kForgeMouth]) s.AppendFormat("%s ", forgeMouths[f[kForgeMouth]].word);
	s += forgeVessels[f[kForgeVessel]].name;
	if (f[kForgeWound]) s.AppendFormat(" %s", forgeWounds[f[kForgeWound]].word);
	return s;
}

// The menu's lines: what a pull fires and how it travels; what a hit does; what it drinks.
FString ForgeSentence(const int* f)
{
	if (!f[kForgeVessel]) return "Pick a Vessel, then carve its runes.";
	auto& v = forgeVessels[f[kForgeVessel]];
	auto r = Rates(f);
	FString s = f[kForgeVessel] == kVesselShotgun ? "Each barrel: " : "Each pull: ";
	if (f[kForgeMouth])
	{
		if (r.perRound > 1) s.AppendFormat("%d %s", r.perRound, forgeMouthPlural[f[kForgeMouth]]);
		else s.AppendFormat("a %s", forgeMouths[f[kForgeMouth]].says);
	}
	else s += v.round;
	if (f[kForgePath]) s.AppendFormat(", %s", forgePaths[f[kForgePath]].says);
	s += ".";
	return s;
}

FString ForgeEffectLine(const int* f)
{
	FString s;
	if (f[kForgeWound]) s += forgeWounds[f[kForgeWound]].says;
	if (f[kForgeMouth] == kMouthCleaver)
		s.AppendFormat("%sCleavers heal you 10 a hit, %d a second at most.", s.IsEmpty() ? "" : " ", kForgeHealPerSecond);
	return s;
}

FString ForgeAmmoLine(const int* f)
{
	if (!f[kForgeVessel]) return "";
	auto r = Rates(f);
	static const char* ammoOne[] = { "", "flare", "shell", "bullet" };
	static const char* ammoMany[] = { "", "flares", "shells", "bullets" };
	int a = forgeVessels[f[kForgeVessel]].ammo;
	FString s = FStringf("Drinks %d %s a %s.", r.drink, r.drink == 1 ? ammoOne[a] : ammoMany[a], f[kForgeVessel] == kVesselShotgun ? "barrel" : "pull");
	if (f[kForgePenance]) s.AppendFormat(" %s", forgePenances[f[kForgePenance]].says);
	return s;
}

// Why a recipe's rune in a slot can't stand (nullptr: it can).
const char* ForgeIllegal(const int* f, int slot)
{
	if (slot != kForgeVessel && f[slot] && !f[kForgeVessel]) return "pick a Vessel first";
	bool missiles = Rates(f).missile;
	switch (slot)
	{
	case kForgeMouth:
		if (f[kForgeMouth] && forgeMouthHeavy[f[kForgeMouth]] && !forgeVessels[f[kForgeVessel]].slow)
			return "too heavy for the tommy gun's rate of fire";
		break;
	case kForgePath:
		if (f[kForgePath] == kPathHound && !missiles) return "it needs missiles: a Mouth, or the flare gun";
		if (f[kForgePath] == kPathChoir && !f[kForgeMouth]) return "it needs a Mouth to sing with";
		break;
	case kForgeWound:
		if (f[kForgeWound] == kWoundPyre && ((f[kForgeVessel] == kVesselFlare && !f[kForgeMouth]) || forgeMouthBurns[f[kForgeMouth]]))
			return "the shot burns already";
		if (f[kForgeWound] == kWoundGrave && !missiles) return "it needs missiles: a Mouth, or the flare gun";
		break;
	case kForgePenance:
		if (f[kForgePenance] && !f[kForgeMouth] && !f[kForgePath] && !f[kForgeWound]) return "a Penance needs a rune to pay for";
		break;
	}
	return nullptr;
}

FString forgeNote; // why a rune was just taken off (the Forge menu shows it)

const char* RuneName(int slot, int value)
{
	switch (slot)
	{
	case kForgeMouth: return forgeMouths[value].name;
	case kForgePath: return forgePaths[value].name;
	case kForgeWound: return forgeWounds[value].name;
	case kForgePenance: return forgePenances[value].name;
	}
	return forgeVessels[value].name;
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
	if (run.forge[kForgeVessel]) Printf(PRINT_NOTIFY, "Your forged gun: %s\n", ForgeName(run.forge).GetChars());
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
	// Armour that would add nothing (already at its caps) isn't offered.
	auto addsArmour = [&](int item)
	{
		auto& a = armorData[item - kItemArmorBasic];
		return (a.armor0 && p->armor[0] < a.armor0max) || (a.armor1 && p->armor[1] < a.armor1max) || (a.armor2 && p->armor[2] < a.armor2max);
	};
	if (addsArmour(kItemArmorBody)) armour.Push({ kItemArmorBody, 3 });
	if (addsArmour(kItemArmorFire)) armour.Push({ kItemArmorFire, 2 });
	if (addsArmour(kItemArmorSpirit)) armour.Push({ kItemArmorSpirit, 2 });
	if (depth >= 0.5 && addsArmour(kItemArmorSuper)) armour.Push({ kItemArmorSuper, 1 });
	auto packFull = [&](int item) { return p->packSlots[gItemData[item - kItemBase].packSlot].curAmount >= 100; };
	if (!packFull(kItemHealthDoctorBag)) care.Push({ kItemHealthDoctorBag, 3 });
	// Healing only where it would heal: below the item's cap, and not under heavy Famine.
	int health = p->GetActor() ? int(p->GetActor()->xspr.health >> 4) : 0;
	bool famished = run.cov[kFamine] >= 3;
	if (!famished && health < gPowerUpInfo[kItemHealthLifeEssense - kItemBase].maxTime) care.Push({ kItemHealthLifeEssense, 2 });
	if (!famished && depth >= 0.3 && health < gPowerUpInfo[kItemHealthLifeSeed - kItemBase].maxTime) care.Push({ kItemHealthLifeSeed, 1 });
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

void MakeChampion(DBloodActor* a, bool grow = true, bool red = false)
{
	a->modFlags |= kModChampion | (red ? kModRedChampion : 0);
	a->xspr.health = uint32_t(a->xspr.health * (red ? kRedHealth : kChampionHealth));
	if (!grow) return;
	double size = red ? kRedSize : kChampionSize;
	a->spr.scale *= size;
	a->xspr.scale = int((a->xspr.scale ? a->xspr.scale : 256) * size); // SEQ frames keep the size
}

// The red champions' speed, for the base's modeActorSpeed (set as each map starts and on each load).
double RedSpeed(DBloodActor* a) { return a && (a->modFlags & kModRedChampion) ? kRedSpeed : 1.; }

// A wave enemy copies a kind alive on the map, its size too: a champion's size without its tier would mislead, so
// the copy keeps the stock look.
void WaveSpawned(DBloodActor* spawned, DBloodActor* kind)
{
	if (!spawned || !kind || !(kind->modFlags & kModChampion)) return;
	spawned->spr.scale /= (kind->modFlags & kModRedChampion) ? kRedSize : kChampionSize;
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

	// One roll each: a red champion, a champion, or neither. The Covenant's Champions multiplies both.
	double chance = (kChampionFirst + (kChampionLast - kChampionFirst) * depth) * (1 + run.cov[kChampions]);
	double red = (kRedFirst + (kRedLast - kRedFirst) * depth) * (1 + run.cov[kChampions]);
	for (auto a : eligible)
	{
		double r = Rand01(rng);
		if (r < red) MakeChampion(a, true, true);
		else if (r < red + chance) MakeChampion(a);
	}
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
		// Bosses and scripted enemies don't grow: a bigger Tchernobog could catch on a low ceiling.
		bool plain = bmod::ShuffleEligible(a, false) && a->xspr.txID == 0 && a->xspr.rxID == 0;
		if (wardens && !(a->modFlags & kModChampion)) MakeChampion(a, plain);
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
		TArray<DBloodActor*> kinds;
		if (!bmod::WaveKinds(kinds))
		{
			run.extraEnemies = 0; // a wave copies the kinds alive on the map: none left, no more Horde
			return;
		}
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
// ammo by the map: as a map starts, its own ammo (its pickups and what its
// enemies drop) is raised until the player's guns could kill its enemies
// rl_ammo_target percent of the way (150: one and a half times). First the
// pickups of those guns' ammo grow, then more pickups are added next to the
// map's own pickups and enemies; each secret area gets caches on top. The
// ammo the player carries isn't counted: the map pays for itself, and a map
// that wants to be fought has to be explored.
//
//---------------------------------------------------------------------------

constexpr int kAmmoTypes = 12;                    // Blood's ammo types, 0 (none) to 11
constexpr int kDamageNone = -1, kDamageAny = -2;  // kDamageAny: the life leech, a random type each shot

// What a unit of each ammo type is worth in a fight: a shot's damage by type, as Blood deals it, the ammo a shot uses,
// how often a shot lands, and how many enemies it hits. Rough by design: misses, overkill and burning are guessed,
// not simulated. scripts/bloodmaps.py's "ammo" census mirrors this table to compare the stock maps (keep in step).
struct AmmoProfile
{
	int ammo, gun;
	const char* name;
	int type1; double dmg1;
	int type2; double dmg2;
	int perShot;
	double hit, targets;
	bool byDamage; // the voodoo doll spends a unit per 4 damage it deals
	bool orElse;   // the flare: the second damage if the target takes it, else the first
};

const AmmoProfile ammoProfiles[] =
{
	{ 1, kWeapFlareGun, "flares", kDamageBullet, 24.5, kDamageBurn, 60, 1, 0.8, 1, false, true }, // 480 of burning, or 20-29 if it can't burn
	{ 2, kWeapShotgun, "shells", kDamageBullet, 72, kDamageNone, 0, 1, 0.6, 1, false, false },      // 16 pellets of 4 a barrel, 6 for both
	{ 3, kWeapTommyGun, "bullets", kDamageBullet, 12, kDamageNone, 0, 1, 0.6, 1, false, false },
	{ 4, kWeapNapalm, "gasoline", kDamageBullet, 75, kDamageExplode, 450, 1, 0.7, 1.5, false, false }, // 50-99, and the blast
	{ 5, kWeapDynamite, "TNT", kDamageExplode, 250, kDamageNone, 0, 1, 0.6, 1.5, false, false },
	{ 6, kWeapSpraycan, "spray", kDamageBurn, 3.5, kDamageNone, 0, 4, 0.8, 1, false, false },        // a lick of flame and its burning
	{ 7, kWeapTeslaCannon, "tesla charge", kDamageTesla, 19.5, kDamageNone, 0, 1, 0.75, 1, false, false },
	{ 8, kWeapLifeLeech, "trapped souls", kDamageAny, 10, kDamageBurn, 45, 1, 0.8, 1, false, false },
	{ 9, kWeapVoodooDoll, "voodoo", kDamageSpirit, 4, kDamageNone, 0, 1, 1, 1, true, false },
	{ 10, kWeapProximity, "proximity bombs", kDamageExplode, 250, kDamageNone, 0, 1, 0.8, 1.5, false, false },
	{ 11, kWeapRemote, "remote bombs", kDamageExplode, 250, kDamageNone, 0, 1, 0.8, 1.5, false, false },
};

const AmmoProfile* ProfileOfAmmo(int ammo)
{
	for (auto& p : ammoProfiles) if (p.ammo == ammo) return &p;
	return nullptr;
}

bool IsBomb(int gun) { return gun == kWeapDynamite || gun == kWeapProximity || gun == kWeapRemote; }

// The ammo type and amount a pickup gives (0 = no ammo).
int ItemAmmo(int item, int* count)
{
	*count = 0;
	if (item >= kItemWeaponBase && item < kItemWeaponMax)
	{
		auto& d = gWeaponItemData[item - kItemWeaponBase];
		if (d.ammoType <= 0 || d.ammoType >= kAmmoTypes) return 0;
		*count = d.count;
		return d.ammoType;
	}
	if (item >= kItemAmmoBase && item < kItemAmmoMax)
	{
		auto& d = gAmmoItemData[item - kItemAmmoBase];
		if (d.type <= 0 || d.type >= kAmmoTypes) return 0;
		*count = d.count;
		return d.type;
	}
	return 0;
}

// The ammo a pickup on the map holds: its own amount, once a run has set one.
int ActorAmmo(DBloodActor* a, int* count)
{
	int at = ItemAmmo(a->spr.type, count);
	if (at && (a->modFlags & kModBigPickup) && a->hasX()) *count = a->xspr.data1;
	return at;
}

// The ammo it takes one gun alone to kill an enemy of this type and health (0: that gun can't hurt it).
double AmmoToKill(const AmmoProfile& p, int type, double health)
{
	auto info = getDudeInfo(type); // damageVal: Blood's damage scale per damage type, for this skill
	auto scale = [&](int t)
	{
		if (t == kDamageAny)
		{
			double sum = 0;
			for (int k = 0; k < kDamageMax; k++) sum += info->damageVal[k];
			return sum / (256. * kDamageMax);
		}
		if (t == kDamageBurn) return std::floor(8. * info->damageVal[t] / 256.) / 8.; // in ticks of 8, rounded down
		return t >= 0 ? info->damageVal[t] / 256. : 0.;
	};
	double second = p.dmg2 * scale(p.type2);
	double shot = p.orElse && second > 0 ? second : p.dmg1 * scale(p.type1) + second;
	if (shot <= 0 || health <= 0) return 0;
	if (p.byDamage) return health / p.dmg1;
	return health * p.perShot / (std::min(shot, health) * p.hit * p.targets); // a shot can't do more than kill
}

// The guns the player has; on the run's first map, the kit's too (it's handed over after the remix).
void OwnedGuns(DBloodPlayer* p, bool* owned)
{
	for (int w = 0; w < kWeapMax; w++) owned[w] = p && p->hasWeapon[w];
	if (run.index == 0 && !run.kitGiven)
		for (int i = 0; i < kNumKit; i++)
			if (run.kit[i] > 0 && WeaponOf(kitItems[i].item)) owned[WeaponOf(kitItems[i].item)] = true;
}

struct AmmoLedger
{
	int enemies = 0;
	double health = 0;
	double need[kAmmoTypes] = {}; // the ammo each gun alone would need to kill every enemy it can hurt
	double hurt[kAmmoTypes] = {}; // the health of those enemies
	double have[kAmmoTypes] = {}; // lying on the map (at the pickups' own amounts), what enemies drop (at their odds), carried if counted
	bool counted = false;
	// What the run did to the map's ammo as it started (rl_ammo prints it): the map's own coverage at stock, with its
	// pickups raised, and with the caches in secrets; the ammo planned, and what found no room.
	double before = 0, after = 0, withSecrets = 0;
	int plan[kAmmoTypes] = {}, unplaced[kAmmoTypes] = {};
	int bigger = 0, added = 0, cached = 0, secretAreas = 0;
};
AmmoLedger ledger; // this map's, as it started

void AddEnemy(AmmoLedger& l, int type, double health)
{
	l.enemies++;
	l.health += health;
	for (auto& g : ammoProfiles)
	{
		double ammo = AmmoToKill(g, type, health);
		l.need[g.ammo] += ammo;
		if (ammo > 0) l.hurt[g.ammo] += health;
	}
}

// What an enemy or thing leaves when it dies: its drop item, and a cultist's gun or ammo at Blood's odds.
void AddDrops(AmmoLedger& l, DBloodActor* a)
{
	int n;
	if (a->hasX() && a->xspr.dropMsg > 0)
		if (int at = ItemAmmo(a->xspr.dropMsg, &n)) l.have[at] += n;
	if (a->spr.type == kDudeCultistTommy) l.have[3] += 0.10 * 50 + 0.40 * 15;
	if (a->spr.type == kDudeCultistShotgun) l.have[2] += 0.11 * 8 + 0.40 * 4;
}

void CountAmmo(AmmoLedger& l, DBloodPlayer* p, bool carried)
{
	l = AmmoLedger();
	l.counted = true;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || !AllowedKillType(a)) continue;
		AddEnemy(l, a->spr.type, a->xspr.health / 16.);
		AddDrops(l, a);
	}
	// The map's dude spawners (275 on the stock maps), each counted once.
	for (int stat : { kStatDecoration, kStatMarker })
	{
		BloodStatIterator it2(stat);
		while (auto a = it2.Next())
		{
			if (a->spr.type != kMarkerDudeSpawn || !a->hasX()) continue;
			int type = a->xspr.data1;
			if (type < kDudeBase || type >= kDudeMax || type == kDudeBat || type == kDudeRat || type == kDudeInnocent || type == kDudeBurningInnocent) continue;
			AddEnemy(l, type, getDudeInfo(type)->startHealth);
		}
	}
	// Horde's waves: that many more of the same mix.
	if (run.extraEnemies > 0 && l.enemies > 0)
	{
		double more = 1. + double(run.extraEnemies) / l.enemies;
		l.health *= more;
		for (auto& n : l.need) n *= more;
		for (auto& h : l.hurt) h *= more;
		l.enemies += run.extraEnemies;
	}
	if (carried) for (int at = 0; at < kAmmoTypes; at++) l.have[at] += p->ammoCount[at];
	BloodStatIterator it3(kStatItem);
	while (auto a = it3.Next())
	{
		int n;
		if (int at = ActorAmmo(a, &n)) l.have[at] += n;
	}
	BloodStatIterator it4(kStatThing); // crates and the like that drop an item
	while (auto a = it4.Next()) AddDrops(l, a);
}

// The share of the map's enemy health a gun can hurt (the spray can's flame does nothing to a phantasm).
double Reach(const AmmoLedger& l, int ammo) { return l.health > 0 ? l.hurt[ammo] / l.health : 0; }

// How many times over the player's guns can kill the map's enemies with the ammo counted (plus extra, if given):
// each gun's ammo over the ammo it needs for what it can hurt, weighed by that share of the map.
double Coverage(const AmmoLedger& l, const bool* owned, const int* extra = nullptr)
{
	double c = 0;
	for (auto& g : ammoProfiles)
		if (owned[g.gun] && l.need[g.ammo] > 0) c += (l.have[g.ammo] + (extra ? extra[g.ammo] : 0)) / l.need[g.ammo] * Reach(l, g.ammo);
	return c;
}

int HudAmount(const AmmoProfile& g, double units) { return int(g.ammo == 6 ? units / 10 + 0.5 : units + 0.5); } // the HUD counts spray in tens
FString AmmoText(const AmmoProfile& g, int units) { return FStringf("%d %s", std::max(HudAmount(g, units), units > 0 ? 1 : 0), g.name); }

constexpr double kBigPickup = 2.0;  // a run's pickup holds up to twice its stock amount (and half a pocket, unless it's more)
constexpr double kBigLook = 1.25;   // a pickup holding more than its stock amount is drawn a quarter bigger
constexpr int kSecretCaches = 2;    // pickups added to each secret area, on top of the target
constexpr int kMaxSecretAreas = 16;
constexpr double kSpotClear = 16, kSpotSpacing = 24, kSpotStepZ = 24; // world units

// The ammo to add to the map, by type: the shortfall from the target, shared by the guns that can hurt the map's
// enemies, each by the share of the map it can hurt (the three bombs together count as one gun).
void PlanAmmo(const bool* owned, int* plan)
{
	double missing = rl_ammo_target / 100. - ledger.before;
	if (missing <= 0) return;
	int bombs = 0;
	for (auto& g : ammoProfiles) if (owned[g.gun] && ledger.need[g.ammo] > 0 && IsBomb(g.gun)) bombs++;
	double weights = 0;
	for (auto& g : ammoProfiles) if (owned[g.gun] && ledger.need[g.ammo] > 0) weights += Reach(ledger, g.ammo) / (IsBomb(g.gun) ? bombs : 1);
	if (weights <= 0) return;
	for (auto& g : ammoProfiles)
	{
		if (!owned[g.gun] || ledger.need[g.ammo] <= 0) continue;
		double reach = Reach(ledger, g.ammo);
		double part = missing * reach / (IsBomb(g.gun) ? bombs : 1) / weights; // in coverage
		plan[g.ammo] = int(std::ceil(part / reach * ledger.need[g.ammo]));
	}
}

int BigAmount(int stock, int ammo) { return std::max(stock, std::min(int(stock * kBigPickup), gAmmoInfo[ammo].max / 2)); }

// The ammo item of a type that holds the most (shells: the box; bullets: the drum), and its amount.
int BiggestItem(int ammo, int* count)
{
	int best = 0;
	*count = 0;
	for (int item = kItemAmmoBase; item < kItemAmmoMax; item++)
	{
		auto& d = gAmmoItemData[item - kItemAmmoBase];
		if (d.type == ammo && d.count > *count && d.textureID().isValid())
		{
			best = item;
			*count = d.count;
		}
	}
	return best;
}

// A pickup's own amount; holding more than its stock amount, it's drawn bigger, still resting on the floor.
void SetAmount(DBloodActor* a, int amount, int stock)
{
	if (!a->hasX()) return;
	a->modFlags |= kModBigPickup;
	a->xspr.data1 = int16_t(std::min(amount, 32767));
	if (amount <= stock) return;
	double top, bottom, newTop, newBottom;
	GetActorExtents(a, &top, &bottom);
	a->spr.scale *= kBigLook;
	GetActorExtents(a, &newTop, &newBottom);
	a->spr.pos.Z += bottom - newBottom;
	a->basePoint.Z += bottom - newBottom;
	a->interpolated = false;
	viewBackupSpriteLoc(a);
}

// A sector a pickup can lie in: not a door, lift, moving floor, teleporter or damage sector, not water, lava, sky,
// a stacked room or a drag, crush or bobbing floor, and high enough to stand in.
bool SpotSectorOk(sectortype* s, double minHeight = 40)
{
	if (!s || (s->floorstat & CSTAT_SECTOR_SKY)) return false;
	if (s->type >= kSectorBase && s->type < kSectorMax) return false;
	if (s->upperLink != nullptr || s->lowerLink != nullptr) return false;
	if (GetExtInfo(s->floortexture).surftype == kSurfLava) return false;
	if (s->hasX())
	{
		auto& x = s->xs();
		if (x.Underwater || x.Depth || x.damageType || x.Drag || x.Crush || (x.bobSpeed && x.bobFloor)) return false;
	}
	return s->floorz - s->ceilingz >= minHeight;
}

// A spot for a pickup: inside a good sector, at about the anchor's floor height, clear of walls and ledges, and not
// on another pickup, a thing or an enemy. Loose (a secret's cache): a niche 24 high will do, nearer walls and items.
bool SpotOk(sectortype* s, const DVector2& p, double anchorZ, const TArray<DVector2>& taken, bool loose = false)
{
	const double clear = loose ? 8 : kSpotClear, spacing = loose ? 12 : kSpotSpacing;
	if (!SpotSectorOk(s, loose ? 24 : 40) || !inside(p.X, p.Y, s)) return false;
	double fz = getflorzofslopeptr(s, p.X, p.Y);
	if (std::abs(fz - anchorZ) > kSpotStepZ) return false;
	for (auto& w : s->walls)
	{
		auto ns = w.nextSector();
		bool hard = !ns || (ns->type >= kSectorBase && ns->type < kSectorMax) || std::abs(getflorzofslopeptr(ns, p.X, p.Y) - fz) > 16;
		if (hard && SquareDistToWall(p.X, p.Y, &w) < clear * clear) return false;
	}
	for (auto& t : taken) if ((t - p).LengthSquared() < spacing * spacing) return false;
	BloodSectIterator it(s);
	while (auto a = it.Next())
	{
		bool item = a->spr.statnum == kStatItem;
		bool solid = item || (a->spr.cstat & CSTAT_SPRITE_BLOCK) || a->spr.statnum == kStatThing || a->spr.statnum == kStatDude;
		double r = item ? spacing : a->clipdist + clear;
		if (solid && (a->spr.pos.XY() - p).LengthSquared() < r * r) return false;
	}
	return true;
}

// A spot minD-maxD from an anchor (on its floor), in sight of it, so the pickup isn't through a wall; with only, in
// that sector (a secret's cache stays in the secret).
bool FindItemSpot(std::mt19937& rng, sectortype* aSect, const DVector3& anchor, double minD, double maxD,
	const TArray<DVector2>& taken, DVector2& out, sectortype*& outSect, sectortype* only = nullptr, int tries = 16)
{
	DVector3 eye(anchor.XY(), anchor.Z - 16);
	for (int k = 0; k < tries; k++)
	{
		DVector2 p = anchor.XY() + DAngle::fromDeg(Rand01(rng) * 360.).ToVector() * (minD + (maxD - minD) * Rand01(rng));
		auto s = aSect;
		updatesector(p, &s);
		if (!s || (only && s != only)) continue;
		if (!cansee(eye, aSect, DVector3(p, getflorzofslopeptr(s, p.X, p.Y) - 16), s)) continue;
		if (!SpotOk(s, p, anchor.Z, taken, only != nullptr)) continue;
		out = p;
		outSect = s;
		return true;
	}
	return false;
}

// A new ammo pickup lying on the floor, like the ones a map places (or an enemy drops).
DBloodActor* SpawnPickup(sectortype* s, const DVector2& p, int type)
{
	if (type < kItemAmmoBase || type >= kItemAmmoMax) return nullptr;
	auto& d = gAmmoItemData[type - kItemAmmoBase];
	if (d.type <= 0 || d.type >= kAmmoTypes || !d.textureID().isValid()) return nullptr;
	double fz = getflorzofslopeptr(s, p.X, p.Y);
	auto a = actSpawnSprite(s, DVector3(p, fz), kStatItem, true); // an xsprite, so its amount and mark are saved
	if (!a) return nullptr;
	a->spr.cstat &= ~CSTAT_SPRITE_BLOCK_ALL;
	a->spr.type = a->spr.inittype = type;
	a->spr.setspritetexture(d.textureID());
	a->spr.shade = d.shade;
	a->spr.pal = d.pal;
	a->spr.scale = DVector2(d.xrepeat * REPEAT_SCALE, d.yrepeat * REPEAT_SCALE);
	double top, bottom;
	GetActorExtents(a, &top, &bottom);
	a->spr.pos.Z -= bottom - fz;
	a->basePoint = a->spr.pos;
	a->interpolated = false;
	viewBackupSpriteLoc(a);
	return a;
}

DVector3 FloorAt(DBloodActor* a) { return DVector3(a->spr.pos.XY(), getflorzofslopeptr(a->sector(), a->spr.pos.X, a->spr.pos.Y)); }

// Caches in the map's secret areas, on top of the target: each gets kSecretCaches of the biggest pickups, for the
// player's guns by their reach. A secret area is a sector that reports a secret, or one holding an item that does.
void PlaceSecretCaches(std::mt19937& rng, const bool* owned, TArray<DVector2>& taken)
{
	int bombs = 0;
	for (auto& g : ammoProfiles) if (owned[g.gun] && ledger.need[g.ammo] > 0 && IsBomb(g.gun)) bombs++;
	TArray<RewardPick> guns;
	for (auto& g : ammoProfiles)
		if (owned[g.gun] && ledger.need[g.ammo] > 0)
			guns.Push({ g.ammo, std::max(1, int(100 * Reach(ledger, g.ammo) / (IsBomb(g.gun) ? bombs : 1))) });
	if (guns.Size() == 0) return;
	TArray<sectortype*> areas;
	for (auto& sect : sector)
		if (sect.hasX() && sect.xs().txID == kChannelSecretFound && sect.xs().command >= kCmdNumberic) areas.Push(&sect);
	BloodStatIterator it(kStatItem);
	while (auto a = it.Next())
		if (a->hasX() && a->xspr.txID == kChannelSecretFound && a->insector() && areas.Find(a->sector()) == areas.Size()) areas.Push(a->sector());
	for (unsigned i = 0; i < areas.Size() && i < unsigned(kMaxSecretAreas); i++)
	{
		auto s = areas[i];
		// The anchor: a pickup in the area, or the middle of its floor.
		DVector3 anchor;
		bool found = false;
		BloodSectIterator si(s);
		while (auto a = si.Next())
		{
			if (a->spr.statnum != kStatItem) continue;
			anchor = FloorAt(a);
			found = true;
			break;
		}
		if (!found)
		{
			if (s->walls.Size() == 0) continue;
			DVector2 c(0, 0);
			for (auto& w : s->walls) c += w.pos;
			c /= double(s->walls.Size());
			if (!inside(c.X, c.Y, s)) continue;
			anchor = DVector3(c, getflorzofslopeptr(s, c.X, c.Y));
		}
		int placed = 0;
		for (int k = 0; k < kSecretCaches; k++)
		{
			int ammo = PickWeighted(rng, guns), stock;
			int item = ammo > 0 ? BiggestItem(ammo, &stock) : 0;
			DVector2 spot;
			sectortype* sect;
			if (!item || !FindItemSpot(rng, s, anchor, 0, 64, taken, spot, sect, s, 48)) continue;
			auto a = SpawnPickup(sect, spot, item);
			if (!a) continue;
			taken.Push(spot);
			SetAmount(a, BigAmount(stock, ammo), stock);
			ledger.cached++;
			placed++;
		}
		if (placed) ledger.secretAreas++;
	}
}

// Raises the map's own ammo to the target (the ledger is counted, without what the player carries). First the map's
// unwired pickups of the short ammo grow, each by a share of its room; then the rest comes as more pickups, of the
// type's biggest kind, next to the map's own pickups and enemies (round the map, two at most by each); then the
// caches in secrets. Uses the remix's rng, so a run's maps come out the same every time.
void PlaceAmmo(std::mt19937& rng, const bool* owned)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !ledger.counted) return;
	ledger.before = Coverage(ledger, owned);
	if (rl_ammo_target <= 0) return; // stock amounts: no bigger pickups, no more, no caches
	PlanAmmo(owned, ledger.plan);
	int left[kAmmoTypes];
	for (int at = 0; at < kAmmoTypes; at++) left[at] = ledger.plan[at];

	// The pickups a run may change (unwired; on a modern map, not one with an amount of its own) and the anchors.
	TArray<DBloodActor*> pickups, anchors;
	BloodStatIterator it(kStatItem);
	while (auto a = it.Next())
	{
		if (!a->hasX() || a->xspr.txID != 0 || a->xspr.rxID != 0 || !a->insector()) continue;
		anchors.Push(a);
		int n;
		if (ItemAmmo(a->spr.type, &n) && !(gModernMap && a->xspr.data1 > 0)) pickups.Push(a);
	}

	// Bigger: each pickup of a short ammo takes a share of what's missing, by the room it has (dithered, so bundles of
	// one stick still add up).
	for (int at = 1; at < kAmmoTypes; at++)
	{
		if (left[at] <= 0) continue;
		double room = 0;
		for (auto a : pickups)
		{
			int n;
			if (ItemAmmo(a->spr.type, &n) == at) room += BigAmount(n, at) - n;
		}
		if (room <= 0) continue;
		double share = std::min(1., left[at] / room), acc = 0;
		int placed = 0;
		for (auto a : pickups)
		{
			int n;
			if (ItemAmmo(a->spr.type, &n) != at) continue;
			acc += (BigAmount(n, at) - n) * share;
			int add = int(acc + 0.5) - placed;
			if (add <= 0) continue;
			placed += add;
			SetAmount(a, n + add, n);
			ledger.bigger++;
		}
		left[at] -= placed;
	}

	// More: next to the map's pickups, then the enemies standing on a floor (each in a shuffled round; pickups first, as
	// an enemy can stand where the player can't go).
	for (int i = int(anchors.Size()) - 1; i > 0; i--) std::swap(anchors[i], anchors[RandInt(rng, 0, i)]);
	int itemAnchors = int(anchors.Size());
	BloodStatIterator di(kStatDude);
	while (auto a = di.Next())
	{
		if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || !a->insector()) continue;
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		if (std::abs(FloorAt(a).Z - bottom) <= 8) anchors.Push(a); // not the flyers and swimmers
	}
	for (int i = int(anchors.Size()) - 1; i > itemAnchors; i--) std::swap(anchors[i], anchors[RandInt(rng, itemAnchors, i)]);
	TArray<DVector2> taken;
	unsigned next = 0;
	for (int at = 1; at < kAmmoTypes; at++)
	{
		int stock, item = BiggestItem(at, &stock);
		while (left[at] > 0 && item && next < anchors.Size() * 2)
		{
			auto anchor = anchors[next++ % anchors.Size()];
			DVector2 spot;
			sectortype* sect;
			if (!FindItemSpot(rng, anchor->sector(), FloorAt(anchor), 24, 64, taken, spot, sect)) continue;
			auto a = SpawnPickup(sect, spot, item);
			if (!a) continue;
			taken.Push(spot);
			int amount = std::max(std::min(left[at], BigAmount(stock, at)), stock);
			SetAmount(a, amount, stock);
			left[at] -= amount;
			ledger.added++;
		}
		ledger.unplaced[at] = std::max(left[at], 0);
	}
	AmmoLedger now;
	CountAmmo(now, p, false);
	ledger.after = Coverage(now, owned);

	PlaceSecretCaches(rng, owned, taken);
	CountAmmo(now, p, false);
	ledger.withSecrets = Coverage(now, owned);
	if (ledger.bigger || ledger.added || ledger.cached)
		Printf(PRINT_NOTIFY, "Ammo on this map: %d pickups bigger, %d more, %d caches in secret areas\n", ledger.bigger, ledger.added, ledger.cached);
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

// Ammo that fits a gun the player has or can find on the map; ammo that brings its own gun (the spray can, the
// voodoo doll) always does.
bool AmmoFits(int item, const bool* usable)
{
	if (WeaponOf(item)) return true;
	int n;
	auto g = ProfileOfAmmo(ItemAmmo(item, &n));
	return g && usable[g->gun];
}

int PickItem(std::mt19937& rng, const ItemCategory* cat, const bool* usable)
{
	auto weight = [&](int i) { return cat->picks[i].weight * (usable && AmmoFits(cat->picks[i].type, usable) ? 4 : 1); };
	int total = 0;
	for (int i = 0; i < cat->count; i++) total += weight(i);
	int r = RandInt(rng, 0, total - 1);
	for (int i = 0; i < cat->count; i++)
	{
		if (r < weight(i)) return cat->picks[i].type;
		r -= weight(i);
	}
	return cat->picks[0].type;
}

void RemixItems(std::mt19937& rng, const bool* owned)
{
	TArray<DBloodActor*> items, ammo;
	BloodStatIterator it(kStatItem);
	while (auto a = it.Next()) items.Push(a);
	bool usable[kWeapMax]; // the guns the player has, or can find on this map once it's rerolled
	for (int w = 0; w < kWeapMax; w++) usable[w] = owned[w];
	for (auto a : items)
	{
		// Keys, flags, quest items and anything unknown stay. So does an item wired into the map's script: picking it
		// up sends a message (E1M6's fire armour opens a door), or a trigger acts on it. Blood sends the message only
		// once the item is taken, which a reroll into something the player can't take yet would hold back. A "secret
		// found" message only counts the secret. 12 such items on the stock maps.
		auto cat = CategoryOf(a->spr.type);
		bool scripted = a->hasX() && ((a->xspr.txID != 0 && a->xspr.txID != kChannelSecretFound) || a->xspr.rxID != 0);
		if (cat && !scripted)
		{
			if (cat->picks == ammoPicks)
			{
				ammo.Push(a);
				continue;
			}
			RetypeItem(a, PickItem(rng, cat, nullptr));
		}
		if (int w = WeaponOf(a->spr.type)) usable[w] = true;
	}
	// Ammo last, leaning to those guns: their ammo weighs 4x. scripts/bloodmaps.py's census (the stock maps, a shotgun
	// and tommy gun kit, the median map): the stock pickups could kill the map's enemies 0.75 times over with those two
	// guns, 0.57 after an even reroll, 0.70 leaning 4x. A stock player, with every gun found so far, has 2.3.
	for (auto a : ammo) RetypeItem(a, PickItem(rng, CategoryOf(a->spr.type), usable));
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
	bool owned[kWeapMax];
	OwnedGuns(getPlayer(myconnectindex), owned);
	RemixItems(rng, owned);
	PickCurse(rng);
	SetupPressure();
	ledger = AmmoLedger();
	if (auto p = getPlayer(myconnectindex))
	{
		CountAmmo(ledger, p, false); // the map's own ammo, after the remix and Horde's count (what's carried isn't counted)
		PlaceAmmo(rng, owned);
	}
	if (run.index == 0)
	{
		if (!run.kitGiven) GiveKit();
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
	bmod::modeEnemyTimeScale = 1. + 0.15 * run.cov[kHaste]; // from the first tic (the ticker keeps it after)
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
	ForgeRecipe(run.forge);
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
constexpr double kUseRange = 48;    // world units (16 map units) from a feature's marker: about one bay
constexpr double kLabelRange = 64;  // a portal's name shows within this (the alcoves are 40 apart)
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
FString cryptMessage; // shown once the Crypt is up (a run's result)

bool CryptAvailable() { return fileSystem.FileExists(kCryptMap); }
bool IsCryptLevel() { return currentLevel && currentLevel->fileName.CompareNoCase(kCryptMap) == 0; }

bool EnterCrypt()
{
	if (!isBlood() || numplayers > 1 || !CryptAvailable()) return false;
	auto map = SetupUserMap(kCryptMap);
	if (!map) return false;
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
	// Locked and unused portals go dark; open ones get rlcrypt.py's alcove lighting back (an older save of the
	// Crypt may hold one dark that has opened since). Keep these in step with the generator.
	TArray<Episode> eps;
	GetEpisodes(eps);
	auto vortex = tileGetTextureID(1019);
	for (int i = 0; i < kNumPortals; i++)
	{
		auto sect = crypt.portal[i];
		if (!sect) continue;
		bool open = PortalOpen(i, int(eps.Size()));
		sect->floorshade = open ? 0 : 48;
		sect->ceilingshade = open ? 4 : 48;
		for (auto& wal : sect->walls) wal.shade = !open ? 48 : wal.walltexture == vortex ? -20 : 4;
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
		bmod::modeActorSpeed = RedSpeed; // only actors a run marked are faster
		bmod::modeWaveSpawned = WaveSpawned;
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
		LevelStarted();
		break;
	case bmod::Event::SaveLoaded:
		bmod::modeActorSpeed = RedSpeed;
		bmod::modeWaveSpawned = WaveSpawned;
		nextCurseTry = 0;
		ledger = AmmoLedger(); // rl_ammo's "at the start" belongs to the map as it started, not to this load
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
		if (run.active && !run.over) StartCurse(); // effects aren't saved: the curse comes back on
		break;
	case bmod::Event::Kill:
		if (run.active && !run.over && AllowedKillType(ev.actor))
		{
			bool champion = (ev.actor->modFlags & kModChampion) != 0, red = (ev.actor->modFlags & kModRedChampion) != 0;
			run.kills++;
			if (champion) run.champions++;
			run.ichor += red ? kIchorRed : champion ? kIchorChampion : kIchorKill;
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
	if (run.offerPending && gamestate == GS_LEVEL && !M_Active() && PlayClock - run.levelClock >= kClock / 2)
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
DMenuItemBase* CreateRunItem(const char* label, const char* command, bool confirm,
	const char* prompt = "This abandons the run in progress: its saves stop loading.\nStart a new run?")
{
	auto cls = confirm ? PClass::FindClass("OptionMenuItemSafeCommand") : nullptr;
	auto init = cls ? dyn_cast<PFunction>(cls->FindSymbol("Init", false)) : nullptr;
	if (!init) return CreateOptionMenuItemCommand(label, FName(command));
	auto item = cls->CreateNew();
	FString labelStr = label, promptStr = prompt;
	// One tic later: the yes/no box closes after running the command, and would take a message the command
	// opens (a kit over the run's points) down with it.
	FStringf delayed("wait 1; %s", command);
	VMValue params[] = { item, &labelStr, FName(delayed.GetChars()).GetIndex(), &promptStr };
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
	if (CryptAvailable() && !(crypt.in && gamestate == GS_LEVEL))
	{
		first = int(opt->mItems.Size());
		add(CreateRunItem("Enter the Crypt: the Armory, the Covenant, the portals", "rl_crypt", run.active && !run.over && gamestate == GS_LEVEL,
			"This leaves the run in progress (it can be continued from its saves).\nGo to the Crypt?"));
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
	if (auto m = FindOptionMenu("RogueliteForge"))
	{
		int f[kForgeSlots];
		ForgeRecipe(f);
		m->mTitle.Format("The Forge: %d points (kit and Forge %d)", ForgeCost(f), KitCost());
		if (m->mItems.Size() > 4)
		{
			SetLabel(m->mItems[0], ForgeName(f));
			SetLabel(m->mItems[1], ForgeSentence(f));
			SetLabel(m->mItems[2], ForgeEffectLine(f));
			SetLabel(m->mItems[3], ForgeAmmoLine(f));
			SetLabel(m->mItems[4], forgeNote);
		}
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

// The Forge's state at work (its functions are further down, after the champion hooks).
struct ForgeAim { bool missile; int type; double off, zoff; DVector3 dv; };
static TArray<ForgeAim>* forgeShots;   // set while a forged gun's stock fire function runs
static bool forgeEmitting;             // the Forge's own shots pass
static TArray<DBloodActor*> forgeHits; // what the Forge's hitscans hit this pull (for the Pyre)
static int forgeImpacts;               // inside a forged missile's impact: its healing is capped
static int forgeHealSecond = -1, forgeHealed;

//---------------------------------------------------------------------------
//
// a run's pickups holding their own amounts (PlaceAmmo): what the pocket
// can't hold stays on the floor, and the message says how much was taken
//
//---------------------------------------------------------------------------

static DBloodActor* lastBig; // the pickup TakePickup just gave from, for PickupTaken in the same PickUp
static int lastTake;

int TakePickup(DBloodActor* item, int room)
{
	lastBig = nullptr;
	if (!item || !(item->modFlags & kModBigPickup) || !item->hasX() || item->xspr.data1 <= 0) return -1;
	// Taken only when the pocket has room for a stock pickup's worth (or what's left, if less): otherwise firing next to
	// a pickup that's lying there half taken would take a round at a time, with the sound and flash each shot.
	int stock;
	ItemAmmo(item->spr.type, &stock);
	int held = item->xspr.data1;
	int take = std::min(held, std::max(room, 0));
	if (take < std::min(held, std::max(stock, 1))) take = 0;
	item->xspr.data1 -= take;
	lastBig = item;
	lastTake = take;
	return take;
}

bool PickupTaken(DBloodActor* item, const char*& msg)
{
	if (!item || item != lastBig || !(item->modFlags & kModBigPickup) || !item->hasX()) return false;
	lastBig = nullptr;
	int n;
	auto g = ProfileOfAmmo(ItemAmmo(item->spr.type, &n));
	static FString text; // the message is printed at once
	if (g && lastTake > 0)
	{
		bool gun = item->spr.type >= kItemWeaponBase && item->spr.type < kItemWeaponMax;
		if (gun && msg) text.Format("%s, %s", msg, AmmoText(*g, lastTake).GetChars());
		else text.Format("Picked up %s", AmmoText(*g, lastTake).GetChars());
		if (item->xspr.data1 > 0) text.AppendFormat(" (%d left)", std::max(HudAmount(*g, item->xspr.data1), 1));
		msg = text.GetChars();
	}
	return item->xspr.data1 > 0;
}

int ScaleDamage(DBloodActor* source, DBloodActor* target, int damage)
{
	if (!source || !target || damage <= 0) return damage;
	if (forgeEmitting && source->IsPlayerActor() && target != source && target->IsDudeActor() && !target->IsPlayerActor())
		if (forgeHits.Find(target) == forgeHits.Size()) forgeHits.Push(target); // a forged hitscan's hit
	auto s = source;
	for (int i = 0; i < 2 && s && !s->IsDudeActor(); i++) s = s->GetOwner(); // a missile's or explosion's owner
	if (!s || s == target || !s->IsDudeActor() || s->IsPlayerActor()) return damage;
	if (s->modFlags & kModRedChampion) damage += damage / 2;  // red champions hit 1.5x
	else if (s->modFlags & kModChampion) damage += damage / 4; // champions 1.25x
	if (run.active && !run.over && target->IsPlayerActor() && run.cov[kWrath] > 0)
		damage = int(damage * (1. + 0.2 * run.cov[kWrath]) + 0.5); // the Covenant's Wrath
	return damage;
}

bool Unflinching(DBloodActor* actor) { return actor && (actor->modFlags & kModRedChampion); }

//---------------------------------------------------------------------------
//
// the Forge at work: the stock fire function runs as ever (hands, sounds,
// ammo, timing), its shots are swallowed with their aims recorded, and the
// recipe's payload goes out along those aims
//
//---------------------------------------------------------------------------


// The run's forged gun, when it's the one in hand.
static bool ForgeOn(DBloodPlayer* p)
{
	int v = run.forge[kForgeVessel];
	return v && run.active && !run.over && !crypt.in && p && p == getPlayer(myconnectindex) && p->GetActor() && p->curWeapon == forgeVessels[v].weapon;
}

static bool PrimaryCallback(DBloodPlayer* p, int vessel, int nCallback)
{
	if (vessel == kVesselShotgun) // alt fire shares FireShotgun: both barrels (and the akimbo pair) stay stock
		return nCallback == nClientFireShotgun && p->weaponQav != qavGetCorrectID(kQAVSHOTF3) && p->weaponQav != qavGetCorrectID(kQAV2SHOTFIR);
	return (vessel == kVesselFlare && nCallback == nClientFireFlare) || (vessel == kVesselTommy && nCallback == nClientFireTommy);
}

static bool FiringCallback(int vessel, int nCallback)
{
	return (vessel == kVesselFlare && (nCallback == nClientFireFlare || nCallback == nClientAltFireFlare)) || (vessel == kVesselShotgun && nCallback == nClientFireShotgun)
		|| (vessel == kVesselTommy && (nCallback == nClientFireTommy || nCallback == nClientFireSpread || nCallback == nClientAltFireSpread || nCallback == nClientAltFireSpread2));
}

static bool BloodPriced() { return run.forge[kForgePenance] == kPenanceBloodPrice; }

// Health enough to pay the Blood Price and live (health is kept in sixteenths).
static bool CanPayInBlood(DBloodPlayer* p)
{
	auto a = p ? p->GetActor() : nullptr;
	return a && a->hasX() && a->xspr.health > unsigned((kBloodPrice << 4) + 16);
}

// Taken from health directly, as the dry life leech feeds on you, but never to death and never scaled by skill.
static bool PayInBlood(DBloodPlayer* p)
{
	if (!CanPayInBlood(p)) return false;
	p->GetActor()->xspr.health -= kBloodPrice << 4;
	return true;
}

// The Hound's Nose: the nearest foe within the cone ahead, in sight.
static DBloodActor* HoundTarget(DBloodPlayer* p, const DVector3& aim)
{
	auto pl = p->GetActor();
	DVector3 eye(pl->spr.pos.XY(), p->zWeapon);
	DAngle ahead = aim.XY().Angle();
	DBloodActor* best = nullptr;
	double bestDist = kHoundRange;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || !AllowedKillType(a) || !a->insector()) continue;
		DVector2 d = a->spr.pos.XY() - eye.XY();
		double dist = d.Length();
		if (dist >= bestDist || absangle(d.Angle(), ahead).Degrees() > kHoundCone) continue;
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		if (!cansee(eye, pl->sector(), DVector3(a->spr.pos.XY(), (top + bottom) / 2), a->sector())) continue;
		best = a;
		bestDist = dist;
	}
	return best;
}

static void EmitForge(DBloodPlayer* p, const TArray<ForgeAim>& shots, int id, int spent)
{
	auto pl = p->GetActor();
	int vessel = run.forge[kForgeVessel];
	auto& v = forgeVessels[vessel];
	auto r = Rates(run.forge);
	int mouth = run.forge[kForgeMouth];
	int fan = run.forge[kForgePath] == kPathChoir ? 3 : 1;
	// The rounds this callback fires: the flare one (its id is the akimbo hand), the sawed-off and tommy their trigger.
	int units = vessel == kVesselFlare ? 1 : std::max(id, 1);
	int perRound = mouth ? std::max(1, std::min(r.perRound, kForgeMissiles / (units * fan))) : 0; // whole fans within the cap

	// The ammo: the stock function took a round a round fired; the rest of the drink is taken here. Short, the payload
	// shrinks to what was paid for; dry with the Blood Price, the pull is paid in health (or not fired at all).
	double share = 1;
	if (!gInfiniteAmmo)
	{
		int owed = units * r.drink, paid = std::max(spent, 0);
		int extra = std::min(std::max(owed - paid, 0), p->ammoCount[v.ammo]);
		if (extra > 0) UseAmmo(p, v.ammo, extra);
		paid += extra;
		if (paid < owed)
		{
			if (BloodPriced() && PayInBlood(p)) share = 1;
			else if (paid > 0) share = double(paid) / owed;
			else return; // nothing paid: nothing fires
		}
	}

	DBloodActor* target = run.forge[kForgePath] == kPathHound && r.missile ? HoundTarget(p, shots[0].dv) : nullptr;
	TArray<ForgeAim> out;
	if (mouth)
	{
		int count = std::max(1, int(perRound * units * share)); // something paid: at least one
		for (int i = 0; i < count; i++)
		{
			auto s = shots[i % shots.Size()];
			s.missile = true;
			s.type = forgeMouthMissile[mouth];
			out.Push(s);
		}
	}
	else
	{
		int count = std::max(1, int(shots.Size() * share + 0.5));
		for (int i = 0; i < count; i++) out.Push(shots[i]);
	}
	if (out.Size() == 0) return;
	if (target) // aim at the foe, so the hunt starts on it (the stock homing only steers sideways and down)
	{
		double top, bottom;
		GetActorExtents(target, &top, &bottom);
		DVector3 at(target->spr.pos.XY(), (top + bottom) / 2);
		DVector3 from(pl->spr.pos.XY(), p->zWeapon);
		for (auto& s : out) if (s.missile) s.dv = (at - from).Unit();
	}
	if (fan > 1)
	{
		TArray<ForgeAim> fanned;
		for (auto& s : out)
			for (double a : { -10., 0., 10. })
			{
				auto t = s;
				t.dv = DVector3(s.dv.XY().Rotated(DAngle::fromDeg(a)), s.dv.Z);
				fanned.Push(t);
			}
		out = std::move(fanned);
	}

	int missiles = 0, vectors = 0;
	forgeHits.Clear();
	forgeEmitting = true;
	for (auto& s : out)
	{
		if (s.missile)
		{
			if (missiles++ >= kForgeMissiles) continue;
			auto m = actFireMissile(pl, s.off, s.zoff, s.dv, s.type);
			if (!m) continue;
			m->modFlags |= kModForged;
			if (target)
			{
				m->SetTarget(target);
				// The stock homing sets the full speed sideways and keeps vel.Z: give it the slope to the foe's middle.
				double top, bottom;
				GetActorExtents(target, &top, &bottom);
				DVector3 d = DVector3(target->spr.pos.XY(), (top + bottom) / 2) - m->spr.pos;
				double flat = d.XY().Length();
				if (flat > 1) m->vel.Z = missileInfo[m->spr.type - kMissileBase].fVelocity() * d.Z / flat;
			}
		}
		else
		{
			if (vectors++ >= kForgeVectors) continue;
			actFireVector(pl, s.off, s.zoff, s.dv, VECTOR_TYPE(s.type));
		}
	}
	forgeEmitting = false;
	if (run.forge[kForgeWound] == kWoundPyre)
		for (auto t : forgeHits) if (t->hasX() && t->xspr.health > 0) actBurnSprite(pl, t, kPyreBurn);
	forgeHits.Clear();
}

void WeaponCallback(int nCallback, int id, void* data)
{
	auto p = static_cast<DBloodPlayer*>(data);
	int v = run.forge[kForgeVessel];
	if (!ForgeOn(p) || !FiringCallback(v, nCallback))
	{
		qavClientCallback[nCallback](id, data);
		return;
	}
	int ammo = forgeVessels[v].ammo, before = p->ammoCount[ammo];
	TArray<ForgeAim> shots;
	if (!PrimaryCallback(p, v, nCallback))
	{
		// Alt fire stays stock and needs ammo: dry (only the Blood Price lets it get this far), it does nothing at all
		// (the tommy's spread would put the gun away).
		if (before == 0 && BloodPriced() && !gInfiniteAmmo) return;
		qavClientCallback[nCallback](id, data);
		return;
	}
	forgeShots = &shots;
	qavClientCallback[nCallback](id, data);
	forgeShots = nullptr;
	if (shots.Size()) EmitForge(p, shots, id, before - p->ammoCount[ammo]);
}

bool SwallowMissile(DBloodActor* shooter, double xyoff, double zoff, const DVector3& dv, int type)
{
	if (!forgeShots || forgeEmitting || !shooter || !shooter->IsPlayerActor()) return false;
	forgeShots->Push({ true, type, xyoff, zoff, dv });
	return true;
}

bool SwallowVector(DBloodActor* shooter, double offset, double zoffset, const DVector3& dv, int type)
{
	if (!forgeShots || forgeEmitting || !shooter || !shooter->IsPlayerActor()) return false;
	forgeShots->Push({ false, type, offset, zoffset, dv });
	return true;
}

bool ForgeLoaded(DBloodPlayer* pPlayer, int ammotype, int count)
{
	// One round, the gun dry, and health to pay: then it fires on health. Bigger checks (the flare's 8-flare starburst,
	// the akimbo sawed-off's 4 shells) get their stock answer.
	if (!BloodPriced() || !ForgeOn(pPlayer) || count != 1 || ammotype != forgeVessels[run.forge[kForgeVessel]].ammo) return false;
	return pPlayer->ammoCount[ammotype] == 0 && CanPayInBlood(pPlayer);
}

bool ForgeLoadedWeapon(DBloodPlayer* pPlayer, int weapon)
{
	int v = run.forge[kForgeVessel];
	if (!v || !BloodPriced() || !run.active || run.over || crypt.in || pPlayer != getPlayer(myconnectindex)) return false;
	return weapon == forgeVessels[v].weapon && pPlayer->hasWeapon[weapon] && pPlayer->ammoCount[forgeVessels[v].ammo] == 0 && CanPayInBlood(pPlayer);
}

ForgeImpact::ForgeImpact(DBloodActor* missile, DBloodActor* hit)
{
	if (!missile || !run.active || run.over) return;
	auto owner = missile->GetOwner();
	// A missile fired point blank hits before EmitForge has marked it: while it emits, the player's missiles are forged.
	bool emitted = forgeEmitting && owner && owner->IsPlayerActor();
	if (!(missile->modFlags & kModForged) && !emitted) return;
	active = true;
	forgeImpacts++;
	// The Pyre: at emission its hits are burned once each by EmitForge; later impacts burn here.
	if (!emitted && run.forge[kForgeWound] == kWoundPyre && hit && hit->IsDudeActor() && !hit->IsPlayerActor() && hit->hasX() && hit->xspr.health > 0)
		actBurnSprite(owner, hit, kPyreBurn);
	if (run.forge[kForgeWound] == kWoundGrave && missile->insector())
	{
		if (auto blast = actSpawnThing(missile->sector(), missile->spr.pos, kThingArmedTNTStick))
		{
			blast->SetOwner(owner);
			actExplodeSprite(blast); // a small blast, the player's
		}
	}
}

ForgeImpact::~ForgeImpact()
{
	if (active) forgeImpacts--;
}

int ScaleHealing(DBloodActor* actor, int add)
{
	if (!run.active || run.over || !actor || !actor->IsPlayerActor() || add <= 0) return add;
	if (forgeImpacts > 0) // healing from a forged missile (the cleaver): kForgeHealPerSecond at most
	{
		int second = PlayClock / kClock;
		if (second != forgeHealSecond) forgeHealSecond = second, forgeHealed = 0;
		add = std::min(add, kForgeHealPerSecond - forgeHealed);
		if (add <= 0) return 0;
		forgeHealed += add;
	}
	if (run.cov[kFamine] <= 0) return add;
	return std::max(1, int(add * (1. - 0.25 * run.cov[kFamine]) + 0.5)); // the Covenant's Famine
}

// The recipe changed (a menu option or the console): take off runes that can't stand with the rest, saying why; a
// chosen Vessel goes in the kit. Also when the kit changed: a Vessel taken out of the kit melts the gun down.
static bool forgeChecking;
static bool forgeConsole; // rl_forge is setting the recipe: a rune that can't fit comes off, it isn't stepped
void ForgeChanged()
{
	if (forgeChecking) return;
	forgeChecking = true;
	FIntCVarRef* cvars[kForgeSlots] = { &rl_forge_vessel, &rl_forge_mouth, &rl_forge_path, &rl_forge_wound, &rl_forge_penance };
	static int last[kForgeSlots]; // the recipe as last accepted, to tell which slot the menu just stepped
	int f[kForgeSlots];
	ForgeRecipe(f);
	forgeNote = "";
	// The Vessel goes in the kit; a gun the Forge put there itself comes out again when the Vessel changes.
	int added = std::clamp((int)rl_forge_added, 0, forgeSlotMax[kForgeVessel]);
	if (added && added != f[kForgeVessel])
	{
		*forgeVessels[added].kit = 0;
		rl_forge_added = 0;
	}
	if (f[kForgeVessel] && *forgeVessels[f[kForgeVessel]].kit < 1)
	{
		*forgeVessels[f[kForgeVessel]].kit = 1;
		rl_forge_added = f[kForgeVessel];
	}
	for (int slot = kForgeMouth; slot < kForgeSlots; slot++)
	{
		const char* why = ForgeIllegal(f, slot);
		if (!why) continue;
		if (forgeNote.IsEmpty()) forgeNote.Format("%s is off: %s", RuneName(slot, f[slot]), why);
		int now = f[slot], n = forgeSlotMax[slot] + 1;
		// The slot the menu just stepped steps on, the same way, to the next rune that fits (none always does);
		// a rune another slot's change put out of place comes off.
		if (now != last[slot] && !forgeConsole)
		{
			bool forward = now == (last[slot] + 1) % n;
			for (int k = 0; k < n && ForgeIllegal(f, slot); k++) f[slot] = forward ? (f[slot] + 1) % n : (f[slot] + n - 1) % n;
		}
		else f[slot] = 0;
		if (ForgeIllegal(f, slot)) f[slot] = 0;
		if (f[slot] && f[slot] != now && forgeNote.Len() < 80) forgeNote.AppendFormat("; %s instead", RuneName(slot, f[slot]));
		*cvars[slot] = f[slot];
	}
	for (int slot = 0; slot < kForgeSlots; slot++) last[slot] = f[slot];
	if (forgeNote.IsNotEmpty()) Printf(PRINT_NOTIFY, "The Forge: %s\n", forgeNote.GetChars());
	forgeChecking = false;
	UpdateMenus();
}

void KitChanged()
{
	int v = std::clamp((int)rl_forge_vessel, 0, forgeSlotMax[kForgeVessel]);
	if (v && !forgeChecking && *forgeVessels[v].kit < 1)
	{
		forgeChecking = true;
		rl_forge_vessel = 0;
		rl_forge_mouth = 0;
		rl_forge_path = 0;
		rl_forge_wound = 0;
		rl_forge_penance = 0;
		rl_forge_added = 0;
		forgeNote = "The Vessel left the kit: the gun is melted down.";
		forgeChecking = false;
	}
	UpdateMenus();
}
void CovenantChanged() { UpdateMenus(); }

void TintSprite(DBloodActor* owner, tspritetype* tspr)
{
	if (!owner || !tspr || !(owner->modFlags & kModChampion)) return;
	int pal = (owner->modFlags & kModRedChampion) ? rl_elite_red_pal : rl_elite_pal;
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
			arc.Array("forge", run.forge, kForgeSlots);
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
		arc.Array("forge", loaded.forge, kForgeSlots);
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
	for (int slot = 0; slot < kForgeSlots; slot++) loaded.forge[slot] = ForgeClamp(slot, loaded.forge[slot]); // a save from another build
	run = std::move(loaded);
	run.active = true;
	run.curseClock = INT_MIN / 2;
	bmod::modeEnemyTimeScale = run.over ? 1. : 1. + 0.15 * run.cov[kHaste];
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

CCMD(rl_forge_clear)
{
	rl_forge_vessel = 0; // ForgeChanged takes a gun the Forge put in the kit out again
	rl_forge_mouth = 0;
	rl_forge_path = 0;
	rl_forge_wound = 0;
	rl_forge_penance = 0;
	Printf(PRINT_NOTIFY, "The Forge: melted down (a run in progress keeps its gun)\n");
}

CCMD(rl_forge)
{
	int f[kForgeSlots];
	if (argv.argc() > 1)
	{
		FIntCVarRef* cvars[kForgeSlots] = { &rl_forge_vessel, &rl_forge_mouth, &rl_forge_path, &rl_forge_wound, &rl_forge_penance };
		forgeConsole = true;
		for (int slot = 0; slot < kForgeSlots && slot + 1 < argv.argc(); slot++) *cvars[slot] = atoi(argv[slot + 1]);
		forgeConsole = false;
		ForgeRecipe(f);
		if (run.active && !run.over)
		{
			for (int slot = 0; slot < kForgeSlots; slot++) run.forge[slot] = f[slot];
			Printf("(forged into the run in progress, for testing; its kit is as it was)\n");
		}
	}
	ForgeRecipe(f);
	auto effect = ForgeEffectLine(f);
	Printf("%s (%d points): %s %s%s%s\n", ForgeName(f).GetChars(), ForgeCost(f), ForgeSentence(f).GetChars(), effect.GetChars(),
		effect.IsEmpty() ? "" : " ", ForgeAmmoLine(f).GetChars());
	Printf("Vessels: 1 flare gun, 2 sawed-off, 3 tommy gun. Mouths: 1 Hellmouth, 2 Cleaver's Psalm. Paths: 1 Hound's Nose,"
		" 2 Choir of Three. Wounds: 1 Pyre, 2 Gravedigger's Blast. Penance: 1 Blood Price.\n");
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

// This map's ammo: how it stood when the map started, and now.
CCMD(rl_ammo)
{
	auto p = getPlayer(myconnectindex);
	if (!run.active || run.over || gamestate != GS_LEVEL || !p || !p->GetActor())
	{
		Printf("rl_ammo: no run in progress\n");
		return;
	}
	bool owned[kWeapMax];
	OwnedGuns(p, owned);
	if (ledger.counted)
	{
		Printf("At the start: %d enemies, %d health. The map's own ammo for your guns could kill them %.2f times over (target %.2f,"
			" rl_ammo_target %d); with %d pickups bigger and %d more, %.2f; with %d caches in %d secret areas, %.2f.\n", ledger.enemies,
			int(ledger.health), ledger.before, rl_ammo_target / 100., (int)rl_ammo_target, ledger.bigger, ledger.added, ledger.after,
			ledger.cached, ledger.secretAreas, ledger.withSecrets);
		FString planned, unplaced;
		for (auto& g : ammoProfiles)
		{
			if (ledger.plan[g.ammo] > 0) planned.AppendFormat("%s%s", planned.IsEmpty() ? "" : ", ", AmmoText(g, ledger.plan[g.ammo]).GetChars());
			if (ledger.unplaced[g.ammo] > 0) unplaced.AppendFormat("%s%s", unplaced.IsEmpty() ? "" : ", ", AmmoText(g, ledger.unplaced[g.ammo]).GetChars());
		}
		if (planned.IsNotEmpty()) Printf("  Added to the map's pickups: %s%s%s\n", planned.GetChars(),
			unplaced.IsEmpty() ? "" : "; found no room for ", unplaced.GetChars());
	}
	AmmoLedger now;
	CountAmmo(now, p, true);
	Printf("Now: %d enemies (a spawner counts as if it hasn't fired), %d health; %.2f times over.\n", now.enemies, int(now.health), Coverage(now, owned));
	for (auto& g : ammoProfiles)
	{
		if (!owned[g.gun]) continue;
		int carried = p->ammoCount[g.ammo];
		if (now.need[g.ammo] <= 0)
			Printf("  %s: %d carried, no use against what's left\n", g.name, HudAmount(g, carried));
		else
			Printf("  %s: %d carried, %d more on the map; killing everything with it alone takes %d (%.2f times over)\n", g.name,
				HudAmount(g, carried), HudAmount(g, now.have[g.ammo] - carried), HudAmount(g, now.need[g.ammo]), now.have[g.ammo] / now.need[g.ammo]);
	}
}

CCMD(rl_kit)
{
	TArray<Episode> eps;
	GetEpisodes(eps);
	FString list;
	for (auto& k : kitItems)
		if (*k.cvar > 0) list.AppendFormat("%s%s%s", list.IsEmpty() ? "" : ", ", k.name, *k.cvar > 1 ? FStringf(" x%d", int(*k.cvar)).GetChars() : "");
	Printf("Kit (%d points): %s\n", KitCost(), list.IsEmpty() ? "nothing" : list.GetChars());
	int f[kForgeSlots];
	ForgeRecipe(f);
	if (f[kForgeVessel]) Printf("Forged: %s (%d points of it)\n", ForgeName(f).GetChars(), ForgeCost(f));
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
