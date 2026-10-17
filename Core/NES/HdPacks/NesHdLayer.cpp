#include "pch.h"
#include "NES/HdPacks/NesHdLayer.h"
#include "NES/BaseMapper.h"
#include "NES/NesConstants.h"
#include "NES/NesDefaultVideoFilter.h"

NesHdLayer::NesHdLayer(BaseMapper* mapper, const SnesHdPack* pack)
{
	_mapper = mapper;
	_pack = pack;
	_chrRom = mapper->HasChrRom();
	if(_chrRom) {
		_tileHash.assign(mapper->GetChrRomSize() / 16 + 1, 0);
	}
}

void NesHdLayer::ReadChr(int32_t tileNumber, uint8_t* chr)
{
	_mapper->CopyChrTile((uint32_t)tileNumber * 16, chr);
}

uint64_t NesHdLayer::ChrHash(int32_t tileNumber)
{
	if(tileNumber < 0 || (size_t)tileNumber >= _tileHash.size()) {
		return 0;
	}

	uint64_t& hash = _tileHash[tileNumber];
	if(hash == 0) {
		uint8_t chr[16];
		ReadChr(tileNumber, chr);
		hash = SnesHdPack::HashNesTile(chr) | 1; //Never 0
	}
	return hash;
}

static void PaletteOf(const HdPpuTileInfo& tile, uint8_t& c1, uint8_t& c2, uint8_t& c3)
{
	c1 = (tile.PaletteColors >> 16) & 0x3F;
	c2 = (tile.PaletteColors >> 8) & 0x3F;
	c3 = tile.PaletteColors & 0x3F;
}

uint16_t NesHdLayer::FindArt(const HdPpuTileInfo& tile)
{
	uint8_t c1, c2, c3;
	PaletteOf(tile, c1, c2, c3);

	if(!_chrRom) {
		return _pack->FindNesTile(SnesHdPack::HashNesTile(tile.TileData) | 1, c1, c2, c3);
	}

	uint64_t key = ((uint64_t)(uint32_t)tile.TileIndex << 24) | (c1 << 16) | (c2 << 8) | c3;
	auto cached = _found.find(key);
	if(cached != _found.end()) {
		return cached->second;
	}

	uint16_t id = _pack->FindNesTile(ChrHash(tile.TileIndex), c1, c2, c3);
	_found[key] = id;
	return id;
}

uint32_t NesHdLayer::CellFor(const HdPpuTileInfo& tile, bool sprite)
{
	uint16_t id = FindArt(tile);
	if(id == 0) {
		return 0;
	}

	bool flipH = sprite && tile.HorizontalMirroring;
	bool flipV = sprite && tile.VerticalMirroring;
	uint8_t x = flipH ? 7 - tile.OffsetX : tile.OffsetX;
	return SnesHdCell::Make(id, x & 7, tile.OffsetY & 7, flipH, flipV, 0, !sprite);
}

int NesHdLayer::SpriteAt(const HdScreenInfo& info, const HdPpuPixelInfo& pixel, uint32_t x, uint32_t y, const bool* suppress)
{
	if(!info.SpritesRow[y] || (info.SpriteLeftMask && x < 8)) {
		return -1;
	}

	bool opaque = pixel.Tile.TileIndex != HdPpuTileInfo::NoTile && pixel.Tile.BgColorIndex != 0;
	for(int j = 0; j < pixel.SpriteCount; j++) {
		const HdPpuTileInfo& sprite = pixel.Sprite[j];
		if(sprite.SpriteColorIndex == 0 || (suppress && suppress[sprite.OamIndex])) {
			continue;
		}
		//The first sprite with a color is the one that counts, and it hides behind an opaque background if it says so
		return (sprite.BackgroundPriority && opaque) ? -1 : j;
	}
	return -1;
}

uint8_t NesHdLayer::BackdropColor(const HdPpuPixelInfo& pixel, bool* opaque)
{
	*opaque = pixel.Tile.TileIndex != HdPpuTileInfo::NoTile && pixel.Tile.BgColorIndex != 0;
	return *opaque ? pixel.Tile.BgColor : pixel.Tile.PpuBackgroundColor;
}

