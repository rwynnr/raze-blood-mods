//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT

This file is part of NBlood.

NBlood is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#include "ns.h"	// Must come before everything else!

#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "v_font.h"

#include "blood.h"
#include "choke.h"
#include "zstring.h"
#include "razemenu.h"
#include "gstrings.h"
#include "v_2ddrawer.h"
#include "v_video.h"
#include "v_font.h"
#include "statusbar.h"
#include "automap.h"
#include "gamefuncs.h"
#include "v_draw.h"
#include "precache.h"
#include "render.h"
#include "razefont.h"
#include "c_bind.h"


EXTERN_CVAR(Bool, r_isometric)
EXTERN_CVAR(Float, r_iso_pitch)
EXTERN_CVAR(Float, r_iso_size)
EXTERN_CVAR(Float, r_iso_aimdist)
EXTERN_CVAR(Float, r_iso_yaw)
extern float gIsoPitchDeg; // hw_entrypoint.cpp
extern float gIsoSizeEff;  // hw_entrypoint.cpp: zoom incl. long-aim easing
DAngle isoCameraYaw(DAngle playerYaw); // hw_entrypoint.cpp
bool isoCursorAimActive(); // hw_entrypoint.cpp
extern DVector2 gIsoCursor;
extern bool gIsoLockActive;
EXTERN_CVAR(Bool, r_iso_longaim)
EXTERN_CVAR(Bool, r_iso_los)
EXTERN_CVAR(Float, g_iso_longrange)
CVARD(Bool, r_iso_markers, true, CVAR_ARCHIVE, "isometric: show off-screen enemies as icons at the screen edge")
CVARD(Float, r_iso_markrange, 4800.f, CVAR_ARCHIVE, "isometric: max distance for off-screen enemy markers (world units)")

CVARD(Bool, r_iso_tracers, true, CVAR_ARCHIVE, "isometric: draw hitscan tracer lines")
CVARD(Bool, r_iso_aimline, true, CVAR_ARCHIVE, "isometric: draw a line along the (auto-aimed) fire direction")
CVARD(Bool, r_iso_weaponpanel, true, CVAR_ARCHIVE, "isometric: draw the current-weapon panel")
CVARD(Bool, r_iso_headicon, false, CVAR_ARCHIVE, "isometric: float the held weapon icon over the player")
CVARD(Bool, r_iso_muzzleflash, true, CVAR_ARCHIVE, "isometric: draw the player muzzle flash on top of the player sprite")

BEGIN_BLD_NS

VIEWPOS gViewPos;
int gViewIndex;

//---------------------------------------------------------------------------
//
// blood2d isometric experiment: screen projection, tracers, aim line, weapon panel.
// The renderer places an orthographic camera looking at the player's eye from
// behind/above (see hw_entrypoint.cpp), so the eye is the viewport center and any
// world point projects linearly. All positions here are in Build world units.
//
//---------------------------------------------------------------------------

struct IsoCam
{
	DVector3 eye;
	DVector2 fwd, right;
	double sinP, cosP, halfH, halfW;
	double vx, vy, vw, vh;
};
static IsoCam isoCam;

static void isoSetupCam(const DVector3& eye, DAngle yaw)
{
	DAngle camYaw = isoCameraYaw(yaw);
	double p = std::clamp((double)gIsoPitchDeg, 5., 89.) * (pi::pi() / 180.); // effective pitch from the renderer (includes Look_Up easing)
	isoCam.eye = eye;
	isoCam.fwd = camYaw.ToVector();
	isoCam.right = (camYaw + DAngle90).ToVector();
	isoCam.sinP = sin(p);
	isoCam.cosP = cos(p);
	isoCam.halfH = std::max((double)gIsoSizeEff, 16.);
	isoCam.halfW = isoCam.halfH * ActiveRatio(screen->GetWidth(), screen->GetHeight());
	isoCam.vx = viewport3d.Left();
	isoCam.vy = viewport3d.Top();
	isoCam.vw = viewport3d.Width();
	isoCam.vh = viewport3d.Height();
}

// Returns viewport-relative pixel position.
static DVector2 isoProject(const DVector3& p)
{
	DVector3 rel = p - isoCam.eye;
	double sx = rel.XY().dot(isoCam.right);
	double sy = rel.XY().dot(isoCam.fwd) * isoCam.sinP - rel.Z * isoCam.cosP; // Build Z points down
	return { isoCam.vw * 0.5 * (1. + sx / isoCam.halfW), isoCam.vh * 0.5 * (1. - sy / isoCam.halfH) };
}

struct IsoTracer { DVector3 a, b; int time; bool player; };
static IsoTracer isoTracers[64];
static int isoTracerHead;

void isoRecordTracer(const DVector3& a, const DVector3& b, bool fromPlayer)
{
	isoTracers[isoTracerHead] = { a, b, PlayClock, fromPlayer };
	isoTracerHead = (isoTracerHead + 1) % countof(isoTracers);
}

static void isoLine(const DVector3& a, const DVector3& b, double thick, uint32_t color, uint8_t alpha)
{
	auto pa = isoProject(a), pb = isoProject(b);
	DVector2 o(isoCam.vx, isoCam.vy);
	twod->AddThickLine(pa + o, pb + o, thick, color, alpha);
}

static void isoDrawTracers()
{
	const int life = 20; // PlayClock runs at 120/s -> ~170ms
	double thick = std::max(1.5, isoCam.vh / 400.);
	for (auto& t : isoTracers)
	{
		int age = PlayClock - t.time;
		if (t.time == 0 || age < 0 || age > life) continue;
		uint8_t alpha = uint8_t(255 * (1. - double(age) / life));
		isoLine(t.a, t.b, thick, t.player ? 0xffffe060 : 0xffff3020, alpha);
	}
}

static const char* const isoWeaponNames[kWeapMax] = {
	"", "PITCHFORK", "FLARE GUN", "SHOTGUN", "TOMMY GUN", "NAPALM", "TNT: FUSE",
	"SPRAY CAN", "TESLA CANNON", "LIFE LEECH", "VOODOO DOLL", "TNT: PROXIMITY", "TNT: REMOTE", "BEAST",
};

bool isoGetLitExplosive(DBloodPlayer* pPlayer, double& fuseFrac, double& fuseSecs, bool& charging); // weapon.cpp

