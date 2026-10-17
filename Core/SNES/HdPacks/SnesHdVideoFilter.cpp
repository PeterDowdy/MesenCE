#include "pch.h"
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPack.h"
#include "SNES/HdPacks/HdCompose.h"

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

void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	if(_baseFrameInfo.Width != 256) {
		SnesDefaultVideoFilter::ApplyFilter(ppuOutputBuffer);
		return;
	}

	//_frameData is null when there are no recorded cells, e.g. after loading a save state
	SnesHdFrameData* hdFrame = (SnesHdFrameData*)_frameData;
	HdCompose::Apply(
		*_pack, hdFrame, GetOutputBuffer(), _frameInfo.Width, _frameInfo.Height, GetOverscan(),
		[&](uint32_t row, uint32_t x) { return _calculatedPalette[ppuOutputBuffer[row * 256 + x]]; },
		[&](uint16_t color) { return _calculatedPalette[color & 0x7FFF]; });
}
