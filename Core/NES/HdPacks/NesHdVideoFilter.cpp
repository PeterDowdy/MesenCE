#include "pch.h"
#include "NES/HdPacks/NesHdVideoFilter.h"
#include "NES/HdPacks/NesHdLayer.h"
#include "NES/NesConsole.h"
#include "NES/NesConstants.h"
#include "SNES/HdPacks/HdCompose.h"

NesHdVideoFilter::NesHdVideoFilter(NesConsole* console, Emulator* emu, SnesHdPack* pack) : NesDefaultVideoFilter(emu)
{
	_pack = pack;
	_layer.reset(new NesHdLayer(console->GetMapper(), pack));
}

NesHdVideoFilter::~NesHdVideoFilter()
{
}

FrameInfo NesHdVideoFilter::GetFrameInfo()
{
	FrameInfo frame = BaseVideoFilter::GetFrameInfo();
	frame.Width *= _pack->Scale;
	frame.Height *= _pack->Scale;
	return frame;
}

void NesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	//_frameData is null when there is nothing recorded, e.g. after loading a save state
	HdScreenInfo* info = (HdScreenInfo*)_frameData;
	SnesHdFrameData* hdFrame = nullptr;
	if(info) {
		_layer->BuildFrame(*info, _frame, _lowRes, _calculatedPalette);
		hdFrame = &_frame;
	}

	constexpr uint32_t Width = NesConstants::ScreenWidth;
	HdCompose::Apply(
		*_pack, hdFrame, GetOutputBuffer(), _frameInfo.Width, _frameInfo.Height, GetOverscan(),
		[&](uint32_t row, uint32_t x) {
			uint32_t over = hdFrame ? _lowRes[row * Width + x] : 0;
			return over ? over : _calculatedPalette[ppuOutputBuffer[row * Width + x]];
		},
		[&](uint16_t color) { return _calculatedPalette[color & 0x1FF]; });
}
