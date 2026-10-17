#pragma once
#include "pch.h"
#include <map>
#include <set>

//Prototype HD pack for SNES sprites and backgrounds. See docs/snes-hd.md in pixel-hd-studio for how it fits together.
//
//The pack lives in <home>/HdPacks/<rom name>/hires.txt, next to its PNG files:
//  <ver>1
//  <scale>4
//  <img>sheet.png
//  <tile>imageIndex,tileData,x,y[,palette]
//  <variant>imageIndex,tileData,x,y,palette
//  <object>name,artImage,artX,artY,artW,artH,offsetX,offsetY,indexImage,indexX,indexY
//  <entry>relX,relY,width,height,flipH,flipV,signature[,palette]       (one per OAM entry of the object)
//
//TILES. "tileData" is an 8x8 tile's CHR data as read from VRAM (hex, in byte order): 32 digits for a 2bpp tile,
//64 for a 4bpp tile (all sprites, and 4bpp backgrounds), 128 for an 8bpp one. "x,y" is the corner of the tile's
//(8 * scale) square in the image. The art is the tile as stored in VRAM: flips are applied at render time. Alpha 0
//shows the normal frame underneath. "palette" is the AUTHORING palette: the tile's colors 1 to N (BGR555, 4 hex digits
//each, N = 3, 15 or 255) that the art was made for. Where the game draws the tile with another palette (a fade, a
//flash, a palette swap) the art is graded by live color / authoring color of the pixel it replaces. Without a
//palette the art is drawn as it is. A <variant> is art for one exact live palette, used instead of the graded art
//(a palette swap that has art of its own).
//
//NES. A pack for a NES game (Kirby's Adventure) has "<console>nes" before anything else, and the same tags with
//the NES's formats: "tileData" is the tile's 16 bytes of CHR data (32 hex digits: the low plane's 8 rows, then the high
//plane's), and the palette is the 3 colors of the palette entries 1 to 3 as NES color numbers (00 to 3F, 2 hex digits
//each: 6 digits). An <entry>'s signature is a hash of the CHR bytes of its tile (8x8 sprites) or its two tiles (8x16, the
//even tile first). There is no Mode 7.
//
//MODE 7. A tile of Mode 7's plane (64 pixel colors, in row order) is written "m7:" and its 128 hex digits, and it has no
//palette: <tile>img,m7:<128 digits>,x,y. Its art is drawn where the plane shows that tile, as it is.
//
//OBJECTS. A game object is a group of OAM entries (a metasprite). The pack lists the entries a object is made of, where
//they sit in the object's box (relX, relY from its top left corner, in pixels), their size, flips, and "signature": a
//hash of the CHR data of the entry's tiles that the recorder writes (MESEN_SNES_HD_OBJECTS). When every entry is in
//OAM at those places (or all of them mirrored, horizontally and/or vertically) the object is replaced as a
//whole: its original pixels aren't drawn, and one image is drawn over the frame with the priority of its first
//entry. The image is (artW x artH) pixels at scale, drawn offsetX/offsetY from the box's corner (in scaled
//pixels, so art can go past the original outline). "indexImage" is a W x H image at the original resolution (R =
//which entry drew the pixel, G = its color index, 0 for none) used to grade the art by palette as for tiles.

struct SnesHdTile
{
	vector<uint32_t> Pixels; //ARGB, (8 * scale) x (8 * scale)
	bool HasReference = false;
	uint8_t Index[64] = {}; //Color index of each pixel of the original tile (0 = transparent)
	uint16_t Reference[16] = {}; //Authoring palette (BGR555), colors 1 and up
};

struct SnesHdEntrySpec
{
	int16_t X = 0;
	int16_t Y = 0;
	uint8_t Width = 8;
	uint8_t Height = 8;
	bool FlipH = false;
	bool FlipV = false;
	uint64_t Signature = 0;
	bool HasReference = false;
	uint16_t Reference[16] = {}; //Authoring palette of the entry (BGR555), colors 1 and up
};

struct SnesHdObject
{
	static constexpr uint32_t MaxEntries = 32;

	string Name;
	vector<SnesHdEntrySpec> Entries;
	uint16_t Width = 0; //The box the entries sit in, in original pixels
	uint16_t Height = 0;

	uint32_t ArtWidth = 0;
	uint32_t ArtHeight = 0;
	int32_t OffsetX = 0;
	int32_t OffsetY = 0;
	vector<uint32_t> Pixels; //ARGB

	//For every original pixel of the box: (entry << 8) | color index of the nearest pixel the original drew (0 = none)
	vector<uint16_t> Source;
};