static void isoTag(double x, double y, const char* text, bool on, double sc)
{
	FFont* font = SmallFont;
	double fsc = 1.5 * sc;
	double w = font->StringWidth(text) * fsc + 8 * sc, h = font->GetHeight() * fsc + 4 * sc;
	Dim(twod, on ? 0xffc02020 : 0xff303030, on ? 0.9f : 0.6f, int(x), int(y), int(w), int(h));
	DrawText(twod, font, on ? CR_WHITE : CR_DARKGRAY, x + 4 * sc, y + 2 * sc, text,
		DTA_ScaleX, fsc, DTA_ScaleY, fsc, TAG_DONE);
}

static void isoBar(double x, double y, double w, double h, double frac, uint32_t color)
{
	Dim(twod, 0xff000000, 0.8f, int(x), int(y), int(w), int(h));
	int fw = int((w - 2) * std::clamp(frac, 0., 1.));
	if (fw > 0) Dim(twod, color, 1.f, int(x + 1), int(y + 1), fw, int(h - 2));
}
static const int isoWeaponTex[kWeapMax] = {
	-1, -1, kTexICONFLAREGUN, kTexICONSHOTGUN, kTexICONTOMMY, kTexICONNAPALM, kTexAmmoIcon5,
	kTexAmmoIcon6, kTexICONTESLA, kTexICONLEECH, kTexAmmoIcon9, kTexAmmoIcon10, kTexAmmoIcon11, -1,
};

static void isoDrawWeaponPanel(DBloodPlayer* pPlayer)
{
	int w = pPlayer->curWeapon;
	if (w <= 0 || w >= kWeapMax) return;
	// Real-pixel layout in the top-right of the 3D viewport, scaled by screen height.
	double sc = isoCam.vh / 480.;
	double right = isoCam.vx + isoCam.vw - 16 * sc;
	double top = isoCam.vy + 16 * sc;
	double boxW = 200 * sc, boxH = 96 * sc;
	Dim(twod, 0, 0.55f, int(right - boxW), int(top), int(boxW), int(boxH));

	if (isoWeaponTex[w] >= 0)
	{
		auto tex = TexMan.GetGameTexture(aTexIds[isoWeaponTex[w]]);
		if (tex && tex->isValid())
		{
			double th = 56 * sc;
			double tw = th * tex->GetDisplayWidth() / std::max(1., (double)tex->GetDisplayHeight());
			DrawTexture(twod, tex, right - boxW + 10 * sc, top + 8 * sc, DTA_DestWidthF, tw, DTA_DestHeightF, th,
				DTA_TopLeft, true, TAG_DONE);
		}
	}
	FFont* font = SmallFont;
	double fsc = 2 * sc;
	DrawText(twod, font, CR_GOLD, right - boxW + 10 * sc, top + boxH - 24 * sc, isoWeaponNames[w],
		DTA_ScaleX, fsc, DTA_ScaleY, fsc, TAG_DONE);

	if (pPlayer->weaponAmmo >= 0)
	{
		int num = pPlayer->ammoCount[pPlayer->weaponAmmo];
		if (pPlayer->weaponAmmo == 6) num /= 10;
		FString s; s.Format("%d", num);
		FFont* big = BigFont;
		double bsc = 1.5 * sc;
		double sw = big->StringWidth(s.GetChars()) * bsc;
		DrawText(twod, big, num > 0 ? CR_WHITE : CR_RED, right - 12 * sc - sw, top + 14 * sc, s.GetChars(),
			DTA_ScaleX, bsc, DTA_ScaleY, bsc, TAG_DONE);
	}

	// Mode tags under the panel: ALT lights while alt-fire is held, AKIMBO while the guns powerup is active.
	double ty = top + boxH + 4 * sc;
	double tx = right - boxW;
	isoTag(tx, ty, "ALT", (pPlayer->cmd.ucmd.actions & SB_ALTFIRE) != 0, sc);
	if (powerupCheck(pPlayer, kPwUpTwoGuns)) isoTag(tx + 46 * sc, ty, "AKIMBO", true, sc);

	// Lit explosive: fuse bar (green -> red) and throw-power bar while charging.
	double frac, secs; bool charging;
	if (isoGetLitExplosive(pPlayer, frac, secs, charging))
	{
		double by = ty + 22 * sc;
		uint32_t fuseColor = frac > 0.5 ? 0xff40c040 : frac > 0.25 ? 0xffe0c020 : 0xffe02020;
		FString label; label.Format("FUSE %.1fs", secs);
		DrawText(twod, SmallFont, CR_WHITE, tx, by, label.GetChars(), DTA_ScaleX, 1.5 * sc, DTA_ScaleY, 1.5 * sc, TAG_DONE);
		isoBar(tx, by + 14 * sc, boxW, 12 * sc, frac, fuseColor);
		if (charging)
		{
			DrawText(twod, SmallFont, CR_WHITE, tx, by + 30 * sc, "THROW", DTA_ScaleX, 1.5 * sc, DTA_ScaleY, 1.5 * sc, TAG_DONE);
			isoBar(tx, by + 44 * sc, boxW, 8 * sc, pPlayer->throwPower, 0xff40a0ff);
		}
	}
}

// Warning above the player when a lit explosive is about to go off in hand.
static void isoDrawFuseWarning(DBloodPlayer* pPlayer)
{
	double frac, secs; bool charging;
	if (!isoGetLitExplosive(pPlayer, frac, secs, charging) || secs > 1.5) return;
	auto actor = pPlayer->GetActor();
	double top, bottom;
	GetActorExtents(actor, &top, &bottom);
	auto p = isoProject(DVector3(actor->spr.pos.XY(), top - 8));
	double sc = isoCam.vh / 480.;
	bool blink = (PlayClock / 8) & 1;
	FFont* big = BigFont;
	double bsc = 1.2 * sc;
	const char* txt = "THROW IT!";
	double w = big->StringWidth(txt) * bsc;
	DrawText(twod, big, blink ? CR_RED : CR_WHITE, isoCam.vx + p.X - w / 2, isoCam.vy + p.Y - big->GetHeight() * bsc,
		txt, DTA_ScaleX, bsc, DTA_ScaleY, bsc, TAG_DONE);
}

