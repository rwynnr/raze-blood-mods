// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2003-2016 Christoph Oelckers
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

#include "filesystem.h"
#include "hw_skydome.h"
#include "hw_portal.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"

#include "v_draw.h"

CVAR(Float, skyoffsettest, 0.f, 0)

// blood2d isometric experiment
extern bool gIsoActive;
EXTERN_CVAR(Int, r_iso_sky)
EXTERN_CVAR(Float, r_iso_skypitch)
EXTERN_CVAR(Float, r_iso_skyfov)

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------
void HWSkyPortal::DrawContents(HWDrawInfo *di, FRenderState &state)
{
	if (gIsoActive)
	{
		// The sky dome is built for a perspective camera at its center; under the
		// isometric ortho projection it smears. Draw it as a perspective backdrop
		// looking at the horizon (same yaw as the iso camera), only inside the
		// stencilled sky area. di is a per-portal copy, so this doesn't leak.
		if (r_iso_sky == 0) return; // leave sky areas black
		di->Viewpoint.HWAngles.Pitch = FAngle::fromDeg(std::clamp((float)r_iso_skypitch, -89.f, 89.f));
		di->Viewpoint.HWAngles.Roll = nullFAngle;
		di->VPUniforms.mClipHeightDirection = 0.f;    // no cutaway plane for the dome
		di->VPUniforms.mClipLine.X = -10000000.0f;
		float ratio = ActiveRatio(screen->GetWidth(), screen->GetHeight());
		float fovx = std::clamp((float)r_iso_skyfov, 30.f, 150.f);
		const double d2r = pi::pi() / 180.;
		float fovy = (float)(2 * atan(tan(fovx * d2r / 2) / ratio) / d2r);
		di->VPUniforms.mProjectionMatrix.loadIdentity();
		di->VPUniforms.mProjectionMatrix.perspective(fovy, ratio, 5.f, 65536.f);
	}

	int indexed = hw_int_useindexedcolortextures;
	hw_int_useindexedcolortextures = false; // this code does not work with indexed textures.

	state.SetNoSoftLightLevel();

	state.ResetColor();
	state.EnableFog(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	bool oldClamp = state.SetDepthClamp(true);
	auto color = shadeToLight(origin->shade);

	di->SetupView(state, 0, 0, 0, !!(mState->MirrorFlag & 1), !!(mState->PlaneMirrorFlag & 1));

	state.SetVertexBuffer(vertexBuffer);
	state.SetTextureMode(TM_OPAQUE);
	auto skybox = origin->texture ? dynamic_cast<FSkyBox*>(origin->texture->GetTexture()) : nullptr;
	if (skybox)
	{
		vertexBuffer->RenderBox(state, skybox, origin->x_offset, false, /*di->Level->info->pixelstretch*/1, { 0, 0, 1 }, { 0, 0, 1 }, color);
	}
	else if (!origin->cloudy)
	{
		auto tex = origin->texture;
		float texh = tex->GetDisplayHeight();
		auto& modelMatrix = state.mModelMatrix;
		auto& textureMatrix = state.mTextureMatrix;
		auto texskyoffset = tex->GetSkyOffset() + skyoffsettest;
		if (!(g_gameType & GAMEFLAG_PSEXHUMED)) texskyoffset += origin->y_offset;

		float repeat_fac = 1;
		if (texh <= 256)
		{
			repeat_fac = 336.f / texh;
			texh = 336;
		}
		modelMatrix.loadIdentity();
		modelMatrix.rotate(-180.0f + origin->x_offset, 0.f, 1.f, 0.f);
		modelMatrix.translate(0.f, -40 + texskyoffset * skyoffsetfactor, 0.f);
		modelMatrix.scale(1.f, texh / 400.f, 1.f);
		textureMatrix.loadIdentity();
		state.EnableTextureMatrix(true);
		textureMatrix.scale(1.f, repeat_fac, 1.f);
		vertexBuffer->DoRenderDome(state, origin->texture, FSkyVertexBuffer::SKYMODE_MAINLAYER, true, color);
		state.EnableTextureMatrix(false);
	}
	else
	{
		vertexBuffer->RenderDome(state, origin->texture, -origin->x_offset, origin->y_offset, false, FSkyVertexBuffer::SKYMODE_MAINLAYER, true, 0, 0, color);
	}
	state.SetTextureMode(TM_NORMAL);
	if (origin->fadecolor & 0xffffff)
	{
		PalEntry FadeColor = origin->fadecolor;
		state.EnableTexture(false);
		state.SetObjectColor(FadeColor);
		state.Draw(DT_Triangles, 0, 12);
		state.EnableTexture(true);
		state.SetObjectColor(0xffffffff);
	}

	//di->lightmode = oldlightmode;
	state.SetDepthClamp(oldClamp);
	hw_int_useindexedcolortextures = indexed;
 }

const char *HWSkyPortal::GetName() { return "Sky"; }
