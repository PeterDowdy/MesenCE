#pragma once
#include "pch.h"
#include "SNES/HdPacks/SnesHdPack.h"
#include "Shared/Video/BaseVideoFilter.h"

//Draws a frame's replaced tiles and objects over the nearest-neighbour upscale of the console's own frame. Shared by the
//SNES and NES video filters: they differ in how the recorded frame is made, in where the colors come from (BGR555 or the NES
//palette: "colorOf" turns a palette number a pack stores into ARGB) and in how a pixel's low resolution color is read
namespace HdCompose
{
	static __forceinline uint32_t BlendArt(uint32_t pixel, uint32_t art)
	{
		uint32_t alpha = art >> 24;
		if(alpha == 0xFF) {
			return art;
		} else if(alpha == 0) {
			return pixel;
		}

		uint32_t inverse = 0xFF - alpha;
		uint32_t r = (((art >> 16) & 0xFF) * alpha + ((pixel >> 16) & 0xFF) * inverse) / 0xFF;
		uint32_t g = (((art >> 8) & 0xFF) * alpha + ((pixel >> 8) & 0xFF) * inverse) / 0xFF;
		uint32_t b = ((art & 0xFF) * alpha + (pixel & 0xFF) * inverse) / 0xFF;
		return 0xFF000000 | (r << 16) | (g << 8) | b;
	}

	//Art made for the color "reference", drawn where the game shows "live" (a fade, a flash, a palette swap): scale it by
	//live / reference. Near black the ratio blows up, so the difference is added instead.
	static __forceinline int GradeChannel(int art, int live, int reference)
	{
		int result = reference >= 24 ? art * live / reference : art + live - reference;
		return result < 0 ? 0 : (result > 255 ? 255 : result);
	}

	static __forceinline uint32_t Grade(uint32_t art, uint32_t live, uint32_t reference)
	{
		uint32_t r = GradeChannel((art >> 16) & 0xFF, (live >> 16) & 0xFF, (reference >> 16) & 0xFF);
		uint32_t g = GradeChannel((art >> 8) & 0xFF, (live >> 8) & 0xFF, (reference >> 8) & 0xFF);
		uint32_t b = GradeChannel(art & 0xFF, live & 0xFF, reference & 0xFF);
		return (art & 0xFF000000) | (r << 16) | (g << 8) | b;
	}

	template<class ColorFn>
	void DrawObjects(const SnesHdPack& pack, SnesHdFrameData* frame, uint32_t* out, uint32_t outWidth, uint32_t outHeight, OverscanDimensions overscan, ColorFn colorOf)
	{
		int scale = (int)pack.Scale;
		int outW = (int)outWidth;
		int outH = (int)outHeight;

		//Back to front: the lower priority first, and at the same priority the higher OAM index (which is behind)
		vector<const SnesHdObjectInstance*> order;
		for(const SnesHdObjectInstance& instance : frame->Objects) {
			order.push_back(&instance);
		}
		std::sort(order.begin(), order.end(), [&](const SnesHdObjectInstance* a, const SnesHdObjectInstance* b) {
			uint8_t pa = frame->SpritePriority[a->SpritePriority];
			uint8_t pb = frame->SpritePriority[b->SpritePriority];
			return pa != pb ? pa < pb : a->OamIndex > b->OamIndex;
		});

		auto floorDiv = [](int value, int divisor) { return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor); };