// Death prompt: Blood restarts on the Open/Use button once the death animation has finished
// (single player reloads the last autosave), but never tells you. Isometric view only.
static void viewDrawDeathPrompt(DBloodPlayer* pPlayer)
{
	auto actor = pPlayer->GetActor();
	if (!actor || actor->xspr.health > 0 || pPlayer->deathTime < 120) return;
	double sc = screen->GetHeight() / 480.;
	double cx = screen->GetWidth() * 0.5, cy = screen->GetHeight() * 0.4;

	FFont* big = BigFont;
	const char* title = "YOU DIED";
	double bsc = 2 * sc;
	DrawText(twod, big, CR_RED, cx - big->StringWidth(title) * bsc / 2, cy, title, DTA_ScaleX, bsc, DTA_ScaleY, bsc, TAG_DONE);

	if (seqGetStatus(actor) >= 0) return; // death animation still playing; Use does nothing yet
	auto keys = Bindings.GetKeysForCommand("+open");
	FString keyName = keys.Size() ? C_NameKeys(keys.Data(), std::min<int>(keys.Size(), 2)) : FString("USE");
	FString line;
	if (gGameOptions.nGameType == 0 && numplayers == 1) line.Format("Press %s to restart from your last save", keyName.GetChars());
	else line.Format("Press %s to respawn", keyName.GetChars());
	FFont* sf = SmallFont;
	double ssc = 2 * sc;
	bool blink = (PlayClock / 30) & 1;
	DrawText(twod, sf, blink ? CR_WHITE : CR_GOLD, cx - sf->StringWidth(line.GetChars()) * ssc / 2, cy + big->GetHeight() * bsc + 12 * sc,
		line.GetChars(), DTA_ScaleX, ssc, DTA_ScaleY, ssc, TAG_DONE);
}


bool isoPlayerCanSee(DBloodPlayer* pPlayer, DBloodActor* actor); // animatesprite.cpp

static void isoBracket(double cx, double cy, double half, double thick, uint32_t color, uint8_t alpha)
{
	double l = half * 0.45;
	for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2)
	{
		DVector2 c(cx + sx * half, cy + sy * half);
		twod->AddThickLine(c, c - DVector2(sx * l, 0), thick, color, alpha);
		twod->AddThickLine(c, c - DVector2(0, sy * l), thick, color, alpha);
	}
}

// Off-screen enemies you can see (line of sight): a small copy of their sprite pinned to the
// screen edge with an arrow pointing at them. The current auto-aim target gets a yellow frame,
// and in long-range aim mode the on-screen target is bracketed too.
static void isoDrawEnemyMarkers(DBloodPlayer* pPlayer)
{
	auto pa = pPlayer->GetActor();
	double sc = isoCam.vh / 480.;
	double margin = 34 * sc;
	DVector2 o(isoCam.vx, isoCam.vy);
	DVector2 center(isoCam.vw * 0.5, isoCam.vh * 0.5);
	double range = std::max((double)r_iso_markrange, r_iso_longaim ? 3200. * (double)g_iso_longrange : 0.);
	auto target = pPlayer->aimTarget;

	BloodStatIterator it(kStatDude);
	while (auto actor = it.Next())
	{
		if (actor == pa || !actor->hasX() || actor->xspr.health <= 0) continue;
		if (!(actor->spr.flags & 8)) continue; // not shootable (e.g. still dormant)
		if (actor->spr.flags & 32) continue;
		double dist = (actor->spr.pos.XY() - pa->spr.pos.XY()).Length();
		if (dist > range) continue;
		if (r_iso_los && !isoPlayerCanSee(pPlayer, actor)) continue;

		double top, bottom;
		GetActorExtents(actor, &top, &bottom);
		auto p = isoProject(DVector3(actor->spr.pos.XY(), (top + bottom) * 0.5));
		bool isTarget = actor == target;
		bool onScreen = p.X >= 0 && p.X <= isoCam.vw && p.Y >= 0 && p.Y <= isoCam.vh;
		if (onScreen)
		{
			if (isTarget && (r_iso_longaim || gIsoLockActive))
			{
				double half = std::max(12. * sc, (bottom - top) * 0.5 * isoCam.vh / (2 * isoCam.halfH) + 6 * sc);
				isoBracket(o.X + p.X, o.Y + p.Y, half, std::max(1.5, 2 * sc), 0xffffe040, 230);
			}
			continue;
		}

		// Clamp the direction from screen center to an inset rectangle.
		DVector2 d = p - center;
		double hx = center.X - margin, hy = center.Y - margin;
		double t = std::min(hx / std::max(fabs(d.X), 1e-6), hy / std::max(fabs(d.Y), 1e-6));
		DVector2 e = center + d * t;
		DVector2 dir = d.Unit();

		// Fade with distance; the aim target is always solid.
		uint8_t alpha = isTarget ? 255 : uint8_t(255 * std::clamp(1.2 - dist / range, 0.35, 1.));
		double box = 26 * sc;
		Dim(twod, isTarget ? 0xff806010 : 0xff300808, alpha / 255.f * 0.7f, int(o.X + e.X - box), int(o.Y + e.Y - box), int(box * 2), int(box * 2));
		if (isTarget) isoBracket(o.X + e.X, o.Y + e.Y, box, std::max(1.5, 2 * sc), 0xffffe040, 255);

		auto tex = TexMan.GetGameTexture(actor->spr.spritetexture());
		if (tex && tex->isValid() && tex->GetDisplayHeight() > 0)
		{
			double th = box * 1.6;
			double tw = th * tex->GetDisplayWidth() / (double)tex->GetDisplayHeight();
			if (tw > box * 1.8) { th *= box * 1.8 / tw; tw = box * 1.8; }
			DrawTexture(twod, tex, o.X + e.X - tw / 2, o.Y + e.Y - th / 2, DTA_DestWidthF, tw, DTA_DestHeightF, th,
				DTA_TopLeft, true, DTA_Alpha, alpha / 255., DTA_TranslationIndex, TRANSLATION(Translation_Remap, actor->spr.pal), TAG_DONE);
		}

		// Arrow just outside the box, pointing toward the enemy.
		DVector2 tip = e + dir * (box + 14 * sc);
		DVector2 base = e + dir * (box + 3 * sc);
		DVector2 side(-dir.Y, dir.X);
		uint32_t ac = isTarget ? 0xffffe040 : 0xffff3020;
		double at = std::max(1.5, 2.5 * sc);
		twod->AddThickLine(o + tip, o + base + side * (8 * sc), at, ac, alpha);
		twod->AddThickLine(o + tip, o + base - side * (8 * sc), at, ac, alpha);
	}

	if (r_iso_longaim)
	{
		const char* txt = "LONG RANGE";
		double fsc = 1.5 * sc;
		DrawText(twod, SmallFont, CR_GOLD, o.X + 16 * sc, o.Y + isoCam.vh - 40 * sc, txt, DTA_ScaleX, fsc, DTA_ScaleY, fsc, TAG_DONE);
	}
}

