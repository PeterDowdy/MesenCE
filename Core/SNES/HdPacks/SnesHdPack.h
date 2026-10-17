#pragma once
#include "pch.h"

//Prototype HD pack for SNES sprites.
//
//The pack lives in <home>/HdPacks/<rom name>/hires.txt, next to its PNG files:
//  <ver>1
//  <scale>4
//  <img>sheet.png
//  <tile>imageIndex,tileData,x,y
//
//"tileData" is the sprite tile's 32 bytes of 4bpp CHR data as read from VRAM (64 hex digits, in byte order).
//"x,y" is the top-left corner of the tile's (8 * scale) square in the image. The art is the tile as stored in
//VRAM: flips are applied at render time. Pixels with alpha 0 show whatever the normal frame has underneath.
//The tile's palette is not part of the key, so a tile looks the same whatever palette the game draws it with.

struct SnesHdTile
{
	vector<uint32_t> Pixels; //ARGB, (8 * scale) x (8 * scale)
};

//What the PPU records for one pixel of a replaced sprite tile (0 = not a replaced sprite pixel)
//  bits 0-15: tile id (1-based index in SnesHdPack::Tiles)
//  bits 16-18: x in the tile as stored in VRAM, 19-21: y in the tile as stored in VRAM
//  bit 22: horizontal flip, bit 23: vertical flip, bits 24-25: sprite priority (0-3)
namespace SnesHdCell
{
	constexpr uint32_t Make(uint16_t tileId, uint8_t x, uint8_t y, bool flipH, bool flipV, uint8_t priority)
	{
		return tileId | (x << 16) | (y << 19) | ((uint32_t)flipH << 22) | ((uint32_t)flipV << 23) | ((uint32_t)priority << 24);
	}

	constexpr uint16_t TileId(uint32_t cell) { return cell & 0xFFFF; }
	constexpr uint8_t X(uint32_t cell) { return (cell >> 16) & 0x07; }
	constexpr uint8_t Y(uint32_t cell) { return (cell >> 19) & 0x07; }
	constexpr bool FlipH(uint32_t cell) { return (cell >> 22) & 0x01; }
	constexpr bool FlipV(uint32_t cell) { return (cell >> 23) & 0x01; }
	constexpr uint8_t Priority(uint32_t cell) { return (cell >> 24) & 0x03; }
}

//One frame of cells (256 wide, same rows as the PPU's frame buffer), handed to the video filter with the frame
struct SnesHdFrameData
{
	static constexpr uint32_t Width = 256;
	static constexpr uint32_t Height = 240;

	vector<uint32_t> Cells = vector<uint32_t>(Width * Height, 0);
};

class SnesHdPack
{
private:
	static constexpr uint32_t MaxScale = 8;

	bool LoadDefinition(const string& definitionFile);
	bool LoadTile(vector<string>& tokens, const string& folder, vector<vector<uint32_t>>& images, vector<std::pair<uint32_t, uint32_t>>& imageSizes);

public:
	uint32_t Scale = 1;
	vector<SnesHdTile> Tiles;
	unordered_map<uint64_t, uint16_t> TileIdByHash;

	//Returns null when there is no pack for this ROM (or it can't be loaded)
	static unique_ptr<SnesHdPack> Load(const string& romFileName);

	//Hash of a tile's 16 words of CHR data
	static uint64_t HashTile(const uint16_t* vram, uint16_t wordAddress);

	__forceinline uint16_t FindTile(const uint16_t* vram, uint16_t wordAddress) const
	{
		auto result = TileIdByHash.find(HashTile(vram, wordAddress));
		return result == TileIdByHash.end() ? 0 : result->second;
	}

	const SnesHdTile& GetTile(uint16_t tileId) const { return Tiles[tileId - 1]; }
};