		for(const SnesHdObjectInstance* instance : order) {
			const SnesHdObject& object = pack.Objects[instance->ObjectId];
			bool mirrorX = instance->Orientation & 1;
			bool mirrorY = instance->Orientation & 2;
			int boxWidth = object.Width * scale;
			int boxHeight = object.Height * scale;
			uint8_t priority = frame->SpritePriority[instance->SpritePriority];

			for(int ay = 0; ay < (int)object.ArtHeight; ay++) {
				int cy = object.OffsetY + ay;
				int hy = instance->Y * scale + (mirrorY ? boxHeight - 1 - cy : cy);
				int oy = hy - (int)overscan.Top * scale;
				if(hy < 0 || oy < 0 || oy >= outH) {
					continue;
				}
				int ly = hy / scale;
				if(ly >= (int)SnesHdFrameData::Height) {
					continue;
				}

				uint8_t brightness = frame->Brightness[ly];
				if(brightness == 0) {
					continue;
				}

				for(int ax = 0; ax < (int)object.ArtWidth; ax++) {
					uint32_t art = object.Pixels[ay * object.ArtWidth + ax];
					if((art >> 24) == 0) {
						continue;
					}

					int cx = object.OffsetX + ax;
					int hx = instance->X * scale + (mirrorX ? boxWidth - 1 - cx : cx);
					int ox = hx - (int)overscan.Left * scale;
					if(hx < 0 || ox < 0 || ox >= outW) {
						continue;
					}
					int lx = hx / scale;
					if(lx >= (int)SnesHdFrameData::Width) {
						continue;
					}

					//Is it in front of whatever the game drew at this pixel?
					uint16_t owner = frame->Owner[ly * SnesHdFrameData::Width + lx];
					if(owner == SnesHdOwner::Hidden || (owner & SnesHdOwner::Masked)) {
						continue;
					}
					bool visible;
					if(SnesHdOwner::IsSprite(owner)) {
						if(frame->SpritesByIndex) {
							visible = instance->OamIndex < SnesHdOwner::OamIndex(owner);
						} else {
							visible = priority > SnesHdOwner::Priority(owner) || (priority == SnesHdOwner::Priority(owner) && instance->OamIndex < SnesHdOwner::OamIndex(owner));
						}
					} else {
						visible = priority > SnesHdOwner::Priority(owner);
					}
					if(!visible) {
						continue;
					}

					//Grade by the original pixel nearest to this one: the color the game would draw it with now over the
					//color the art was made for
					int sx = std::clamp(floorDiv(cx, scale), 0, (int)object.Width - 1);
					int sy = std::clamp(floorDiv(cy, scale), 0, (int)object.Height - 1);
					uint16_t source = object.Source[sy * object.Width + sx];
					uint8_t entry = source >> 8;
					uint8_t colorIndex = source & 0xFF;
					if(colorIndex > 0 && colorIndex < 16 && entry < object.Entries.size() && object.Entries[entry].HasReference) {
						uint16_t liveColor = frame->SpritePalette[instance->Palettes[entry] * 16 + colorIndex];
						art = Grade(art, colorOf(liveColor), colorOf(object.Entries[entry].Reference[colorIndex]));
					}

					if(brightness < 15) {
						uint32_t r = ((art >> 16) & 0xFF) * brightness / 15;
						uint32_t g = ((art >> 8) & 0xFF) * brightness / 15;
						uint32_t b = (art & 0xFF) * brightness / 15;
						art = (art & 0xFF000000) | (r << 16) | (g << 8) | b;
					}

					uint32_t& pixel = out[oy * outW + ox];
					pixel = BlendArt(pixel, art);
				}
			}
		}
	}

	//"lowRes(row, x)" is the console's own color for the pixel at that frame buffer position (ARGB)
	template<class PixelFn, class ColorFn>
	void Apply(const SnesHdPack& pack, SnesHdFrameData* hdFrame, uint32_t* out, uint32_t outWidth, uint32_t outHeight, OverscanDimensions overscan, PixelFn lowRes, ColorFn colorOf)
	{
		uint32_t scale = pack.Scale;
		uint32_t tileSize = 8 * scale;

		//The art of the pixel's tile at this position in the (scale x scale) block of the pixel, graded by the palette
		//it was authored for against the color the game shows (the pixel it replaces)
		auto getArt = [&](uint32_t cell, uint32_t dx, uint32_t dy, uint32_t live) {
			const SnesHdTile& tile = pack.GetTile(SnesHdCell::TileId(cell));
			uint32_t hx = SnesHdCell::X(cell) * scale + (SnesHdCell::FlipH(cell) ? scale - 1 - dx : dx);
			uint32_t hy = SnesHdCell::Y(cell) * scale + (SnesHdCell::FlipV(cell) ? scale - 1 - dy : dy);
			uint32_t art = tile.Pixels[hy * tileSize + hx];
			if(tile.HasReference) {
				uint8_t index = tile.Index[SnesHdCell::Y(cell) * 8 + SnesHdCell::X(cell)];
				if(index > 0 && index < 16) {
					art = Grade(art, live, colorOf(tile.Reference[index]));
				}
			}
			return art;
		};

		for(uint32_t oy = 0; oy < outHeight; oy++) {
			uint32_t srcRow = oy / scale + overscan.Top;
			uint32_t dy = oy % scale;

			for(uint32_t ox = 0; ox < outWidth; ox++) {
				uint32_t srcX = ox / scale + overscan.Left;
				uint32_t pixel = lowRes(srcRow, srcX);

				if(hdFrame) {
					uint32_t dx = ox % scale;
					uint32_t index = srcRow * SnesHdFrameData::Width + srcX;
					uint32_t live = pixel;
					if(uint32_t under = hdFrame->Under[index]) {
						pixel = BlendArt(pixel, getArt(under, dx, dy, live));
					}
					if(uint32_t cell = hdFrame->Cells[index]) {
						pixel = BlendArt(pixel, getArt(cell, dx, dy, live));
					}
				}

				out[oy * outWidth + ox] = pixel;
			}
		}

		if(hdFrame && !hdFrame->Objects.empty()) {
			DrawObjects(pack, hdFrame, out, outWidth, outHeight, overscan, colorOf);
		}
	}
}