// Fixed-camera aim cursor: world point under the cursor on the player's eye plane (Build XY).
// player.cpp turns the player toward it each tic.
DVector2 gIsoCursorWorld;
bool gIsoCursorValid;

static void isoUpdateAndDrawCursor()
{
	gIsoCursorValid = false;
	if (!isoCursorAimActive()) return;
	double ratio = isoCam.halfW / isoCam.halfH;
	gIsoCursor.X = std::clamp(gIsoCursor.X, -ratio, ratio);
	double sx = gIsoCursor.X * isoCam.halfH, sy = gIsoCursor.Y * isoCam.halfH;
	gIsoCursorWorld = isoCam.eye.XY() + isoCam.right * sx + isoCam.fwd * (sy / std::max(isoCam.sinP, 0.05));
	gIsoCursorValid = true;

	double px = isoCam.vx + isoCam.vw * 0.5 * (1. + sx / isoCam.halfW);
	double py = isoCam.vy + isoCam.vh * 0.5 * (1. - sy / isoCam.halfH);
	double sc = isoCam.vh / 480.;
	double r = 9 * sc, gap = 3 * sc, th = std::max(1.5, 2 * sc);
	for (int pass = 0; pass < 2; pass++) // dark outline, then bright cross
	{
		uint32_t c = pass ? 0xffffe040 : 0xff000000;
		double t = pass ? th : th + 2;
		twod->AddThickLine(DVector2(px - r, py), DVector2(px - gap, py), t, c, 230);
		twod->AddThickLine(DVector2(px + gap, py), DVector2(px + r, py), t, c, 230);
		twod->AddThickLine(DVector2(px, py - r), DVector2(px, py - gap), t, c, 230);
		twod->AddThickLine(DVector2(px, py + gap), DVector2(px, py + r), t, c, 230);
	}
}

static void isoDrawOverlay(DBloodPlayer* pPlayer)
{
	auto actor = pPlayer->GetActor();
	int health = actor->xspr.health >> 4;
	if (actor->xspr.health <= 0) return; // dead: no aim line/marker; the death prompt is drawn by the caller

	// Actual fire direction (after auto-aim), from the weapon origin.
	DVector3 aim = pPlayer->aim;
	double len2 = aim.XY().Length();
	if (len2 > 0) aim /= len2; else aim = DVector3(actor->spr.Angles.Yaw.ToVector(), 0);
	DVector3 muzzle(actor->spr.pos.XY(), pPlayer->zWeapon);

	if (r_iso_aimline)
	{
		double thick = std::max(1., isoCam.vh / 600.);
		isoLine(muzzle, muzzle + aim * 320., thick, 0xffffffff, 70);
	}
	if (r_iso_tracers) isoDrawTracers();

	auto mk = isoProject(muzzle + aim * (double)r_iso_aimdist);
	DrawIsoAimMarker(health, mk.X, mk.Y);

	if (r_iso_markers) isoDrawEnemyMarkers(pPlayer);
	isoUpdateAndDrawCursor();
	if (r_iso_weaponpanel) isoDrawWeaponPanel(pPlayer);
	isoDrawFuseWarning(pPlayer);

	// Muzzle flash: Blood puts its shoot-effect sprite just in front of the player, which from a
	// camera behind the player is hidden by the player's own sprite. Redraw it on top, additive.
	if (r_iso_muzzleflash && pPlayer->flashEffect)
	{
		auto tex = TexMan.GetGameTexture(aTexIds[kTexSHOOTEFFECT]);
		if (tex && tex->isValid())
		{
			POSTURE* pPosture = &pPlayer->pPosture[pPlayer->lifeMode][pPlayer->posture];
			DVector3 fp(actor->spr.pos.XY() + aim.XY() * (pPosture->xOffset + 12.), actor->spr.pos.Z - pPosture->zOffset);
			auto p = isoProject(fp);
			double size = 72 * isoCam.vh / 480. * (320. / isoCam.halfH); // shrink as you zoom out
			size = std::clamp(size, 16., 160.);
			double h = size * tex->GetDisplayHeight() / std::max(1., (double)tex->GetDisplayWidth());
			DrawTexture(twod, tex, isoCam.vx + p.X - size / 2, isoCam.vy + p.Y - h / 2, DTA_DestWidthF, size, DTA_DestHeightF, h,
				DTA_TopLeft, true, DTA_LegacyRenderStyle, STYLE_Add, TAG_DONE);
		}
	}
}

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewBackupView(DBloodPlayer* pPlayer)
{
	const auto pActor = pPlayer->GetActor();
	pPlayer->ozView = pPlayer->zView;
	pPlayer->ozWeapon = pPlayer->zWeapon - pPlayer->zView - 12;
	pPlayer->obobHeight = pPlayer->bobHeight;
	pPlayer->obobWidth = pPlayer->bobWidth;
	pPlayer->oswayHeight = pPlayer->swayHeight;
	pPlayer->oswayWidth = pPlayer->swayWidth;
	pActor->backuploc();
	pActor->interpolated = true;
}

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewDrawText(FFont* pFont, const char* pString, int x, int y, int nShade, int nPalette, int position, bool shadow)
{
	if (!pString) return;

	//y += pFont->yoff;

	if (position == 1) x -= pFont->StringWidth(pString) / 2;
	if (position == 2) x -= pFont->StringWidth(pString);


	if (shadow)
	{
		DrawText(twod, pFont, CR_UNTRANSLATED, x + 1, y + 1, pString, DTA_FullscreenScale, FSMode_Fit320x200, DTA_Color, 0xff000000, DTA_Alpha, 0.5, TAG_DONE);
	}
	DrawText(twod, pFont, CR_NATIVEPAL, x, y, pString, DTA_FullscreenScale, FSMode_Fit320x200, DTA_TranslationIndex, TRANSLATION(Translation_Remap, nPalette),
		DTA_Color, shadeToLight(nShade), TAG_DONE);

}

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewDrawAimedPlayerName(DBloodPlayer* pPlayer)
{
	if (!cl_idplayers || (pPlayer->aim.XY().isZero()))
		return;

	int hit = HitScan(pPlayer->GetActor(), pPlayer->zView, pPlayer->aim, CLIPMASK0, 512);
	if (hit == 3)
	{
		auto actor = gHitInfo.actor();
		if (actor && actor->IsPlayerActor())
		{
			int nPlayer = actor->spr.type - kDudePlayer1;
			const char* szName = PlayerName(nPlayer);
			int nPalette = (getPlayer(nPlayer)->teamId & 3) + 11;
			viewDrawText(DigiFont, szName, 160, 125, -128, nPalette, 1, 1);
		}
	}
}

