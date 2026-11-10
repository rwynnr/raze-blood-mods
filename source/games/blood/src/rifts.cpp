//---------------------------------------------------------------------------
//
// blood2d roguelite: rifts (rl_rifts). Design: blood2d workspace, docs/roguelite.md,
// "Momentum and rifts: decisions (round 5)", item 7 (Design A, "Rift on wake").
//
// The plan. As a map of a run starts (Remix), a random stream of its own picks,
// for each buried or laying axe zombie that isn't wired into the map (no rx or
// tx) and carries no key, whether it stays a zombie (about half on the run's
// first map, falling to a quarter on its last) or what comes through its rift
// instead. The zombies' own threat pays for them: 2 points each, the axe
// zombie's cost in the run's table, spent so a dear kind is paid for by cheap
// ones, from a pool that widens through the run (the first third: brown and
// red spiders and butchers; then shotgun and tommy cultists and black spiders;
// the last third: hounds, flesh and stone gargoyles, phantasms, tesla and TNT
// cultists).
// At most 1 gunman (a cultist) in 4 of a wake group (zombies chained within
// 320 units, their wake range). Each is a blue champion at the remix's odds,
// never red. A kind must fit the spot (headroom and elbow room). The plan is
// saved with each zombie (DBloodActor::riftPlan), so a load brings back the
// same foes.
//
// The rift. When such a zombie wakes (Blood's own rule, unchanged), it doesn't
// climb out: it vanishes, and Blood's respawn sparkle (FX_29, 2-3x, by the
// size of what's coming) opens with the respawn sound (350), one sound to a
// group: a rift is silent if another within 320 units sounded in the last
// second. A second sparkle follows at 0.73 s, and the foe arrives at 1.5 s
// plus its own 0-0.5 s (the plan's stagger), unless something holds it:
// anyone standing where it would be (within both clipdists and a margin),
// you inside its melee reach, or you within 128 units and not facing it. A
// held rift sparkles on, silently, every 0.73 s. Then the zombie is retyped in
// place (the same actor: the map's wiring, its drop, the kill count and its
// flags hold), set on the floor facing you, fades in over 0.25 s, can be hit
// at once but doesn't attack (nor flinch) for 0.5 s, and comes for you.
//
// While it waits a rift can't be seen or shot and doesn't block, but blasts
// reach it: it loses health but can't die, flinch or burn, and the share it
// lost carries over to what arrives (a champion's and the Covenant's Flesh
// health too, which the base's RetypeDude would clamp away). A kind that no
// longer fits the spot as the zombie wakes: the stock rise.
//
// Hooks into stock code (one-liners, marked "blood2d roguelite:"):
// aiActivateDude (the wake), actDamageSprite (a rift can't die), actBurnSprite
// (nor burn), RecoilDude (nor flinch), viewProcessSprites (the fade), the save
// list of AI states (five states, appended) and DBloodActor's save (the plan,
// written only when set). The roguelite calls PlanRifts from Remix and
// RiftLedger from CountAmmo, and keeps the run's side (RiftRunLive...).
//
//---------------------------------------------------------------------------

#include "ns.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <random>
#include "build.h"
#include "blood.h"
#include "c_cvars.h"

CVARD(Bool, rl_rifts, true, CVAR_ARCHIVE, "roguelite: a woken buried or laying zombie opens a rift and the map's plan sends another foe through (0 = the stock rise)")

BEGIN_BLD_NS

