// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_scene.cpp
** manages the rendering of the player's view
**
*/

#include "gi.h"
#include "build.h"
#include "v_draw.h"
//#include "a_dynlight.h"
#include "v_video.h"
#include "m_png.h"
#include "i_time.h"
#include "inputstate.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"

#include "hw_renderstate.h"
#include "hw_lightbuffer.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "hw_clipper.h"
//#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"

#include "hw_drawstructs.h"
#include "hw_drawlist.h"
#include "hw_drawinfo.h"
#include "gamecvars.h"
#include "render.h"
#include "gamestruct.h"
#include "gamehud.h"
#include "c_dispatch.h"

EXTERN_CVAR(Bool, cl_capfps)

PalEntry GlobalMapFog;
float GlobalFogDensity = 350.f;
TArray<PortalDesc> allPortals;
void Draw2D(F2DDrawer* drawer, FRenderState& state);

CVARD(Bool, hw_hightile, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable hightile texture rendering")
bool hw_int_useindexedcolortextures;
CUSTOM_CVARD(Bool, hw_useindexedcolortextures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable indexed color texture rendering")
{
	if (screen) screen->SetTextureFilterMode();
}
CVARD(Bool, hw_models, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable model rendering")

// blood2d isometric experiment
CVARD(Bool, r_isometric, false, CVAR_ARCHIVE, "enable orthographic isometric camera")
CVARD(Bool, r_iso_longaim, false, 0, "isometric: long-range aim mode (zoomed-out camera, extended auto-aim range)")
CVARD(Float, r_iso_longzoom, 1.8f, CVAR_ARCHIVE, "isometric: zoom-out factor in long-range aim mode")
CVARD(Bool, r_iso_fixedcam, false, CVAR_ARCHIVE, "isometric: camera keeps a fixed world angle; movement keys become screen-relative")
CVARD(Bool, r_iso_spritestretch, true, CVAR_ARCHIVE, "isometric: stretch upright sprites by 1/cos(pitch) so they keep their drawn proportions")
CVARD(Float, r_iso_fixedyaw, 45.f, CVAR_ARCHIVE, "isometric: world yaw of the fixed camera, degrees")
float gIsoSizeEff = 320.f; // effective half-height this frame (includes long-aim zoom easing); the Blood overlay projects with it

EXTERN_CVAR(Float, r_iso_yaw)
CVARD(Bool, r_iso_cursoraim, true, CVAR_ARCHIVE, "isometric fixed camera: mouse moves an aim cursor and the player faces it")
CVARD(Float, r_iso_cursorspeed, 1.f, CVAR_ARCHIVE, "isometric: aim cursor mouse speed")
CVARD(Bool, r_iso_cursorinvert, false, CVAR_ARCHIVE, "isometric: invert aim cursor vertical movement")

// Fixed-camera yaw actually used this frame: eased toward r_iso_fixedyaw so 90-degree snaps turn smoothly.
static double isoFixedYawEased = 45.;
static bool isoFixedYawValid = false;
static void isoUpdateFixedYaw(double dt)
{
	if (!isoFixedYawValid) { isoFixedYawEased = r_iso_fixedyaw; isoFixedYawValid = true; return; }
	double d = remainder(r_iso_fixedyaw - isoFixedYawEased, 360.);
	isoFixedYawEased += (fabs(d) < 0.05) ? d : d * std::min(1., dt * 10.);
}

// Yaw (Build convention) the iso camera looks along, given the player's yaw.
DAngle isoCameraYaw(DAngle playerYaw)
{
	return (r_iso_fixedcam ? DAngle::fromDeg(isoFixedYawEased) : playerYaw) + DAngle::fromDeg(r_iso_yaw);
}

// Aim cursor, in units of the iso view's half-height relative to the screen center (+Y = up).
DVector2 gIsoCursor(0., 0.4);
bool gIsoLockActive = false; // set by the game while a target lock is held
bool isoCursorAimActive() { return r_isometric && r_iso_fixedcam && r_iso_cursoraim; }
// Mouse doesn't turn the player while the cursor or a target lock is steering.
bool isoMouseCaptured() { return r_isometric && ((r_iso_fixedcam && r_iso_cursoraim) || gIsoLockActive); }

CCMD(iso_rotleft)
{
	r_iso_fixedyaw = (float)remainder(r_iso_fixedyaw + 90., 360.);
}

CCMD(iso_rotright)
{
	r_iso_fixedyaw = (float)remainder(r_iso_fixedyaw - 90., 360.);
}

CCMD(iso_longaim)
{
	r_iso_longaim = !r_iso_longaim;
	Printf(PRINT_NOTIFY, "%s\n", r_iso_longaim ? "Long-range aim ON" : "Long-range aim OFF");
}

CCMD(iso_fixedcam)
{
	r_iso_fixedcam = !r_iso_fixedcam;
	Printf(PRINT_NOTIFY, "%s\n", r_iso_fixedcam ? "Fixed camera (screen-relative movement)" : "Follow camera");
}

CCMD(iso_toggle)
{
	r_isometric = !r_isometric;
	Printf(PRINT_NOTIFY, "%s\n", r_isometric ? "Isometric view" : "First-person view");
}
CVARD(Float, r_iso_pitch, 35.f, CVAR_ARCHIVE, "isometric camera downward pitch in degrees")
CVARD(Float, r_iso_size, 320.f, CVAR_ARCHIVE, "isometric half-height of visible area in world units (zoom)")
CVARD(Float, r_iso_dist, 2048.f, CVAR_ARCHIVE, "isometric camera distance from the player")
CVARD(Float, r_iso_yaw, 0.f, CVAR_ARCHIVE, "isometric camera yaw offset from player facing, degrees")
CVARD(Float, r_iso_cut, 16.f, CVAR_ARCHIVE, "isometric cutaway: hide geometry this far above the player eye (world units); negative disables")
CVARD(Int, r_iso_cutmode, 1, CVAR_ARCHIVE, "isometric cutaway: 0 = fixed r_iso_cut above eye, 1 = just under current sector ceiling (incl. sky sectors)")
CVARD(Float, r_iso_cutmargin, 2.f, CVAR_ARCHIVE, "isometric auto cutaway: distance below the ceiling to cut (world units)")
CVARD(Float, r_iso_lookpitch, 15.f, CVAR_ARCHIVE, "isometric: camera pitch while holding Look_Up")
CVARD(Float, r_iso_lookcut, 160.f, CVAR_ARCHIVE, "isometric: extra cutaway height while holding Look_Up (world units)")
CVARD(Float, r_iso_aimdist, 96.f, CVAR_ARCHIVE, "isometric aim marker distance ahead of the player (world units)")
CVARD(Int, r_iso_sky, 1, CVAR_ARCHIVE, "isometric sky: 0 = black, 1 = perspective backdrop")
CVARD(Float, r_iso_skypitch, -15.f, CVAR_ARCHIVE, "isometric sky backdrop pitch (negative looks up)")
CVARD(Float, r_iso_skyfov, 90.f, CVAR_ARCHIVE, "isometric sky backdrop horizontal field of view")

// Set while the main isometric view is being processed so sprite rotation frames are picked
// from the camera instead of the player position (render-space coordinates).
bool gIsoActive = false;
DVector3 gIsoCamRender;
DVector3 gIsoSpriteView; // render space: far point along the camera line, for sprite rotation frames
float gIsoLookAmt = 0.f;   // 0..1, eased while Look_Up is held
float gIsoPitchDeg = 35.f; // effective camera pitch this frame (the Blood overlay projects with it)



#if 0
void CollectLights(FLevelLocals* Level)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}
#endif


//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

void RenderViewpoint(FRenderViewpoint& mainvp, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	/*
	if (mainview && toscreen)
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level);
		});
		screen->UpdateShadowMap();
	}
	*/

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	const int eyeCount = vrmode->mEyeCount;
	screen->FirstEye();
	hw_int_useindexedcolortextures = eyeCount > 1? false : *hw_useindexedcolortextures;

	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		const auto& eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			bool useSSAO = (gl_ssao != 0);
			screen->SetSceneRenderTarget(useSSAO);
			RenderState.SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(nullptr, mainvp, nullptr);
		di->SetVisibility();
		auto& vp = di->Viewpoint;
		vp = mainvp;

		di->Set3DViewport(RenderState);
		float flash = 8.f / (r_scenebrightness + 8.f);
		di->Viewpoint.FieldOfView = FAngle::fromDeg(fov);	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		const bool iso = r_isometric && mainview && toscreen && eyeCount == 1;

		// Stereo mode specific perspective projection
		if (iso)
		{
			// Orthographic: r_iso_size is the half-height of the visible area in world units.
			// Depth range spans both sides of the camera so nothing gets near-clipped.
			static uint64_t zoomLastMs = 0;
			static float longAmt = 0.f;
			uint64_t zNow = I_msTime();
			double zdt = zoomLastMs ? std::min((zNow - zoomLastMs) / 1000., 0.1) : 0.;
			zoomLastMs = zNow;
			longAmt += float(((r_iso_longaim ? 1. : 0.) - longAmt) * std::min(1., zdt * 6.));
			float h = std::max((float)r_iso_size, 16.f) * (1.f + (std::max((float)r_iso_longzoom, 1.f) - 1.f) * longAmt);
			gIsoSizeEff = h;
			float w = h * ratio;
			float depth = 65536.f;
			VSMatrix m(1);
			m.ortho(-w, w, -h, h, -depth, depth);
			di->VPUniforms.mProjectionMatrix = m;
		}
		else
			di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio, false);

		// Stereo mode specific viewpoint adjustment
		if (eye.mShiftFactor != 0)
		{
			vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees());
			sectortype* sect = &sector[vp.SectCount];
			updatesector(DVector2(vp.Pos.X, -vp.Pos.Y), &sect);
			vp.SectCount = sectindex(sect);
		}

		if (iso)
		{
			// Visibility traversal (vp.Pos / vp.SectCount) stays at the player so the
			// Build sector walk works; only the view matrix comes from the camera
			// placed up and behind the player.
			// Hold Look_Up (PgUp by default): ease the camera flatter and lift the cutaway to see above the player.
			static uint64_t isoLastMs = 0;
			uint64_t nowMs = I_msTime();
			double dt = isoLastMs ? std::min((nowMs - isoLastMs) / 1000., 0.1) : 0.;
			isoLastMs = nowMs;
			double target = buttonMap.ButtonDown(gamefunc_Look_Up) ? 1. : 0.;
			gIsoLookAmt += float((target - gIsoLookAmt) * std::min(1., dt * 8.));
			gIsoPitchDeg = std::clamp((float)r_iso_pitch + ((float)r_iso_lookpitch - (float)r_iso_pitch) * gIsoLookAmt, 5.f, 89.f);
			vp.HWAngles.Pitch = FAngle::fromDeg(gIsoPitchDeg);
			vp.HWAngles.Roll = nullFAngle;
			isoUpdateFixedYaw(dt);
			if (r_iso_fixedcam) vp.HWAngles.Yaw = FAngle::fromDeg(float(isoFixedYawEased) - 90.f + r_iso_yaw); // HW yaw = build yaw - 90
			else vp.HWAngles.Yaw += FAngle::fromDeg(r_iso_yaw);
			{
				// Face sprites billboard along ViewVector, which was set from the player's yaw. Point it
				// along the camera instead, or sprites turn edge-on (invisible) when the camera doesn't
				// look where the player faces (fixed camera, r_iso_yaw).
				double FocalTangent = tan(vp.FieldOfView.Radians() / 2);
				DAngle an = DAngle::fromDeg(270. - vp.HWAngles.Yaw.Degrees());
				vp.TanSin = FocalTangent * an.Sin();
				vp.TanCos = FocalTangent * an.Cos();
				vp.ViewVector = an.ToVector();
			}
			// render space: X = build X, Y = -build Y, Z = height. HW yaw = build yaw - 90.
			double yaw = (vp.HWAngles.Yaw.Degrees() + 90.) * (pi::pi() / 180.);
			double pitch = vp.HWAngles.Pitch.Radians();
			double d = r_iso_dist;
			double horiz = d * cos(pitch);
			float cx = float(vp.Pos.X - cos(yaw) * horiz);
			float cy = float(vp.Pos.Y + sin(yaw) * horiz);
			float cz = float(vp.Pos.Z + d * sin(pitch));
			// Cutaway: clip geometry above a height so roofs and upper walls don't block the view.
			// Mode 1 (auto): just under the ceiling of the player's current sector; none under open sky.
			// Mode 0: fixed r_iso_cut above the eye. Eased so stepping between rooms doesn't pop.
			static double isoCutEased = 0; static bool isoCutValid = false;
			double cutTarget = vp.Pos.Z + r_iso_cut;
			bool cutOn = r_iso_cut >= 0;
			if (r_iso_cutmode == 1 && vp.SectCount >= 0 && vp.SectCount < (int)sector.Size())
			{
				auto sec = &sector[vp.SectCount];
				// Sky sectors get cut at their ceiling too: leaving the sky plane in place makes it
				// cover the floor when viewed from above.
				cutTarget = -getceilzofslopeptr(sec, vp.Pos.X, -vp.Pos.Y) - r_iso_cutmargin; // build Z is down
				cutOn = true;
			}
			if (!isoCutValid) isoCutEased = cutTarget;
			isoCutEased += (cutTarget - isoCutEased) * std::min(1., dt * 10.);
			isoCutValid = true;
			if (cutOn) di->SetClipHeight(float(isoCutEased + r_iso_lookcut * gIsoLookAmt), -1.f);
			di->SetupView(RenderState, cx, cy, cz, false, false);
			gIsoActive = true;
			gIsoCamRender = DVector3(cx, cy, cz);
			// Rotation frames: in ortho every sprite is seen from the same direction, so pick frames from
			// a point far back along the camera's line, not the camera's actual (finite) position.
			const double isoFar = 65536.;
			gIsoSpriteView = DVector3(vp.Pos.X - cos(yaw) * isoFar, vp.Pos.Y + sin(yaw) * isoFar, cz);
		}
		else
			di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);
		di->ProcessScene(toscreen);
		gIsoActive = false;

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(RenderState); // do not call this for camera textures.

			if (RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			screen->PostProcessScene(false, CM_DEFAULT, flash, []() { 
				hw_int_useindexedcolortextures = false;
				PostProcess.Unclock();
				Draw2D(&twodpsp, *screen->RenderState()); // draws the weapon sprites
				PostProcess.Clock();
				});
			PostProcess.Unclock();
		}
		di->EndDrawInfo();
		if (eyeCount - eye_ix > 1)
			screen->NextEye(eyeCount);
	}
	hw_int_useindexedcolortextures = false;
}