extern DAngle random_angles[16][3];

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewInit(void)
{
	Printf("Initializing status bar\n");

	for (int i = 0; i < 16; i++)
	{
		random_angles[i][0] = RandomAngle();
		random_angles[i][1] = RandomAngle();
		random_angles[i][2] = RandomAngle();
	}
}

int othercameradist = 1280;
int othercameraclock;

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

#if 0
void CalcOtherPosition(DBloodActor* actor, int* pX, int* pY, int* pZ, sectortype** vsectnum, int nAng, fixed_t zm, int smoothratio) // currently unused
{
	int vX = MulScale(-Cos(nAng), 1280, 30);
	int vY = MulScale(-Sin(nAng), 1280, 30);
	int vZ = FixedToInt(MulScale(zm, 1280, 3)) - (16 << 8);
	int bakCstat = pSprite->cstat;
	pSprite->cstat &= ~CSTAT_SPRITE_BLOCK_HITSCAN;
	assert(validSectorIndex(*vsectnum));
	FindSector(*pX, *pY, *pZ, vsectnum);
	int nHSector;
	int hX, hY;
	vec3_t pos = { *pX, *pY, *pZ };
	hitscan(&pos, *vsectnum, vX, vY, vZ, &hitdata, CLIPMASK1);
	nHSector = hitdata.sect;
	hX = hitdata.pos.x;
	hY = hitdata.pos.y;
	int dX = hX - *pX;
	int dY = hY - *pY;
	if (abs(vX) + abs(vY) > abs(dX) + abs(dY))
	{
		*vsectnum = nHSector;
		dX -= Sgn(vX) << 6;
		dY -= Sgn(vY) << 6;
		int nDist;
		if (abs(vX) > abs(vY))
		{
			nDist = ClipHigh(DivScale(dX, vX, 16), othercameradist);
		}
		else
		{
			nDist = ClipHigh(DivScale(dY, vY, 16), othercameradist);
		}
		othercameradist = nDist;
	}
	*pX += MulScale(vX, othercameradist, 16);
	*pY += MulScale(vY, othercameradist, 16);
	*pZ += MulScale(vZ, othercameradist, 16);
	int myclock = PlayClock + MulScale(4, smoothratio, 16);
	othercameradist = ClipHigh(othercameradist + ((myclock - othercameraclock) << 10), 65536);
	othercameraclock = myclock;
	assert(validSectorIndex(*vsectnum));
	FindSector(*pX, *pY, *pZ, vsectnum);
	pSprite->cstat = bakCstat;
}
#endif

//---------------------------------------------------------------------------
//
// by NoOne: show warning msgs in game instead of throwing errors (in some cases)
//
//---------------------------------------------------------------------------

void viewSetSystemMessage(const char* pMessage, ...) {
	char buffer[1024]; va_list args; va_start(args, pMessage);
	vsprintf(buffer, pMessage, args);

	Printf(PRINT_HIGH | PRINT_NOTIFY, "%s\n", buffer); // print it also in console
}

void viewSetMessage(const char* pMessage, const char* color, const MESSAGE_PRIORITY priority)
{
	int printlevel = priority <= MESSAGE_PRIORITY_NORMAL ? PRINT_LOW : priority < MESSAGE_PRIORITY_SYSTEM ? PRINT_MEDIUM : PRINT_HIGH;
	Printf(printlevel | PRINT_NOTIFY, "%s%s\n", color? color : TEXTCOLOR_TAN, pMessage);
}

