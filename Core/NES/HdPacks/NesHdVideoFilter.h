#pragma once
#include "pch.h"
#include "NES/NesDefaultVideoFilter.h"
#include "SNES/HdPacks/SnesHdPack.h"

class NesConsole;
class NesHdLayer;

//Draws the normal frame at the pack's scale (nearest neighbour), then the pack's art over the tiles and objects of the
//frame the HD PPU recorded
class NesHdVideoFilter : public NesDefaultVideoFilter
{
private:
	SnesHdPack* _pack;
	unique_ptr<NesHdLayer> _layer;
	SnesHdFrameData _frame;
	vector<uint32_t> _lowRes;

public:
	NesHdVideoFilter(NesConsole* console, Emulator* emu, SnesHdPack* pack);
	virtual ~NesHdVideoFilter();

	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
	FrameInfo GetFrameInfo() override;
};