//Where the matcher found a pack object in OAM
struct SnesHdObjectInstance
{
	uint16_t ObjectId = 0; //Index in SnesHdPack::Objects
	int16_t X = 0; //The object's box in the frame buffer's coordinates
	int16_t Y = 0;
	uint8_t Orientation = 0; //Bit 0: mirrored horizontally, bit 1: vertically
	uint8_t SpritePriority = 0; //0-3, of the first entry
	uint8_t OamIndex = 0; //The lowest OAM index among its entries
	uint8_t Palettes[SnesHdObject::MaxEntries] = {}; //The sprite palette (0-7) each entry is drawn with
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

//Who drew a pixel of the frame, so an object knows whether it's above or below it:
//  bits 0-3: the priority number the layer was drawn with (0 = backdrop), bit 4: a sprite, bits 5-11: its OAM index,
//  bit 12: the game hides sprites here (a window, the layer being off, a color window clipping to black)
namespace SnesHdOwner
{
	constexpr uint16_t Hidden = 0xFFFF; //Not part of the picture (the rows around a 224 line frame)
	constexpr uint16_t Sprite(uint8_t priority, uint8_t oamIndex) { return priority | 0x10 | (oamIndex << 5); }
	constexpr uint8_t Priority(uint16_t owner) { return owner & 0x0F; }
	constexpr bool IsSprite(uint16_t owner) { return owner & 0x10; }
	constexpr uint16_t Masked = 0x1000;
	constexpr uint8_t OamIndex(uint16_t owner) { return (owner >> 5) & 0x7F; }
}

//One frame of what the PPU recorded (256 wide, same rows as the PPU's frame buffer), handed to the video filter
//with the frame. "Under" holds the background tile beneath a sprite's transparent pixel, so the sprite's art can
//draw over the background's art.
struct SnesHdFrameData
{
	static constexpr uint32_t Width = 256;
	static constexpr uint32_t Height = 240;

	vector<uint32_t> Cells = vector<uint32_t>(Width * Height, 0);
	vector<uint32_t> Under = vector<uint32_t>(Width * Height, 0);
	vector<uint16_t> Owner = vector<uint16_t>(Width * Height, SnesHdOwner::Hidden);

	uint8_t Brightness[Height] = {}; //0-15 per row, 0 when blanked

	//Replaced objects, the sprite palettes (CGRAM colors 128 to 255) their art is graded against, and the
	//priority number of each sprite priority (0-3) in the BG mode of the frame
	vector<SnesHdObjectInstance> Objects;
	uint16_t SpritePalette[128] = {};
	uint8_t SpritePriority[4] = {};

	//NES: a sprite is above another by OAM index alone, whatever its priority
	bool SpritesByIndex = false;
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
	bool _hasMode7Tiles = false;
	bool _isNes = false;

	struct LoadState
	{
		vector<vector<uint32_t>> Images;
		vector<std::pair<uint32_t, uint32_t>> ImageSizes;
		SnesHdObject Current; //The object the <entry> tags that follow belong to
		uint32_t IndexImage = 0; //Where its index map is: it can't be read until its entries give the size of its box
		uint32_t IndexX = 0;
		uint32_t IndexY = 0;
	};

	bool LoadDefinition(const string& definitionFile);
	bool LoadTile(vector<string>& tokens, LoadState& state, bool variant);
	bool LoadNesTile(vector<string>& tokens, LoadState& state, bool variant);
	bool LoadObject(vector<string>& tokens, LoadState& state);
	bool LoadEntry(vector<string>& tokens, LoadState& state);
	bool FinishObject(LoadState& state);

public:
	uint32_t Scale = 1;
	vector<SnesHdTile> Tiles;
	vector<SnesHdObject> Objects;
	unordered_map<uint64_t, vector<uint16_t>> ObjectsByAnchor; //Signature of an object's first entry -> objects, the largest first

	//Returns null when there is no pack for this ROM (or it can't be loaded, or it's for the other console)
	static unique_ptr<SnesHdPack> Load(const string& romFileName, bool nes = false);

	bool IsNes() const { return _isNes; }

	//NES: hash of a tile's 16 CHR bytes, of the 3 colors of its palette, and of the signature of an 8x8 or 8x16 sprite's tiles
	static uint64_t HashNesTile(const uint8_t* chr);
	static uint64_t HashNesPalette(uint8_t color1, uint8_t color2, uint8_t color3);
	static string NesTileHex(const uint8_t* chr);

	//Id of the art for this tile (by HashNesTile) with this palette, or 0
	uint16_t FindNesTile(uint64_t tileHash, uint8_t color1, uint8_t color2, uint8_t color3) const
	{
		auto result = _entries.find(tileHash);
		if(result == _entries.end()) {
			return 0;
		}

		const TileEntry& entry = result->second;
		if(!entry.Variants.empty()) {
			uint64_t paletteHash = HashNesPalette(color1, color2, color3);
			for(auto& variant : entry.Variants) {
				if(variant.first == paletteHash) {
					return variant.second;
				}
			}
		}
		return entry.DefaultId;
	}