void viewSetErrorMessage(const char* pMessage)
{
	Printf(PRINT_BOLD | PRINT_NOTIFY, "%s\n", pMessage);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void DoLensEffect(void)
{
	// the lens effect depends on the software renderer and easy availability of the pixel data. 
	// If this ever gets redone for hardware rendering it needs very different handling on the GPU side.
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void UpdateDacs(int nPalette, bool bNoTint)
{
	gLastPal = 0;
	auto& tint = lookups.tables[MAXPALOOKUPS - 1];
	tint.tintFlags = 0;
	switch (nPalette)
	{
	case 0:
	default:
		tint.tintColor.r = 255;
		tint.tintColor.g = 255;
		tint.tintColor.b = 255;
		break;
	case 1:
		tint.tintColor.r = 132;
		tint.tintColor.g = 164;
		tint.tintColor.b = 255;
		break;
	case 2:
		tint.tintColor.r = 255;
		tint.tintColor.g = 126;
		tint.tintColor.b = 105;
		break;
	case 3:
		tint.tintColor.r = 162;
		tint.tintColor.g = 186;
		tint.tintColor.b = 15;
		break;
	case 4:
		tint.tintColor.r = 255;
		tint.tintColor.g = 255;
		tint.tintColor.b = 255;
		break;
	}
	videoSetPalette(nPalette);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void UpdateBlend(DBloodPlayer* pPlayer)
{
	int nRed = 0;
	int nGreen = 0;
	int nBlue = 0;

	nRed += pPlayer->pickupEffect;
	nGreen += pPlayer->pickupEffect;
	nBlue -= pPlayer->pickupEffect;

	nRed += ClipHigh(pPlayer->painEffect, 85) * 2;
	nGreen -= ClipHigh(pPlayer->painEffect, 85) * 3;
	nBlue -= ClipHigh(pPlayer->painEffect, 85) * 3;

	nRed -= pPlayer->blindEffect;
	nGreen -= pPlayer->blindEffect;
	nBlue -= pPlayer->blindEffect;

	nRed -= pPlayer->chokeEffect >> 6;
	nGreen -= pPlayer->chokeEffect >> 5;
	nBlue -= pPlayer->chokeEffect >> 6;

	nRed = ClipRange(nRed, -255, 255);
	nGreen = ClipRange(nGreen, -255, 255);
	nBlue = ClipRange(nBlue, -255, 255);

	videoTintBlood(nRed, nGreen, nBlue);
}

// int gVisibility;

int deliriumTilt, deliriumPitch;
int deliriumPitchO;
DAngle deliriumTurnO, deliriumTurn;
DAngle gScreenTiltO, gScreenTilt;

int gShowFrameRate = 1;

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void viewUpdateDelirium(DBloodPlayer* pPlayer)
{
	gScreenTiltO = gScreenTilt;
	deliriumTurnO = deliriumTurn;
	deliriumPitchO = deliriumPitch;
	int powerCount;
	if ((powerCount = powerupCheck(pPlayer, kPwUpDeliriumShroom)) != 0)
	{
		int tilt1 = 170, tilt2 = 170, pitch = 20;
		int timer = PlayClock * 2;
		if (powerCount < 512)
		{
			int powerScale = IntToFixed(powerCount) / 512;
			tilt1 = MulScale(tilt1, powerScale, 16);
			tilt2 = MulScale(tilt2, powerScale, 16);
			pitch = MulScale(pitch, powerScale, 16);
		}
		double sin2 = BobVal(2 * timer);
		double sin3 = BobVal(3 * timer);
		double sin4 = BobVal(4 * timer);
		double sin5 = BobVal(5 * timer);
		gScreenTilt = DAngle::fromBuild((sin2 + sin3) * tilt1 * 0.5);
		deliriumTurn = DAngle::fromBuild((sin3 + sin4) * tilt2 * 0.5);
		deliriumPitch = int((sin4 + sin5) * pitch * 0.5);
		return;
	}
	gScreenTilt = gScreenTilt.Normalized180();
	if (gScreenTilt > nullAngle)
	{
		gScreenTilt -= mapangle(8);
		if (gScreenTilt < nullAngle)
			gScreenTilt = nullAngle;
	}
	else if (gScreenTilt < nullAngle)
	{
		gScreenTilt += mapangle(8);
		if (gScreenTilt >= nullAngle)
			gScreenTilt = nullAngle;
	}
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void viewUpdateShake(DBloodPlayer* pPlayer, DVector3& cPos, DRotator& cAngles, double& pshakeX, double& pshakeY)
{
	auto doEffect = [&](const int& effectType)
	{
		if (effectType)
		{
			int nValue = ClipHigh(effectType * 8, 2000);
			cAngles.Pitch -= maphoriz(QRandom2F(nValue * (1. / 256.)));
			cAngles.Yaw += DAngle::fromDeg(QRandom2F(nValue * (360. / 524288.)));
			cPos.X += QRandom2F(nValue * maptoworld) * maptoworld;
			cPos.Y += QRandom2F(nValue * maptoworld) * maptoworld;
			cPos.Z += QRandom2F(nValue) * zmaptoworld;
			pshakeX += QRandom2F(nValue) * (1. / 256.);
			pshakeY += QRandom2F(nValue) * (1. / 256.);
		}
	};
	doEffect(pPlayer->flickerEffect);
	doEffect(pPlayer->quakeEffect);

	cAngles.Pitch -= DAngle::fromDeg((1 - BobVal((pPlayer->tiltEffect << 2) + 512)) * 13.2);
}


int gLastPal = 0;

int32_t g_frameRate;

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

static void DrawMap(DBloodPlayer* pPlayer, const double interpfrac)
{
	int tm = 0;
	if (viewport3d.Left() > 0)
	{
		setViewport(Hud_Stbar);
		tm = 1;
	}
	DrawOverheadMap(pPlayer->GetActor()->interpolatedpos(interpfrac).XY(), pPlayer->getRenderAngles(interpfrac).Yaw, interpfrac);
	if (tm)
		setViewport(hud_size);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

static void SetupView(DBloodPlayer* pPlayer, DVector3& cPos, DRotator& cAngles, sectortype*& pSector, double& zDelta, double& shakeX, double& shakeY, const double interpfrac)
{
	double bobWidth, bobHeight;

	pSector = pPlayer->GetActor()->sector();
#if 0
	if (numplayers > 1 && pPlayer == gMe && gPrediction && gMe->actor->xspr.health > 0)
	{
		nSectnum = predict.sectnum;
		cX = interpolatedvalue(predictOld.x, predict.x, interpfrac);
		cY = interpolatedvalue(predictOld.y, predict.y, interpfrac);
		cZ = interpolatedvalue(predictOld.viewz, predict.viewz, interpfrac);
		zDelta = interpolatedvalue(predictOld.weaponZ, predict.weaponZ, interpfrac);
		bobWidth = interpolatedvalue(predictOld.bobWidth, predict.bobWidth, interpfrac);
		bobHeight = interpolatedvalue(predictOld.bobHeight, predict.bobHeight, interpfrac);
		shakeX = interpolatedvalue(predictOld.shakeBobX, predict.shakeBobX, interpfrac);
		shakeY = interpolatedvalue(predictOld.shakeBobY, predict.shakeBobY, interpfrac);

		if (!SyncInput())
		{
			cAngles.Yaw = bamang(predict.angle.asbam() + predict.look_ang.asbam());
			cAngles.Pitch = predict.horiz + predict.horizoff;
			cAngles.Roll = predict.cAngles.Roll;
		}
		else
		{
			cAngles.Yaw = interpolatedvalue(predictOld.angle + predictOld.look_ang, predict.angle + predict.look_ang, interpfrac);
			cAngles.Pitch = interpolatedvalue(predictOld.horiz + predictOld.horizoff, predict.horiz + predict.horizoff, interpfrac);
			cAngles.Roll = interpolatedvalue(predictOld.rotscrnang, predict.rotscrnang, interpfrac);
		}
	}
	else
#endif
	{
		cPos = pPlayer->GetActor()->getRenderPos(interpfrac);
		cAngles = pPlayer->getRenderAngles(interpfrac);
		zDelta = interpolatedvalue(pPlayer->ozWeapon, pPlayer->zWeapon - pPlayer->zView - 12, interpfrac);
		bobWidth = interpolatedvalue(pPlayer->obobWidth, pPlayer->bobWidth, interpfrac);
		bobHeight = interpolatedvalue(pPlayer->obobHeight, pPlayer->bobHeight, interpfrac);
		shakeX = interpolatedvalue(pPlayer->oswayWidth, pPlayer->swayWidth, interpfrac);
		shakeY = interpolatedvalue(pPlayer->oswayHeight, pPlayer->swayHeight, interpfrac);
	}

	viewUpdateShake(pPlayer, cPos, cAngles, shakeX, shakeY);
	bmod::ModifyView(cPos, cAngles, interpfrac);

	if (r_isometric)
	{
		// isometric: keep the view origin at the player's eye; the renderer offsets the camera.
	}
	else if (gViewPos == 0)
	{
		if (cl_viewhbob)
		{
			cPos -= cAngles.Yaw.ToVector().Rotated90CW() * bobWidth;
		}
		if (cl_viewvbob)
		{
			cPos.Z += bobHeight;
		}
		cPos.Z -= 10. * (cAngles.Pitch / DAngle90);
	}
	else
	{
		calcChaseCamPos(cPos, pPlayer->GetActor(), &pSector, cAngles, interpfrac, 80.);
	}
	if (pSector != nullptr)
		CheckLink(cPos, &pSector);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void renderCrystalBall()
{
#if 0
	int tmp = (PlayClock / 240) % (gNetPlayers - 1);
	int i = connecthead;
	while (1)
	{
		if (i == gViewIndex)
			i = connectpoint2[i];
		if (tmp == 0)
			break;
		i = connectpoint2[i];
		tmp--;
	}
	PLAYER* pOther = &gPlayer[i];
	//othercameraclock = PlayClock + MulScale(4, (int)gInterpolate, 16);;
	//renderSetTarget(4079, 128, 128);
	renderSetAspect(65536, 78643);
	int vd8 = pOther->actor->spr.x;
	int vd4 = pOther->actor->spr.y;
	int vd0 = pOther->zView;
	int vcc = pOther->actor->spr.sectnum;
	int v50 = pOther->actor->spr.angle;
	int v54 = 0;
	if (pOther->flickerEffect)
	{
		int nValue = ClipHigh(pOther->flickerEffect * 8, 2000);
		v54 += QRandom2(nValue >> 8);
		v50 += QRandom2(nValue >> 8);
		vd8 += QRandom2(nValue >> 4);
		vd4 += QRandom2(nValue >> 4);
		vd0 += QRandom2(nValue);
	}
	if (pOther->quakeEffect)
	{
		int nValue = ClipHigh(pOther->quakeEffect * 8, 2000);
		v54 += QRandom2(nValue >> 8);
		v50 += QRandom2(nValue >> 8);
		vd8 += QRandom2(nValue >> 4);
		vd4 += QRandom2(nValue >> 4);
		vd0 += QRandom2(nValue);
	}
	CalcOtherPosition(pOther->actor, &vd8, &vd4, &vd0, &vcc, v50, 0, (int)gInterpolate);
	CheckLink(&vd8, &vd4, &vd0, &vcc);
	uint8_t v14 = 0;
	if (IsUnderwaterSector(vcc))
	{
		v14 = 10;
	}
	drawrooms(vd8, vd4, vd0, v50, v54, vcc);
	viewProcessSprites(vd8, vd4, vd0, v50, gInterpolate);
	renderDrawMasks();
#endif
}


//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void viewDrawScreen(bool sceneonly)
{
	DBloodPlayer* pPlayer = getPlayer(gViewIndex);

	FireProcess();

	double interpfrac;

	if (!paused && (!M_Active() || gGameOptions.nGameType != 0))
	{
		interpfrac = !cl_interpolate || cl_capfps ? 1. : I_GetTimeFrac() * 1.;
	}
	else interpfrac = 1.;

	// update render angles.
	pPlayer->updateCameraAngles(interpfrac);

	if (cl_interpolate)
	{
		DoInterpolations(interpfrac);
	}

	if (automapMode != am_full)
	{
		DoSectorLighting();
	}
	if (automapMode == am_off)
	{
		int basepal = 0;
		if (powerupCheck(pPlayer, kPwUpDeathMask) > 0) basepal = 4;
		else if (powerupCheck(pPlayer, kPwUpReflectShots) > 0) basepal = 1;
		else if (pPlayer->isUnderwater) {
			if (pPlayer->nWaterPal) basepal = pPlayer->nWaterPal;
			else {
				if (pPlayer->GetActor()->xspr.medium == kMediumWater) basepal = 1;
				else if (pPlayer->GetActor()->xspr.medium == kMediumGoo) basepal = 3;
				else basepal = 2;
			}
		}
		UpdateDacs(basepal);
		UpdateBlend(pPlayer);

		DVector3 cPos;
		DRotator cAngles;
		sectortype* pSector;
		double zDelta;
		double shakeX, shakeY;
		SetupView(pPlayer, cPos, cAngles, pSector, zDelta, shakeX, shakeY, interpfrac);

		DAngle tilt = interpolatedvalue(gScreenTiltO, gScreenTilt, interpfrac);
		bool bDelirium = powerupCheck(pPlayer, kPwUpDeliriumShroom) > 0;
		static bool bDeliriumOld = false;
		//int tiltcs, tiltdim;
		uint8_t otherview = powerupCheck(pPlayer, kPwUpCrystalBall) > 0;
		if (tilt.Degrees() || bDelirium)
		{
			cAngles.Roll = -tilt;
		}
		else if (otherview && gNetPlayers > 1)
		{
#if 0    
			renderCrystalBall();
#endif
		}
		else
		{
			othercameraclock = PlayClock + int(4 * interpfrac);
		}

		if (!bDelirium)
		{
			deliriumTilt = 0;
			deliriumTurn = nullAngle;
			deliriumPitch = 0;
		}
		int brightness = 0;

		BloodStatIterator it(kStatExplosion);
		while (auto actor = it.Next())
		{
			if (actor->hasX() && gotsector[actor->sectno()])
			{
				brightness += actor->xspr.data3 * 32;
			}
		}
		it.Reset(kStatProjectile);
		while (auto actor = it.Next())
		{
			switch (actor->spr.type) {
			case kMissileFlareRegular:
			case kMissileTeslaAlt:
			case kMissileFlareAlt:
			case kMissileTeslaRegular:
				if (gotsector[actor->sectno()]) brightness += 256;
				break;
			}
		}
		g_relvisibility = (int32_t)(ClipLow(gVisibility - 32 * pPlayer->visibility - brightness, 0)) - g_visibility;
		cAngles.Yaw += interpolatedvalue(deliriumTurnO, deliriumTurn, interpfrac);

		if (pSector != nullptr)
		{
			double ceilingZ, floorZ;
			calcSlope(pSector, cPos, &ceilingZ, &floorZ);
			if ((cPos.Z > floorZ - 1) && (pSector->upperLink == nullptr)) // clamp to floor
			{
				cPos.Z = floorZ - 1;
			}
			if ((cPos.Z < ceilingZ + 1) && (pSector->lowerLink == nullptr)) // clamp to ceiling
			{
				cPos.Z = ceilingZ + 1;
			}
		}

		if ((tilt.Degrees() || bDelirium) && !sceneonly)
		{
			if (gDeliriumBlur)
			{
				// todo: Set up a blurring postprocessing shader.
				//const float fBlur = pow(1.f/3.f, 30.f/g_frameRate);
				//g lAccum(GL _MULT, fBlur);
				//g lAccum(GL _ACCUM, 1.f-fBlur);
				//g lAccum(GL _RETURN, 1.f);
			}
		}

		if (!sceneonly) hudDraw(pPlayer, pSector, shakeX, shakeY, zDelta, cAngles.Roll, basepal, interpfrac);
		DAngle deliriumPitchI = interpolatedvalue(maphoriz(deliriumPitchO), maphoriz(deliriumPitch), interpfrac);
		auto bakCstat = pPlayer->GetActor()->spr.cstat;
		if (!r_isometric)
			pPlayer->GetActor()->spr.cstat |= (gViewPos == 0) ? CSTAT_SPRITE_INVISIBLE : CSTAT_SPRITE_TRANSLUCENT | CSTAT_SPRITE_TRANS_FLIP;
		cAngles.Pitch -= deliriumPitchI;
		render_drawrooms(pPlayer->GetActor(), cPos, pSector, cAngles, interpfrac);
		pPlayer->GetActor()->spr.cstat = bakCstat;
		bDeliriumOld = bDelirium && gDeliriumBlur;

		if (sceneonly) return;
		auto offsets = pPlayer->getCrosshairOffsets(interpfrac);
		if (!r_isometric)
			DrawCrosshair(pPlayer->GetActor()->xspr.health >> 4, offsets.first.X, offsets.first.Y, 2, offsets.second);
		else
		{
			isoSetupCam(cPos, cAngles.Yaw);
			isoDrawOverlay(pPlayer);
		}
		if (r_isometric) viewDrawDeathPrompt(pPlayer); // first person stays visually stock Blood
#if 0 // This currently does not work. May have to be redone as a hardware effect.
		if (v4 && gNetPlayers > 1)
		{
			DoLensEffect();
			r otatesprite(IntToFixed(280), IntToFixed(35), 53248, 512, 4077, v10, v14, 512 + 6, gViewX0, gViewY0, gViewX1, gViewY1);
			r otatesprite(IntToFixed(280), IntToFixed(35), 53248, 0, 1683, v10, 0, 512 + 35, gViewX0, gViewY0, gViewX1, gViewY1);
		}
#endif

#if 0
		int tmpSect = nSectnum;
		if ((vf0 & 0xc000) == 0x4000)
		{
			tmpSect = vf0 & (kMaxWalls - 1);
		}
		int v8 = byte_1CE5C2 > 0 && (sector[tmpSect].ceilingstat & CSTAT_SECTOR_SKY);
		if (gWeather.at12d8 > 0 || v8)
		{
			gWeather.Draw(cX, cY, cZ, cAngles.Yaw.Tan() * (1 << 23), cAngles.Pitch.Tan() * (1 << 23) + deliriumPitch, gWeather.at12d8);
			if (v8)
			{
				gWeather.at12d8 = ClipRange(delta * 8 + gWeather.at12d8, 0, 4095);
			}
			else
			{
				gWeather.at12d8 = ClipRange(gWeather.at12d8 - delta * 64, 0, 4095);
			}
		}
#endif
	}
	UpdateDacs(0, true);    // keep the view palette active only for the actual 3D view and its overlays.

	MarkSectorSeen(pPlayer->GetActor()->sector());

	if (automapMode != am_off)
	{
		DrawMap(pPlayer, interpfrac);
	}
	if (!bmod::mods.hideHud) UpdateStatusBar(pPlayer);
	bmod::OnDrawOverlay(pPlayer);

	viewDrawAimedPlayerName(pPlayer);
	if (paused)
	{
		auto text = GStrings.GetString("TXTB_PAUSED");
		viewDrawText(PickBigFont(text), text, 160, 10, 0, 0, 1, 0);
	}
	else if (pPlayer->pnum != myconnectindex)
	{
		FStringf gTempStr("] %s [", PlayerName(pPlayer->pnum));
		viewDrawText(OriginalSmallFont, gTempStr.GetChars(), 160, 10, 0, 0, 1, 0);
	}
	if (cl_interpolate)
	{
		RestoreInterpolations();
	}
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

bool GameInterface::GenerateSavePic()
{
	viewDrawScreen(true);
	return true;
}


//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

bool GameInterface::DrawAutomapPlayer(const DVector2& mxy, const DVector2& cpos, const DAngle cang, const DVector2& xydim, const double czoom, double const interpfrac)
{
	auto cangvect = cang.ToVector();

	for (int i = connecthead; i >= 0; i = connectpoint2[i])
	{
		if (i == gViewIndex || gGameOptions.nGameType == 1)
		{
			auto actor = getPlayer(i)->GetActor();
			auto vect = OutAutomapVector(mxy - cpos, cangvect, czoom, xydim);

			DrawTexture(twod, TexMan.GetGameTexture(actor->spr.spritetexture(), true), vect.X, vect.Y, DTA_ClipLeft, viewport3d.Left(), DTA_ClipTop, viewport3d.Top(), DTA_ScaleX, czoom * (2. / 3.), DTA_ScaleY, czoom * (2. / 3.), DTA_CenterOffset, true,
				DTA_ClipRight, viewport3d.Right(), DTA_ClipBottom, viewport3d.Bottom(), DTA_Alpha, (actor->spr.cstat & CSTAT_SPRITE_TRANSLUCENT ? 0.5 : 1.), DTA_TranslationIndex, TRANSLATION(Translation_Remap, actor->spr.pal), TAG_DONE);
		}
	}
	return true;
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void SerializeView(FSerializer& arc)
{
	if (arc.BeginObject("view"))
	{
		arc("screentilt", gScreenTilt)
			("deliriumtilt", deliriumTilt)
			("deliriumturn", deliriumTurn)
			("deliriumpitch", deliriumPitch)
			.EndObject();
	}
}


END_BLD_NS
