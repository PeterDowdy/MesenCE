#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPack.h"

SnesHdVideoFilter::SnesHdVideoFilter(Emulator* emu, SnesHdPack* pack) : SnesDefaultVideoFilter(emu)
{
	_pack = pack;
}

FrameInfo SnesHdVideoFilter::GetFrameInfo()
{
	if(_baseFrameInfo.Width != 256) {
		return SnesDefaultVideoFilter::GetFrameInfo();
	}

	FrameInfo frame = BaseVideoFilter::GetFrameInfo();
	frame.Width *= _pack->Scale;
	frame.Height *= _pack->Scale;
	return frame;
}

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

void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	if(_baseFrameInfo.Width != 256) {
		SnesDefaultVideoFilter::ApplyFilter(ppuOutputBuffer);
		return;
	}

	//_frameData is null when there are no recorded cells, e.g. after loading a save state
	SnesHdFrameData* hdFrame = (SnesHdFrameData*)_frameData;

	uint32_t* out = GetOutputBuffer();
	OverscanDimensions overscan = GetOverscan();
	uint32_t scale = _pack->Scale;
	uint32_t tileSize = 8 * scale;

	//The art of the pixel's tile at this position in the (scale x scale) block of the pixel
	auto getArt = [&](uint32_t cell, uint32_t dx, uint32_t dy) {
		uint32_t hx = SnesHdCell::X(cell) * scale + (SnesHdCell::FlipH(cell) ? scale - 1 - dx : dx);
		uint32_t hy = SnesHdCell::Y(cell) * scale + (SnesHdCell::FlipV(cell) ? scale - 1 - dy : dy);
		return _pack->GetTile(SnesHdCell::TileId(cell)).Pixels[hy * tileSize + hx];
	};

	for(uint32_t oy = 0; oy < _frameInfo.Height; oy++) {
		uint32_t srcRow = oy / scale + overscan.Top;
		uint32_t dy = oy % scale;

		for(uint32_t ox = 0; ox < _frameInfo.Width; ox++) {
			uint32_t srcX = ox / scale + overscan.Left;
			uint32_t pixel = _calculatedPalette[ppuOutputBuffer[srcRow * 256 + srcX]];

			if(hdFrame) {
				uint32_t dx = ox % scale;
				uint32_t index = srcRow * SnesHdFrameData::Width + srcX;
				if(uint32_t under = hdFrame->Under[index]) {
					pixel = BlendArt(pixel, getArt(under, dx, dy));
				}
				if(uint32_t cell = hdFrame->Cells[index]) {
					pixel = BlendArt(pixel, getArt(cell, dx, dy));
				}
			}

			out[oy * _frameInfo.Width + ox] = pixel;
		}
	}
}
