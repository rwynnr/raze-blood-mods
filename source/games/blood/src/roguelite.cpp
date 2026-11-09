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
//   1.25x size and damage, 1.1x for its bullets and shells, tinted blue), and
//   a rarer one, from 4% to 12%, a red champion: 5x health, 1.5x damage (1.2x
//   for its bullets and shells), 1.4x size, moving and animating 1.35x as fast
//   but attacking at stock speed, and it flinches only from a heavy hit of the
//   player's (32 health in a tic, once every 3 s). As they die, red champions
//   drop body armour, and champions basic armour one time in four;
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
// swallowed and their aims recorded, and the payload goes out along them. Its
// alt fire is a forged mode of its own the same way (both barrels, the sweep,
// the starburst), unless rl_forge_alt is 0, which leaves the stock alt fire.
// Litanies: two parts of a forged gun that together wake an effect neither has
// (docs/roguelite-forge.md). Shown once both are carved, kept in the Book across
// runs; at most two sing on a gun; banes add a point of Dread.
//
// Omens: timed boons the dead drop (rl_omens 0: none). Every champion drops
// one (a blue champion a common or rare one, a red one a rare or relic), a
// plain enemy one time in 25; they lie where it fell until the map ends. Most
// are Blood's own powerups on short timers. A gamble never drops on its own: a
// champion sometimes leaves a cursed reliquary (it falls with a bell, and
// Blood's atom motes circle it), which names both its halves when you come
// near or walk into it, and is taken with Use. A kill of yours while a boon runs
// adds a second to it (up to twice its time); taking one again adds its whole
// time (up to three times). They show in the shared chip row, last through a
// save and load, and end with the map.
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
//   rl_omen list                         the Omens and the gambles, numbered, with what runs now and the drop odds
//   rl_omen <name|number> [seconds]      grant an Omen (or a gamble) at once, e.g. "rl_omen twin", "rl_omen 9 30" (testing)
//   rl_omen_drop / rl_reliquary <name|number>  an Omen's pickup / a gamble's cursed reliquary in front of you (testing)
//   rl_unlock <count|all>                set how many of this game's episodes are unlocked (testing)
//   rl_resetprogress                     lock this game's episodes again; empty the ichor bank and the wins
//   openmenu RogueliteMenu               the Roguelite menu (it is also on the main menu)
// CVARs: rl_skill (0-4, set by the menu), rl_elite_pal (the champions' tint, a palette lookup number:
// 0-14 in Blood, 0 = none), rl_ammo_target (the supplies' target, percent; 0 = no supplies), rl_omens (0: no Omens), and the meta progress, kept in the config: rl_progress (per game, since
// Blood and Cryptic Passage share a config: episodes unlocked, the gauntlet won, the run whose saves can
// still be continued) and, shared by both, rl_ichor, rl_runs, rl_wins.
//
// Hooks into stock code (one-liners): levelEndLevel (the next map), playerProcess (Use while dead),
// GameInterface::NewGame, SerializeGameState (the save block), DBloodActor::modFlags (champions, saved
// with the actor), actDamageSprite and viewProcessSprites (champion damage and tint),
// GameInterface::MenuOpened (the menu's run list), the core main loop's return to the main menu
// (the run's result and writing the config), and the core intermission's choice of a map's intro movie.
// The Omens: PickUp (an Omen's pickup is the run's; a reliquary isn't taken by walking over it) and
// actDamageSprite (Bloodlust's heal from the health your hit took).
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
CVARD(Bool, rl_omens, true, CVAR_ARCHIVE, "roguelite: the dead drop Omens (timed boons) and champions cursed reliquaries (0 = none)")

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
CVARD(String, rl_forge_sing, "", CVAR_ARCHIVE, "roguelite Forge: Litanies chosen to sing (ids), and '-id' for ones silenced")
CVARD(Bool, rl_forge_alt, true, CVAR_ARCHIVE, "roguelite Forge: the forged gun's alt fire is a forged mode of its own (0 = its stock alt fire)")
CVARD(String, rl_litanies, "", CVAR_ARCHIVE, "roguelite Forge: the Litanies found (ids), kept across runs")

BEGIN_BLD_NS

// animatesprite.cpp's view effects, for a cursed reliquary's (OmenItemSprite).
tspritetype* viewInsertTSprite(tspriteArray& tsprites, sectortype* pSector, int nStatnum, tspritetype const* const parentTSprite);
extern DAngle random_angles[16][3]; // filled by viewInit

namespace rogue
{

// The Forge's Litanies, defined after the run's code (see "the Forge at work").
static void ResetLitanies();
static void LitaniesLoaded();
static void ForgeTick();
static bool Sings(int k);
// The red champions' flinches (Unflinching), defined with the champion hooks.
static void StaggerTick();
static void StaggerReset();

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
constexpr double kChampionHealth = 2.5, kChampionSize = 1.25; // and 1.25x damage, 1.1x for a foe's hitscan (ScaleDamage)
constexpr double kRedFirst = 0.04, kRedLast = 0.12;           // red champions: the rarer tier
constexpr double kRedHealth = 5.0, kRedSize = 1.4, kRedSpeed = 1.35; // and 1.5x damage, 1.2x hitscan; few flinches
// A playtest on Extra Crispy with a full Covenant: "the red champions can just vaporize you without good armor. The
// hitscan is a bit intense." A red tommy cultist killed you in 0.4 s without armour (1.5x a round, 16 rounds a
// second, never flinching). Now a foe's hitscan takes less of the tiers' bonus and of Wrath's, reds attack at stock
// speed (RedSpeed), your heavy hits make them flinch now and then (Unflinching), and champions drop armour.
constexpr double kChampionArmourChance = 0.25;                // a champion's basic armour as it dies (a red's body armour: always)
constexpr int kStaggerHit = 32 << 4;                          // a hit of yours this big (a tic's, before its armour) makes a red flinch
constexpr int kStaggerEvery = 3 * kClock;                     // at most this often
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
	{ &rl_cov_wrath, "Wrath", 1 },         // enemies deal +20% damage per rank (+10% with their bullets and shells)
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

struct ForgeVesselInfo
{
	const char* name;
	int weapon, ammo;
	FIntCVarRef* kit;  // its kit line: forging a gun puts it in the kit
	int kitCost;
	int stockMissile;  // its own round, if a missile (the flare), else 0
	bool slow;         // at least 0.3 s between pulls: may take a heavy Mouth
	const char* round; // its own round, in a sentence
};
const ForgeVesselInfo forgeVessels[] =
{
	{ "", 0, 0, nullptr, 0, 0, false, "" },
	{ "Flare Gun", kWeapFlareGun, 1, &rl_kit_flare, 10, kMissileFlareRegular, true, "a flare" },
	{ "Sawed-off", kWeapShotgun, 2, &rl_kit_shotgun, 20, 0, true, "its pellets" },
	{ "Tommy Gun", kWeapTommyGun, 3, &rl_kit_tommy, 30, 0, false, "a bullet" },
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
// What a Mouth fires for a round of its Vessel's ammo, and the rounds that round drinks, set by hand so each recipe is
// a clear step up from its stock gun (a rule from the payloads' worth undercharged the fireball and starved the
// cleaver). The sawed-off's are a barrel's. The tommy can't take Hellmouth (too heavy for its rate of fire).
struct ForgeLoad { int perRound, drink; };
const ForgeLoad forgeLoads[4][3] = // [Vessel][Mouth]
{
	{ { 0, 1 }, { 1, 1 }, { 1, 1 } }, // no Vessel: never fired
	{ { 0, 1 }, { 1, 1 }, { 5, 1 } }, // flare gun: a fireball a flare; five cleavers a flare
	{ { 0, 1 }, { 1, 2 }, { 3, 1 } }, // sawed-off: a fireball for two shells; three cleavers a shell
	{ { 0, 1 }, { 1, 2 }, { 1, 2 } }, // tommy gun: no fireball (ForgeIllegal); a cleaver for two bullets
};
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
	{ "Pyre", "of the Pyre", 10, "Lodges a flare: foes burn hard for 4 s." }, // kept short: it shares a line with the Mouth's
	{ "Gravedigger's Blast", "of the Grave", 20, "Bursts where it lands, 4 a second at most." },
};
const ForgeRuneInfo forgePenances[] =
{
	{ "", "", 0, "" },
	{ "Blood Price", "Blood-Priced", 15, "Dry, a pull costs 4 health." },
};
constexpr int kForgeMissiles = 12, kForgeVectors = 32; // most a pull fires (the flare's Choir: three fans of four cleavers)
// The alt modes (EmitForge): both barrels, the tommy's sweep, the flare's starburst. An alt pull has its own caps (the
// stock alt already fires 64 pellets akimbo, and 8 sub-flares), and the Mouth's payload half again over the rounds it
// drinks, as stock's alt is (armour-piercing shells 6 to 4, tommy bullets 20 to 12). A pull its cap clips drinks only
// for what it fires (AltOwed).
constexpr int kForgeAltMissiles = 16, kForgeAltVectors = 64;
constexpr double kForgeAltBonus = 1.5;
constexpr int kStarburstFlares = 8;                  // the starburst: the stock alt's price in flares (the most a forged one drinks), and its 8 sub-flares
constexpr int kSweepSteps = 14;                      // the tommy's alt sweep: 14 steps, its callback id the step's place
constexpr double kStarburstCone = 60;                // degrees: a hunting starburst child looks for its foe this wide
constexpr int kChoirPerAim = 4;                      // the Choir: a Mouth's missiles in each of its three aims, at most
constexpr int kCerberusFlying = 8;                   // Cerberus: forged fireballs flying at once, at most
constexpr double kCleaverSpeed = 1.5;                // forged cleavers fly half again as fast as the Butcher's
constexpr int kCleaverBite = 8, kCleaverBiteRange = 6; // and wound 8-13 more than his (the stock 10-19: 18-32 in all)
constexpr int kGraveBursts = 4;                      // Gravedigger's Blast: the Grave's bursts a second, at most
// Burn time counts down a PlayClock at a time: 480 is 4 s (a stock flare's). A hit tops it back up to that, never past.
constexpr int kPyreBurn = 480;
constexpr int kPyreMarks = 12;                       // the Pyre's flares lodged at once, at most (one a foe)
constexpr int kWitchfireBurn = 240;                  // Witchfire's scattered fire: 2 s (you are in it too)
constexpr int kBloodPrice = 4;                       // health a dry pull costs
constexpr int kForgeHealPerSecond = 20;              // most the forge heals you a second
constexpr double kFetchMinRange = 128;               // the Fetch: a cleaver striking closer to you than this isn't sent back
constexpr double kHoundCone = 25, kHoundRange = 1024; // degrees, world units
const int forgeSlotMax[kForgeSlots] = { 3, 2, 2, 2, 1 };

//---------------------------------------------------------------------------
//
// Litanies: pairs of parts that wake an effect neither has
//
//---------------------------------------------------------------------------

enum { kLitShears, kLitCerberus, kLitBloodFeud, kLitCenser, kLitHymn, kLitCandle, kLitRequiem, kLitLarder, kLitLastRites,
	kLitWitchfire, kLitFetch, kLitLeap, kNumLitanies };
enum { kKindBoon, kKindGamble, kKindBane, kKindOddity };
enum { kRivalMiss = 1, kRivalGrave = 2 }; // Litanies that change the same thing can't both sing
struct ForgePart { int slot, value; };   // a Vessel part is its own round: it needs no Mouth carved
struct Litany
{
	const char* id;
	const char* name;
	ForgePart a, b;
	int kind;
	const char* sentence;
	int rivals;
};
const Litany litanies[kNumLitanies] =
{
	{ "shears", "The Shears", { kForgeMouth, kMouthCleaver }, { kForgePath, kPathChoir }, kKindBoon,
		"Aimed at a foe, the cleavers close on it from three sides like shears.", 0 },
	{ "cerberus", "Cerberus", { kForgeMouth, kMouthHellmouth }, { kForgePath, kPathHound }, kKindBoon,
		"Each hunting fireball grows a second head that hunts another foe.", 0 },
	{ "bloodfeud", "Blood Feud", { kForgePath, kPathHound }, { kForgePenance, kPenanceBloodPrice }, kKindBoon,
		"Dry, your hunters go for whoever last drew your blood.", 0 },
	{ "censer", "The Censer", { kForgeVessel, kVesselShotgun }, { kForgeWound, kWoundPyre }, kKindBoon,
		"Each pull also swings three spray-can flames along the spread.", 0 },
	{ "hymn", "Hymn of Ash", { kForgeMouth, kMouthHellmouth }, { kForgePath, kPathChoir }, kKindGamble,
		"A fireball that strikes flesh gives a round back; stone drinks one.", 0 },
	{ "candle", "Corpse-Candle", { kForgeVessel, kVesselFlare }, { kForgeWound, kWoundGrave }, kKindGamble,
		"A flare lodged in flesh is a fuse: a bigger blast in 1.5 s.", 0 },
	{ "requiem", "Requiem", { kForgePath, kPathChoir }, { kForgeWound, kWoundGrave }, kKindGamble,
		"Blasts that land together ring out as one, bigger; it can reach you.", kRivalGrave },
	{ "larder", "The Larder of Knives", { kForgeMouth, kMouthCleaver }, { kForgeWound, kWoundGrave }, kKindGamble,
		"Missed cleavers stick where they land; a foe who comes near sets them off.", kRivalMiss | kRivalGrave },
	{ "lastrites", "Last Rites", { kForgeMouth, kMouthHellmouth }, { kForgePenance, kPenanceBloodPrice }, kKindBane,
		"Dry pulls can kill you; near death, blood-bought fireballs burst bigger.", 0 },
	{ "witchfire", "Witchfire", { kForgeMouth, kMouthCleaver }, { kForgeWound, kWoundPyre }, kKindBane,
		"A cleaver striking a burning foe scatters its fire on all near, you too.", 0 },
	{ "fetch", "The Fetch", { kForgeMouth, kMouthCleaver }, { kForgePath, kPathHound }, kKindOddity,
		"A hunting cleaver that strikes stone flies back; catch it to win back what it cost.", kRivalMiss },
	{ "leap", "Brimstone Leap", { kForgeMouth, kMouthHellmouth }, { kForgeWound, kWoundGrave }, kKindOddity,
		"Your own blasts throw you instead of wounding you; their fire still bites.", 0 },
};
const char* const litanyKinds[] = { "boon", "gamble", "bane", "oddity" };
const char* const circleNames[kForgeSlots] = { "the Vessel", "the Mouth", "the Path", "the Wound", "the Penance" };
constexpr int kMaxSinging = 2;

// The Litany announced on this map, and which have fired here (each is announced once a map).
int litanyShown = -1, litanyShownAt = INT_MIN / 2;
unsigned litanyFired;
constexpr int kLitanyTitle = 2 * kClock;

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
	int litanies = 0;             // the Litanies that sing on it (bits)
	int omenRolls = 0;            // Omens: the deaths rolled for a drop so far (each roll is seeded by its number)
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
		auto& load = forgeLoads[f[kForgeVessel]][m];
		r.perRound = load.perRound;
		r.drink = load.drink;
		r.missile = true;
	}
	else r.missile = v.stockMissile != 0;
	// The Choir sings each round three times over (three fireballs, or three fans of cleavers) for twice the drink.
	if (f[kForgePath] == kPathChoir)
	{
		r.drink *= 2;
		if (r.perRound) r.perRound = std::min(r.perRound, kChoirPerAim);
	}
	return r;
}

// An alt pull's Mouth payload, before the Choir fans it three ways: for the rounds it fires (units), times its bonus
// (kForgeAltBonus; the sweep doubles its odd steps instead), within the alt's cap in whole fans; then the share of its
// drink paid of that. Something paid fires at least one. EmitForge fires it and the menu counts it, so they agree.
int AltPayload(const int* f, int units, double share, double bonus)
{
	auto r = Rates(f);
	int fan = f[kForgePath] == kPathChoir ? 3 : 1;
	units = std::max(units, 1);
	int perRound = std::max(1, std::min(r.perRound, kForgeAltMissiles / (units * fan)));
	int full = std::max(1, std::min(int(perRound * units * bonus + 1e-6), kForgeAltMissiles / fan));
	// A short pull fires its share of the capped payload, rounded down: shrunk before the cap, it would fire more a
	// round than a full pull does.
	return std::max(1, int(full * share + 1e-6));
}

// What an alt pull drinks, for the rounds it fires (units): their drink, as for the primary, but only for the share of
// the payload the alt's cap lets it fire (a round's drink buys perRound of the Mouth's missiles, times the bonus),
// rounded up and at least a round, so a clipped pull is never worse for its ammo than the primary. The starburst
// drinks this too (16 flares' worth under the Choir), and never more than the stock alt's 8 (with no Mouth, the 8).
int AltOwed(const int* f, int units, double bonus, bool starburst)
{
	auto r = Rates(f);
	units = std::max(units, 1);
	int owed = units * r.drink;
	if (f[kForgeMouth] && r.perRound > 0)
	{
		int uncapped = std::max(1, int(r.perRound * units * bonus + 1e-6));
		int fired = AltPayload(f, units, 1, bonus);
		if (fired < uncapped) owed = std::max(1, (owed * fired + uncapped - 1) / uncapped);
	}
	else if (starburst) owed = kStarburstFlares;
	return starburst ? std::min(owed, kStarburstFlares) : owed;
}

// The starburst's ring, each carrier's (the Choir fires three): the Mouth's payload for the stock alt's 8 flares, within
// the cap (AltOwed charges only what it fires), or with no Mouth the stock alt's 8 sub-flares, forged.
int StarburstCount(const int* f, double share)
{
	if (f[kForgeMouth]) return AltPayload(f, kStarburstFlares, share, kForgeAltBonus);
	return std::clamp(int(kStarburstFlares * share + 0.5), 1, kForgeAltMissiles);
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
	// The Choir's fans of several (the cleavers) are counted whole: "12 cleavers, in three fans of 4".
	bool fans = f[kForgeMouth] && f[kForgePath] == kPathChoir && r.perRound > 1;
	if (fans) s.AppendFormat("%d %s, in three fans of %d", 3 * r.perRound, forgeMouthPlural[f[kForgeMouth]], r.perRound);
	else if (f[kForgeMouth])
	{
		if (r.perRound > 1) s.AppendFormat("%d %s", r.perRound, forgeMouthPlural[f[kForgeMouth]]);
		else s.AppendFormat("a %s", forgeMouths[f[kForgeMouth]].says);
	}
	else s += v.round;
	if (f[kForgePath] && !fans) s.AppendFormat(", %s", forgePaths[f[kForgePath]].says);
	s += ".";
	return s;
}

int ResolveSinging(const int* f);

// Kept to about 110 characters with every part in: the menu doesn't wrap it.
FString ForgeEffectLine(const int* f)
{
	FString s;
	auto add = [&](const char* text) { s.AppendFormat("%s%s", s.IsEmpty() ? "" : " ", text); };
	if (f[kForgeVessel] == kVesselFlare && !f[kForgeMouth]) add("Flares strike for 20-29, then burn.");
	if (f[kForgeWound]) add(forgeWounds[f[kForgeWound]].says);
	// Your forged blasts hurt you half (ScaleDamage): the Grave's, Hellmouth's, and with the forged alt the flare's
	// starburst (its carrier struck early, its forged sub-flares). Not while Brimstone Leap sings: then they throw you
	// instead, and its own line says so.
	bool halfOnYou = f[kForgeWound] == kWoundGrave || f[kForgeMouth] == kMouthHellmouth || (f[kForgeVessel] == kVesselFlare && rl_forge_alt);
	if (halfOnYou && !(ResolveSinging(f) & (1 << kLitLeap))) add("Your blasts hurt you half.");
	if (f[kForgeMouth] == kMouthCleaver) add(FStringf("Cleavers heal half the wound, %d a second.", kForgeHealPerSecond).GetChars());
	return s;
}