//===========================================================================
//
// Set up the view point.
//
//===========================================================================

FRenderViewpoint SetupViewpoint(DCoreActor* cam, const DVector3& position, int sectnum, const DRotator& angles, float fov = -1)
{
	FRenderViewpoint r_viewpoint{};
	r_viewpoint.CameraActor = cam;
	r_viewpoint.SectNums = nullptr;
	r_viewpoint.SectCount = sectnum;
	r_viewpoint.Pos = { position.X, -position.Y, -position.Z };
	r_viewpoint.HWAngles.Yaw = FAngle::fromDeg(-90. + angles.Yaw.Degrees());
	r_viewpoint.HWAngles.Pitch = FAngle::fromDeg(ClampViewPitch(angles.Pitch).Degrees());
	r_viewpoint.HWAngles.Roll = FAngle::fromDeg(angles.Roll.Degrees());
	r_viewpoint.FieldOfView = FAngle::fromDeg(fov > 0? fov :  r_fov);
	r_viewpoint.RotAngle = angles.Yaw.BAMs();
	double FocalTangent = tan(r_viewpoint.FieldOfView.Radians() / 2);
	DAngle an = DAngle::fromDeg(270. - r_viewpoint.HWAngles.Yaw.Degrees());
	r_viewpoint.TanSin = FocalTangent * an.Sin();
	r_viewpoint.TanCos = FocalTangent * an.Cos();
	r_viewpoint.ViewVector = an.ToVector();
	return r_viewpoint;
}