	//Hash of a tile's CHR data (4 words per bpp), and of the colors 1 to N of the palette that starts at paletteBase
	static uint64_t HashTile(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp);
	static uint64_t HashMode7Tile(const uint16_t* vram, uint8_t tile); //A tile of Mode 7's plane: the high bytes of its 64 words
	static string TileHex(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp); //The tile as the <tile> tag writes it
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

	bool HasMode7Tiles() const { return _hasMode7Tiles; }
	uint16_t FindMode7Tile(const uint16_t* vram, uint8_t tile) const
	{
		auto result = _entries.find(HashMode7Tile(vram, tile));
		return result == _entries.end() ? 0 : result->second.DefaultId;
	}
	bool HasObjects() const { return !Objects.empty(); }
};

//Writes every tile and palette the PPU fetches to a text file, as the tileData and palette fields of a <tile> tag
//(plus whether it was drawn as a sprite or a background): the tiles a pack needs art for. A line is
//  S|B <bpp> <tileData> <palette> <first frame> <fetches>
//where the fetches are how many times the PPU fetched that tile in that palette (a tile on screen for a frame is
//fetched once for each of its 8 rows in every scanline it covers), so tiles drawn for a moment can be told from the
//ones that stay. Enabled by setting the environment variable MESEN_SNES_HD_RECORD to the file's path.
class SnesHdRecorder
{
private:
	struct Row
	{
		string Text;
		uint32_t FirstFrame = 0;
		uint32_t Fetches = 0;
	};

	string _path;
	std::map<std::pair<uint64_t, uint64_t>, size_t> _index;
	vector<Row> _rows;
	bool _dirty = false;

public:
	static unique_ptr<SnesHdRecorder> Create(bool nes = false); //NES: MESEN_NES_HD_RECORD, and the tiles are 2bpp with 3 NES colors

	void Record(bool sprite, uint8_t bpp, const uint16_t* vram, uint16_t wordAddress, const uint16_t* cgram, uint16_t paletteBase, uint32_t frame);
	void RecordNes(bool sprite, const uint8_t* chr, const uint8_t* colors, uint32_t frame, uint32_t pixels); //NES: "pixels" is how many it drew
	void Flush();
};

//Writes the pictures the background layers show while a game is played (MESEN_SNES_HD_PLANES=<folder>), as the
//authoring unit for backgrounds: a studio restyles a picture as a whole, then cuts the art back into the tiles it is made
//of (the pack replaces tiles, so this never has to match a picture again at run time, which animation and streaming
//would break). For each distinct plane, a layer's whole tilemap area (or Mode 7's 1024x1024 plane):
//  planes.txt         ``P <key> <layer> <bpp> <width> <height> <first frame> <frames shown> <spread>`` per plane
//  plane_<key>.png    the picture at the original resolution (ARGB, alpha 0 where nothing is drawn), in the palette with
//                     the most spread between its darkest and brightest colors that it was shown in (a fade narrows it)
//  plane_<key>.json   its cells: ``{width, height, bpp, mode7, cols, rows, cgram, tiles: [tile data hex],
//                     cells: [[tile, flipH, flipV, palette base], ...]}``, one cell per 8x8 pixels in reading order. Tile
//                     data is as the pack's <tile> tag writes it (for Mode 7, the 64 pixel colors). cgram is the 256 colors
//                     (4 hex digits each) the picture was drawn with
//The key is a hash of the layer's tilemap and the graphics of the tiles it uses: it names a state of the picture, and a
//picture whose tiles animate has one plane per state.
class SnesHdPlaneRecorder
{
private:
	struct Plane
	{
		uint8_t Layer = 0;
		uint8_t Bpp = 0;
		uint16_t Width = 0;
		uint16_t Height = 0;
		uint32_t FirstFrame = 0;
		uint32_t Frames = 0;
		int Spread = -1;
	};

	string _folder;
	std::map<uint64_t, Plane> _planes;
	bool _dirty = false;

public:
	static unique_ptr<SnesHdPlaneRecorder> Create(bool nes = false); //NES: MESEN_NES_HD_PLANES (Core/NES/HdPacks/NesHdLayer.h)

	//Counts a frame the plane was shown. True when its files should be (re)written: it's new, or the palette it is shown
	//in now has clearly more spread than the one they were drawn in
	bool Seen(uint64_t key, uint8_t layer, uint8_t bpp, uint16_t width, uint16_t height, uint32_t frame, int spread);
	void Write(uint64_t key, const vector<uint32_t>& pixels, uint16_t width, uint16_t height, const string& cells);
	void Flush();
};