void NesHdLayer::BuildEntries(const HdScreenInfo& info, vector<SnesHdOamEntry>& entries)
{
	entries.clear();
	if(!_chrRom || !info.SpritesEnabled) {
		return;
	}

	uint8_t height = info.LargeSprites ? 16 : 8;
	for(int i = 0; i < 64; i++) {
		uint8_t y = info.Oam[i * 4];
		uint8_t attributes = info.Oam[i * 4 + 2];
		uint8_t x = info.Oam[i * 4 + 3];
		if(y >= 0xEF) {
			continue; //Parked off the bottom of the screen
		}

		int32_t t0 = info.SpriteTile[i][0];
		int32_t t1 = info.SpriteTile[i][1];
		uint64_t signature;
		if(height == 16) {
			//The tiles of an 8x16 sprite are next to each other: one that never got drawn (off the screen) is known from the other
			if(t0 < 0 && t1 < 0) {
				continue;
			}
			if(t0 < 0) {
				t0 = t1 - 1;
			} else if(t1 < 0) {
				t1 = t0 + 1;
			}
			uint64_t a = ChrHash(t0);
			uint64_t b = ChrHash(t1);
			if(a == 0 || b == 0) {
				continue;
			}
			signature = a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
		} else {
			if(t0 < 0) {
				continue;
			}
			signature = ChrHash(t0);
			if(signature == 0) {
				continue;
			}
		}

		SnesHdOamEntry entry;
		entry.Index = (uint8_t)i;
		entry.X = x;
		entry.Y = (int16_t)(y + 1);
		entry.Width = 8;
		entry.Height = height;
		entry.Palette = attributes & 0x03;
		entry.Priority = (attributes & 0x20) ? 0 : 1; //Index in the frame's SpritePriority: behind the background, in front of it
		entry.FlipH = (attributes & 0x40) != 0;
		entry.FlipV = (attributes & 0x80) != 0;
		entry.Signature = signature;
		entry.CellCount = height / 8;
		entry.Cells[0] = (uint16_t)t0;
		entry.Cells[1] = (uint16_t)t1;
		entries.push_back(entry);
	}
}

void NesHdLayer::BuildFrame(HdScreenInfo& info, SnesHdFrameData& frame, vector<uint32_t>& lowRes, const uint32_t* palette)
{
	constexpr uint32_t Width = NesConstants::ScreenWidth;
	constexpr uint32_t Height = NesConstants::ScreenHeight;

	std::fill(frame.Cells.begin(), frame.Cells.end(), 0);
	std::fill(frame.Under.begin(), frame.Under.end(), 0);
	std::fill(frame.Owner.begin(), frame.Owner.end(), SnesHdOwner::Hidden);
	frame.Objects.clear();
	frame.SpritesByIndex = true;
	frame.SpritePriority[0] = 1; //A sprite behind the background; the background itself counts 2, a sprite in front 3
	frame.SpritePriority[1] = 3;
	for(uint32_t row = 0; row < Height; row++) {
		frame.Brightness[row] = 15;
	}
	lowRes.assign(Width * Height, 0);

	for(int palette = 0; palette < 4; palette++) {
		for(int c = 1; c < 4; c++) {
			frame.SpritePalette[palette * 16 + c] = info.PaletteRam[0x10 + palette * 4 + c] & 0x3F;
		}
	}

	bool suppress[128] = {};
	if(_pack->HasObjects() && _chrRom && info.SpritesEnabled) {
		vector<SnesHdOamEntry> entries;
		BuildEntries(info, entries);
		SnesHdObjectMatcher::Match(*_pack, entries, frame.Objects, suppress);
	}

	for(uint32_t y = 0; y < Height; y++) {
		for(uint32_t x = 0; x < Width; x++) {
			uint32_t p = y * Width + x;
			const HdPpuPixelInfo& pixel = info.ScreenTiles[p];
			bool backgroundThere = pixel.Tile.TileIndex != HdPpuTileInfo::NoTile;

			int original = SpriteAt(info, pixel, x, y, nullptr);
			int shown = original;
			if(original >= 0 && suppress[pixel.Sprite[original].OamIndex]) {
				//An object replaces this sprite: show what is under it (the next sprite, or the background)
				shown = SpriteAt(info, pixel, x, y, suppress);

				uint8_t color;
				bool opaque;
				if(shown >= 0) {
					color = pixel.Sprite[shown].SpriteColor;
				} else {
					color = BackdropColor(pixel, &opaque);
				}
				lowRes[p] = palette[(pixel.EmphasisBits << 6) | (color & (pixel.Grayscale ? 0x30 : 0x3F))] | 0xFF000000;
			}

			bool opaque = backgroundThere && pixel.Tile.BgColorIndex != 0;
			uint16_t owner = opaque ? 2 : 0;
			if(shown >= 0) {
				const HdPpuTileInfo& sprite = pixel.Sprite[shown];
				owner = SnesHdOwner::Sprite(sprite.BackgroundPriority ? 1 : 3, sprite.OamIndex);
				if(uint32_t cell = CellFor(sprite, true)) {
					frame.Cells[p] = cell;
					if(backgroundThere) {
						frame.Under[p] = CellFor(pixel.Tile, false);
					}
				}
			} else if(backgroundThere) {
				frame.Cells[p] = CellFor(pixel.Tile, false);
			}

			if((info.SpriteLeftMask && x < 8) || !info.SpritesRow[y]) {
				owner |= SnesHdOwner::Masked;
			}
			frame.Owner[p] = owner;
		}
	}
}