void DoWriteSavePic(FileWriter* file, uint8_t* scr, int width, int height, bool upsidedown)
{
	int pixelsize = 3;

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, nullptr, SS_RGB, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
// Currently a bit messy because the game side still needs to be able to
// handle Polymost.
//
//===========================================================================
bool writingsavepic;
FileWriter* savefile;
int savewidth, saveheight;

void WriteSavePic(FileWriter* file, int width, int height)
{
	writingsavepic = true;
	savefile = file;
	savewidth = width;
	saveheight = height;
	/*bool didit =*/ gi->GenerateSavePic();
	writingsavepic = false;
}

void RenderToSavePic(FRenderViewpoint& vp, FileWriter* file, int width, int height)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	auto& RenderState = *screen->RenderState();

	// we must be sure the GPU finished reading from the buffer before we fill it with new data.
	screen->WaitForCommands(false);

	// Switch to render buffers dimensioned for the savepic
	screen->SetSaveBuffers(true);
	screen->ImageTransitionScene(true);

	RenderState.SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	screen->mLights->Clear();
	screen->mViewpoints->Clear();

	twodpsp.Clear();

	RenderViewpoint(vp, &bounds, vp.FieldOfView.Degrees(), 1.333f, 1.333f, true, false);


	int numpixels = width * height;
	TArray<uint8_t> scr(numpixels * 3, true);
	screen->CopyScreenToBuffer(width, height, scr.Data());

	DoWriteSavePic(file, scr.Data(), width, height, screen->FlipSavePic());

	// Switch back the screen render buffers
	screen->SetViewportRects(nullptr);
	screen->SetSaveBuffers(false);
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime;
}