// The alt mode's sentence, as EmitForge fires it with one gun and ammo enough (AltPayload counts both, AltOwed the cost).
FString ForgeAltLine(const int* f)
{
	if (!rl_forge_alt) return "Alt fire: stock.";
	int m = f[kForgeMouth], fan = f[kForgePath] == kPathChoir ? 3 : 1;
	switch (f[kForgeVessel])
	{
	case kVesselShotgun: // FireShotgun(2): 32 armour-piercing pellets for 2 shells
		if (!m) return "Alt: both barrels, 32 armor-piercing pellets for 2 shells.";
		return FStringf("Alt: both barrels, %d %s for %d shells.", AltPayload(f, 2, 1, kForgeAltBonus) * fan, forgeMouthPlural[m],
			AltOwed(f, 2, kForgeAltBonus, false));
	case kVesselTommy:
	{
		if (!m) return FStringf("Alt: a sweep of %d armor-piercing bullets.", kSweepSteps);
		int n = 0, cost = 0;
		for (int step = 1; step <= kSweepSteps; step++)
		{
			double bonus = (step & 1) ? 2 : 1;
			n += AltPayload(f, 1, 1, bonus) * fan;
			cost += AltOwed(f, 1, bonus, false);
		}
		return FStringf("Alt: a sweep of %d %s for %d bullets.", n, forgeMouthPlural[m], cost);
	}
	case kVesselFlare:
	{
		// The stock alt fires only with 8 flares loaded: one that drinks fewer says so ("3 of 8").
		int owed = AltOwed(f, kStarburstFlares, kForgeAltBonus, true);
		if (!m) return FStringf("Alt: a starburst of %d forged sub-flares for %d flares.", StarburstCount(f, 1), owed);
		FString cost = owed < kStarburstFlares ? FStringf("%d of %d", owed, kStarburstFlares) : FStringf("%d", owed);
		return FStringf("Alt: %s of %d %s for %s flares.", fan > 1 ? "3 starbursts" : "a starburst", StarburstCount(f, 1), forgeMouthPlural[m],
			cost.GetChars());
	}
	}
	return "";
}