namespace rogue
{

namespace rifts
{

constexpr int kSecond = 120;           // PlayClock units, and AI state ticks, a second
constexpr int kSparkTicks = 88;        // FX_29 is SEQ 47 RESPAWN: 11 frames of 8 ticks, 0.73 s (it removes itself)
constexpr int kArriveTicks = 180;      // the stock rise's 1.5 s (zombieEUp)
constexpr int kStaggerStep = 4;        // the plan's stagger: 0-15 steps of this, 0-0.5 s
constexpr int kHoldTicks = 60;         // after arriving: no attack, no flinch, for 0.5 s
constexpr int kFadeTicks = 30;         // and fading in over the first 0.25 s of it
constexpr double kGroupRange = 320;    // a wake group: zombies chained this close (their hearing); the sound's reach too
constexpr int kSoundMemory = kSecond;  // a rift is silent if another that near sounded within this
constexpr double kNear = 128;          // you this near and not facing it: it waits
constexpr double kViewCone = 45;       // facing it: within this many degrees of your aim (a 90 degree view), in sight
constexpr double kMargin = 4;          // the room it needs past both bodies' clipdists
constexpr double kSparkPerHeight = 24; // the sparkle's size: the arrival's height over this, from 2x to 3x

// The plan, packed in DBloodActor::riftPlan: the type - kDudeBase (0: no plan), a blue champion, the stagger's steps.
constexpr uint16_t kPlanType = 0xFF, kPlanChampion = 0x100;
constexpr int kPlanStaggerShift = 9;

int PlanType(uint16_t plan) { return (plan & kPlanType) ? kDudeBase + (plan & kPlanType) : 0; }
bool PlanChampion(uint16_t plan) { return (plan & kPlanChampion) != 0; }
int PlanStagger(uint16_t plan) { return ((plan >> kPlanStaggerShift) & 15) * kStaggerStep; }

// What can come through, from which part of the run (0 its first third, 1 the middle, 2 the last third). Gunmen
// (cultists) are capped per wake group. The costs are the run's own (RiftThreatCost).
struct RiftKind { int type; int from; bool gunman; };
const RiftKind riftKinds[] =
{
	{ kDudeSpiderBrown, 0, false }, { kDudeSpiderRed, 0, false }, { kDudeZombieButcher, 0, false },
	{ kDudeCultistShotgun, 1, true }, { kDudeCultistTommy, 1, true }, { kDudeSpiderBlack, 1, false },
	{ kDudeHellHound, 2, false }, { kDudeGargoyleFlesh, 2, false }, { kDudePhantasm, 2, false },
	{ kDudeCultistTesla, 2, true }, { kDudeCultistTNT, 2, true }, { kDudeGargoyleStone, 2, false },
};

// A kind's height as it stands: its idle SEQ's first frame (the tile at the frame's size), as GetActorExtents measures.
double TypeHeight(int type)
{
	auto info = getDudeInfo(type);
	auto seq = info ? getSequence(info->seqStartID) : nullptr;
	if (!seq || seq->nFrames <= 0) return 64;
	auto& frame = seq->frames[0];
	auto tex = TexMan.GetGameTexture(seqGetTexture(&frame));
	if (!tex) return 64;
	return tex->GetDisplayHeight() * (frame.scaley ? frame.scaley : 64) * REPEAT_SCALE;
}

// How near you must be for its first attack to be a melee one (the distances its AI tests, in world units).
double MeleeReach(int type)
{
	switch (type)
	{
	case kDudeSpiderBrown:
	case kDudeSpiderBlack: return 115.1875; // the leap (aispid.cpp), past the bite's 57.5625
	case kDudeSpiderRed: return 57.5625;    // the bite only
	case kDudeHellHound: return 38.375;     // the bite (its fire breath is ranged)
	case kDudeZombieButcher:
	case kDudeGargoyleFlesh:
	case kDudeGargoyleStone:
	case kDudePhantasm: return 64;          // the cleaver, the claws
	default: return 0;                      // the cultists: no melee
	}
}

bool Champion(DBloodActor* a) { return (a->modFlags & kModChampion) || PlanChampion(a->riftPlan); }

// The arrival's height, grown if it comes as a champion (a planned one, or a zombie the Covenant's Wardens made one).
double ArrivalHeight(DBloodActor* a)
{
	int type = PlanType(a->riftPlan);
	if (!type) return 64;
	return TypeHeight(type) * (Champion(a) ? RiftRunRules(0).championSize : 1.);
}

// The arrival's health, in sixteenths: the share of its whole the zombie (or its rift) kept, of the new kind's whole.
// Both wholes take the Covenant's Flesh, and the zombie's its tier (the Wardens), which RetypeDude would clamp away.
// tier: with the arrival's tier factor too (the ammo ledger; MakeChampion applies it to the real one).
double ArrivalHealth(DBloodActor* a, int type, bool tier)
{
	auto rules = RiftRunRules(0);
	bool red = (a->modFlags & kModRedChampion) != 0;
	double was = red ? rules.redHealth : (a->modFlags & kModChampion) ? rules.championHealth : 1.;
	double whole = double(getDudeInfo(a->spr.type)->startHealth << 4) * rules.flesh * was;
	double kept = whole > 0 ? std::clamp(double(a->xspr.health) / whole, 0., 1.) : 1.;
	double health = double(getDudeInfo(type)->startHealth << 4) * rules.flesh * kept;
	if (tier && Champion(a)) health *= red ? rules.redHealth : rules.championHealth;
	return std::max(health, 16.);
}

// It fits where the zombie lies: dry floor, headroom over its height, elbow room around it (the base's wave spots'
// tests, bloodmod.cpp FindWaveSpot: four rays of 1.5x its clipdist, from its middle).
bool Fits(DBloodActor* a, int type, double height)
{
	auto sect = a->sector();
	if (!sect || IsUnderwaterSector(sect)) return false;
	DVector2 p = a->spr.pos.XY();
	double cz, fz;
	calcSlope(sect, p.X, p.Y, &cz, &fz);
	if (fz - cz < height + 4) return false;
	double room = std::max(getDudeInfo(type)->fClipdist(), 4.) * 1.5;
	DVector3 mid(p, fz - height * 0.5);
	for (int q = 0; q < 4; q++)
	{
		DVector2 e = p + DAngle::fromDeg(q * 90.).ToVector() * room;
		auto es = sect;
		updatesector(e, &es);
		if (!es || !cansee(mid, sect, DVector3(e, mid.Z), es)) return false;
	}
	return true;
}

DBloodActor* LivePlayer()
{
	auto p = getPlayer(myconnectindex);
	auto pl = p ? p->GetActor() : nullptr;
	return pl && pl->hasX() && pl->xspr.health > 0 && pl->insector() ? pl : nullptr;
}

// Nothing holds the arrival: nobody stands where it would be (you, or a solid foe, within both clipdists and a margin,
// at its height: a sleeping zombie, a waiting rift or a phantasm between attacks isn't solid), you aren't inside its
// melee reach, and you aren't near it and facing away (or with it out of sight).
bool NothingHolds(DBloodActor* a)
{
	int type = PlanType(a->riftPlan);
	auto sect = a->sector();
	if (!type || !sect) return true;
	double height = ArrivalHeight(a);
	DVector2 spot = a->spr.pos.XY();
	double fz = getflorzofslopeptr(sect, spot.X, spot.Y);
	double clip = getDudeInfo(type)->fClipdist();
	BloodStatIterator it(kStatDude);
	while (auto other = it.Next())
	{
		if (other == a || !other->hasX() || other->xspr.health <= 0 || (other->spr.flags & 32)) continue;
		if (!other->IsPlayerActor() && !(other->spr.cstat & (CSTAT_SPRITE_BLOCK | CSTAT_SPRITE_BLOCK_HITSCAN))) continue;
		double r = clip + other->clipdist + kMargin;
		if ((other->spr.pos.XY() - spot).LengthSquared() >= r * r) continue;
		double top, bottom;
		GetActorExtents(other, &top, &bottom);
		if (bottom <= fz - height || top >= fz) continue; // over it or under it
		return false;
	}
	auto pl = LivePlayer();
	if (!pl) return true;
	DVector2 to = spot - pl->spr.pos.XY();
	double dist = to.Length();
	if (dist < MeleeReach(type)) return false;
	if (dist < kNear)
	{
		auto p = getPlayer(myconnectindex);
		bool facing = absangle(to.Angle(), pl->spr.Angles.Yaw).Degrees() <= kViewCone
			&& cansee(DVector3(pl->spr.pos.XY(), p->zView), pl->sector(), DVector3(spot, fz - height * 0.5), sect);
		if (!facing) return false;
	}
	return true;
}

// One sound to a group: the rifts that sounded lately (not saved: after a load, at worst one more rift sounds).
struct Sounded { DVector2 pos; int clock; };
TArray<Sounded> sounded;

bool ShouldSound(const DVector2& pos)
{
	for (int i = int(sounded.Size()) - 1; i >= 0; i--)
		if (PlayClock - sounded[i].clock >= kSoundMemory || sounded[i].clock > PlayClock) sounded.Delete(i);
	for (auto& s : sounded) if ((s.pos - pos).LengthSquared() < kGroupRange * kGroupRange) return false;
	sounded.Push({ pos, PlayClock });
	return true;
}

// Blood's respawn sparkle on the floor where it lies, grown for what's coming; and the respawn sound if it's the first.
void SparkAt(DBloodActor* a, bool sound)
{
	auto sect = a->sector();
	if (!sect) return;
	DVector3 pos(a->spr.pos.XY(), getflorzofslopeptr(sect, a->spr.pos.X, a->spr.pos.Y));
	if (auto fx = gFX.fxSpawnActor(FX_29, sect, pos))
	{
		double size = std::clamp(ArrivalHeight(a) / kSparkPerHeight, 2., 3.);
		fx->spr.scale *= size; // this frame; the SEQ's next frames take xspr.scale
		if (fx->hasX()) fx->xspr.scale = int(256 * size);
	}
	if (sound) sfxPlay3DSound(a, 350, -1, 0);
}

// A rift with no plan left (a save from another build): the zombie rises as Blood has it.
void RiseStock(DBloodActor* a)
{
	a->riftPlan = 0;
	aiNewState(a, a->spr.type == kDudeZombieAxeLaying ? &zombie13AC2C : &zombieEUp);
}

// The map's own look for a kind (its palette and size), as the remix keeps it; else stock (its SEQ sets its own).
void LookOf(int type, int& pal, DVector2& scale)
{
	pal = 0;
	scale = DVector2(1, 1);
	BloodStatIterator it(kStatDude);
	while (auto d = it.Next())
		if (d->spr.type == type && d->hasX() && d->xspr.health > 0 && !(d->modFlags & kModChampion))
		{
			pal = d->spr.pal;
			scale = d->spr.scale;
			return;
		}
}

// The foe comes through: the zombie retyped in place, on the floor where it lay, facing you.
void Arrive(DBloodActor* a)
{
	int type = PlanType(a->riftPlan);
	if (!type)
	{
		RiseStock(a);
		return;
	}
	bool champion = Champion(a), red = (a->modFlags & kModRedChampion) != 0;
	int health = int(ArrivalHealth(a, type, false));
	a->modFlags &= ~(kModChampion | kModRedChampion); // RiftMakeChampion sets them again; every other bit stays
	a->SetTarget(nullptr); // with a target RetypeDude would wake it at once, past the hold
	int pal;
	DVector2 scale;
	LookOf(type, pal, scale);
	bmod::RetypeDude(a, type, pal, scale);
	a->xspr.health = health;
	if (champion) RiftMakeChampion(a, red);
	a->riftPlan = 0;
	if (auto sect = a->sector())
	{
		double top, bottom;
		GetActorExtents(a, &top, &bottom);
		a->spr.pos.Z += getflorzofslopeptr(sect, a->spr.pos.X, a->spr.pos.Y) - bottom;
	}
	a->vel.Zero();
	if (auto pl = LivePlayer())
	{
		a->spr.Angles.Yaw = (pl->spr.pos.XY() - a->spr.pos.XY()).Angle();
		aiSetTarget(a, pl); // your hits don't wake it early: they come from its target
	}
	a->xspr.goalAng = a->spr.Angles.Yaw;
	a->interpolated = false;
	viewBackupSpriteLoc(a);
	aiNewState(a, &riftHold);
}

bool TryArrive(DBloodActor* a)
{
	if (!NothingHolds(a)) return false;
	Arrive(a);
	return true;
}

// riftOpen's entry: the zombie vanishes (no frame of it shows, nothing but a blast reaches it, and a blast doesn't move
// it), and the first sparkle opens.
void OpenEnter(DBloodActor* a)
{
	seqKill(a);
	a->spr.cstat |= CSTAT_SPRITE_INVISIBLE;
	a->spr.cstat &= ~(CSTAT_SPRITE_BLOCK | CSTAT_SPRITE_BLOCK_HITSCAN);
	a->spr.flags &= ~(kPhysMove | kHitagAutoAim);
	a->vel.X = a->vel.Y = 0;
	a->xspr.burnTime = 0;
	a->SetTarget(nullptr);
	SparkAt(a, ShouldSound(a->spr.pos.XY()));
}

// riftSpark's entry: the second sparkle, back to back with the first; the foe is due at 1.5 s and its stagger.
void SparkEnter(DBloodActor* a)
{
	SparkAt(a, false);
	a->xspr.stateTimer = uint16_t(kArriveTicks - kSparkTicks + PlanStagger(a->riftPlan));
}

// riftWait's entry, at the time it's due and again each 0.73 s while something holds it: it arrives, or sparkles on.
void WaitEnter(DBloodActor* a)
{
	if (!TryArrive(a)) SparkAt(a, false);
}

// A waiting rift's move: a sloped or panning floor doesn't carry it off, and it targets no one (a hit gives it its
// attacker as a target, as any sleeping foe gets one), so nothing takes it for a foe that has noticed you.
void HoldStill(DBloodActor* a)
{
	a->vel.X = a->vel.Y = 0;
	if (a->GetTarget()) a->SetTarget(nullptr);
}

void WaitMove(DBloodActor* a)
{
	HoldStill(a);
	TryArrive(a);
}

// riftHold's move: it turns to you while it holds its fire.
void HoldTurn(DBloodActor* a)
{
	if (auto pl = LivePlayer()) a->xspr.goalAng = (pl->spr.pos.XY() - a->spr.pos.XY()).Angle();
	aiMoveTurn(a);
}

// riftDone's entry: it comes for you, as any enemy that has noticed you.
void HoldDone(DBloodActor* a)
{
	if (!a->GetTarget())
		if (auto pl = LivePlayer()) aiSetTarget(a, pl);
	aiActivateDude(a);
}

bool Plannable(DBloodActor* a)
{
	if (a->IsPlayerActor() || !a->hasX() || a->xspr.health <= 0 || (a->spr.flags & 32)) return false;
	bool buried = a->spr.type == kDudeZombieAxeBuried && a->xspr.aiState == &zombieEIdle;
	bool laying = a->spr.type == kDudeZombieAxeLaying && a->xspr.aiState == &zombieSIdle;
	if (!buried && !laying) return false;
	if (a->xspr.txID != 0 || a->xspr.rxID != 0) return false; // wired into the map: stays as Blood placed it
	if (a->xspr.key > 0 || (a->xspr.dropMsg >= kItemKeyBase && a->xspr.dropMsg < kItemKeyMax)) return false; // a key carrier keeps its kind, as in the remix
	return a->sector() && !IsUnderwaterSector(a->sector());
}

// A zombie asleep whose wake opens a rift (this very hit may wake it), or a waiting rift: it can't die or burn.
bool Shielded(DBloodActor* a)
{
	if (IsRift(a)) return true;
	if (!a || !a->hasX() || !a->riftPlan || !rl_rifts || !RiftRunLive()) return false;
	return (a->spr.type == kDudeZombieAxeBuried && a->xspr.aiState == &zombieEIdle) || (a->spr.type == kDudeZombieAxeLaying && a->xspr.aiState == &zombieSIdle);
}

// The map's plan (PlanRifts).
void Plan(uint32_t seed, int mapIndex, double depth)
{
	TArray<DBloodActor*> zombies;
	BloodStatIterator it(kStatDude);
	while (auto a = it.Next()) if (Plannable(a)) zombies.Push(a);
	int n = int(zombies.Size());
	if (n == 0) return;
	// A stream of its own, so rl_rifts on or off leaves the rest of the remix as it was.
	std::mt19937 rng((seed + 0x52494654u) ^ (0x85EBCA6Bu * uint32_t(mapIndex + 1)));
	auto randInt = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	auto rand01 = [&]() { return std::uniform_real_distribution<double>(0., 1.)(rng); };
	depth = std::clamp(depth, 0., 1.);

	// The wake groups: zombies chained within kGroupRange of each other.
	TArray<int> root, members, gunmen, order;
	root.Resize(n);
	members.Resize(n);
	gunmen.Resize(n);
	order.Resize(n);
	for (int i = 0; i < n; i++)
	{
		root[i] = order[i] = i;
		members[i] = gunmen[i] = 0;
	}
	auto find = [&](int i) { while (root[i] != i) i = root[i] = root[root[i]]; return i; };
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if ((zombies[i]->spr.pos.XY() - zombies[j]->spr.pos.XY()).LengthSquared() <= kGroupRange * kGroupRange)
				root[find(i)] = find(j);
	for (int i = 0; i < n; i++) members[find(i)]++;

	// In random order: the first ones stay zombies (half on the first map, a quarter on the last), and each of the rest
	// takes a random kind it can afford, leaving the cheapest kind's cost for each one still to come. So the rifts spend
	// the zombies' threat and no more, and a dear kind is paid for by cheap ones (a hound by three brown spiders). The
	// remix's own rule (up to 1.5x an even share) would almost never let a kind dearer than 3 through at 2 a zombie.
	for (int i = n - 1; i > 0; i--) std::swap(order[i], order[randInt(0, i)]);
	int stay = int(n * (0.5 - 0.25 * depth) + 0.5);
	auto rules = RiftRunRules(depth);
	int zombieCost = RiftThreatCost(kDudeZombieAxeNormal);
	double budget = double(zombieCost) * (n - stay);
	int part = depth < 1. / 3 ? 0 : depth < 2. / 3 ? 1 : 2;
	struct Option { int type, cost; bool gunman; double height; };
	TArray<Option> pool;
	int cheapest = INT_MAX;
	for (auto& k : riftKinds)
	{
		if (k.from > part) continue;
		pool.Push({ k.type, RiftThreatCost(k.type), k.gunman, TypeHeight(k.type) });
		cheapest = std::min(cheapest, pool.Last().cost);
	}
	TArray<int> afford;
	for (int i = stay; i < n; i++)
	{
		auto a = zombies[order[i]];
		int g = find(order[i]);
		bool grown = (a->modFlags & kModChampion) != 0; // a champion already (the Covenant's Wardens): it arrives as one
		double cap = std::max(budget - double(cheapest) * (n - i - 1), double(cheapest));
		afford.Clear();
		for (unsigned k = 0; k < pool.Size(); k++)
		{
			auto& o = pool[k];
			if (o.cost > cap || (o.gunman && (gunmen[g] + 1) * 4 > members[g])) continue; // at most 1 gunman in 4
			if (Fits(a, o.type, o.height * (grown ? rules.championSize : 1.))) afford.Push(int(k));
		}
		bool champion = rand01() < rules.blueChance; // drawn for each, whatever fits
		int stagger = randInt(0, 15);
		if (afford.Size() == 0)
		{
			budget -= zombieCost; // nothing it can afford fits: it stays a zombie
			continue;
		}
		auto& o = pool[afford[randInt(0, int(afford.Size()) - 1)]];
		budget -= o.cost;
		if (o.gunman) gunmen[g]++;
		if (grown || !Fits(a, o.type, o.height * rules.championSize)) champion = false; // no room to grow: a plain one
		a->riftPlan = uint16_t((o.type - kDudeBase) | (champion ? kPlanChampion : 0) | (stagger << kPlanStaggerShift));
	}
}

// The zombie wakes (RiftWake): its rift opens, or it rises as Blood has it.
bool Wake(DBloodActor* zombie)
{
	if (!zombie || !zombie->hasX() || !zombie->riftPlan) return false;
	int type = PlanType(zombie->riftPlan);
	if (!rl_rifts || !RiftRunLive() || !type || !Fits(zombie, type, ArrivalHeight(zombie)))
	{
		zombie->riftPlan = 0; // the stock rise, from now on
		return false;
	}
	aiNewState(zombie, &riftOpen);
	return true;
}

void RiftEvent(const bmod::EventInfo& ev)
{
	if (ev.type == bmod::Event::LevelStart || ev.type == bmod::Event::SaveLoaded) sounded.Clear();
}

struct RiftRegistrar
{
	RiftRegistrar() { bmod::AddListener(RiftEvent); }
} riftRegistrar;

} // namespace rifts

// The rift's states, in the save list (loadsave.cpp). No SEQ of their own: the zombie's was stopped as it vanished, and
// the arrival keeps the one RetypeDude starts (its idle).
AISTATE riftOpen = { kAiStateOther, -1, -1, rifts::kSparkTicks, rifts::OpenEnter, rifts::HoldStill, nullptr, &riftSpark };
AISTATE riftSpark = { kAiStateOther, -1, -1, rifts::kArriveTicks - rifts::kSparkTicks, rifts::SparkEnter, rifts::HoldStill, nullptr, &riftWait };
AISTATE riftWait = { kAiStateOther, -1, -1, rifts::kSparkTicks, rifts::WaitEnter, rifts::WaitMove, nullptr, &riftWait };
AISTATE riftHold = { kAiStateOther, -1, -1, rifts::kHoldTicks, nullptr, rifts::HoldTurn, nullptr, &riftDone };
AISTATE riftDone = { kAiStateOther, -1, -1, 0, rifts::HoldDone, nullptr, nullptr, nullptr };

//---------------------------------------------------------------------------
//
// hooks (rifts.h)
//
//---------------------------------------------------------------------------

bool IsRift(DBloodActor* a)
{
	if (!a || !a->hasX()) return false;
	auto st = a->xspr.aiState;
	return st == &riftOpen || st == &riftSpark || st == &riftWait;
}

void PlanRifts(uint32_t seed, int mapIndex, double depth)
{
	if (rl_rifts && RiftRunLive()) rifts::Plan(seed, mapIndex, depth);
}

bool RiftWake(DBloodActor* zombie)
{
	return rifts::Wake(zombie);
}

void RiftLedger(DBloodActor* a, int& type, double& health)
{
	int planned = a && a->hasX() ? rifts::PlanType(a->riftPlan) : 0;
	if (!planned) return;
	health = rifts::ArrivalHealth(a, planned, true) / 16.;
	type = planned;
}

int RiftDamage(DBloodActor* target, DAMAGE_TYPE damageType, int damage)
{
	if (damage <= 0 || damageType < 0 || damageType >= kDamageMax || !target || !target->IsDudeActor() || !rifts::Shielded(target))
		return damage;
	int factor = getDudeInfo(target->spr.type)->damageVal[damageType]; // as actDamageDude scales it
	if (factor <= 0) return damage; // it takes none of this anyway
	int room = int(target->xspr.health) - 16; // all but its last point
	if (room <= 0) return 0;
	return std::min(damage, int((int64_t(room) << 8) / factor));
}

bool RiftFireproof(DBloodActor* target)
{
	return target && target->IsDudeActor() && rifts::Shielded(target);
}

bool RiftSteady(DBloodActor* actor)
{
	return actor && actor->hasX() && (IsRift(actor) || actor->xspr.aiState == &riftHold);
}

// An arrival's first 0.25 s: a third, then two thirds there (Build's two translucency levels; the tsprite only, so
// nothing of it is kept).
void RiftSprite(DBloodActor* owner, tspritetype* tspr)
{
	if (!owner || !tspr || !owner->hasX() || owner->xspr.aiState != &riftHold) return;
	int since = rifts::kHoldTicks - int(owner->xspr.stateTimer);
	if (since >= rifts::kFadeTicks) return;
	tspr->cstat |= CSTAT_SPRITE_TRANSLUCENT;
	if (since < rifts::kFadeTicks / 2) tspr->cstat |= CSTAT_SPRITE_TRANS_FLIP;
	else tspr->cstat &= ~CSTAT_SPRITE_TRANS_FLIP;
}

} // namespace rogue

END_BLD_NS
