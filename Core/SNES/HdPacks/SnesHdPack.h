#pragma once
#include "pch.h"
#include <set>

//Prototype HD pack for SNES sprites and backgrounds.
//
//The pack lives in <home>/HdPacks/<rom name>/hires.txt, next to its PNG files:
//  <ver>1
//  <scale>4
//  <img>sheet.png
//  <tile>imageIndex,tileData,x,y[,palette]
//
//"tileData" is an 8x8 tile's CHR data as read from VRAM (hex, in byte order): 32 digits for a 2bpp tile, 64 for a
//4bpp tile (all sprites, and 4bpp backgrounds), 128 for an 8bpp one.
//"x,y" is the top-left corner of the tile's (8 * scale) square in the image. The art is the tile as stored in VRAM:
//flips are applied at render time. Pixels with alpha 0 show whatever the normal frame has underneath.
//"palette" is optional: the tile's colors 1 to N (BGR555, 4 hex digits each, N = 3, 15 or 255) as they are in CGRAM.
//A tile with art for specific palettes uses the art whose palette matches, and otherwise the art without a palette.
//
//Sprites and backgrounds share the tile art: a tile with the same data looks the same wherever it is drawn.

struct SnesHdTile
{
	vector<uint32_t> Pixels; //ARGB, (8 * scale) x (8 * scale)
};

//What the PPU records for one pixel of a replaced tile (0 = not a replaced pixel)
//  bits 0-15: tile id (1-based index in SnesHdPack::Tiles)
//  bits 16-18: x in the tile as stored in VRAM, 19-21: y in the tile as stored in VRAM
//  bit 22: horizontal flip, bit 23: vertical flip, bits 24-25: sprite priority (0-3), bit 26: background tile
namespace SnesHdCell
{
	constexpr uint32_t Make(uint16_t tileId, uint8_t x, uint8_t y, bool flipH, bool flipV, uint8_t priority, bool background = false)
	{
		return tileId | (x << 16) | (y << 19) | ((uint32_t)flipH << 22) | ((uint32_t)flipV << 23) | ((uint32_t)priority << 24) | ((uint32_t)background << 26);
	}

	constexpr uint16_t TileId(uint32_t cell) { return cell & 0xFFFF; }
	constexpr uint8_t X(uint32_t cell) { return (cell >> 16) & 0x07; }
	constexpr uint8_t Y(uint32_t cell) { return (cell >> 19) & 0x07; }
	constexpr bool FlipH(uint32_t cell) { return (cell >> 22) & 0x01; }
	constexpr bool FlipV(uint32_t cell) { return (cell >> 23) & 0x01; }
	constexpr uint8_t Priority(uint32_t cell) { return (cell >> 24) & 0x03; }
	constexpr bool IsBackground(uint32_t cell) { return (cell >> 26) & 0x01; }
}

//One frame of cells (256 wide, same rows as the PPU's frame buffer), handed to the video filter with the frame.
//"Under" holds the background tile beneath a sprite's transparent pixel, so the sprite's art can draw over the
//background's art.
struct SnesHdFrameData
{
	static constexpr uint32_t Width = 256;
	static constexpr uint32_t Height = 240;

	vector<uint32_t> Cells = vector<uint32_t>(Width * Height, 0);
	vector<uint32_t> Under = vector<uint32_t>(Width * Height, 0);
};

class SnesHdPack
{
private:
	static constexpr uint32_t MaxScale = 8;

	struct TileEntry
	{
		uint16_t DefaultId = 0;
		vector<std::pair<uint64_t, uint16_t>> Variants; //palette hash, tile id
	};

	unordered_map<uint64_t, TileEntry> _entries;

	bool LoadDefinition(const string& definitionFile);
	bool LoadTile(vector<string>& tokens, vector<vector<uint32_t>>& images, vector<std::pair<uint32_t, uint32_t>>& imageSizes);

public:
	uint32_t Scale = 1;
	vector<SnesHdTile> Tiles;

	//Returns null when there is no pack for this ROM (or it can't be loaded)
	static unique_ptr<SnesHdPack> Load(const string& romFileName);

	//Hash of a tile's CHR data (4 words per bpp), and of the colors 1 to N of the palette that starts at paletteBase
	static uint64_t HashTile(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp);
	static uint64_t HashPalette(const uint16_t* cgram, uint16_t paletteBase, uint8_t bpp);

	//Id of the art for this tile with this palette, or 0
	uint16_t FindTile(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp, const uint16_t* cgram, uint16_t paletteBase) const
	{
		auto result = _entries.find(HashTile(vram, wordAddress, bpp));
		if(result == _entries.end()) {
			return 0;
		}

		const TileEntry& entry = result->second;
		if(!entry.Variants.empty()) {
			uint64_t paletteHash = HashPalette(cgram, paletteBase, bpp);
			for(auto& variant : entry.Variants) {
				if(variant.first == paletteHash) {
					return variant.second;
				}
			}
		}
		return entry.DefaultId;
	}

	const SnesHdTile& GetTile(uint16_t tileId) const { return Tiles[tileId - 1]; }
};

//Writes every tile and palette the PPU fetches to a text file, as the tileData and palette fields of a <tile> tag
//(plus whether it was drawn as a sprite or a background): the tiles a pack needs art for. Enabled by setting the
//environment variable MESEN_SNES_HD_RECORD to the file's path.
class SnesHdRecorder
{
private:
	string _path;
	std::set<std::pair<uint64_t, uint64_t>> _seen;
	vector<string> _lines;
	bool _dirty = false;

public:
	static unique_ptr<SnesHdRecorder> Create();

	void Record(bool sprite, uint8_t bpp, const uint16_t* vram, uint16_t wordAddress, const uint16_t* cgram, uint16_t paletteBase);
	void Flush();
};