void animatecamsprite(double s);


void render_drawrooms(DCoreActor* playersprite, const DVector3& position, sectortype* sect, const DRotator& angles, double interpfrac, float fov)
{
	updatesector(position.XY(), &sect);
	if (sect == nullptr) return;

	iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;
	checkBenchActive();

	// reset statistics counters
	ResetProfilingData();

	// Get this before everything else
	FRenderViewpoint r_viewpoint = SetupViewpoint(playersprite, position, sectindex(sect), angles, fov);
	r_viewpoint.TicFrac = !cl_capfps ? interpfrac : 1.;

	screen->mLights->Clear();
	screen->mViewpoints->Clear();
	screen->mVertexData->Reset();

	if (writingsavepic) // hack alert! The save code should not go through render_drawrooms, but we can only clean up the game side when Polymost is gone for good.
	{
		RenderToSavePic(r_viewpoint, savefile, savewidth, saveheight);
		return;
	}

	// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
	auto RenderState = screen->RenderState();
	CheckTimer(*RenderState, 0/*ShaderStartTime*/);

	// prepare all camera textures that have been used in the last frame.
	gi->UpdateCameras(r_viewpoint.TicFrac);

	RenderState->SetVertexBuffer(screen->mVertexData);

	// now render the main view
	float fovratio;
	float ratio = ActiveRatio(screen->GetWidth(), screen->GetHeight());
	if (ratio >= 1.33f)
	{
		fovratio = 1.33f;
	}
	else
	{
		fovratio = ratio;
	}

	screen->ImageTransitionScene(true); // Only relevant for Vulkan.

	RenderViewpoint(r_viewpoint, nullptr, r_viewpoint.FieldOfView.Degrees(), ratio, fovratio, true, true);
	All.Unclock();
}

void render_camtex(DCoreActor* playersprite, const DVector3& position, sectortype* sect, const DRotator& angles, FGameTexture* camtex, IntRect& rect, double interpfrac)
{
	updatesector(position, &sect);
	if (!sect) return;

	screen->RenderState()->SetVertexBuffer(screen->mVertexData);

	// now render the main view
	float ratio = camtex->GetDisplayWidth() / camtex->GetDisplayHeight();

	FRenderViewpoint r_viewpoint = SetupViewpoint(playersprite, position, sectindex(sect), angles);
	r_viewpoint.TicFrac = !cl_capfps ? interpfrac : 1.;

	RenderViewpoint(r_viewpoint, &rect, r_viewpoint.FieldOfView.Degrees(), ratio, ratio, false, false);
	All.Unclock();
}

FSerializer& Serialize(FSerializer& arc, const char* key, PortalDesc& obj, PortalDesc* defval)
{
	if (arc.BeginObject(key))
	{
		arc("type", obj.type)
			("d", obj.delta)
			("targets", obj.targets)
			.EndObject();
	}
	return arc;
}