FString ForgeAmmoLine(const int* f)
{
	if (!f[kForgeVessel]) return "";
	auto r = Rates(f);
	static const char* ammoOne[] = { "", "flare", "shell", "bullet" };
	static const char* ammoMany[] = { "", "flares", "shells", "bullets" };
	int a = forgeVessels[f[kForgeVessel]].ammo;
	FString s = FStringf("Drinks %d %s a %s.", r.drink, r.drink == 1 ? ammoOne[a] : ammoMany[a], f[kForgeVessel] == kVesselShotgun ? "barrel" : "pull");
	s.AppendFormat(" %s", ForgeAltLine(f).GetChars());
	// Dry, every Vessel's alt fires its primary (an alt pull is never paid in blood), so this holds for both.
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

// Litanies: is a part carved, is a Litany lit (both its parts are), and which sing (two at most).
bool PartCarved(const int* f, const ForgePart& part)
{
	if (part.slot == kForgeVessel) return f[kForgeVessel] == part.value && !f[kForgeMouth];
	return f[kForgeVessel] && f[part.slot] == part.value;
}

bool LitanyLit(const int* f, int k) { return PartCarved(f, litanies[k].a) && PartCarved(f, litanies[k].b); }

FString PartName(const ForgePart& part)
{
	if (part.slot == kForgeVessel) return FStringf("the %s's own round", forgeVessels[part.value].name);
	return RuneName(part.slot, part.value);
}

TArray<FString> SingList()
{
	auto v = FString(*rl_forge_sing).Split(",", FString::TOK_SKIPEMPTY);
	return v;
}

int LitanyIndex(const char* id)
{
	for (int k = 0; k < kNumLitanies; k++) if (!stricmp(litanies[k].id, id)) return k;
	return -1;
}

// The singing set: the Litanies chosen to sing (lit ones, in the order chosen), then by default the first lit ones in
// table order that aren't banes or silenced; rivals give way to the first; two at most.
int ResolveSinging(const int* f)
{
	int sing = 0, count = 0, rivals = 0;
	auto take = [&](int k)
	{
		if (k < 0 || count >= kMaxSinging || (sing & (1 << k)) || !LitanyLit(f, k) || (litanies[k].rivals & rivals)) return;
		sing |= 1 << k;
		rivals |= litanies[k].rivals;
		count++;
	};
	auto list = SingList();
	unsigned silenced = 0;
	for (auto& e : list)
	{
		if (e[0] == '-') { int k = LitanyIndex(e.GetChars() + 1); if (k >= 0) silenced |= 1u << k; }
		else take(LitanyIndex(e.GetChars()));
	}
	for (int k = 0; k < kNumLitanies; k++)
		if (litanies[k].kind != kKindBane && !(silenced & (1u << k))) take(k);
	return sing;
}

bool LitanyFound(int k)
{
	auto found = FString(*rl_litanies).Split(",", FString::TOK_SKIPEMPTY);
	for (auto& e : found) if (!e.CompareNoCase(litanies[k].id)) return true;
	return false;
}

unsigned litanyNew; // found this session (the menu says "new")

// Both parts carved: the Litany is found, and the Book keeps it.
void DiscoverLitanies(const int* f)
{
	for (int k = 0; k < kNumLitanies; k++)
	{
		if (!LitanyLit(f, k) || LitanyFound(k)) continue;
		FString found = *rl_litanies;
		found.AppendFormat("%s%s", found.IsEmpty() ? "" : ",", litanies[k].id);
		rl_litanies = found.GetChars();
		litanyNew |= 1u << k;
		saveConfig = true;
		Printf(PRINT_NOTIFY, "Litany found: %s\n", litanies[k].name);
	}
}

// The lit Litanies of a recipe, in table order (the Forge menu's lines).
int LitLitanies(const int* f, int* out, int most)
{
	int n = 0;
	for (int k = 0; k < kNumLitanies && n < most; k++) if (LitanyLit(f, k)) out[n++] = k;
	return n;
}

// A hint at the Litanies a recipe could still wake: their missing part's circle, never the rune.
FString LitanyRiddle(const int* f)
{
	if (!f[kForgeVessel]) return "";
	int stirs[kForgeSlots] = {}, total = 0;
	for (int k = 0; k < kNumLitanies; k++)
	{
		if (LitanyLit(f, k)) continue;
		bool a = PartCarved(f, litanies[k].a), b = PartCarved(f, litanies[k].b);
		if (a == b) continue;
		auto& missing = a ? litanies[k].b : litanies[k].a;
		if (missing.slot == kForgeVessel) continue;
		stirs[missing.slot]++;
		total++;
	}
	if (!total) return "";
	static const char* const circle[kForgeSlots] = { "Vessel", "Mouth", "Path", "Wound", "Penance" };
	FString s = FStringf("%d more %s stir, by the circle of the rune that wakes %s: ", total, total == 1 ? "Litany" : "Litanies", total == 1 ? "it" : "them");
	bool first = true;
	for (int slot = 0; slot < kForgeSlots; slot++)
	{
		if (!stirs[slot]) continue;
		s.AppendFormat("%s%s %d", first ? "" : ", ", circle[slot], stirs[slot]);
		first = false;
	}
	return s;
}

// The run's Dread: the Covenant's, and a point for each bane that sings.
int RunDread()
{
	int d = Dread(run.cov);
	for (int k = 0; k < kNumLitanies; k++) if ((run.litanies & (1 << k)) && litanies[k].kind == kKindBane) d++;
	return d;
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
	if (run.forge[kForgeVessel])
	{
		FString sing;
		for (int k = 0; k < kNumLitanies; k++) if (run.litanies & (1 << k)) sing.AppendFormat("%s%s", sing.IsEmpty() ? "" : " and ", litanies[k].name);
		Printf(PRINT_NOTIFY, "Your forged gun: %s%s%s\n", ForgeName(run.forge).GetChars(), sing.IsEmpty() ? "" : "; singing: ", sing.GetChars());
	}
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

// The red champions' speed, for the base's modeActorSpeed (set as each map starts and on each load): their extra moves
// and their animation, but not while they attack, where a red tommy cultist's 12 rounds a second became 16. An attack
// is a state whose SEQ calls back (funcId, -1 for none) as it chases: a gun's fire, a throw, a bite, a slash (stateType
// can't tell: the fire states are kAiStateChase too). Of the kinds a red can be, the one other state that calls back,
// a zombie standing up, isn't a chase state. In an attack a red turns at stock speed too (its moveFunc's extra calls).
double RedSpeed(DBloodActor* a)
{
	if (!a || !(a->modFlags & kModRedChampion)) return 1.;
	auto st = a->hasX() ? a->xspr.aiState : nullptr;
	if (st && st->funcId >= 0 && st->stateType == kAiStateChase) return 1.;
	return kRedSpeed;
}

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
// minClear: at least this clear of walls and solid sprites, loose or not (a big pickup: a cursed reliquary).
bool SpotOk(sectortype* s, const DVector2& p, double anchorZ, const TArray<DVector2>& taken, bool loose = false, double minClear = 0)
{
	const double clear = std::max(loose ? 8 : kSpotClear, minClear), spacing = loose ? 12 : kSpotSpacing;
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
// that sector (a secret's cache stays in the secret). minClear: as SpotOk's.
bool FindItemSpot(std::mt19937& rng, sectortype* aSect, const DVector3& anchor, double minD, double maxD,
	const TArray<DVector2>& taken, DVector2& out, sectortype*& outSect, sectortype* only = nullptr, int tries = 16, double minClear = 0)
{
	DVector3 eye(anchor.XY(), anchor.Z - 16);
	for (int k = 0; k < tries; k++)
	{
		DVector2 p = anchor.XY() + DAngle::fromDeg(Rand01(rng) * 360.).ToVector() * (minD + (maxD - minD) * Rand01(rng));
		auto s = aSect;
		updatesector(p, &s);
		if (!s || (only && s != only)) continue;
		if (!cansee(eye, aSect, DVector3(p, getflorzofslopeptr(s, p.X, p.Y) - 16), s)) continue;
		if (!SpotOk(s, p, anchor.Z, taken, only != nullptr, minClear)) continue;
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

// The Red Potion is a 2-health bonus (no stock map places one): at weight 2 the remix made 18% of health pickups duds.
// It stays in the list, so one a map did place still rerolls, but its weight went to the Life Essence.
const ItemPick healthPicks[] =
{
	{ kItemHealthRedPotion, 0 }, { kItemHealthLifeEssense, 6 }, { kItemHealthMedPouch, 3 },
	{ kItemHealthDoctorBag, 1 }, { kItemHealthLifeSeed, 1 },
};
// Fire and spirit armour stop no bullets (they give no armor[0]): at 2 each, over a third of rerolled armour did nothing
// against the hitscan cultists. Their weight went to body and super armour. Still 11 in all, so the RNG a seed's
// other rerolls draw doesn't shift.
const ItemPick armorPicks[] =
{
	{ kItemArmorBasic, 4 }, { kItemArmorBody, 3 }, { kItemArmorFire, 1 }, { kItemArmorSpirit, 1 }, { kItemArmorSuper, 2 },
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

// An effect of e's group is running (chaos's, the curse's or an Omen's), one that starting e would end.
bool RivalActive(const bmod::Effect* e)
{
	for (int i = 0; i < bmod::NumEffects(); i++)
		if (bmod::IsActive(&bmod::GetEffect(i)) && bmod::Rivals(&bmod::GetEffect(i), e)) return true;
	return false;
}

// Starts the curse unless it's on already, or a rival effect (from chaos mode) is on: then it comes back later.
bool StartCurse()
{
	auto e = CurseEffect();
	if (!e) return false;
	if (bmod::IsActive(e)) return true;
	if (RivalActive(e)) return false;
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
int BankedIchor() { return int(run.ichor * (1. + kDreadIchor * RunDread()) + 0.5); }

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
	int dread = RunDread();
	FString bonus = dread ? FStringf(", +%d%% for Dread %d", int(kDreadIchor * 100 * dread + 0.5), dread) : FString();
	return FStringf("Maps cleared: %d of %d\nKills: %d (%d champions)\nIchor: +%d%s (banked: %d)",
		run.cleared, int(run.maps.Size()), run.kills, run.champions, BankedIchor(), bonus.GetChars(), (int)rl_ichor);
}

void EndOmens(bool stock, bool leaving = false); // the Omens, with the drops below

void RunOver()
{
	if (!run.active || run.over) return;
	run.over = true;
	if (auto e = CurseEffect()) bmod::StopEffect(e); // a clean view behind the "Run over" screen
	// The Omens' effects too. Their stock powerups run out on the dead player as Blood's own do (this comes before
	// Blood takes the gun away: ending the akimbo now would raise it again).
	EndOmens(false);
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
	run.litanies = ResolveSinging(run.forge);
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
// a champion's armour, dropped as it dies
//
//---------------------------------------------------------------------------

// A dropped item moved to a spot, its bottom still on the floor.
void LayDrop(DBloodActor* item, sectortype* sect, const DVector2& spot)
{
	double top, bottom;
	GetActorExtents(item, &top, &bottom);
	double lift = item->spr.pos.Z - bottom; // as dropped, its bottom rests on the floor
	ChangeActorSect(item, sect);
	SetActor(item, DVector3(spot, getflorzofslopeptr(sect, spot.X, spot.Y) + lift));
	item->interpolated = false;
	viewBackupSpriteLoc(item);
}

// A red champion drops body armour, a champion basic armour one time in four: the armour that stops bullets (fire and
// spirit armour stop none), as on the hardest settings the champions' hitscan is what kills you. The roll is the
// run's (its seed and the champion's number in the run), so a reload rolls the same. A dropped item has no xsprite
// or mark, so the ammo ledger (counted as the map starts), the remix (the same) and the big pickups (marked, with an
// xsprite) never take it for theirs. It lies where the champion fell if a pickup can lie there; else on a spot in
// sight of that one, or else near you (a flyer shot over lava or a pit, a foe in a door or on a lift); if none
// will do, where it fell, as a stock drop would (in water, say, where you can swim to it).
void DropChampionArmour(DBloodActor* victim, bool red)
{
	std::mt19937 rng(run.seed ^ (0x9E3779B9u * uint32_t(run.champions)) ^ 0x2545F491u);
	if (!red && Rand01(rng) >= kChampionArmourChance) return;
	auto item = actDropObject(victim, red ? kItemArmorBody : kItemArmorBasic);
	if (!item || !item->insector() || SpotSectorOk(item->sector())) return;
	TArray<DVector2> taken;
	DVector2 spot;
	sectortype* sect = nullptr;
	bool found = FindItemSpot(rng, item->sector(), FloorAt(item), 16, 96, taken, spot, sect);
	auto p = getPlayer(myconnectindex);
	auto pl = p ? p->GetActor() : nullptr;
	if (!found && pl && pl->insector()) found = FindItemSpot(rng, pl->sector(), FloorAt(pl), 24, 80, taken, spot, sect);
	if (!found || !sect) return;
	LayDrop(item, sect, spot);
}

//---------------------------------------------------------------------------
//
// the Omens: timed boons the dead drop, and the gambles of the cursed
// reliquaries champions leave (opt-in: taken with Use, never walked into).
// The run keeps its own list of the Omens running, the truth for their time;
// the base effects and stock powerups they run on are checked against it
// each tic, as chaos (in the play build) can end a rival effect.
//
//---------------------------------------------------------------------------

enum { kOmenCommon, kOmenRare, kOmenRelic, kOmenGamble };
const char* const omenTierNames[] = { "common", "rare", "relic", "gamble" };

struct OmenInfo
{
	const char* name;
	int tier;
	int seconds;
	int powerup;          // the stock powerup it runs on (its pwUpTime), -1: none
	const char* effect;   // the base effect it runs, or null
	int deal;             // your hits on foes deal this many times their damage (1: as ever)
	bool bloodlust;       // your hits heal you
	int look;             // the stock item it's drawn as (its picture and size), in its tier's palette
	const char* sentence; // what it does
};

// The pool: mostly Blood's own powerups on short timers. Hollow Bones is drawn as the feather fall, Bottomless as the
// wine bottle, Dread as the tome, Bloodlust as the skull grail, Fury as the rage shroom; a reliquary is a black chest.
const OmenInfo omenInfo[] =
{
	{ "Hollow Bones", kOmenCommon, 20, -1, "lowgrav", 1, false, kItemFeatherFall, "low gravity" },
	{ "Bottomless", kOmenCommon, 10, -1, "infammo", 1, false, 133, "infinite ammo" },
	{ "Dread", kOmenCommon, 10, -1, "slowmo", 1, false, 136, "enemies at half speed" },
	{ "Shroud", kOmenCommon, 12, kPwUpShadowCloak, nullptr, 1, false, kItemShadowCloak, "a shadow cloak: enemies can't see you" },
	{ "Twin Vessel", kOmenRare, 15, kPwUpTwoGuns, nullptr, 1, false, kItemTwoGuns, "guns akimbo, the forged gun too" },
	{ "Bloodlust", kOmenRare, 15, -1, nullptr, 1, true, 134, "your hits heal you" },
	{ "Fury", kOmenRare, 10, -1, nullptr, 2, false, 127, "you deal 2x" },
	{ "Mirror Skin", kOmenRelic, 10, kPwUpReflectShots, nullptr, 1, false, kItemReflectShots, "reflective shots: a foe's bullets hit the foe" },
	{ "Death's Grin", kOmenRelic, 4, kPwUpDeathMask, nullptr, 1, false, kItemDeathMask, "invulnerable" },
	// The gambles, only ever in a cursed reliquary: both halves run together for the time. Frenzy's 3x is the run's own.
	{ "Glass Cannon", kOmenGamble, 20, -1, "glasscannon", 1, false, 137, "deal 4x, take 2x" },
	{ "Frenzy", kOmenGamble, 15, -1, "fast", 3, false, 137, "enemies twice as fast, you deal 3x" },
	{ "Powder Keg", kOmenGamble, 20, -1, "bigboom", 1, false, 137, "every blast, yours and theirs, twice as big" },
};
constexpr int kNumOmens = int(countof(omenInfo));

// The drops. A champion always drops an Omen, a plain enemy one time in 25; a champion's cursed reliquary comes besides.
constexpr double kOmenPlain = 1. / 25;
constexpr double kOmenBlueRare = 0.35;                            // a blue champion's is rare, else common
constexpr double kOmenRedRelic = 0.25;                            // a red champion's is a relic, else rare
constexpr double kOmenPlainRare = 0.25, kOmenPlainRelic = 0.05;   // a plain enemy's, else common
constexpr double kReliquaryBlue = 1. / 10, kReliquaryRed = 1. / 3; // plain enemies never leave one
// The look: an inert item (the wine goblet's type: a treasure no stock map places, to which stock code gives no effect,
// should any reach its pickup) drawn as the Omen's look. Common ones keep Blood's colours, rare ones take the blue
// champions' COLD, relics the red champions' BEAST. A reliquary takes FLAME: no stock lookup turns a whole sprite purple,
// and FLAME turns over a third of the black chest violet and the rest red, the nearest they come. All are lit like
// Blood's keys, so a drop shows in the dark and isn't taken for a map's own item. Each keeps its palette on a floor with
// its own (OmenItemSprite), as stock items take the floor's.
// A reliquary, a choice rather than a pickup walked into, is made hard to miss: it falls with a bell and a message, it's
// drawn half as big again, and Blood's own atom motes circle it (OmenItemSprite). It names itself from further off than
// Use takes it (Use stays tight: a press at a bare wall, hunting a secret, never takes a gamble), and when walked into.
constexpr int kOmenItem = 132;
constexpr int omenPal[] = { 0, 10, 2, 9 };
constexpr int kOmenShade = -128;
constexpr int kOmenKillTime = kClock;                // a kill of yours adds this to each boon running, up to 2x its time
constexpr double kReliquaryRange = 96;               // Use takes a reliquary this near (to the eye)
constexpr double kReliquaryCone = 25;                // and this close to where you look, up and down too (degrees)
constexpr double kReliquaryNameRange = 160;          // it names itself this near (along the floor)
constexpr double kReliquaryNameCone = 30;            // and this close to where you face (left and right only: degrees)
constexpr int kReliquaryTouchHint = kClock / 2;      // walked into, it names itself until this long after
constexpr double kReliquaryLook = 1.5;               // it's drawn this much bigger than the stock chest (repeat 60)
constexpr double kReliquaryClear = 24;               // and so lies this far from walls and solid sprites (kSpotClear: 16)
constexpr double kReliquaryHaloZ = 32;               // its motes circle a point this high above its floor
// It falls with BELL: no stock code rings 270, and no stock map's ambient or sector sound uses it. (Eleven of the maps'
// sound generators do, on E1M4, E1M8, E4M9, E6M1 and two BloodBath maps: each off until the map's wiring turns it on.)
constexpr int kReliquaryBell = 270;
constexpr int kLustPerSecond = 15 << 4;              // Bloodlust heals at most this a second (health is in sixteenths)
constexpr int kLustTop = 100 << 4;                   // and never above this
// The chips' gold (chaos's twist gold), purple, and the map curse's red (chaos's nasty red).
constexpr uint32_t kBoonChip = 0xfff2c230, kGambleChip = 0xffb45cff, kCurseChip = 0xffff4a3a;

struct RunningOmen
{
	int id;
	int left;  // PlayClock units
	int base;  // its time as it began (kills extend it to 2x this, taking it again to 3x)
	int peak;  // the most time it has had left (the chip's fill is the share of this still left)
	bool owns; // it started its base effect (one the curse or chaos runs already isn't its to stop)
};
TArray<RunningOmen> omens;  // oldest first: the chips' order
FString reliquaryHint;      // the cursed reliquary you face, named (the drawer shows it)
DBloodActor* reliquaryTouched; // the reliquary you last walked into (compared, never followed)
int reliquaryTouchedAt = INT_MIN / 2;
int lustSecond = -1, lustHealed, lustSoundAt = INT_MIN / 2;
DBloodActor* omenPowerItem; // a map's powerup being picked up while an Omen runs on its timer (compared, never followed)
int omenPowerBefore;        // that timer as the pickup began

bool OmensLive() { return rl_omens && run.active && !run.over && !crypt.in; }
const bmod::Effect* OmenEffect(int id) { return omenInfo[id].effect ? bmod::FindEffect(omenInfo[id].effect) : nullptr; }

RunningOmen* RunningOf(int id)
{
	for (auto& r : omens) if (r.id == id) return &r;
	return nullptr;
}

bool IsOmenItem(DBloodActor* a)
{
	return a && a->spr.statnum == kStatItem && (a->modFlags & kModOmen) && a->hasX() && a->xspr.data1 >= 0 && a->xspr.data1 < kNumOmens;
}

// Your own deed: the player, or a missile, blast or thing of yours.
bool ByYou(DBloodActor* a)
{
	for (int i = 0; i < 2 && a && !a->IsDudeActor(); i++) a = a->GetOwner();
	auto p = getPlayer(myconnectindex);
	return a && p && a == p->GetActor();
}

// Fury's and Frenzy's factor on your hits on foes (together, they multiply).
int OmenDeal()
{
	int deal = 1;
	if (OmensLive()) for (auto& r : omens) deal *= omenInfo[r.id].deal;
	return deal;
}

bool BloodlustOn()
{
	if (OmensLive()) for (auto& r : omens) if (omenInfo[r.id].bloodlust) return true;
	return false;
}

// Time for a stock powerup an Omen runs on, added to its pwUpTime (time it held already is the map's own powerup's, and
// stays). From 0, Blood's own start comes first, once (the akimbo raise, the mirror's echo, the mask's armour):
// powerupActivate sets only a timer at 0, and repeats the mask's armour each time. True: it did (and chimed).
bool AddPowerTime(DBloodPlayer* p, int n, int add)
{
	if (n < 0 || add <= 0) return false;
	bool fresh = p->pwUpTime[n] <= 0;
	if (fresh)
	{
		powerupActivate(p, n);
		p->pwUpTime[n] = 0;
	}
	p->pwUpTime[n] = std::min(p->pwUpTime[n] + add, gPowerUpInfo[n].maxTime);
	return fresh;
}

// The time an Omen still holds taken off its stock powerup, the map's own left; at 0 it ends as Blood ends it. Leaving
// the map, it doesn't: Blood's player start clears every powerup on the next map without ending one, and the weapon's
// state is carried over from this moment (the akimbo's end would raise the gun again).
void TakePowerTime(DBloodPlayer* p, int n, int left, bool leaving)
{
	if (n < 0 || p->pwUpTime[n] <= 0) return;
	p->pwUpTime[n] = std::max(p->pwUpTime[n] - std::max(left, 0), 0);
	if (p->pwUpTime[n] == 0 && !leaving) powerupDeactivate(p, n);
}

// An Omen ends: its base effect stops, if it started it; and with stock (rl_omens going off, the map ending: leaving)
// the time it still holds on a stock powerup goes too. Running out needs no more: that time ran out with it.
void EndOmen(unsigned i, bool stock, bool leaving = false)
{
	auto r = omens[i];
	omens.Delete(i);
	if (r.owns)
		if (auto e = OmenEffect(r.id)) bmod::StopEffect(e);
	auto p = getPlayer(myconnectindex);
	if (stock && p) TakePowerTime(p, omenInfo[r.id].powerup, r.left, leaving);
}

void EndOmens(bool stock, bool leaving)
{
	while (omens.Size()) EndOmen(omens.Size() - 1, stock, leaving);
	reliquaryHint = "";
}

// A map starts: nothing runs (the base stopped the effects as the last one went, and Blood's player start clears the
// powerups).
void ResetOmens()
{
	omens.Clear();
	reliquaryHint = "";
	reliquaryTouched = nullptr;
	lustSecond = -1;
	lustSoundAt = INT_MIN / 2;
	omenPowerItem = nullptr;
}

// An Omen can begin now (one running can always take more time): not when its base effect can't run (Dread and Frenzy
// need enemies left), nor when starting it would end another Omen's (Dread's slow and Frenzy's speed are rivals: the one
// running stays, and the newcomer waits on the floor). *rival: that Omen, else -1.
bool CanGrant(int id, int* rival = nullptr)
{
	if (rival) *rival = -1;
	if (id < 0 || id >= kNumOmens) return false;
	if (RunningOf(id)) return true;
	auto e = OmenEffect(id);
	if (omenInfo[id].effect && (!e || (!bmod::IsActive(e) && !bmod::CanStart(e)))) return false;
	for (auto& r : omens)
		if (bmod::Rivals(OmenEffect(r.id), e))
		{
			if (rival) *rival = r.id;
			return false;
		}
	return true;
}

// An Omen begins, or one running gets more time: clocks of it, up to 3x its time. False: it can't take hold now
// (CanGrant), and its pickup stays. A base effect the curse or chaos runs already does: the Omen rides it, and leaves it
// running as it ends.
bool GrantOmen(int id, int clocks, bool* chimed = nullptr)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor() || id < 0 || id >= kNumOmens || clocks <= 0) return false;
	auto& o = omenInfo[id];
	auto r = RunningOf(id);
	if (!r)
	{
		if (!CanGrant(id)) return false;
		auto e = OmenEffect(id);
		bool owns = e && !bmod::IsActive(e) && bmod::StartEffect(e); // ends a rival first: Dread's slow ends a fast curse
		omens.Push({ id, 0, clocks, 0, owns });
		r = &omens.Last();
	}
	r->base = std::max(r->base, clocks);
	int left = std::max(std::min(r->left + clocks, 3 * r->base), r->left);
	bool fresh = AddPowerTime(p, o.powerup, left - r->left);
	r->left = left;
	r->peak = std::max(r->peak, left);
	if (chimed) *chimed = fresh;
	return true;
}

// A kill of yours: a second more on each boon running, up to twice its time. A gamble's time stays as it said.
void ExtendOmens()
{
	auto p = getPlayer(myconnectindex);
	if (!p || !OmensLive()) return;
	for (auto& r : omens)
	{
		if (omenInfo[r.id].tier == kOmenGamble || r.left >= 2 * r.base) continue;
		int left = std::min(r.left + kOmenKillTime, 2 * r.base);
		AddPowerTime(p, omenInfo[r.id].powerup, left - r.left);
		r.left = left;
		r.peak = std::max(r.peak, left);
	}
}

// Bloodlust: a quarter of the health your hit took from a live foe (taken, so none past its death), at most 15 health a
// second and never above 100, and less under the Covenant's Famine. Its cap is its own, apart from the cleavers'. The
// heal sound plays at most every half second, and only when health rose.
void BloodlustHeal(int lost)
{
	auto p = getPlayer(myconnectindex);
	auto pl = p ? p->GetActor() : nullptr;
	if (!pl || !pl->hasX() || pl->xspr.health <= 0 || lost <= 0) return;
	int health = int(pl->xspr.health);
	if (health >= kLustTop) return;
	int add = lost / 4;
	if (run.cov[kFamine] > 0) add = int(add * std::max(0., 1. - 0.25 * run.cov[kFamine]));
	int second = PlayClock / kClock;
	if (second != lustSecond) lustSecond = second, lustHealed = 0;
	add = std::min({ add, kLustPerSecond - lustHealed, kLustTop - health });
	if (add <= 0) return;
	lustHealed += add;
	pl->xspr.health = unsigned(health + add);
	if (PlayClock - lustSoundAt >= kClock / 2 || PlayClock < lustSoundAt)
	{
		lustSoundAt = PlayClock;
		sfxPlay3DSound(pl->spr.pos, 780, pl->sector());
	}
}

// An Omen's pickup or a cursed reliquary lying on the floor at p: an inert item drawn as its look, in its tier's
// palette, lit (a reliquary half as big again); with an xsprite, so its mark and its Omen (data1) are saved with it.
DBloodActor* SpawnOmenItem(sectortype* s, const DVector2& p, int id)
{
	auto& o = omenInfo[id];
	auto& d = gItemData[o.look - kItemBase];
	if (!s || !d.textureID().isValid()) return nullptr;
	double look = o.tier == kOmenGamble ? kReliquaryLook : 1;
	double fz = getflorzofslopeptr(s, p.X, p.Y);
	auto a = actSpawnSprite(s, DVector3(p, fz), kStatItem, true);
	if (!a) return nullptr;
	a->spr.cstat &= ~CSTAT_SPRITE_BLOCK_ALL;
	a->spr.type = a->spr.inittype = kOmenItem;
	a->spr.setspritetexture(d.textureID());
	a->spr.shade = kOmenShade;
	a->spr.pal = omenPal[o.tier];
	a->spr.scale = DVector2(d.xrepeat * look * REPEAT_SCALE, d.yrepeat * look * REPEAT_SCALE);
	a->modFlags |= kModOmen;
	a->xspr.data1 = int16_t(id);
	double top, bottom;
	GetActorExtents(a, &top, &bottom);
	a->spr.pos.Z -= bottom - fz;
	a->basePoint = a->spr.pos;
	a->interpolated = false;
	viewBackupSpriteLoc(a);
	return a;
}

// A drop lies beside where the enemy fell, in sight of the spot (not on its armour or its stock drop); with no room
// there, on the spot itself if a pickup can lie there; else as a champion's armour goes (near it, or else near you).
// A reliquary, drawn big, keeps further from walls.
void PlaceOmenItem(DBloodActor* item, std::mt19937& rng)
{
	if (!item->insector()) return;
	TArray<DVector2> taken;
	DVector2 spot;
	sectortype* sect = nullptr;
	double clear = omenInfo[item->xspr.data1].tier == kOmenGamble ? kReliquaryClear : 0;
	bool found = FindItemSpot(rng, item->sector(), FloorAt(item), 24, 64, taken, spot, sect, nullptr, 16, clear);
	if (!found && SpotSectorOk(item->sector())) return;
	if (!found) found = FindItemSpot(rng, item->sector(), FloorAt(item), 16, 96, taken, spot, sect, nullptr, 16, clear);
	auto p = getPlayer(myconnectindex);
	auto pl = p ? p->GetActor() : nullptr;
	if (!found && pl && pl->insector()) found = FindItemSpot(rng, pl->sector(), FloorAt(pl), 24, 80, taken, spot, sect, nullptr, 16, clear);
	if (!found || !sect) return;
	LayDrop(item, sect, spot);
	item->basePoint = item->spr.pos;
}

int PickOmen(std::mt19937& rng, int tier)
{
	int ids[kNumOmens], n = 0;
	for (int i = 0; i < kNumOmens; i++) if (omenInfo[i].tier == tier) ids[n++] = i;
	return n ? ids[RandInt(rng, 0, n - 1)] : -1;
}

// As an enemy dies in a run (outside the Crypt): a champion's Omen (a blue one's common 65% or rare, a red one's rare
// 75% or a relic), a plain enemy's one time in 25 (common 70%, rare 25%, relic 5%); and besides, a red champion's
// cursed reliquary one time in 3, a blue one's one time in 10, a random gamble in it. The rolls are the run's (its seed
// and the deaths rolled so far, which a save keeps), so a reload rolls the same. A reliquary falls with a bell at the
// chest and a message (an Omen falls silent: walking into it takes it).
void DropOmens(DBloodActor* victim, bool champion, bool red)
{
	if (!rl_omens || !victim || !victim->insector()) return;
	std::mt19937 rng(run.seed ^ (0x9E3779B9u * uint32_t(run.omenRolls + 1)) ^ 0x6A09E667u);
	run.omenRolls++;
	int tier = -1;
	double r = Rand01(rng);
	if (red) tier = r < kOmenRedRelic ? kOmenRelic : kOmenRare;
	else if (champion) tier = r < kOmenBlueRare ? kOmenRare : kOmenCommon;
	else if (r < kOmenPlain)
	{
		double t = Rand01(rng);
		tier = t < kOmenPlainRelic ? kOmenRelic : t < kOmenPlainRelic + kOmenPlainRare ? kOmenRare : kOmenCommon;
	}
	int omen = tier >= 0 ? PickOmen(rng, tier) : -1;
	double reliquary = red ? kReliquaryRed : champion ? kReliquaryBlue : 0;
	int gamble = reliquary > 0 && Rand01(rng) < reliquary ? PickOmen(rng, kOmenGamble) : -1;
	for (int id : { omen, gamble })
	{
		if (id < 0) continue;
		auto s = victim->sector();
		auto pos = victim->spr.pos;
		updatesector(pos, &s);
		if (!s) s = victim->sector(); // blasted outside every sector (as actSpawnFloor has it)
		auto item = SpawnOmenItem(s, pos.XY(), id);
		if (!item) continue;
		PlaceOmenItem(item, rng);
		if (id != gamble) continue;
		sfxPlay3DSound(item->spr.pos, kReliquaryBell, item->sector()); // unattached: it rings on if the chest is taken at once
		viewSetMessage("A cursed reliquary falls", TEXTCOLOR_PURPLE); // the gambles' colour (the hint, the chip)
	}
}

bool IsReliquary(DBloodActor* a)
{
	return IsOmenItem(a) && omenInfo[a->xspr.data1].tier == kOmenGamble && !(a->spr.flags & 32) && a->insector();
}

// The cursed reliquary you face, in sight; the nearest that can take hold now (CanGrant). *blocked: the nearest you face
// that can't, so the hint can say why. Facing one to take it (Use): within kReliquaryRange of your eye and
// kReliquaryCone of your view (where Use's own scan aims, its pitch too: one at your feet you look down at). Facing one
// to name it (naming): within kReliquaryNameRange along the floor and kReliquaryNameCone of where you face, your pitch
// aside, as one lying ahead is below your view until you're nearly on it.
DBloodActor* NearReliquary(DBloodPlayer* p, DBloodActor** blocked = nullptr, bool naming = false)
{
	if (blocked) *blocked = nullptr;
	auto pl = p ? p->GetActor() : nullptr;
	if (!pl || !pl->insector()) return nullptr;
	DVector3 eye(pl->spr.pos.XY(), p->zView);
	DVector3 view = DVector3(pl->spr.Angles.Yaw.ToVector(), p->slope).Unit(); // as ActionScan aims
	DVector2 facing = pl->spr.Angles.Yaw.ToVector();
	double range = naming ? kReliquaryNameRange : kReliquaryRange;
	double cone = DAngle::fromDeg(naming ? kReliquaryNameCone : kReliquaryCone).Cos();
	DBloodActor* best = nullptr;
	double bestDist = range, blockedDist = range;
	BloodStatIterator it(kStatItem);
	while (auto a = it.Next())
	{
		if (!IsReliquary(a)) continue;
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		DVector3 mid(a->spr.pos.XY(), (top + bottom) / 2);
		DVector3 d = mid - eye;
		double dist = naming ? d.XY().Length() : d.Length();
		double ahead = naming ? (d.XY() | facing) : (d | view);
		if (dist >= range || ahead < dist * cone) continue;
		bool takes = CanGrant(a->xspr.data1);
		double& nearest = takes ? bestDist : blockedDist;
		if ((!takes && !blocked) || dist >= nearest || !cansee(eye, pl->sector(), mid, a->sector())) continue;
		nearest = dist;
		(takes ? best : *blocked) = a;
	}
	return best;
}

// Why a gamble can't take hold now (CanGrant false): "while Dread runs", or "here now" (no enemies left for Frenzy).
FString HoldBar(int id)
{
	int rival;
	CanGrant(id, &rival);
	return rival >= 0 ? FStringf("while %s runs", omenInfo[rival].name) : FString("here now");
}

// Use with nothing else to do (ActionScan found nothing to push: a door, switch or the like comes first; a fresh press,
// as Blood's Use is a one-shot input) facing a cursed reliquary: its gamble begins, or one running gets its time again.
// One that can't take hold now says why and stays. True: the press took one.
bool TakeReliquary(DBloodPlayer* p)
{
	if (!OmensLive() || !p || p != getPlayer(myconnectindex)) return false;
	DBloodActor* blocked;
	auto a = NearReliquary(p, &blocked);
	if (!a)
	{
		if (blocked)
			viewSetMessage(FStringf("%s can't take hold %s", omenInfo[blocked->xspr.data1].name, HoldBar(blocked->xspr.data1).GetChars()).GetChars());
		return false;
	}
	int id = a->xspr.data1;
	auto& o = omenInfo[id];
	if (!GrantOmen(id, o.seconds * kClock)) return false; // CanGrant said it could: it can't fail
	sfxPlay3DSound(p->GetActor(), 463, -1, 0); // the voodoo doll's laugh
	p->pickupEffect = 30;
	viewSetMessage(FStringf("Cursed reliquary: %s, %d s: %s", o.name, bmod::SecondsLeft(RunningOf(id)->left), o.sentence).GetChars(),
		nullptr, MESSAGE_PRIORITY_PICKUP);
	actPostSprite(a, kStatFree);
	reliquaryHint = "";
	return true;
}

// Each tic: the Omens' time runs down, and one whose base effect or stock powerup was ended by another (chaos starting a
// rival, a powerup cleared) is dropped; one riding an effect another ran takes it over as that one stops on its own (not
// when a rival replaced it: restarting would end the newcomer). Then a reliquary near you is named, with how to take it
// or why it can't take hold now. Not live (rl_omens off, the run over, the Crypt): everything ends.
void OmenTick()
{
	if (!OmensLive())
	{
		if (omens.Size()) EndOmens(true);
		reliquaryHint = "";
		return;
	}
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	for (int i = int(omens.Size()) - 1; i >= 0; i--)
	{
		auto& r = omens[i];
		auto& o = omenInfo[r.id];
		r.left -= kTicsPerFrame;
		bool lost = o.powerup >= 0 && p->pwUpTime[o.powerup] <= 0;
		auto e = OmenEffect(r.id);
		if (e && !bmod::IsActive(e))
		{
			if (r.owns || RivalActive(e) || !bmod::CanStart(e) || !bmod::StartEffect(e)) lost = true;
			else r.owns = true;
		}
		if (lost || r.left <= 0) EndOmen(i, false);
	}
	// The hint: the reliquary Use would act on; else one you walked into just now; else one you face further off.
	DBloodActor* blocked;
	auto rel = NearReliquary(p, &blocked);
	if (!rel) rel = blocked;
	bool inReach = rel != nullptr;
	if (!rel && PlayClock - reliquaryTouchedAt >= 0 && PlayClock - reliquaryTouchedAt < kReliquaryTouchHint)
	{
		BloodStatIterator it(kStatItem);
		while (auto a = it.Next())
			if (a == reliquaryTouched && IsReliquary(a)) rel = a;
	}
	if (!rel)
	{
		rel = NearReliquary(p, &blocked, true);
		if (!rel) rel = blocked;
	}
	if (!rel) reliquaryHint = "";
	else
	{
		int id = rel->xspr.data1;
		auto& o = omenInfo[id];
		FString tail = !CanGrant(id) ? FStringf("It can't take hold %s.", HoldBar(id).GetChars())
			: FString(inReach ? "Use to take." : "Look down at it and Use to take.");
		reliquaryHint.Format("Cursed reliquary: %s: %s, %d s. %s", o.name, o.sentence, o.seconds, tail.GetChars());
	}
}

// A save loaded: effects aren't saved (the base stops them all as a level goes), so each Omen's starts again, unless
// the curse (restarted just before) runs it already. The stock powerups' time is the save's own.
void OmensLoaded()
{
	reliquaryHint = "";
	reliquaryTouched = nullptr;
	lustSecond = -1;
	lustSoundAt = INT_MIN / 2;
	omenPowerItem = nullptr;
	if (!OmensLive())
	{
		EndOmens(true);
		return;
	}
	for (auto& r : omens)
	{
		auto e = OmenEffect(r.id);
		r.owns = e && !bmod::IsActive(e) && bmod::StartEffect(e);
	}
}

// A map's powerup taken while an Omen runs on its timer: Blood's pickup sets only a timer at 0, so it would have given
// nothing. Its own time goes on top (and stays as the Omen ends).
void StackPowerPickup(DBloodActor* item)
{
	omenPowerItem = nullptr;
	auto p = getPlayer(myconnectindex);
	int n = item->spr.type - kItemBase;
	if (!p || n < 0 || n >= kMaxPowerUps || p->pwUpTime[n] > omenPowerBefore) return;
	p->pwUpTime[n] = std::min(omenPowerBefore + gPowerUpInfo[n].bonusTime, gPowerUpInfo[n].maxTime);
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
		ResetLitanies();
		StaggerReset();
		ResetOmens();
		bmod::modeActorSpeed = RedSpeed; // only actors a run marked are faster
		bmod::modeWaveSpawned = WaveSpawned;
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
		LevelStarted();
		break;
	case bmod::Event::SaveLoaded:
		LitaniesLoaded();
		StaggerReset(); // the flinches aren't saved: a loaded red can flinch at once
		bmod::modeActorSpeed = RedSpeed;
		bmod::modeWaveSpawned = WaveSpawned;
		nextCurseTry = 0;
		ledger = AmmoLedger(); // rl_ammo's "at the start" belongs to the map as it started, not to this load
		if (IsCryptLevel()) SetupCrypt();
		else crypt.in = false;
		if (run.active && !run.over) StartCurse(); // effects aren't saved: the curse comes back on
		OmensLoaded(); // and the Omens' (after the curse, so one the curse runs is the curse's)
		break;
	case bmod::Event::LevelEnd:
		EndOmens(true, true); // the Omens end with the map (a map's own powerup keeps its time)
		break;
	case bmod::Event::Kill:
		if (run.active && !run.over && AllowedKillType(ev.actor))
		{
			bool champion = (ev.actor->modFlags & kModChampion) != 0, red = (ev.actor->modFlags & kModRedChampion) != 0;
			run.kills++;
			if (champion) run.champions++;
			run.ichor += red ? kIchorRed : champion ? kIchorChampion : kIchorKill;
			if (champion && !crypt.in) DropChampionArmour(ev.actor, red);
			if (!crypt.in) DropOmens(ev.actor, champion, red);
			if (ByYou(ev.other)) ExtendOmens();
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
	StaggerTick(); // first: this ticker runs right after the tic's AI pass, before the Forge's (whose hits count next)
	if (refusedLoad)
	{
		refusedLoad = false;
		menuMessage = "THAT RUN IS OVER\n\nA run ends when you die, when you win it,\nor when you start another one.\n\nStart a new run from the Roguelite menu.";
		gameaction = ga_mainmenu;
		return;
	}
	bmod::modeEnemyTimeScale = run.active && !run.over ? 1. + 0.15 * run.cov[kHaste] : 1.; // the Covenant's Haste lasts the run
	OmenTick(); // before the curse's retry, which then finds a rival an Omen just let go
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

void DrawLitanyTitle()
{
	int age = PlayClock - litanyShownAt;
	if (litanyShown < 0 || litanyShown >= kNumLitanies || age < 0 || age >= kLitanyTitle) return;
	double alpha = std::clamp((kLitanyTitle - age) / (0.5 * kClock), 0., 1.);
	FStringf title("Litany: %s", litanies[litanyShown].name);
	double w = SmallFont->StringWidth(title.GetChars());
	DrawText(twod, SmallFont, CR_GOLD, 160 - w / 2, 50 + 2 * BigFont->GetHeight() + 24, title.GetChars(), DTA_FullscreenScale, FSMode_Fit320x200,
		DTA_Alpha, alpha, TAG_DONE);
}

// The Omens' chips in the shared row, oldest first (gold a boon, purple a gamble, blinking their last 3 s). The map's
// curse is on the run line, but "No HUD" (chaos's, or the curse itself) hides that: then, with the Omens on (rl_omens),
// it's a red chip first, with no countdown (while it runs: one a chaos rival holds off isn't). And the cursed reliquary
// you face, named where the Crypt's prompts go: even under "No HUD", as it's how you choose whether to take one.
void DrawOmens()
{
	auto curse = CurseEffect();
	if (bmod::mods.hideHud && OmensLive() && curse && bmod::IsActive(curse))
	{
		bmod::Chip c;
		c.text.Format("Curse: %s", bmod::EffectTitle(curse));
		c.color = PalEntry(kCurseChip);
		bmod::AddChip(c);
	}
	for (auto& r : omens)
	{
		auto& o = omenInfo[r.id];
		bmod::Chip c;
		c.text.Format("%s %ds", o.name, bmod::SecondsLeft(r.left));
		c.color = PalEntry(o.tier == kOmenGamble ? kGambleChip : kBoonChip);
		c.frac = double(r.left) / std::max({ r.base, r.peak, 1 });
		c.alpha = r.left <= 3 * kClock && (PlayClock / 15) % 2 ? 0.3 : 1.;
		bmod::AddChip(c);
	}
	if (reliquaryHint.IsEmpty()) return;
	double W = twod->GetWidth(), ssc = 1.5 * twod->GetHeight() / 480.;
	double w = SmallFont->StringWidth(reliquaryHint.GetChars());
	if (w * ssc > W * 0.9) ssc = W * 0.9 / w; // one line, whatever the screen
	DrawCentered(SmallFont, CR_PURPLE, twod->GetHeight() * 0.62, reliquaryHint.GetChars(), ssc);
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
	DrawLitanyTitle();
	DrawOmens(); // the chip row isn't the HUD: chaos's chips show under "No HUD" too
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

void BuildBookMenu();

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
		// Items 6-10: Vessel..Penance; 11 blank; 12 the Litanies' heading; 13, 15, 17, 19 the lit Litanies (centred, short)
		// and 14, 16, 18, 20 their sentences; 21 the riddle.
		if (m->mItems.Size() > 21)
		{
			int lit[4], n = LitLitanies(f, lit, 4), sing = ResolveSinging(f);
			for (int i = 0; i < 4; i++)
			{
				if (i >= n)
				{
					SetLabel(m->mItems[13 + 2 * i], " ");
					SetLabel(m->mItems[14 + 2 * i], " ");
					continue;
				}
				int k = lit[i];
				auto& L = litanies[k];
				SetLabel(m->mItems[13 + 2 * i], FStringf("%s %s (%s%s)%s", (sing & (1 << k)) ? "[sings]" : "[silent]", L.name, litanyKinds[L.kind],
					L.kind == kKindBane ? ", +1 Dread" : "", (litanyNew & (1u << k)) ? " NEW" : ""));
				SetLabel(m->mItems[14 + 2 * i], L.sentence);
			}
			FString riddle = LitanyRiddle(f);
			if (riddle.IsEmpty() && f[kForgeVessel] && !n) riddle = "No Litany yet: carve runes from other circles.";
			SetLabel(m->mItems[21], riddle.IsEmpty() ? FString(" ") : riddle);
		}
	}
	BuildBookMenu();
	if (auto m = FindOptionMenu("RogueliteCovenant"))
	{
		int dread = ChosenDread();
		m->mTitle = dread ? FStringf("The Covenant: Dread %d", dread) : FString("The Covenant");
		if (m->mItems.Size())
			SetLabel(m->mItems[0], dread ? FStringf("Dread %d: +%d%% ichor", dread, int(kDreadIchor * 100 * dread + 0.5)) : FString("No conditions: Dread 0"));
	}
}

// The Book of Litanies: the found ones with their parts, the rest as riddles.
void BuildBookMenu()
{
	auto opt = FindOptionMenu("RogueliteBook");
	if (!opt) return;
	opt->mItems.Clear();
	auto add = [&](DMenuItemBase* item)
	{
		if (!item) return;
		GC::WriteBarrier(opt, item);
		opt->mItems.Push(item);
	};
	auto text = [&](const FString& t, int color) { add(CreateOptionMenuItemStaticText(t.GetChars(), color < 0 ? -1 : 0x12340000 | color)); };
	int found = 0;
	for (int k = 0; k < kNumLitanies; k++) if (LitanyFound(k)) found++;
	opt->mTitle.Format("The Book of Litanies: %d of %d", found, kNumLitanies);
	text(" ", -1);
	static const int kindColour[] = { CR_GREEN, CR_GOLD, CR_RED, CR_PURPLE };
	for (int k = 0; k < kNumLitanies; k++)
	{
		auto& L = litanies[k];
		if (LitanyFound(k))
		{
			text(FStringf("%s (%s): %s + %s", L.name, litanyKinds[L.kind], PartName(L.a).GetChars(), PartName(L.b).GetChars()), kindColour[L.kind]);
			text(L.sentence, CR_GRAY);
		}
		else text(FStringf("??? (%s): a part of %s and one of %s", litanyKinds[L.kind], circleNames[L.a.slot], circleNames[L.b.slot]), CR_DARKGRAY);
	}
	opt->mSelectedItem = -1;
	opt->mScrollPos = 0;
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
		bmod::AddTicker([] { ForgeTick(); });
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

// Use with nothing in reach to push: a cursed reliquary you face is taken (the Omens). A door or switch comes first.
bool OnUseNothing(DBloodPlayer* pPlayer)
{
	return TakeReliquary(pPlayer);
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
// A forged cleaver's impact: the stock code wounds the foe, then heals you; the heal is half what the wound took.
static bool forgeBiting;               // inside a forged cleaver's impact
static DBloodActor* forgeBite;         // the live foe it struck (null: none, so no heal)
static int forgeBiteHealth;            // that foe's health before the wound (sixteenths)
static bool forgeBiteDeeper;           // its stock wound is still to come, and bites deeper (ScaleDamage)
static DBloodActor* forgeSparingNow;   // the Larder's blast being worked out: it doesn't hurt you (kModSpareYou)
static DBloodActor* forgeBlastNow;     // any forged blast being worked out: yours hurts you half
static DBloodActor* forgeScent;          // Blood Feud: the last foe that drew your blood (compared, never followed)
static int forgeScentAt = INT_MIN / 2;
static DBloodActor* forgeExplosionNow;   // Brimstone Leap: the forged blast being worked out (kModKicked once it threw you)
static bool Sings(int k) { return run.active && !run.over && (run.litanies & (1 << k)); }
constexpr double kLeapKick = -14;        // Brimstone Leap's throw, upward (z grows down)
static void LitanyFired(int k);

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

// An Omen's pickup is the run's: walked into, it begins (or adds its time), with Blood's powerup chime, the pickup flash
// and a message. A cursed reliquary isn't taken by walking over it (Use takes it: TakeReliquary), only named (OmenTick);
// nor is either with the Omens off (rl_omens 0, the run over), when they lie inert. A map's powerup the stock pickup is
// about to take while an Omen runs on its timer is noted, for PickupTaken.
bool ClaimPickup(DBloodPlayer* pPlayer, DBloodActor* item)
{
	omenPowerItem = nullptr;
	if (!pPlayer || !item) return false;
	if (!IsOmenItem(item))
	{
		int n = item->spr.type - kItemBase;
		if (n >= 0 && n < kMaxPowerUps && pPlayer == getPlayer(myconnectindex) && OmensLive())
			for (auto& r : omens)
				if (omenInfo[r.id].powerup == n)
				{
					omenPowerItem = item;
					omenPowerBefore = pPlayer->pwUpTime[n];
				}
		return false;
	}
	if (pPlayer != getPlayer(myconnectindex) || !OmensLive()) return true;
	int id = item->xspr.data1;
	auto& o = omenInfo[id];
	if (o.tier == kOmenGamble)
	{
		reliquaryTouched = item; // OmenTick names it, and how to take it
		reliquaryTouchedAt = PlayClock;
		return true;
	}
	bool chimed = false;
	if (!GrantOmen(id, o.seconds * kClock, &chimed)) return true; // it can't take hold now (CanGrant): it stays
	if (!chimed) sfxPlay3DSound(pPlayer->GetActor(), 776, -1, 0); // Blood's powerup chime (powerupActivate plays it)
	pPlayer->pickupEffect = 30;
	viewSetMessage(FStringf("Omen: %s, %d s: %s", o.name, bmod::SecondsLeft(RunningOf(id)->left), o.sentence).GetChars(),
		nullptr, MESSAGE_PRIORITY_PICKUP);
	actPostSprite(item, kStatFree);
	return true;
}

bool PickupTaken(DBloodActor* item, const char*& msg)
{
	if (item && item == omenPowerItem) StackPowerPickup(item);
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

//---------------------------------------------------------------------------
//
// the champion hooks: their damage, their flinches
//
//---------------------------------------------------------------------------

// A foe's hitscan shot as it's worked out: SwallowVector sees every vector fired, and actFireVector deals the wound
// within the same call. Bites, slashes and missiles wound as bullets too (kDamageBullet), so the damage type alone
// can't tell a cultist's bullet from an axe. Compared, never followed.
static DBloodActor* foeHitscan;
static int foeHitscanAt = INT_MIN / 2;

// The red champions you hit: what you dealt each since the AI last looked (StaggerTick clears it right after the AI's
// pass, as aiProcessDudes clears cumulDamage), and when it last flinched. Compared, never followed: a red that dies or
// is freed stays only until its 3 seconds are up, and the list is emptied as each map starts and on each load.
struct Stagger { DBloodActor* red; int hit; int flinchAt; };
static TArray<Stagger> staggers;

static void StaggerHit(DBloodActor* red, int damage)
{
	for (auto& t : staggers)
		if (t.red == red)
		{
			t.hit += damage;
			return;
		}
	staggers.Push({ red, damage, INT_MIN / 2 });
}

// Right after the tic's AI pass (the run's ticker): the hits are counted afresh, and a red whose flinch is 3 seconds
// old (or that was never made to flinch) is let go. A tic with no AI pass (enemies slowed) keeps counting, as
// cumulDamage does.
static void StaggerTick()
{
	if (staggers.Size() == 0 || bmod::EnemyPasses() <= 0) return;
	for (int i = int(staggers.Size()) - 1; i >= 0; i--)
	{
		staggers[i].hit = 0;
		if (PlayClock - staggers[i].flinchAt >= kStaggerEvery || staggers[i].flinchAt > PlayClock) staggers.Delete(i);
	}
}

static void StaggerReset() { staggers.Clear(); foeHitscan = nullptr; }

int ScaleDamage(DBloodActor* source, DBloodActor* target, DAMAGE_TYPE damageType, int damage)
{
	if (!source || !target || damage <= 0) return damage;
	if (forgeEmitting && source->IsPlayerActor() && target != source && target->IsDudeActor() && !target->IsPlayerActor())
		if (forgeHits.Find(target) == forgeHits.Size()) forgeHits.Push(target); // a forged hitscan's hit
	// A forged cleaver bites deeper than the Butcher's: its stock wound (the first the impact deals the foe) grows, so
	// the heal that follows it (ScaleHealing) counts the whole of it. Dealt apart, after, it could land on a foe the
	// stock wound had just killed.
	if (forgeBiteDeeper && target == forgeBite && source->IsPlayerActor())
	{
		forgeBiteDeeper = false;
		damage += (kCleaverBite + int(Random(kCleaverBiteRange))) << 4;
	}
	// Brimstone Leap: your own forged blast throws you, the first tic it reaches you, and doesn't wound you.
	if (forgeExplosionNow && target->IsPlayerActor() && target == getPlayer(myconnectindex)->GetActor())
	{
		if (!(forgeExplosionNow->modFlags & kModKicked))
		{
			forgeExplosionNow->modFlags |= kModKicked;
			DVector2 away = target->spr.pos.XY() - forgeExplosionNow->spr.pos.XY();
			if (away.LengthSquared() > 1) target->vel.XY() += away.Unit() * 4.;
			target->vel.Z = std::min(target->vel.Z, kLeapKick); // a little over a jump (-11.7): a 64-unit ledge from your feet
			LitanyFired(kLitLeap);
		}
		return 0;
	}
	// The Larder of Knives: its blasts are set for foes and spare you.
	if (forgeSparingNow && target->IsPlayerActor() && target == getPlayer(myconnectindex)->GetActor()) return 0;
	// Any other forged blast of yours (the Grave's, a fireball's) hurts you half: the Forge's blasts land close.
	if (forgeBlastNow && target->IsPlayerActor() && target == getPlayer(myconnectindex)->GetActor() && forgeBlastNow->GetOwner() == target)
		damage -= damage / 2;
	auto s = source;
	for (int i = 0; i < 2 && s && !s->IsDudeActor(); i++) s = s->GetOwner(); // a missile's or explosion's owner
	if (s && s != target && s->IsDudeActor() && !s->IsPlayerActor() && target->IsPlayerActor())
	{
		forgeScent = s; // Blood Feud's quarry
		forgeScentAt = PlayClock;
	}
	// Fury and Frenzy (Omens): your hits on foes, times their factor, on top of the Forge's bite and the base's Glass
	// Cannon (applied before this), as those stack already. A red champion's flinch counts the hit as dealt.
	if (s && s != target && s->IsPlayerActor() && target->IsDudeActor() && !target->IsPlayerActor())
		if (int deal = OmenDeal(); deal != 1) damage *= deal;
	// Your hit on a red champion, as dealt (before its own armour: its type's and the skill's), for its flinch.
	if (s && s != target && s->IsPlayerActor() && (target->modFlags & kModRedChampion) && target->spr.statnum == kStatDude)
		StaggerHit(target, damage);
	if (!s || s == target || !s->IsDudeActor() || s->IsPlayerActor()) return damage;
	// A foe's hitscan, its bullets and shells (the tommy and shotgun cultists'), takes less of the tiers' bonus and of
	// Wrath's: on Extra Crispy, with Wrath's 5 ranks, a red tommy cultist's rounds at 1.5x and 2x killed in 0.2 s.
	// Its bites, slashes, missiles and blasts keep the whole bonus.
	bool hitscan = damageType == kDamageBullet && source == foeHitscan && foeHitscanAt == PlayClock;
	if (s->modFlags & kModRedChampion) damage += hitscan ? damage / 5 : damage / 2;   // red champions hit 1.5x (1.2x shooting)
	else if (s->modFlags & kModChampion) damage += hitscan ? damage / 10 : damage / 4; // champions 1.25x (1.1x shooting)
	if (run.active && !run.over && target->IsPlayerActor() && run.cov[kWrath] > 0)
		damage = int(damage * (1. + (hitscan ? 0.1 : 0.2) * run.cov[kWrath]) + 0.5); // the Covenant's Wrath
	return damage;
}

// Bloodlust: a hit of yours on a live foe, its health as the hit began; the heal comes once the wound is dealt.
OmenWound::OmenWound(DBloodActor* source, DBloodActor* target)
{
	if (!omens.Size() || !source || !target || target->IsPlayerActor() || !AllowedKillType(target) || !target->hasX()
		|| target->xspr.health <= 0 || !BloodlustOn() || !ByYou(source))
		return;
	foe = target;
	healthBefore = int(target->xspr.health);
}

OmenWound::~OmenWound()
{
	if (foe && foe->hasX()) BloodlustHeal(healthBefore - std::min(healthBefore, int(foe->xspr.health)));
}

// Red champions don't flinch (nor stagger under tesla), but for a heavy hit of yours: one that deals a red 32 health or
// more, as dealt, before its armour (the tic's hits together, as Blood adds them up for a flinch, so a sawed-off
// blast's pellets count as one hit), at most once every 3 seconds each. It breaks a red tommy cultist's endless burst.
// RecoilDude still asks only when the tic's wound (after armour) is one that would make the stock enemy flinch.
bool Unflinching(DBloodActor* actor)
{
	if (!actor || !(actor->modFlags & kModRedChampion)) return false;
	for (auto& t : staggers)
		if (t.red == actor)
		{
			if (t.hit < kStaggerHit || PlayClock - t.flinchAt < kStaggerEvery) return true;
			t.hit = 0;
			t.flinchAt = PlayClock;
			return false;
		}
	return true;
}

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

// How a firing callback of the forged gun fires: as its forged primary, as its forged alt mode (both barrels, the
// tommy's sweep, the flare's starburst), or stock (its other callbacks, and alt fire with rl_forge_alt off).
enum { kFireStock, kFirePrimary, kFireAlt };
static int FireMode(DBloodPlayer* p, int vessel, int nCallback)
{
	bool alt = false;
	switch (vessel)
	{
	case kVesselFlare:
		if (nCallback == nClientFireFlare) return kFirePrimary;
		alt = nCallback == nClientAltFireFlare;
		break;
	case kVesselShotgun: // alt fire shares FireShotgun: both barrels (and the akimbo pair) are told apart by their QAV
		if (nCallback != nClientFireShotgun) return kFireStock;
		alt = p->weaponQav == qavGetCorrectID(kQAVSHOTF3) || p->weaponQav == qavGetCorrectID(kQAV2SHOTFIR);
		if (!alt) return kFirePrimary;
		break;
	case kVesselTommy:
		if (nCallback == nClientFireTommy) return kFirePrimary;
		alt = nCallback == nClientAltFireSpread2;
		break;
	}
	return alt && rl_forge_alt ? kFireAlt : kFireStock;
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
	if (!a || !a->hasX() || a->xspr.health <= 0) return false;
	if (Sings(kLitLastRites)) return true; // Last Rites: at any health
	return a->xspr.health > unsigned((kBloodPrice << 4) + 16);
}

// Taken from health directly, as the dry life leech feeds on you, but never to death and never scaled by skill.
static bool PayInBlood(DBloodPlayer* p)
{
	if (!CanPayInBlood(p)) return false;
	auto a = p->GetActor();
	if (a->xspr.health < unsigned((kBloodPrice << 4) + 16)) // only Last Rites gets here: the price is your life
	{
		// A damage type the jump boots or a powerup don't block (any armour can't soak this much).
		static const DAMAGE_TYPE kinds[] = { kDamageSpirit, kDamageBullet, kDamageExplode, kDamageBurn, kDamageTesla, kDamageDrown, kDamageFall };
		for (auto t : kinds)
			if (!p->damageControl[t] && !p->godMode)
			{
				LitanyFired(kLitLastRites);
				actDamageSprite(a, a, t, 1000 << 4);
				break;
			}
		return false; // the killing pull fires nothing (and one that can't kill doesn't fire)
	}
	a->xspr.health -= kBloodPrice << 4;
	return true;
}

// A foe not yet in the fight, which no shot should hunt: a buried axe zombie still under the earth and a laying one still
// on the floor (neither can be shot until it rises, and a blast beside one wakes it), and a gargoyle statue (it takes no
// damage, and wakes only as the map's wiring says). By its AI state, not its hitscan bit: a phantasm's idle and moving
// frames lack that bit, and it's a foe all the same.
static bool Dormant(DBloodActor* a)
{
	switch (a->spr.type)
	{
	case kDudeZombieAxeBuried: return a->xspr.aiState == &zombieEIdle;
	case kDudeZombieAxeLaying: return a->xspr.aiState == &zombieSIdle;
	case kDudeGargoyleStatueFlesh:
	case kDudeGargoyleStatueStone: return true;
	default: return false;
	}
}

// The Hound's Nose: the nearest foe within the cone ahead, in sight.
// The nearest live foe in sight from a point, within a cone around a heading and a range (exclude: one not to pick),
// not a dormant one.
// Given the foes already hunted (a foe once for each hunter), the least hunted one wins, nearest first: hunters share
// a pack out in turn.
static DBloodActor* HoundFoe(const DVector3& from, sectortype* sect, DAngle heading, double cone, double range, DBloodActor* exclude = nullptr,
	const TArray<DBloodActor*>* hunted = nullptr)
{
	DBloodActor* best = nullptr;
	double bestDist = range;
	int bestUses = INT_MAX;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next())
	{
		if (a == exclude || a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || !AllowedKillType(a) || !a->insector() || Dormant(a)) continue;
		DVector2 d = a->spr.pos.XY() - from.XY();
		double dist = d.Length();
		int uses = 0;
		if (hunted) for (auto h : *hunted) if (h == a) uses++;
		if (dist >= range || uses > bestUses || (uses == bestUses && dist >= bestDist) || absangle(d.Angle(), heading).Degrees() > cone) continue;
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		if (!cansee(from, sect, DVector3(a->spr.pos.XY(), (top + bottom) / 2), a->sector())) continue;
		best = a;
		bestDist = dist;
		bestUses = uses;
	}
	return best;
}

static DBloodActor* HoundTarget(DBloodPlayer* p, const DVector3& aim)
{
	auto pl = p->GetActor();
	return HoundFoe(DVector3(pl->spr.pos.XY(), p->zWeapon), pl->sector(), aim.XY().Angle(), kHoundCone, kHoundRange);
}

static DVector3 MiddleOf(DBloodActor* a)
{
	double top, bottom;
	GetActorExtents(a, &top, &bottom);
	return DVector3(a->spr.pos.XY(), (top + bottom) / 2);
}

static int Offspring(DBloodActor* a) { return (a->modFlags & kModOffspring) >> 4; }
static void SetOffspring(DBloodActor* a, int code) { a->modFlags = uint8_t((a->modFlags & ~kModOffspring) | ((code & 3) << 4)); }

// A forged missile's speed: a cleaver flies kCleaverSpeed times the Butcher's (it was too slow to land on a moving
// foe), the rest as stock. EmitForge, ForgeSpawnMissile, the Fetch and the stock homing (MissileSpeedScale) all use it.
static double ForgeSpeed(int type)
{
	double speed = missileInfo[type - kMissileBase].fVelocity();
	return type == kMissileButcherKnife ? speed * kCleaverSpeed : speed;
}

// A missile set to hunt a foe (already flying at it): the stock homing sets the full speed sideways and keeps vel.Z,
// so it starts on the slope to the foe's middle.
static void AimAt(DBloodActor* m, DBloodActor* foe)
{
	m->SetTarget(foe);
	DVector3 d = MiddleOf(foe) - m->spr.pos;
	double flat = d.XY().Length();
	if (flat > 1) m->vel.Z = ForgeSpeed(m->spr.type) * d.Z / flat;
}

// A missile a Litany spawns away from the gun (a second head, a cleaver flying back): the player's, forged, with its
// offspring code, set up as actFireMissile does but without its trace from the shooter's eye.
static DBloodActor* ForgeSpawnMissile(const DVector3& pos, sectortype* sect, const DVector3& dir, int type, int offspring)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor() || !sect || type < kMissileBase || type >= kMissileMax || dir.LengthSquared() <= 0) return nullptr;
	auto pl = p->GetActor();
	auto& mi = missileInfo[type - kMissileBase];
	auto m = actSpawnSprite(sect, pos, kStatProjectile, true);
	if (!m) return nullptr;
	m->spr.cstat2 |= CSTAT2_SPRITE_MAPPED;
	m->spr.type = type;
	m->spr.shade = mi.shade;
	m->spr.pal = 0;
	m->clipdist = mi.fClipDist();
	m->spr.flags = 1;
	m->spr.scale = DVector2(mi.xrepeat * REPEAT_SCALE, mi.yrepeat * REPEAT_SCALE);
	m->spr.setspritetexture(mi.textureID());
	m->spr.Angles.Yaw = dir.XY().Angle() + mapangle(mi.angleOfs);
	m->vel = dir.Unit() * ForgeSpeed(type);
	m->SetOwner(pl);
	m->spr.cstat &= ~CSTAT_SPRITE_BLOCK; // unlike actFireMissile's: the Forge's missiles don't strike each other
	m->SetTarget(nullptr);
	m->modFlags |= kModForged;
	SetOffspring(m, offspring);
	evPostActor(m, 600, kCallbackRemove);
	actBuildMissile(m, pl);
	return m;
}

// The forged starburst's ring (the flare's alt): count children leave a point in the ring FlareBurst throws around
// the carrier's flight (vel), a step round it each and every other one at half the spread: the Mouth's missiles, or
// with no Mouth forged sub-flares, so the Wound comes with each. They are offspring (1), so a Litany that only takes
// the gun's own shots (Cerberus, the Hymn, the Fetch) leaves them be. Under the Hound's Nose each child hunts a foe
// of its own from there, the least hunted one in its cone, nearest first.
static void ReleaseStarburst(const DVector3& pos, sectortype* sect, const DVector3& vel, int count)
{
	if (!sect || count <= 0 || vel.XY().LengthSquared() <= 0) return;
	int mouth = run.forge[kForgeMouth];
	int type = mouth ? forgeMouthMissile[mouth] : kMissileFlareAlt;
	bool hound = run.forge[kForgePath] == kPathHound;
	auto heading = vel.XY().Angle().ToVector();
	double radius = FixedToFloat(0x55555); // FlareBurst's
	TArray<DBloodActor*> hunted;
	for (int i = 0; i < count; i++)
	{
		auto ring = DAngle::fromDeg(360. * i / count).ToVector().Rotated90CW() * radius;
		if (i & 1) ring *= 0.5;
		DVector3 v = vel + DVector3(DVector2(0, ring.X).Rotated(heading.X, heading.Y), ring.Y);
		DBloodActor* foe = hound ? HoundFoe(pos, sect, v.XY().Angle(), kStarburstCone, kHoundRange, nullptr, &hunted) : nullptr;
		DVector3 dir = foe ? MiddleOf(foe) - pos : v;
		auto m = ForgeSpawnMissile(pos, sect, dir.Unit(), type, 1);
		if (!m) continue;
		if (type == kMissileFlareAlt)
		{
			// A stock sub-flare, as FlareBurst spawns it: it doesn't burst again, trails no sparks and doesn't hiss.
			evKillActor(m, kCallbackFXFlareBurst);
			evKillActor(m, kCallbackFXFlareSpark);
			sfxKill3DSound(m, -1, -1);
			m->spr.setspritetexture(aTexIds[kTexFLAREBURST]);
			m->spr.shade = -128;
			m->spr.scale = DVector2(0.5, 0.5);
			m->clipdist = 0.5;
			if (!foe) m->vel = v;
		}
		if (foe)
		{
			AimAt(m, foe);
			hunted.Push(foe);
		}
	}
}

// A blast of the player's: small (a TNT stick, 75), standard (a bundle, 150) or large (225), forged. One that spares
// you is marked kModSpareYou; the explosion the thing becomes is the same actor, so it keeps the mark (ForgeExplosion).
static void ForgeBlast(const DVector3& pos, sectortype* sect, int kind, bool spareYou = false)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor() || !sect) return;
	auto t = actSpawnThing(sect, pos, kind == 0 ? kThingArmedTNTStick : kThingArmedTNTBundle);
	if (!t) return;
	t->SetOwner(p->GetActor());
	t->modFlags |= kModForged | (spareYou ? kModSpareYou : 0);
	actExplodeSprite(t);
	if (kind >= 2 && t->spr.type == kExplosionStandard) // retyped to the large explosion: actCheckExplosion reads its type
	{
		auto& e = explodeInfo[kExplosionLarge];
		t->spr.type = kExplosionLarge;
		t->spr.scale = DVector2(e.repeat * REPEAT_SCALE, e.repeat * REPEAT_SCALE);
		t->xspr.data1 = e.ticks;
		t->xspr.data2 = e.quakeEffect;
		t->xspr.data3 = e.flashEffect;
		sfxPlay3DSound(t, 305, -1, 0);
	}
}

// Healing from a Litany, under the Forge's cap (and Famine). No Litany heals now (the Fetch gives a round back).
[[maybe_unused]] static void ForgeHeal(int n)
{
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor() || p->GetActor()->xspr.health <= 0) return;
	forgeImpacts++;
	actHealDude(p->GetActor(), n, getDudeInfo(kDudePlayer1)->startHealth);
	forgeImpacts--;
}

// A burn that tops up to n, never adds (adding climbs to the 2400 cap).
static void BurnTo(DBloodActor* source, DBloodActor* a, int n)
{
	if (!a || !a->hasX() || a->xspr.burnTime >= unsigned(n)) return;
	if (!a->xspr.burnTime) evPostActor(a, 0, kCallbackFXFlameLick);
	actBurnSprite(source, a, n - a->xspr.burnTime);
}

// A singing Litany at work: announced the first time on each map.
static void LitanyFired(int k)
{
	if (k < 0 || k >= kNumLitanies || (litanyFired & (1u << k))) return;
	litanyFired |= 1u << k;
	litanyShown = k;
	litanyShownAt = PlayClock;
	Printf(PRINT_NOTIFY, "Litany: %s\n", litanies[k].name);
}

// Witchfire: when the Pyre lit each foe (a foe burning but not listed counts as lit long ago).
struct LitAt { DBloodActor* actor; int at; };
static TArray<LitAt> forgeLit;
static void NoteLit(DBloodActor* a)
{
	for (auto& l : forgeLit) if (l.actor == a) { l.at = PlayClock; return; }
	if (forgeLit.Size() > 64) forgeLit.Delete(0);
	forgeLit.Push({ a, PlayClock });
}
static int LitFor(DBloodActor* a)
{
	for (auto& l : forgeLit) if (l.actor == a) return PlayClock - l.at;
	return INT_MAX / 2;
}

// The Pyre's flare: lodged in the foe on the side the shot came from, as actImpactMissile lodges a stock flare in
// flesh (actCheckFlares carries it with its bearer and gibs it when the bearer dies). One a foe, kPyreMarks at most.
// It throws fewer sparks than a stock flare, lodged or flying (the lite spark, every 12 PlayClock, not 4: up to twelve
// marks at once), and ForgeTick puts it out with the fire.
static void PyreMark(DBloodActor* source, DBloodActor* t)
{
	int marks = 0;
	BloodStatIterator it(kStatFlare);
	while (auto f = it.Next())
	{
		if (!(f->modFlags & kModPyreMark) || (f->spr.flags & 32)) continue; // 32: already on its way out
		if (f->GetTarget() == t) return;
		marks++;
	}
	if (marks >= kPyreMarks || !t->insector()) return;
	double top, bottom;
	GetActorExtents(t, &top, &bottom);
	DAngle side = (source->spr.pos.XY() - t->spr.pos.XY()).Angle();
	DVector3 pos(t->spr.pos.XY() + side.ToVector() * t->clipdist * 0.5, bottom - (bottom - top) * 0.6); // chest high
	auto f = actSpawnSprite(t->sector(), pos, kStatFlare, true);
	if (!f) return;
	auto& mi = missileInfo[kMissileFlareRegular - kMissileBase];
	// A stock lodged flare's type. The view gives a kStatFlare no halo or glow (only a flying one), as with a stock lodged
	// flare: what shows the burn is the foe's flames (BurnTo) and the bright flesh-hit tile.
	f->spr.type = kMissileFlareRegular;
	f->spr.cstat2 |= CSTAT2_SPRITE_MAPPED;
	f->spr.cstat &= ~CSTAT_SPRITE_BLOCK_ALL;
	f->spr.shade = mi.shade;
	f->spr.pal = 0;
	f->spr.scale = DVector2(mi.xrepeat * REPEAT_SCALE, mi.yrepeat * REPEAT_SCALE);
	f->spr.setspritetexture(aTexIds[kTexFLESHHIT]);
	f->clipdist = mi.fClipDist();
	f->SetOwner(source);
	f->SetTarget(t);
	f->xspr.TargetPos.Z = pos.Z - t->spr.pos.Z;
	f->xspr.goalAng = side - t->spr.Angles.Yaw;
	f->xspr.state = 1;
	f->modFlags = uint8_t(kModPyreMark); // not kModForged: a load would take a forged flare in flesh for a Corpse-Candle
	evPostActor(f, 0, kCallbackFXFlareSparkLite);
}

// The Pyre's touch: a foe that can burn is lit, or its burn topped back up to the Pyre's 4 s (never past: adding would
// climb to the 2400 cap), with flames (BurnTo) and a flare lodged in it (PyreMark). Foes that can't burn, and the
// burning kinds a burn has already turned them into, are left alone, so nothing shows a fire that isn't there.
static void PyreLight(DBloodActor* source, DBloodActor* t)
{
	if (!source || !t || !t->IsDudeActor() || t->IsPlayerActor() || !t->hasX() || t->xspr.health <= 0 || IsBurningDude(t)) return;
	if (!getDudeInfo(t->spr.type)->damageVal[kDamageBurn]) return;
	bool was = t->xspr.burnTime > 0;
	BurnTo(source, t, kPyreBurn);
	if (!was)
	{
		NoteLit(t); // Witchfire: when it was lit
		sfxPlay3DSound(t, 353, -1, 0); // a sizzle (the flare's own hiss, 422, loops)
	}
	PyreMark(source, t);
}

// Hymn of Ash: a pull's refunds, and which missile belongs to which pull.
struct HymnPull { int id, refundsLeft, ammo, at; };
struct HymnShot { DBloodActor* missile; int pull; };
static TArray<HymnPull> hymnPulls;  // a pull is known by its id (records come and go)
static TArray<HymnShot> hymnShots;
static int hymnNow = -1;            // the pull being fired, by id (for point-blank impacts)
static int hymnNextId = 1;
static HymnPull* HymnRecord(int id)
{
	for (auto& r : hymnPulls) if (r.id == id) return &r;
	return nullptr;
}
static bool forgeBloodBoughtNow;    // the pull being fired was paid in blood (Last Rites, point blank)

// Corpse-Candle's lit flares, Requiem's waiting blasts, the Fetch's cleavers to send back.
struct Candle { DBloodActor* flare; int blowAt; DVector3 pos; sectortype* sect; };
struct Toll { DVector3 pos; sectortype* sect; int at; };
struct Fetching { DVector3 pos; sectortype* sect; bool alt, blood; }; // blood: the cleaver was bought with blood
static TArray<Candle> candles;
static TArray<Toll> tolls;
static TArray<Fetching> fetching;
static int fetchCredit;              // the Fetch: cleavers caught, in thirds of a drink, toward the next round given back
// The Fetch: cleavers an alt pull fired, and the twins they send back (compared, never followed). The alt's bonus makes
// each cost a third less, so a catch gives back a third less: else the bonus would come back as ammo. A cleaver leaves
// the list when it strikes or is caught, and ForgeTick drops any no longer flying (a timer took it), so the list holds
// only cleavers in the air: an address the GC hands to a later missile is never taken for an alt one's.
static TArray<DBloodActor*> fetchAlt;
static bool FetchAlt(DBloodActor* m) { return fetchAlt.Find(m) < fetchAlt.Size(); }
static void NoteFetchAlt(DBloodActor* m)
{
	if (fetchAlt.Size() >= 256) fetchAlt.Delete(0);
	fetchAlt.Push(m);
}
static void DropFetchAlt(DBloodActor* m)
{
	for (int i = int(fetchAlt.Size()) - 1; i >= 0; i--) if (fetchAlt[i] == m) fetchAlt.Delete(i);
}
static void PruneFetchAlt()
{
	if (!fetchAlt.Size()) return;
	TArray<DBloodActor*> flying;
	BloodStatIterator it(kStatProjectile);
	while (auto m = it.Next()) if ((m->modFlags & kModForged) && m->spr.type == kMissileButcherKnife) flying.Push(m);
	for (int i = int(fetchAlt.Size()) - 1; i >= 0; i--) if (flying.Find(fetchAlt[i]) >= flying.Size()) fetchAlt.Delete(i);
}

// The forged starburst: each carrier's ring, kept from the pull that fired it until it bursts (a record lost to a save
// is worked out again from the recipe); rings thrown by a carrier that struck early, to go out on the Forge's tic; and
// the ring of the starburst being fired (a carrier fired point blank strikes before its record is kept).
struct BurstRecord { DBloodActor* carrier; int count; };
struct Starburst { DVector3 pos; sectortype* sect; DVector3 vel; int count; };
static TArray<BurstRecord> forgeBursts;
static TArray<Starburst> starbursts;
static int forgeBurstNow;
static int BurstCount(DBloodActor* carrier)
{
	for (int i = int(forgeBursts.Size()) - 1; i >= 0; i--)
		if (forgeBursts[i].carrier == carrier)
		{
			int n = forgeBursts[i].count;
			forgeBursts.Delete(i);
			return n;
		}
	if (forgeEmitting && forgeBurstNow > 0) return forgeBurstNow;
	return StarburstCount(run.forge, 1);
}
// The tommy's sweep under the Hound's Nose: the foes this sweep has hunted, so it rakes across a pack.
static TArray<DBloodActor*> sweepHunted;
static int sweepAt = INT_MIN / 2;
static int witchfireNext = INT_MIN / 2;
static int lastTollAt = INT_MIN / 2;

// Gravedigger's Blast: when its last kGraveBursts bursts went off (PlayClock, oldest next), so that no more than that
// go off in any second: the tommy's cleavers would otherwise set off fifteen. A hit over the budget doesn't burst.
static int graveBursts[kGraveBursts] = { INT_MIN / 2, INT_MIN / 2, INT_MIN / 2, INT_MIN / 2 };
static int graveNext;
static bool GraveBurst()
{
	int& oldest = graveBursts[graveNext];
	int since = PlayClock - oldest;
	if (since >= 0 && since < kClock) return false; // (a PlayClock that went back, a new map's, doesn't hold it up)
	oldest = PlayClock;
	graveNext = (graveNext + 1) % kGraveBursts;
	return true;
}

static void ResetLitanies()
{
	forgeScent = nullptr;
	forgeScentAt = INT_MIN / 2;
	forgeExplosionNow = nullptr;
	forgeSparingNow = nullptr;
	forgeBlastNow = nullptr;
	forgeBiting = false;
	forgeBiteDeeper = false;
	forgeBite = nullptr;
	for (auto& at : graveBursts) at = INT_MIN / 2;
	fetchCredit = 0;
	fetchAlt.Clear();
	forgeBursts.Clear();
	starbursts.Clear();
	forgeBurstNow = 0;
	sweepHunted.Clear();
	sweepAt = INT_MIN / 2;
	forgeLit.Clear();
	hymnPulls.Clear();
	hymnShots.Clear();
	hymnNow = -1;
	forgeBloodBoughtNow = false;
	candles.Clear();
	tolls.Clear();
	fetching.Clear();
	witchfireNext = INT_MIN / 2;
	litanyFired = 0;
	litanyShown = -1;
}

// A save loaded: the lists start clean, and lodged candles get a fresh fuse.
static void LitaniesLoaded()
{
	ResetLitanies();
	if (!run.active || run.over || !Sings(kLitCandle)) return;
	BloodStatIterator it(kStatFlare);
	while (auto f = it.Next())
		if ((f->modFlags & kModForged) && f->insector()) candles.Push({ f, PlayClock + kClock * 3 / 2, f->spr.pos, f->sector() });
}

static bool IsLive(DBloodActor* a, int stat)
{
	if (!a) return false;
	BloodStatIterator it(stat);
	while (auto b = it.Next()) if (b == a) return true;
	return false;
}

// A pull's payload. alt: the gun's alt mode, forged (FireMode): both barrels fire the Mouth's payload for both, half
// again over; the tommy's sweep fires it along each of its 14 aims, twice on every other one; the flare's starburst
// fires its carrier as stock, forged, and the carrier throws a ring of the payload when it bursts (ForgeBurst) or
// strikes early (ForgeImpact).
static void EmitForge(DBloodPlayer* p, const TArray<ForgeAim>& shots, int id, int spent, bool alt)
{
	auto pl = p->GetActor();
	int vessel = run.forge[kForgeVessel];
	auto& v = forgeVessels[vessel];
	auto r = Rates(run.forge);
	int mouth = run.forge[kForgeMouth];
	int fan = run.forge[kForgePath] == kPathChoir ? 3 : 1;
	bool starburst = alt && vessel == kVesselFlare;
	bool sweep = alt && vessel == kVesselTommy;
	int capM = alt ? kForgeAltMissiles : kForgeMissiles, capV = alt ? kForgeAltVectors : kForgeVectors;
	// The rounds this callback fires: the flare one (its id is the akimbo hand), the sawed-off and tommy their trigger.
	// An alt pull's: the starburst's 8 (the stock alt's price), both barrels (four akimbo, by id), and a sweep step the
	// shots it fired (its id is the step's place in the sweep, not a count).
	int units = starburst ? kStarburstFlares : sweep ? int(shots.Size()) : vessel == kVesselFlare ? 1 : std::max(id, 1);
	int perRound = mouth ? std::max(1, std::min(r.perRound, capM / (units * fan))) : 0; // whole fans within the cap

	// The ammo: the stock function took a round a round fired; the rest of the drink is taken here. Short, the payload
	// shrinks to what was paid for; dry with the Blood Price, the pull is paid in health (or not fired at all). An alt
	// pull is never paid in blood (dry, every alt fires the primary instead): short, it only shrinks. An alt pull drinks
	// only for what its cap lets it fire (AltOwed): the starburst at most the stock alt's 8 flares, with the Choir too.
	double share = 1;
	double altBonus = sweep ? ((id & 1) ? 2. : 1.) : kForgeAltBonus;
	int owed = alt ? AltOwed(run.forge, units, altBonus, starburst) : units * r.drink;
	int roundsPaid = owed;
	bool bloodPaid = false;
	if (!gInfiniteAmmo)
	{
		int paid = std::max(spent, 0);
		// A clipped alt pull drinks less than the stock alt took (the starburst's 8 flares, the akimbo pair's 4
		// shells): the rest goes back. AltOwed is at least a round, so the pull is never free.
		if (alt && paid > owed)
		{
			p->ammoCount[v.ammo] = ClipHigh(p->ammoCount[v.ammo] + (paid - owed), gAmmoInfo[v.ammo].max);
			paid = owed;
		}
		int extra = std::min(std::max(owed - paid, 0), p->ammoCount[v.ammo]);
		if (extra > 0) UseAmmo(p, v.ammo, extra);
		paid += extra;
		roundsPaid = paid;
		if (paid < owed)
		{
			if (!alt && BloodPriced() && PayInBlood(p)) share = 1, bloodPaid = true;
			else if (pl->xspr.health <= 0) return; // Last Rites took your life: the pull fires nothing
			else if (paid > 0) share = double(paid) / owed;
			else return; // nothing paid: nothing fires
		}
	}

	// The Hound's Nose: the foes this pull hunts, its missiles shared out among them in turn. Both barrels hunt up to
	// three foes, not one; a sweep step hunts the least hunted foe of its sweep, so the sweep rakes across a pack; the
	// starburst's carrier flies as stock and its children hunt (ReleaseStarburst).
	TArray<DBloodActor*> prey;
	if (run.forge[kForgePath] == kPathHound && r.missile && !starburst)
	{
		DVector3 eye(pl->spr.pos.XY(), p->zWeapon);
		DAngle heading = shots[0].dv.XY().Angle();
		if (sweep)
		{
			if (PlayClock - sweepAt > kClock / 4 || PlayClock < sweepAt) sweepHunted.Clear(); // a new sweep
			sweepAt = PlayClock;
			if (auto t = HoundFoe(eye, pl->sector(), heading, kHoundCone, kHoundRange, nullptr, &sweepHunted))
			{
				if (sweepHunted.Size() >= 64) sweepHunted.Delete(0);
				sweepHunted.Push(t);
				prey.Push(t);
			}
		}
		else if (alt)
		{
			for (int i = 0; i < 3; i++)
			{
				auto t = HoundFoe(eye, pl->sector(), heading, kHoundCone, kHoundRange, nullptr, &prey);
				if (!t || prey.Find(t) < prey.Size()) break; // no other foe in the cone
				prey.Push(t);
			}
		}
		else if (auto t = HoundTarget(p, shots[0].dv)) prey.Push(t);
	}
	// Blood Feud: a pull paid in blood hunts whoever last drew your blood, anywhere in sight.
	if (bloodPaid && run.forge[kForgePath] == kPathHound && Sings(kLitBloodFeud) && forgeScent && IsLive(forgeScent, kStatDude)
		&& forgeScent->hasX() && forgeScent->xspr.health > 0 && PlayClock - forgeScentAt >= 0 && PlayClock - forgeScentAt < 5 * kClock)
	{
		DVector3 eye(pl->spr.pos.XY(), p->zWeapon);
		if ((forgeScent->spr.pos.XY() - eye.XY()).Length() <= 2048 && cansee(eye, pl->sector(), MiddleOf(forgeScent), forgeScent->sector()))
		{
			prey.Clear();
			prey.Push(forgeScent);
			LitanyFired(kLitBloodFeud);
		}
	}
	TArray<ForgeAim> out;
	int children = 0; // the starburst: each carrier's ring
	if (starburst)
	{
		children = StarburstCount(run.forge, share);
		out.Push(shots[0]); // the carrier keeps its type (kMissileFlareAlt): the Choir fans it below, three that share the cap
	}
	else if (mouth)
	{
		// Something paid: at least one. Both barrels' payload is half again over; a sweep step's comes twice on every
		// other step (half again over the sweep).
		int count = !alt ? std::max(1, int(perRound * units * share)) : AltPayload(run.forge, units, share, altBonus);
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
	DVector3 from(pl->spr.pos.XY(), p->zWeapon);
	if (prey.Size()) // aim at the foe, so the hunt starts on it (the stock homing only steers sideways and down)
		for (unsigned i = 0; i < out.Size(); i++) if (out[i].missile) out[i].dv = (MiddleOf(prey[i % prey.Size()]) - from).Unit();
	// Copies of one aim (the flare's five cleavers) fly a little apart, +2, -2, +4, -4 degrees and on: stacked, they'd
	// all strike the one spot. Copy k of an aim is the k-th time the payload loop came round to it.
	if (mouth)
		for (unsigned i = shots.Size(); i < out.Size(); i++)
		{
			int k = int(i / shots.Size());
			double step = (k & 1 ? 2. : -2.) * ((k + 1) / 2);
			out[i].dv = DVector3(out[i].dv.XY().Rotated(DAngle::fromDeg(step)), out[i].dv.Z);
		}
	bool shears = false;
	if (fan > 1)
	{
		// The Shears: with a foe close to the aim, each fan's three cleavers converge on it from the barrel's sides (not
		// the starburst's carriers: their cleavers are in the ring).
		DBloodActor* quarry = Sings(kLitShears) && mouth == kMouthCleaver && !starburst ? HoundFoe(from, pl->sector(), out[0].dv.XY().Angle(), 5, 1024) : nullptr;
		TArray<ForgeAim> fanned;
		DVector2 across = (pl->spr.Angles.Yaw + DAngle90).ToVector();
		for (unsigned j = 0; j < out.Size(); j++)
		{
			auto& s = out[j];
			// The Shears' copies of one blade (the flare's four, a barrel's three pellets) would share its spawn point and
			// aim, and fly as one: each leaves 5 units further aside (0, +5, -5, +10, ...) and keeps half that apart at the
			// quarry, close enough that each still strikes it.
			double spread = ((j & 1) ? 5. : -5.) * ((j + 1) / 2);
			for (int i = 0; i < 3; i++)
			{
				auto t = s;
				if (quarry)
				{
					double apart = spread;
					double off = (i - 1) * 24. + apart;
					if (off != 0) // actFireMissile's own spawn trace (actor.cpp): where it would strike a wall, the blade fires from the middle
					{
						double cd = missileInfo[kMissileButcherKnife - kMissileBase].fClipDist() + pl->clipdist;
						DVector2 side = across * (s.off + off) + pl->spr.Angles.Yaw.ToVector() * cd;
						int h = HitScan(pl, pl->spr.pos.Z + s.zoff, DVector3(side, 0), CLIPMASK0, cd * 4);
						if (h == 0 || h == 3) off = 0, apart = 0;
					}
					DVector3 start = from + DVector3(across * off, 0);
					t.off = s.off + off;
					t.dv = (MiddleOf(quarry) + DVector3(across * (apart * 0.5), 0) - start).Unit();
				}
				else t.dv = DVector3(s.dv.XY().Rotated(DAngle::fromDeg((i - 1) * 10.)), s.dv.Z);
				fanned.Push(t);
			}
		}
		out = std::move(fanned);
		if (quarry) shears = true, LitanyFired(kLitShears);
	}

	// Hymn of Ash: the pull's refunds (one a fireball on flesh), up to what it drank less a round for each round it
	// fired, never more than it paid (both barrels: up to 6 of 8 shells under the Choir). The starburst's ring is
	// offspring, which the Hymn leaves be.
	hymnNow = -1;
	if (Sings(kLitHymn) && mouth == kMouthHellmouth && !starburst)
	{
		if (hymnPulls.Size() >= 32) hymnPulls.Delete(0);
		hymnNow = hymnNextId++;
		hymnPulls.Push({ hymnNow, std::max(std::min(owed, roundsPaid) - units, 0), v.ammo, PlayClock });
	}
	forgeBloodBoughtNow = bloodPaid;
	forgeBurstNow = children;

	int missiles = 0, vectors = 0;
	forgeHits.Clear();
	forgeEmitting = true;
	for (unsigned i = 0; i < out.Size(); i++)
	{
		auto& s = out[i];
		if (s.missile)
		{
			if (missiles++ >= capM) continue;
			auto m = actFireMissile(pl, s.off, s.zoff, s.dv, s.type);
			if (!m) continue;
			m->modFlags |= kModForged | (bloodPaid ? kModBloodBought : 0);
			// The Forge's missiles don't strike each other: they leave from one spot along near aims, and a copy that
			// struck the one ahead fell as debris (or, under the Larder, stuck in the air beside you). The Shears'
			// converging blades need it too.
			m->spr.cstat &= ~CSTAT_SPRITE_BLOCK;
			if (m->spr.type == kMissileButcherKnife) m->vel = s.dv.Unit() * ForgeSpeed(m->spr.type); // a forged cleaver flies faster
			if (hymnNow >= 0) hymnShots.Push({ m, hymnNow });
			if (alt && m->spr.type == kMissileButcherKnife && Sings(kLitFetch)) NoteFetchAlt(m);
			if (starburst)
			{
				if (forgeBursts.Size() >= 32) forgeBursts.Delete(0);
				forgeBursts.Push({ m, children });
			}
			if (prey.Size()) AimAt(m, prey[i % prey.Size()]);
		}
		else
		{
			if (vectors++ >= capV) continue;
			actFireVector(pl, s.off, s.zoff, s.dv, VECTOR_TYPE(s.type));
		}
	}
	// The Censer: three spray-can flames along the spread, its leftmost, middle and rightmost pellets; both barrels
	// swing three a barrel, spaced evenly across it.
	if (Sings(kLitCenser) && !mouth && vessel == kVesselShotgun)
	{
		DAngle aim = shots[0].dv.XY().Angle();
		TArray<int> order;
		for (unsigned i = 0; i < out.Size(); i++) if (!out[i].missile) order.Push(int(i));
		int flames = std::min(alt ? 3 * units : 3, int(order.Size()));
		if (flames >= 3)
		{
			std::sort(order.begin(), order.end(), [&](int a, int b) { return deltaangle(aim, out[a].dv.XY().Angle()).Degrees() < deltaangle(aim, out[b].dv.XY().Angle()).Degrees(); });
			int n = int(order.Size());
			for (int j = 0; j < flames; j++)
			{
				int i = order[(j * (n - 1) + (flames - 1) / 2) / (flames - 1)]; // three: the first, the middle (n/2) and the last
				auto m = actFireMissile(pl, out[i].off, out[i].zoff, out[i].dv, kMissileFlameSpray);
				if (m) { m->modFlags |= kModForged; SetOffspring(m, 1); m->spr.cstat &= ~CSTAT_SPRITE_BLOCK; }
			}
			LitanyFired(kLitCenser);
		}
	}
	forgeEmitting = false;
	hymnNow = -1;
	forgeBloodBoughtNow = false;
	forgeBurstNow = 0;
	if (run.forge[kForgeWound] == kWoundPyre)
		for (auto t : forgeHits) PyreLight(pl, t);
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
	int mode = FireMode(p, v, nCallback);
	if (mode == kFireStock)
	{
		// Alt fire left stock needs ammo: dry (only the Blood Price lets it get this far), it does nothing at all
		// (the tommy's spread would put the gun away).
		if (before == 0 && BloodPriced() && !gInfiniteAmmo) return;
		qavClientCallback[nCallback](id, data);
		return;
	}
	bool alt = mode == kFireAlt;
	if (!alt || before > 0 || gInfiniteAmmo)
	{
		TArray<ForgeAim> shots;
		forgeShots = &shots;
		qavClientCallback[nCallback](id, data);
		forgeShots = nullptr;
		if (shots.Size()) EmitForge(p, shots, id, before - p->ammoCount[ammo], alt);
	}
	// An alt pull is never paid in blood: dry, it fires nothing. The tommy's sweep puts the gun away once dry, as the
	// stock sweep does (its own check came before the Forge drank the rest of the step's rounds).
	if (alt && v == kVesselTommy && !gInfiniteAmmo && p->ammoCount[ammo] == 0 && p->weaponState != -1)
	{
		WeaponLower(p);
		p->weaponState = -1;
	}
}

bool SwallowMissile(DBloodActor* shooter, double xyoff, double zoff, const DVector3& dv, int type)
{
	if (!forgeShots || forgeEmitting || !shooter || !shooter->IsPlayerActor()) return false;
	forgeShots->Push({ true, type, xyoff, zoff, dv });
	return true;
}

bool SwallowVector(DBloodActor* shooter, double offset, double zoffset, const DVector3& dv, int type)
{
	// A foe's vector, for ScaleDamage: a hitscan (the bullet and shell kinds, unlimited in range) or not (a bite, a
	// slash). Only noted: the shot goes out as stock.
	if (shooter && shooter->IsDudeActor() && !shooter->IsPlayerActor())
	{
		foeHitscan = type >= kVectorShell && type <= kVectorTommyRegular ? shooter : nullptr;
		foeHitscanAt = PlayClock;
	}
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

// FlareBurst: a forged starburst's carrier (the flare's alt, offspring 0) throws its own ring, once (it then counts as
// offspring); false leaves the stock burst.
bool ForgeBurst(DBloodActor* carrier)
{
	if (!carrier || !run.active || run.over || !(carrier->modFlags & kModForged) || carrier->spr.type != kMissileFlareAlt || Offspring(carrier) != 0)
		return false;
	SetOffspring(carrier, 1);
	int count = BurstCount(carrier);
	if (carrier->insector()) ReleaseStarburst(carrier->spr.pos, carrier->sector(), carrier->vel, count);
	return true;
}

ForgeImpact::ForgeImpact(DBloodActor* m, DBloodActor* hit, int code, walltype* hitWall)
{
	if (!m || !run.active || run.over) return;
	auto owner = m->GetOwner();
	// A missile fired point blank hits before EmitForge has marked it: while it emits, the player's missiles are forged.
	bool emitted = forgeEmitting && owner && owner->IsPlayerActor();
	if (!(m->modFlags & kModForged) && !emitted) return;
	active = true;
	missile = m;
	type = m->spr.type;
	hitCode = code;
	wall = (code == 0 || code == 4) ? hitWall : nullptr;
	flight = m->vel.XY().LengthSquared() > 0 ? m->vel.XY().Angle().Degrees() : m->spr.Angles.Yaw.Degrees();
	forgeImpacts++;
	m->modFlags |= kModForged; // a point-blank missile is marked here (its knife, its candle)
	liveHit = hit && hit->IsDudeActor() && !hit->IsPlayerActor() && hit->hasX() && hit->xspr.health > 0;
	sky = !hit && m->insector() && (m->sector()->ceilingstat & CSTAT_SECTOR_SKY) && m->spr.pos.Z <= m->sector()->ceilingz + 4;
	int offspring = Offspring(m);
	bloodBought = (m->modFlags & kModBloodBought) || (emitted && forgeBloodBoughtNow);

	if (liveHit) foe = hit;

	// The forged starburst's carrier, struck before it burst (FlareBurst): it throws its ring from there, on the Forge's
	// next tic (not from inside the missiles' move). Into a body it struck the ring goes on, from a little back along
	// its flight, so it strikes it; off a wall, floor or ceiling it bounces away; into the sky it's lost, as the stock
	// carrier is. Once: the carrier then counts as offspring, and its own stock blast is all the Grave gets of it.
	bool carrier = type == kMissileFlareAlt && offspring == 0;
	if (carrier)
	{
		SetOffspring(m, 1);
		int count = BurstCount(m);
		DVector3 v = m->vel;
		if (!sky && m->insector() && v.XY().LengthSquared() > 0)
		{
			DVector3 pos = m->spr.pos;
			auto sect = m->sector();
			if (code == 0 || code == 4)
			{
				if (hitWall) // mirrored in the wall (the normal's side doesn't matter)
				{
					DVector2 n = hitWall->normalAngle().ToVector();
					v.XY() -= n * (2 * v.XY().dot(n));
				}
			}
			else if (code == 1 || code == 2) v.Z = -v.Z;
			else if (code == 3)
			{
				DVector3 back = pos - v.Unit() * 16; // where it flew a moment ago
				updatesector(back.XY(), &sect);
				if (sect) pos = back;
				else sect = m->sector();
			}
			if (starbursts.Size() < 16) starbursts.Push({ pos, sect, v, count });
		}
	}

	// The cleaver's bite: the stock impact wounds the foe and then heals you 10; ScaleHealing makes that heal half the
	// health the wound took from a live foe, and nothing at all when the cleaver struck no live foe. ScaleDamage makes
	// the wound deeper (kCleaverBite).
	if (type == kMissileButcherKnife)
	{
		auto p = getPlayer(myconnectindex);
		biting = true;
		forgeBiting = true;
		forgeBite = liveHit ? hit : nullptr;
		forgeBiteHealth = liveHit ? int(hit->xspr.health) : 0;
		forgeBiteDeeper = liveHit;
		healthBefore = p && p->GetActor() && p->GetActor()->hasX() ? int(p->GetActor()->xspr.health) : 0;
	}
	// The bare forged flare: the stock flare only burns a foe that can burn (at most 15 a second, however long), so it
	// strikes as well, as Blood 1.0x's flare did (the stock code does that itself when that option is on, and to a foe
	// that can't burn). The destructor deals it, once the stock impact has lit the foe.
	if (type == kMissileFlareRegular && offspring == 0 && run.forge[kForgeVessel] == kVesselFlare && !run.forge[kForgeMouth] && liveHit
		&& code == 3 && getDudeInfo(hit->spr.type)->damageVal[kDamageBurn] != 0 && !(gGameOptions.weaponsV10x && !VanillaMode()))
		flareStrike = true;

	// Witchfire: a cleaver striking a foe that has burned a while scatters its fire on every body near, you too.
	if (Sings(kLitWitchfire) && type == kMissileButcherKnife && offspring == 0 && liveHit && hit->xspr.burnTime > 0 && LitFor(hit) >= kClock / 4
		&& PlayClock >= witchfireNext)
	{
		witchfireNext = PlayClock + kClock / 2;
		BloodStatIterator it(kStatDude);
		while (auto d = it.Next())
		{
			if (d == hit || !d->hasX() || d->xspr.health <= 0 || (d->spr.pos.XY() - hit->spr.pos.XY()).Length() > 128) continue;
			if (!cansee(MiddleOf(hit), hit->sector(), MiddleOf(d), d->sector())) continue;
			BurnTo(owner, d, kWitchfireBurn);
		}
		LitanyFired(kLitWitchfire);
	}
	// The Pyre: at emission its hits are lit once each by EmitForge; later impacts light here.
	if (!emitted && run.forge[kForgeWound] == kWoundPyre && liveHit) PyreLight(owner, hit);
	// Hymn of Ash: a fireball on flesh gives a round back; one on stone (or nothing) drinks one more.
	if (Sings(kLitHymn) && type == kMissileFireballNapalm && offspring == 0)
	{
		int pull = emitted ? hymnNow : -1;
		for (auto& h : hymnShots) if (h.missile == m) pull = h.pull;
		auto p = getPlayer(myconnectindex);
		auto record = HymnRecord(pull);
		if (record && p && p->GetActor())
		{
			auto& rec = *record;
			if (liveHit && rec.refundsLeft > 0)
			{
				rec.refundsLeft--;
				p->ammoCount[rec.ammo] = ClipHigh(p->ammoCount[rec.ammo] + 1, gAmmoInfo[rec.ammo].max);
				sfxPlay3DSound(p->GetActor(), 782, -1, 0);
				LitanyFired(kLitHymn);
			}
			else if (!hit) UseAmmo(p, rec.ammo, 1);
		}
	}
	// Last Rites: a blood-bought fireball near death bursts bigger (the one extra blast: it takes the Grave's place).
	bool bigger = false;
	if (Sings(kLitLastRites) && type == kMissileFireballNapalm && bloodBought && owner && owner->hasX())
	{
		int health = owner->xspr.health >> 4;
		if (health < 50 && m->insector())
		{
			ForgeBlast(m->spr.pos, m->sector(), health < 20 ? 2 : 1);
			LitanyFired(kLitLastRites);
			bigger = true;
		}
	}
	if (run.forge[kForgeWound] == kWoundGrave && m->insector() && !bigger && !carrier)
	{
		bool flesh = hit && (hit->IsDudeActor() || hit->spr.statnum == kStatThing) && GetExtInfo(hit->spr.spritetexture()).surftype == kSurfFlesh;
		bool candle = Sings(kLitCandle) && type == kMissileFlareRegular && flesh;
		if (Sings(kLitLarder) && type == kMissileButcherKnife && !liveHit && !sky && offspring == 0
			&& (wall || !(m->sector()->floorstat & CSTAT_SECTOR_SKY))) // off a wall it lies on the floor: not in a sky pit
			larder = true; // the Larder of Knives: a missed cleaver sticks and waits (the destructor sets it; ForgeTick keeps 8 at most)
		else if (!GraveBurst()) // the rest are the Grave's bursts, kGraveBursts a second: past that, a hit doesn't burst
		{
			if (candle) m->modFlags &= ~kModForged; // a flare lodged without a fuse: a load doesn't light it either
		}
		else
		{
			if (candle)
			{
				// Corpse-Candle: the flare lodged in flesh holds the blast; it bursts bigger in 1.5 s, or when its bearer dies.
				if (candles.Size() < 16) candles.Push({ m, PlayClock + kClock * 3 / 2, m->spr.pos, m->sector() });
				sfxPlay3DSound(m, 450, -1, 0);
				LitanyFired(kLitCandle);
			}
			else if (Sings(kLitRequiem))
			{
				if (tolls.Size() < 32) tolls.Push({ m->spr.pos, m->sector(), PlayClock }); // Requiem: it waits to ring with the others
				lastTollAt = PlayClock;
			}
			else ForgeBlast(m->spr.pos, m->sector(), 0); // the Grave's small blast, the player's
		}
	}
	// The Fetch: a hunting cleaver that strikes anything but a foe (not the sky) flies back (the destructor queues it),
	// if it struck far enough off that it doesn't come straight back into your hand.
	if (Sings(kLitFetch) && type == kMissileButcherKnife && !liveHit && !sky && offspring == 0 && run.forge[kForgePath] == kPathHound
		&& owner && (m->spr.pos.XY() - owner->spr.pos.XY()).Length() >= kFetchMinRange)
		fetch = true;
}

ForgeImpact::~ForgeImpact()
{
	if (!active) return;
	forgeImpacts--;
	// A cleaver's strike ends it (debris, or a Larder knife): it leaves the Fetch's alt list whatever it struck.
	bool wasAlt = missile && FetchAlt(missile);
	if (wasAlt) DropFetchAlt(missile);
	// The bare forged flare's strike (Blood 1.0x's 20-29), after the stock burn, on a foe still standing: dealt to one
	// the stock impact just killed, it would land on the corpse it was turned into.
	if (flareStrike && foe && missile && foe->IsDudeActor() && foe->spr.statnum == kStatDude && foe->hasX() && foe->xspr.health > 0)
		actDamageSprite(missile->GetOwner(), foe, kDamageBullet, (20 + int(Random(10))) << 4);
	if (biting)
	{
		// A bite that healed you shows: blood from the foe it was drunk from, and a short gold flash as a pickup gives.
		auto bitten = forgeBite;
		forgeBiting = false;
		forgeBiteDeeper = false;
		forgeBite = nullptr;
		auto p = getPlayer(myconnectindex);
		if (bitten && p && p->GetActor() && p->GetActor()->hasX() && int(p->GetActor()->xspr.health) > healthBefore)
		{
			fxSpawnBlood(bitten, 0);
			p->pickupEffect = std::max(p->pickupEffect, 10);
		}
	}
	if (!missile) return;
	// The explosion a fireball becomes is the same actor, and on a blast 64 means it spares you (kModSpareYou): the
	// blood-bought mark is spent once the impact is worked out.
	missile->modFlags &= ~kModBloodBought;
	if (!missile->insector()) return;
	if (larder)
	{
		// The stock impact turned the cleaver into a puff of debris: it stays a cleaver, stuck where it struck. Its tile
		// spins (2138 animates through the seven after it), so a stuck one shows one of those frames, still.
		seqKill(missile);
		missile->spr.setspritetexture(tileGetTextureID(missileInfo[kMissileButcherKnife - kMissileBase].picno + (wall ? 1 : 4)));
		missile->spr.type = kSpriteDecoration;
		missile->vel.Zero();
		missile->spr.cstat &= ~CSTAT_SPRITE_ALIGNMENT_MASK;
		DVector3 pos = missile->spr.pos;
		if (wall)
		{
			// Flush on the wall at the strike point, as the stock code lays a bullet hole there (actFireVector), on the
			// side it came from (a masked wall can be struck from behind).
			DVector2 at = NearestPointOnWall(pos.X, pos.Y, wall);
			DAngle normal = wall->normalAngle();
			if ((pos.XY() - at).dot(normal.ToVector()) < 0) normal += DAngle180;
			missile->spr.Angles.Yaw = normal;
			missile->spr.cstat |= CSTAT_SPRITE_ALIGNMENT_WALL | CSTAT_SPRITE_YCENTER;
			missile->spr.cstat2 |= CSTAT2_SPRITE_DECAL;
			SetActor(missile, DVector3(at + normal.ToVector() * 0.5, pos.Z));
		}
		else
		{
			// On the floor it lies flat along its flight; off the ceiling or a body it drops there and lies the same.
			missile->spr.Angles.Yaw = DAngle::fromDeg(flight);
			missile->spr.cstat |= CSTAT_SPRITE_ALIGNMENT_FLOOR;
			missile->spr.cstat &= ~CSTAT_SPRITE_YCENTER;
			SetActor(missile, DVector3(pos.XY(), getflorzofslopeptr(missile->sector(), pos) - 0.25));
		}
		if (missile->hasX()) missile->xspr.data2 = kClock / 4; // it arms in a second (counted in tics, 4 PlayClock each)
		evKillActor(missile, kCallbackRemove);               // the missile's 5-second removal doesn't apply to a knife
		actPostSprite(missile, kStatDecoration);             // eight at most: ForgeTick sets off the oldest past that
	}
	else if (fetch)
	{
		DVector3 back = missile->spr.pos;
		if (missile->vel.LengthSquared() > 0) back -= missile->vel.Unit() * 4;
		if (fetching.Size() < 8) fetching.Push({ back, missile->sector(), wasAlt, bloodBought });
		seqKill(missile);
		actPostSprite(missile, kStatFree);
	}
}

// While a forged blast is worked out, ScaleDamage sees it: Brimstone Leap turns the damage it does you into a throw,
// a blast of the Larder's (kModSpareYou) does you none, and any other of yours does you half.
ForgeExplosion::ForgeExplosion(DBloodActor* explosion)
{
	bool forged = explosion && run.active && !run.over && (explosion->modFlags & kModForged);
	forgeExplosionNow = forged && Sings(kLitLeap) ? explosion : nullptr;
	forgeSparingNow = forged && (explosion->modFlags & kModSpareYou) ? explosion : nullptr;
	forgeBlastNow = forged ? explosion : nullptr;
}

ForgeExplosion::~ForgeExplosion() { forgeExplosionNow = nullptr; forgeSparingNow = nullptr; forgeBlastNow = nullptr; }

// The stock homing sets a hunting missile's speed from its type each tic: a forged cleaver keeps the Forge's.
double MissileSpeedScale(DBloodActor* m)
{
	if (!m || !run.active || run.over || !(m->modFlags & kModForged) || m->spr.type != kMissileButcherKnife) return 1;
	return kCleaverSpeed;
}

// The Pyre's lodged flares, each tic: a flare burns its bearer a second time over (the stock burn is 8 a tic, so twice
// as hard), and goes out with the fire, or when its bearer has become one of the burning kinds (which burn out on
// their own). One whose bearer died or is gone bursts, as actCheckFlares bursts a stock flare (it has no 5-second
// removal to fall back on, as a fired flare has).
static void PyreTick(DBloodActor* pl)
{
	TArray<DBloodActor*> bearers;
	BloodStatIterator it(kStatFlare);
	while (auto f = it.Next())
	{
		if (!(f->modFlags & kModPyreMark) || (f->spr.flags & 32)) continue; // 32: already on its way out
		auto t = f->GetTarget();
		if (!t || t->spr.statnum != kStatDude || !t->hasX() || t->xspr.health <= 0)
		{
			GibSprite(f, GIBTYPE_17, nullptr, nullptr);
			actPostSprite(f, kStatFree);
		}
		else if (t->xspr.burnTime == 0 || IsBurningDude(t)) actPostSprite(f, kStatFree);
		else bearers.Push(t);
	}
	for (auto t : bearers) if (t->hasX() && t->xspr.health > 0) actDamageSprite(pl, t, kDamageBurn, 8);
}

// The Litanies that live between shots: second heads, candles, tolls, knives, cleavers flying back (and the Pyre's
// flares, which aren't a Litany's).
static void ForgeTick()
{
	if (!run.active || run.over || crypt.in || gamestate != GS_LEVEL) return;
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	auto pl = p->GetActor();
	PruneFetchAlt(); // after the post pass: what struck, was caught or timed out this tic has left the missiles
	if (run.forge[kForgeWound] == kWoundPyre) PyreTick(pl);
	// The starburst's rings thrown by carriers that struck early (ForgeImpact).
	if (starbursts.Size())
	{
		auto rings = std::move(starbursts);
		starbursts.Clear();
		for (auto& s : rings) ReleaseStarburst(s.pos, s.sect, s.vel, s.count);
	}
	if (!run.litanies)
	{
		actPostProcess(); // the flares put out
		return;
	}

	// Cerberus: a hunting fireball 256 from you grows a second head for another foe (kCerberusFlying fireballs at most).
	if (Sings(kLitCerberus))
	{
		TArray<DBloodActor*> heads;
		int flying = 0;
		BloodStatIterator it(kStatProjectile);
		while (auto m = it.Next())
		{
			if (!(m->modFlags & kModForged) || m->spr.type != kMissileFireballNapalm) continue;
			flying++;
			auto t = m->GetTarget();
			if (Offspring(m) == 0 && t && t->hasX() && t->xspr.health > 0 && (m->spr.pos.XY() - pl->spr.pos.XY()).Length() >= 256) heads.Push(m);
		}
		for (auto m : heads)
		{
			if (flying >= kCerberusFlying || !m->insector()) break;
			auto second = HoundFoe(m->spr.pos, m->sector(), m->vel.XY().Angle(), kHoundCone, kHoundRange, m->GetTarget());
			if (!second) continue;
			DVector3 dir = (MiddleOf(second) - m->spr.pos).Unit();
			auto head = ForgeSpawnMissile(m->spr.pos + dir * 8, m->sector(), dir, kMissileFireballNapalm, 1);
			SetOffspring(m, 3);
			if (!head) continue;
			head->SetTarget(second);
			flying++;
			LitanyFired(kLitCerberus);
		}
	}
	// Corpse-Candle: the lodged flare's blast, when its fuse runs out or its bearer dies.
	for (int i = int(candles.Size()) - 1; i >= 0; i--)
	{
		auto& c = candles[i];
		bool lodged = IsLive(c.flare, kStatFlare);
		if (lodged && c.flare->insector()) { c.pos = c.flare->spr.pos; c.sect = c.flare->sector(); }
		if (PlayClock >= c.blowAt || !lodged)
		{
			ForgeBlast(c.pos, c.sect, 1);
			if (lodged) c.flare->modFlags &= ~kModForged; // spent: a load doesn't light it again
			candles.Delete(i);
		}
	}
	// Requiem: blasts that landed together ring out as one: two as a standard, three or more as a large.
	{
		// They ring once no blast has landed for 8 PlayClock (or the oldest has waited 32), all together.
		TArray<Toll> ready;
		if (tolls.Size() && (PlayClock - lastTollAt >= 8 || PlayClock - tolls[0].at >= 32))
		{
			ready = std::move(tolls);
			tolls.Clear();
		}
		while (ready.Size())
		{
			TArray<Toll> group;
			group.Push(ready[0]);
			ready.Delete(0);
			for (int i = int(ready.Size()) - 1; i >= 0; i--)
			{
				bool near = true;
				for (auto& g : group) if ((g.pos - ready[i].pos).Length() > 128) near = false;
				if (near) { group.Push(ready[i]); ready.Delete(i); }
			}
			DVector3 mid(0, 0, 0);
			for (auto& g : group) mid += g.pos;
			mid /= double(group.Size());
			auto sect = group[0].sect;
			updatesector(mid.XY(), &sect);
			if (!sect) sect = group[0].sect;
			ForgeBlast(mid, sect, group.Size() >= 3 ? 2 : group.Size() == 2 ? 1 : 0);
			if (group.Size() >= 2) LitanyFired(kLitRequiem);
		}
	}
	// The Larder of Knives: an armed knife goes off when a foe comes within 64 of it. You don't set them off, and their
	// blasts spare you (kModSpareYou).
	if (Sings(kLitLarder))
	{
		// Eight at most: past that the oldest go off (sparing you too). Counted here, not as each sticks: by now the post
		// pass has settled every knife a volley stuck this tic, and freed the ones already set off. Oldest first (Blood
		// adds to a list's tail).
		TArray<DBloodActor*> knives;
		BloodStatIterator kt(kStatDecoration);
		while (auto k = kt.Next())
			if ((k->modFlags & kModForged) && k->spr.type == kSpriteDecoration && !(k->spr.flags & 32)) knives.Push(k);
		unsigned excess = knives.Size() > 8 ? knives.Size() - 8 : 0;
		TArray<DBloodActor*> blown;
		for (unsigned i = 0; i < knives.Size(); i++)
		{
			auto k = knives[i];
			if (i < excess)
			{
				if (k->insector()) ForgeBlast(k->spr.pos, k->sector(), 0, true);
				actPostSprite(k, kStatFree);
				continue;
			}
			if (!k->hasX() || !k->insector()) continue;
			if (k->xspr.data2 > 0) { k->xspr.data2--; continue; }
			BloodStatIterator di(kStatDude);
			while (auto d = di.Next())
			{
				if (d->IsPlayerActor() || !d->hasX() || d->xspr.health <= 0) continue;
				if ((d->spr.pos.XY() - k->spr.pos.XY()).Length() > 64 || std::abs(d->spr.pos.Z - k->spr.pos.Z) > 64) continue;
				if (!cansee(k->spr.pos, k->sector(), MiddleOf(d), d->sector())) continue;
				blown.Push(k);
				break;
			}
		}
		for (auto k : blown)
		{
			ForgeBlast(k->spr.pos, k->sector(), 0, true);
			actPostSprite(k, kStatFree);
			LitanyFired(kLitLarder);
		}
	}
	// The Fetch: cleavers that struck stone fly back at your chest; caught within 48, they give back what they cost.
	if (Sings(kLitFetch))
	{
		DVector3 chest(pl->spr.pos.XY(), p->zWeapon);
		int twins = 0;
		BloodStatIterator it(kStatProjectile);
		while (auto m = it.Next()) if ((m->modFlags & kModForged) && Offspring(m) == 2) twins++;
		for (auto& f : fetching)
		{
			if (twins >= 8) break;
			DVector3 dir = chest - f.pos;
			if (dir.LengthSquared() <= 1) continue;
			auto twin = ForgeSpawnMissile(f.pos, f.sect, dir.Unit(), kMissileButcherKnife, 2);
			if (!twin) continue;
			if (f.alt) NoteFetchAlt(twin);
			if (f.blood) twin->modFlags |= kModBloodBought; // offspring 2: no Litany reads it, and its strike clears it
			evPostActor(twin, 3 * kClock, kCallbackRemove);
			twins++;
			LitanyFired(kLitFetch);
		}
		fetching.Clear();
		TArray<DBloodActor*> caught;
		BloodStatIterator it2(kStatProjectile);
		while (auto m = it2.Next())
		{
			if (!(m->modFlags & kModForged) || Offspring(m) != 2) continue;
			DVector3 d = chest - m->spr.pos;
			if (d.Length() < 48) { caught.Push(m); continue; }
			m->vel = d.Unit() * ForgeSpeed(m->spr.type);
		}
		// The Vessel's ammo back for what the caught cleavers cost (Rates: a round's drink buys perRound of them), so a
		// flare gun firing five a flare gives one back for every five caught. A round a cleaver would let a flare shot
		// at a wall bring five flares back. An alt pull's cleaver cost a third less (kForgeAltBonus): it gives that
		// much back. Counted in thirds of a drink. One bought with blood (the Blood Price, dry) cost no ammo, so it
		// gives none back: else health would turn into ammo.
		auto r = Rates(run.forge);
		int ammo = forgeVessels[run.forge[kForgeVessel]].ammo;
		for (auto m : caught)
		{
			bool wasAlt = FetchAlt(m);
			if (wasAlt) DropFetchAlt(m);
			if (!(m->modFlags & kModBloodBought)) fetchCredit += (wasAlt ? 2 : 3) * r.drink;
			evKillActor(m);
			actPostSprite(m, kStatFree);
			bool refund = false;
			while (r.perRound > 0 && fetchCredit >= 3 * r.perRound)
			{
				fetchCredit -= 3 * r.perRound;
				p->ammoCount[ammo] = ClipHigh(p->ammoCount[ammo] + 1, gAmmoInfo[ammo].max);
				refund = true;
			}
			if (refund) sfxPlay3DSound(pl, 782, -1, 0); // as Hymn of Ash's refund
		}
	}
	// Hymn of Ash's records last as long as a missile can fly (5 s); a shot goes with its record.
	for (int i = int(hymnPulls.Size()) - 1; i >= 0; i--) if (PlayClock - hymnPulls[i].at > 5 * kClock) hymnPulls.Delete(i);
	for (int i = int(hymnShots.Size()) - 1; i >= 0; i--) if (!HymnRecord(hymnShots[i].pull)) hymnShots.Delete(i);
	// What this tic posted (blasts, freed knives and twins) is done now, not left for the next tic's post pass: a save,
	// a load or a level change can come between.
	actPostProcess();
}

int ScaleHealing(DBloodActor* actor, int add)
{
	if (!run.active || run.over || !actor || !actor->IsPlayerActor() || add <= 0) return add;
	if (forgeBiting)
	{
		// A forged cleaver's stock heal (10, whatever it did): half the health its wound took from a live foe, rounded,
		// and at least 1; none when it struck no live foe. actHealDude counts whole health; health is in sixteenths.
		int lost = forgeBite && forgeBite->hasX() ? std::max(forgeBiteHealth - int(forgeBite->xspr.health), 0) : 0;
		if (lost <= 0) return 0;
		// Already whole (actHealDude's threshold here is your start health): nothing heals, so none of the cap is spent.
		if (actor->hasX() && actor->xspr.health >= unsigned(getDudeInfo(actor->spr.type)->startHealth << 4)) return 0;
		add = std::max(1, (lost / 2 + 8) >> 4);
	}
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
	DiscoverLitanies(f);
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
	if (owner && tspr && owner == forgeScent && Sings(kLitBloodFeud) && PlayClock - forgeScentAt < 5 * kClock)
	{
		auto p = getPlayer(myconnectindex);
		if (p && p->ammoCount[forgeVessels[run.forge[kForgeVessel]].ammo] == 0)
			tspr->shade = int8_t(std::clamp(tspr->shade + int(24 * (0.5 + 0.5 * sin(PlayClock * 0.12))), -128, 127)); // Blood Feud's quarry
	}
	if (!owner || !tspr || !(owner->modFlags & kModChampion)) return;
	int pal = (owner->modFlags & kModRedChampion) ? rl_elite_red_pal : rl_elite_pal;
	if (pal > 0 && pal < MAXREALPAL && lookups.checkTable(pal)) tspr->pal = pal;
	tspr->shade = int8_t(std::max(tspr->shade - 12, -128)); // and a little brighter
}

// An item drawn, after a coloured floor has put its palette on it (viewApplyDefaultPal): an Omen keeps its tier's, as the
// tier is what it tells. And a cursed reliquary is circled by Blood's own "atom" view effect (kViewEffectAtom, which no
// stock code draws), its maths as animatesprite.cpp has it: 16 fullbright motes (tile 1720) on randomly tilted rings, a
// shell of radius 32 turning once a second. Here it's centred kReliquaryHaloZ above the chest's floor, so no mote dips
// under it and its top is at your eye even with the chest below the screen, and the motes take the chest's FLAME (they
// stay bright in it, and their own blue is the rare Omens' COLD). Drawn each frame, so nothing of it is saved.
void OmenItemSprite(tspriteArray& tsprites, tspritetype* tspr)
{
	auto a = tspr ? static_cast<DBloodActor*>(tspr->ownerActor) : nullptr;
	if (!IsOmenItem(a) || !OmensLive()) return;
	int tier = omenInfo[a->xspr.data1].tier;
	tspr->pal = omenPal[tier];
	if (tier != kOmenGamble) return;
	double top, bottom;
	GetActorExtents(a, &top, &bottom);
	DVector3 centre(tspr->pos.XY(), bottom - kReliquaryHaloZ);
	for (int i = 0; i < 16; i++)
	{
		auto m = viewInsertTSprite(tsprites, tspr->sectp, 32767, tspr);
		if (!m) break;
		auto ang = mapangle((PlayClock * 2048) / 120).Normalized360() + random_angles[i][2];
		auto vect = DVector3(32 * ang.ToVector(), 0);
		DVector2 pt(vect.Y, vect.Z);
		pt = pt.Rotated(random_angles[i][0]);
		vect.Y = pt.X;
		pt.X = vect.X;
		pt = pt.Rotated(random_angles[i][1]);
		vect.X = pt.X;
		vect.Z = pt.Y;
		m->pos = centre + vect;
		m->setspritetexture(aTexIds[kTexATOMEFFECT]);
		m->shade = -128;
		m->pal = omenPal[kOmenGamble];
	}
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
				("unspent", run.unspent)
				("omenrolls", run.omenRolls);
			arc.Array("cov", run.cov, kNumConditions);
			arc.Array("kit", run.kit, kNumKit);
			arc.Array("offers", run.offers, 3);
			arc.Array("forge", run.forge, kForgeSlots);
			arc("litanies", run.litanies);
			// The Omens running, four numbers each: the Omen, its time left, its base time, the most it has had. Their
			// stock powerups' time is the player's own (saved with it); their base effects start again on the load.
			TArray<int> running;
			for (auto& r : omens)
			{
				running.Push(r.id);
				running.Push(r.left);
				running.Push(r.base);
				running.Push(r.peak);
			}
			arc("omens", running);
			arc.EndObject();
		}
		return;
	}

	// Reading: the save's run, if it has one that can still go on.
	startingRun = false;
	refusedLoad = false;
	omens.Clear();
	reliquaryHint = "";
	Run loaded;
	TArray<int> running;
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
			("unspent", loaded.unspent)
			("omenrolls", loaded.omenRolls);
		arc.Array("cov", loaded.cov, kNumConditions);
		arc.Array("kit", loaded.kit, kNumKit);
		arc.Array("offers", loaded.offers, 3);
		arc.Array("forge", loaded.forge, kForgeSlots);
		arc("litanies", loaded.litanies);
		arc("omens", running);
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
	for (unsigned i = 0; i + 3 < running.Size(); i += 4) // the save's Omens (OmensLoaded starts their effects)
		if (running[i] >= 0 && running[i] < kNumOmens && running[i + 1] > 0 && running[i + 2] > 0 && !RunningOf(running[i]))
			omens.Push({ running[i], running[i + 1], running[i + 2], std::max(running[i + 3], running[i + 1]), false });
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

// Sing or silence the n-th lit Litany of the recipe (the Forge menu's Litany lines).
CCMD(rl_forge_sing)
{
	int f[kForgeSlots];
	ForgeRecipe(f);
	int lit[4], n = LitLitanies(f, lit, 4);
	int i = argv.argc() > 1 ? atoi(argv[1]) - 1 : -1;
	if (i < 0 || i >= n) return;
	int k = lit[i];
	auto list = SingList();
	auto drop = [&](const char* id) { for (int j = int(list.Size()) - 1; j >= 0; j--) if (!list[j].CompareNoCase(id) || (list[j][0] == '-' && !list[j].Mid(1).CompareNoCase(id))) list.Delete(j); };
	auto write = [&]
	{
		FString out;
		for (auto& e : list) out.AppendFormat("%s%s", out.IsEmpty() ? "" : ",", e.GetChars());
		rl_forge_sing = out.GetChars();
	};
	if (ResolveSinging(f) & (1 << k))
	{
		drop(litanies[k].id);
		list.Push(FStringf("-%s", litanies[k].id));
	}
	else
	{
		drop(litanies[k].id);
		list.Insert(0, litanies[k].id); // chosen first, so it wins over the defaults
		write();
		// Still silent: its rival gives way first, then the singer chosen longest ago, until it sings. Only this
		// gun's lit Litanies are touched (a choice made for another recipe stays as it was).
		for (int guard = 0; guard < kNumLitanies && !(ResolveSinging(f) & (1 << k)); guard++)
		{
			int singing = ResolveSinging(f), victim = -1;
			for (int j = 0; j < kNumLitanies && victim < 0; j++)
				if (j != k && (singing & (1 << j)) && (litanies[j].rivals & litanies[k].rivals)) victim = j;
			for (int j = int(list.Size()) - 1; j >= 0 && victim < 0; j--)
			{
				int q = list[j][0] == '-' ? -1 : LitanyIndex(list[j].GetChars());
				if (q >= 0 && q != k && (singing & (1 << q))) victim = q;
			}
			for (int j = kNumLitanies - 1; j >= 0 && victim < 0; j--) if (j != k && (singing & (1 << j))) victim = j;
			if (victim < 0) break;
			drop(litanies[victim].id);
			list.Push(FStringf("-%s", litanies[victim].id));
			write();
		}
	}
	write();
	UpdateMenus();
}

CCMD(rl_litanies)
{
	int found = 0;
	for (int k = 0; k < kNumLitanies; k++) if (LitanyFound(k)) found++;
	Printf("The Book of Litanies: %d of %d found\n", found, kNumLitanies);
	for (int k = 0; k < kNumLitanies; k++)
	{
		auto& L = litanies[k];
		if (LitanyFound(k)) Printf("  %s (%s): %s + %s. %s\n", L.name, litanyKinds[L.kind], PartName(L.a).GetChars(), PartName(L.b).GetChars(), L.sentence);
		else Printf("  ??? (%s): a part of %s and one of %s\n", litanyKinds[L.kind], circleNames[L.a.slot], circleNames[L.b.slot]);
	}
	if (run.active && !run.over && run.litanies)
		for (int k = 0; k < kNumLitanies; k++) if (run.litanies & (1 << k)) Printf("Singing in this run: %s\n", litanies[k].name);
}

// Every legal recipe: the Litanies it lights and the two that sing by default.
CCMD(rl_litanies_audit)
{
	int recipes = 0, none = 0;
	int f[kForgeSlots];
	for (f[kForgeVessel] = 1; f[kForgeVessel] <= forgeSlotMax[kForgeVessel]; f[kForgeVessel]++)
	for (f[kForgeMouth] = 0; f[kForgeMouth] <= forgeSlotMax[kForgeMouth]; f[kForgeMouth]++)
	for (f[kForgePath] = 0; f[kForgePath] <= forgeSlotMax[kForgePath]; f[kForgePath]++)
	for (f[kForgeWound] = 0; f[kForgeWound] <= forgeSlotMax[kForgeWound]; f[kForgeWound]++)
	for (f[kForgePenance] = 0; f[kForgePenance] <= forgeSlotMax[kForgePenance]; f[kForgePenance]++)
	{
		bool legal = true;
		for (int slot = kForgeMouth; slot < kForgeSlots; slot++) if (ForgeIllegal(f, slot)) legal = false;
		if (!legal) continue;
		recipes++;
		FString lit;
		int sing = 0, count = 0, rivals = 0;
		for (int k = 0; k < kNumLitanies; k++)
		{
			if (!LitanyLit(f, k)) continue;
			lit.AppendFormat("%s%s", lit.IsEmpty() ? "" : ", ", litanies[k].name);
			if (count < kMaxSinging && litanies[k].kind != kKindBane && !(litanies[k].rivals & rivals)) { sing |= 1 << k; rivals |= litanies[k].rivals; count++; }
		}
		int parts = (f[kForgeMouth] != 0) + (f[kForgePath] != 0) + (f[kForgeWound] != 0) + (f[kForgePenance] != 0);
		if (lit.IsEmpty() && parts >= 2) none++;
		FString singing;
		for (int k = 0; k < kNumLitanies; k++) if (sing & (1 << k)) singing.AppendFormat("%s%s", singing.IsEmpty() ? "" : ", ", litanies[k].name);
		Printf("%s: %s%s%s\n", ForgeName(f).GetChars(), lit.IsEmpty() ? "none" : lit.GetChars(), singing.IsEmpty() ? "" : "; sing: ", singing.GetChars());
	}
	Printf("%d legal recipes; %d with two or more runes light no Litany\n", recipes, none);
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
			run.litanies = ResolveSinging(run.forge);
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

// The Omens, for testing every one without a run of luck. An Omen is named by its number in "rl_omen list", by its name
// without spaces or capitals ("twinvessel", "deathsgrin"), or by the start of that ("twin").
static int FindOmen(const char* arg)
{
	auto squash = [](const char* s)
	{
		FString out;
		for (; *s; s++) if (isalnum((unsigned char)*s)) out += char(tolower((unsigned char)*s));
		return out;
	};
	char* end;
	long n = strtol(arg, &end, 10);
	if (*arg && !*end) return n >= 1 && n <= kNumOmens ? int(n) - 1 : -1;
	FString want = squash(arg);
	if (want.IsEmpty()) return -1;
	for (int i = 0; i < kNumOmens; i++) if (squash(omenInfo[i].name).Compare(want) == 0) return i;
	for (int i = 0; i < kNumOmens; i++) if (squash(omenInfo[i].name).IndexOf(want) == 0) return i;
	return -1;
}

static bool OmenReady(const char* cmd)
{
	auto p = getPlayer(myconnectindex);
	if (OmensLive() && gamestate == GS_LEVEL && p && p->GetActor() && p->GetActor()->insector()) return true;
	Printf("%s: no run in progress (or rl_omens is 0, or this is the Crypt)\n", cmd);
	return false;
}

// One lying in front of you (or beside you, with no room ahead; at your feet, with none at all).
static void SpawnOmenAhead(const char* cmd, int id)
{
	auto p = getPlayer(myconnectindex);
	auto pl = p->GetActor();
	std::mt19937 rng(uint32_t(PlayClock) ^ 0x5BD1E995u);
	DVector2 ahead = pl->spr.pos.XY() + pl->spr.Angles.Yaw.ToVector() * 64;
	auto s = pl->sector();
	updatesector(ahead, &s);
	DVector3 floor = FloorAt(pl);
	TArray<DVector2> taken;
	DVector2 spot = pl->spr.pos.XY();
	sectortype* sect = pl->sector();
	double clear = omenInfo[id].tier == kOmenGamble ? kReliquaryClear : 0; // a reliquary is drawn big (as PlaceOmenItem)
	if (s && cansee(DVector3(pl->spr.pos.XY(), p->zView), pl->sector(), DVector3(ahead, getflorzofslopeptr(s, ahead.X, ahead.Y) - 16), s)
		&& SpotOk(s, ahead, floor.Z, taken, true, clear))
	{
		spot = ahead;
		sect = s;
	}
	else FindItemSpot(rng, pl->sector(), floor, 24, 80, taken, spot, sect, nullptr, 16, clear);
	if (SpawnOmenItem(sect, spot, id)) Printf(PRINT_NOTIFY, "%s: %s\n", cmd, omenInfo[id].name);
	else Printf("%s: no room for it\n", cmd);
}

CCMD(rl_omen)
{
	if (argv.argc() < 2)
	{
		Printf("usage: rl_omen list | rl_omen <name|number> [seconds]   (rl_omen_drop, rl_reliquary <name|number>: one in front of you)\n");
		return;
	}
	if (!stricmp(argv[1], "list"))
	{
		for (int i = 0; i < kNumOmens; i++)
		{
			auto& o = omenInfo[i];
			auto r = RunningOf(i);
			Printf("%2d  %-13s %-7s %2d s  %s%s\n", i + 1, o.name, omenTierNames[o.tier], o.seconds, o.sentence,
				r ? FStringf("  (running: %d s left)", bmod::SecondsLeft(r->left)).GetChars() : "");
		}
		Printf("Drops: every champion (blue: common %d%%, rare %d%%; red: rare %d%%, relic %d%%), a plain enemy 1 in %d (common %d%%,"
			" rare %d%%, relic %d%%). Cursed reliquaries (the gambles, taken with Use): red champions 1 in %d, blue 1 in %d. rl_omens %d\n",
			int(100 * (1 - kOmenBlueRare) + 0.5), int(100 * kOmenBlueRare + 0.5), int(100 * (1 - kOmenRedRelic) + 0.5), int(100 * kOmenRedRelic + 0.5),
			int(1 / kOmenPlain + 0.5), int(100 * (1 - kOmenPlainRare - kOmenPlainRelic) + 0.5), int(100 * kOmenPlainRare + 0.5),
			int(100 * kOmenPlainRelic + 0.5), int(1 / kReliquaryRed + 0.5), int(1 / kReliquaryBlue + 0.5), (int)rl_omens);
		return;
	}
	int id = FindOmen(argv[1]);
	if (id < 0)
	{
		Printf("rl_omen: no Omen '%s' (rl_omen list)\n", argv[1]);
		return;
	}
	if (!OmenReady("rl_omen")) return;
	int seconds = argv.argc() > 2 ? std::clamp(atoi(argv[2]), 1, 600) : omenInfo[id].seconds;
	if (!GrantOmen(id, seconds * kClock))
	{
		Printf("rl_omen: %s can't take hold %s\n", omenInfo[id].name, HoldBar(id).GetChars());
		return;
	}
	Printf(PRINT_NOTIFY, "Omen: %s, %d s: %s\n", omenInfo[id].name, bmod::SecondsLeft(RunningOf(id)->left), omenInfo[id].sentence);
}

CCMD(rl_omen_drop)
{
	int id = argv.argc() > 1 ? FindOmen(argv[1]) : -1;
	if (id < 0 || omenInfo[id].tier == kOmenGamble)
	{
		Printf("usage: rl_omen_drop <name|number>   (an Omen; a gamble comes in a cursed reliquary: rl_reliquary)\n");
		return;
	}
	if (OmenReady("rl_omen_drop")) SpawnOmenAhead("rl_omen_drop", id);
}

CCMD(rl_reliquary)
{
	int id = argv.argc() > 1 ? FindOmen(argv[1]) : -1;
	if (id < 0 || omenInfo[id].tier != kOmenGamble)
	{
		Printf("usage: rl_reliquary <name|number>   (a gamble: Glass Cannon, Frenzy, Powder Keg; rl_omen list)\n");
		return;
	}
	if (OmenReady("rl_reliquary")) SpawnOmenAhead("rl_reliquary", id);
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
