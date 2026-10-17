#pragma once
#include "pch.h"
#include "SNES/SnesDefaultVideoFilter.h"

class SnesHdPack;

//Draws the normal frame at the pack's scale (nearest neighbour), then the pack's art over the sprite tiles
//the PPU recorded for this frame. High resolution frames (512 wide) are left to the default filter.
class SnesHdVideoFilter : public SnesDefaultVideoFilter
{
private:
	SnesHdPack* _pack;

public:
	SnesHdVideoFilter(Emulator* emu, SnesHdPack* pack);

	void ApplyFilter(uint16_t* ppuOutputBuffer) override;

protected:
	FrameInfo GetFrameInfo() override;
};
