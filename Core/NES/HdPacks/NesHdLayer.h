#pragma once
#include "pch.h"
#include "NES/HdPacks/HdData.h"
#include "SNES/HdPacks/SnesHdPack.h"
#include "SNES/HdPacks/SnesHdObjects.h"

class BaseMapper;

//The HD layer for NES games (see SnesHdPack.h for the pack and docs/snes-hd.md in pixel-hd-studio for the approach). It works
//on what the HD PPU records of a frame (HdScreenInfo), so it needs no hooks in the rendering itself, and reuses the SNES layer's
//pack, object matcher, recorders and compositor. Tiles are found by their CHR data, objects (an OAM entry's 8x8 or 8x16 tiles
//together make a signature) by the same matcher as SNES. Objects need CHR ROM; with CHR RAM only tiles work.
class NesHdLayer
{
private:
	BaseMapper* _mapper;
	const SnesHdPack* _pack;
	bool _chrRom;
	vector<uint64_t> _tileHash; //By CHR ROM tile number; 0 = not computed yet
	unordered_map<uint64_t, uint16_t> _found; //(tile number, palette) -> art id, for CHR ROM tiles

	uint16_t FindArt(const HdPpuTileInfo& tile);
	uint32_t CellFor(const HdPpuTileInfo& tile, bool sprite);

public:
	NesHdLayer(BaseMapper* mapper, const SnesHdPack* pack);

	bool IsChrRom() const { return _chrRom; }
	uint64_t ChrHash(int32_t tileNumber);
	void ReadChr(int32_t tileNumber, uint8_t* chr);

	//The sprite that shows at this pixel (an index into pixel.Sprite), or -1. "suppress" holds the OAM entries that don't
	//count because an object replaces them (can be null)
	static int SpriteAt(const HdScreenInfo& info, const HdPpuPixelInfo& pixel, uint32_t x, uint32_t y, const bool* suppress);

	//The OAM entries as the object matcher and recorder want them
	void BuildEntries(const HdScreenInfo& info, vector<SnesHdOamEntry>& entries);

	//What the console shows at a pixel without its sprites: NES color number
	static uint8_t BackdropColor(const HdPpuPixelInfo& pixel, bool* opaque);

	//Fills "frame" and "lowRes" (ARGB where the pixel differs from the PPU's own, 0 elsewhere) from the recorded frame
	void BuildFrame(HdScreenInfo& info, SnesHdFrameData& frame, vector<uint32_t>& lowRes, const uint32_t* palette);
};

//The pictures the background shows (MESEN_NES_HD_PLANES=<folder>), as SnesHdPlaneRecorder writes them for the SNES (planes.txt,
//plane_<key>.png and .json with the cells of the nametables, the 512x480 area or the part of it that isn't a mirror). Cells
//are [tile slot, 0, 0, palette 0-3] and "palette" is the 32 bytes of palette RAM (2 hex digits each); "bpp" is 2. A state that
//is shown for fewer than 10 frames in a row (the background streaming in as the screen scrolls) isn't recorded.
class NesHdPlaneRecorder
{
private:
	unique_ptr<SnesHdPlaneRecorder> _recorder;
	BaseMapper* _mapper;
	uint32_t _palette[512] = {};
	uint64_t _lastKey = 0;
	uint32_t _stable = 0;

public:
	static bool Requested();

	NesHdPlaneRecorder(BaseMapper* mapper, NesConfig& config);

	void Record(const HdScreenInfo& info);
	void Flush();
};

//MESEN_NES_HD_RECORD=<file> (tiles, as SnesHdRecorder writes them for the SNES) and MESEN_NES_HD_OBJECTS=<folder> (as
//SnesHdObjectRecorder), taken from the frames the HD PPU records
class NesHdRecorders
{
private:
	unique_ptr<SnesHdRecorder> _tiles;
	unique_ptr<SnesHdObjectRecorder> _objects;
	unique_ptr<NesHdPlaneRecorder> _planes;
	NesHdLayer* _layer = nullptr;
	uint32_t _palette[512] = {};
	uint8_t _currentPalette[16] = {}; //The sprite palettes of the frame being recorded

public:
	static bool Requested();

	NesHdRecorders(NesHdLayer* layer, BaseMapper* mapper, NesConfig& config);
	~NesHdRecorders();

	void Record(HdScreenInfo& info, const uint16_t* ppuBuffer);
	void Flush();
};