bool NesHdRecorders::Requested()
{
	const char* tiles = std::getenv("MESEN_NES_HD_RECORD");
	const char* objects = std::getenv("MESEN_NES_HD_OBJECTS");
	return (tiles && *tiles) || (objects && *objects);
}

NesHdRecorders::NesHdRecorders(NesHdLayer* layer, NesConfig& config)
{
	_layer = layer;
	NesDefaultVideoFilter::GetFullPalette(_palette, config, PpuModel::Ppu2C02);
	_tiles = SnesHdRecorder::Create(true);
	_objects = SnesHdObjectRecorder::Create(true);
}

NesHdRecorders::~NesHdRecorders()
{
	Flush();
}

void NesHdRecorders::Flush()
{
	if(_tiles) {
		_tiles->Flush();
	}
	if(_objects) {
		_objects->Flush();
	}
}

void NesHdRecorders::Record(HdScreenInfo& info, const uint16_t* ppuBuffer)
{
	if(_tiles) {
		//Count the pixels of every tile (and palette) that shows: a background tile where no sprite covers it, a sprite where it wins
		const HdPpuTileInfo* lastTile = nullptr;
		bool lastSprite = false;
		uint32_t run = 0;
		uint8_t lastColors[3] = {};
		int32_t lastIndex = -1;
		uint8_t lastChr[16] = {};

		auto flush = [&]() {
			if(run > 0 && lastTile) {
				_tiles->RecordNes(lastSprite, lastChr, lastColors, info.FrameNumber, run);
			}
			run = 0;
			lastTile = nullptr;
		};

		for(uint32_t p = 0; p < NesConstants::ScreenPixelCount; p++) {
			const HdPpuPixelInfo& pixel = info.ScreenTiles[p];
			int sprite = NesHdLayer::SpriteAt(info, pixel, p % NesConstants::ScreenWidth, p / NesConstants::ScreenWidth, nullptr);
			const HdPpuTileInfo* tile = sprite >= 0 ? &pixel.Sprite[sprite] : (pixel.Tile.TileIndex != HdPpuTileInfo::NoTile ? &pixel.Tile : nullptr);
			if(!tile) {
				flush();
				continue;
			}

			uint8_t colors[3] = { (uint8_t)((tile->PaletteColors >> 16) & 0x3F), (uint8_t)((tile->PaletteColors >> 8) & 0x3F), (uint8_t)(tile->PaletteColors & 0x3F) };
			bool isSprite = sprite >= 0;
			if(lastTile && lastSprite == isSprite && lastIndex == tile->TileIndex && memcmp(colors, lastColors, 3) == 0 && (_layer->IsChrRom() || memcmp(tile->TileData, lastChr, 16) == 0)) {
				run++;
				continue;
			}

			flush();
			lastTile = tile;
			lastSprite = isSprite;
			lastIndex = tile->TileIndex;
			memcpy(lastColors, colors, 3);
			if(_layer->IsChrRom()) {
				_layer->ReadChr(tile->TileIndex, lastChr);
			} else {
				memcpy(lastChr, tile->TileData, 16);
			}
			run = 1;
		}
		flush();
	}

	if(_objects) {
		vector<SnesHdOamEntry> entries;
		_layer->BuildEntries(info, entries);

		SnesHdRecordSource source;
		source.ColorsPerPalette = 3;
		source.DigitsPerColor = 2;
		source.ColorIndex = [this](const SnesHdOamEntry& e, int sx, int sy) {
			int cellY = e.FlipV ? (e.Height / 8 - 1 - sy / 8) : sy / 8;
			int px = e.FlipH ? 7 - (sx % 8) : sx % 8;
			int py = e.FlipV ? 7 - (sy % 8) : sy % 8;
			uint8_t chr[16];
			_layer->ReadChr(e.Cells[cellY], chr);
			int shift = 7 - px;
			return (uint8_t)(((chr[py] >> shift) & 1) | (((chr[py + 8] >> shift) & 1) << 1));
		};
		source.Argb = [this](const SnesHdOamEntry& e, uint8_t color) { return _palette[_currentPalette[e.Palette * 4 + color] & 0x3F]; };
		memcpy(_currentPalette, info.PaletteRam + 0x10, 16);
		source.Reference = [&info](const SnesHdOamEntry& e, SnesHdEntrySpec& spec) {
			spec.HasReference = true;
			for(int c = 1; c < 4; c++) {
				spec.Reference[c] = info.PaletteRam[0x10 + e.Palette * 4 + c] & 0x3F;
			}
		};
		_objects->Record(info.FrameNumber, entries, source);
	}
}
