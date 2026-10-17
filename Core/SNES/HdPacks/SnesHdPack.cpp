#include "pch.h"
#include "SNES/HdPacks/SnesHdPack.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/StringUtilities.h"

uint64_t SnesHdPack::HashTile(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp)
{
	//FNV-1a over the tile's bytes (low byte of each word first). The address wraps within the 32k words of VRAM
	uint64_t hash = 0xCBF29CE484222325ULL;
	for(int i = 0; i < bpp * 4; i++) {
		uint16_t word = vram[(wordAddress + i) & 0x7FFF];
		hash = (hash ^ (word & 0xFF)) * 0x100000001B3ULL;
		hash = (hash ^ (word >> 8)) * 0x100000001B3ULL;
	}
	return hash;
}

uint64_t SnesHdPack::HashPalette(const uint16_t* cgram, uint16_t paletteBase, uint8_t bpp)
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	for(int i = 1; i < (1 << bpp); i++) {
		uint16_t color = cgram[(paletteBase + i) & 0xFF];
		hash = (hash ^ (color & 0xFF)) * 0x100000001B3ULL;
		hash = (hash ^ (color >> 8)) * 0x100000001B3ULL;
	}
	return hash;
}

unique_ptr<SnesHdPack> SnesHdPack::Load(const string& romFileName)
{
	string romName = FolderUtilities::GetFilename(romFileName, false);
	string definitionFile = FolderUtilities::CombinePath(FolderUtilities::CombinePath(FolderUtilities::GetHdPackFolder(), romName), "hires.txt");
	if(!ifstream(definitionFile)) {
		return nullptr;
	}

	unique_ptr<SnesHdPack> pack(new SnesHdPack());
	if(!pack->LoadDefinition(definitionFile)) {
		return nullptr;
	}

	MessageManager::Log("[HDPack] SNES HD pack loaded: " + definitionFile + " (scale " + std::to_string(pack->Scale) + ", " + std::to_string(pack->Tiles.size()) + " tiles)");
	return pack;
}

bool SnesHdPack::LoadDefinition(const string& definitionFile)
{
	string folder = FolderUtilities::GetFolderName(definitionFile);

	vector<vector<uint32_t>> images;
	vector<std::pair<uint32_t, uint32_t>> imageSizes;

	ifstream file(definitionFile);
	string line;
	while(std::getline(file, line)) {
		if(!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		size_t tagEnd = line.find('>');
		if(line.empty() || line[0] != '<' || tagEnd == string::npos) {
			continue;
		}

		string tag = line.substr(1, tagEnd - 1);
		string value = line.substr(tagEnd + 1);

		if(tag == "scale") {
			Scale = (uint32_t)std::stoi(value);
			if(Scale < 1 || Scale > MaxScale) {
				MessageManager::Log("[HDPack] Invalid scale: " + value);
				return false;
			}
		} else if(tag == "img") {
			vector<uint8_t> pngData;
			uint32_t width, height;
			if(!PNGHelper::ReadPNG(FolderUtilities::CombinePath(folder, value), pngData, width, height)) {
				MessageManager::Log("[HDPack] Could not read image: " + value);
				return false;
			}
			vector<uint32_t> pixels(width * height);
			memcpy(pixels.data(), pngData.data(), pixels.size() * sizeof(uint32_t));
			images.push_back(std::move(pixels));
			imageSizes.push_back({ width, height });
		} else if(tag == "tile") {
			vector<string> tokens = StringUtilities::Split(value, ',');
			if(!LoadTile(tokens, images, imageSizes)) {
				return false;
			}
		}
	}

	return !Tiles.empty();
}

static vector<uint16_t> ParseWords(const string& hex)
{
	//Every 2 hex digits are a byte, low byte of each word first
	vector<uint16_t> words(hex.size() / 4);
	for(size_t i = 0; i < words.size(); i++) {
		words[i] = HexUtilities::FromHex(hex.substr(i * 4, 2)) | (HexUtilities::FromHex(hex.substr(i * 4 + 2, 2)) << 8);
	}
	return words;
}

bool SnesHdPack::LoadTile(vector<string>& tokens, vector<vector<uint32_t>>& images, vector<std::pair<uint32_t, uint32_t>>& imageSizes)
{
	size_t digits = tokens.size() >= 2 ? tokens[1].size() : 0;
	if((tokens.size() != 4 && tokens.size() != 5) || (digits != 32 && digits != 64 && digits != 128)) {
		MessageManager::Log("[HDPack] Invalid tile tag (expected imageIndex,tileData(32, 64 or 128 hex digits),x,y[,palette])");
		return false;
	}

	uint8_t bpp = (uint8_t)(digits / 16);
	uint32_t imageIndex = (uint32_t)std::stoi(tokens[0]);
	uint32_t x = (uint32_t)std::stoi(tokens[2]);
	uint32_t y = (uint32_t)std::stoi(tokens[3]);
	uint32_t size = 8 * Scale;
	if(imageIndex >= images.size() || x + size > imageSizes[imageIndex].first || y + size > imageSizes[imageIndex].second) {
		MessageManager::Log("[HDPack] Tile outside its image: " + tokens[1]);
		return false;
	}

	if(Tiles.size() >= 0xFFFF) {
		MessageManager::Log("[HDPack] Too many tiles");
		return false;
	}

	//The key is the same hash the PPU computes from VRAM (and from CGRAM for the palette)
	vector<uint16_t> tileWords = ParseWords(tokens[1]);
	uint64_t tileHash = HashTile(tileWords.data(), 0, bpp);

	bool hasPalette = tokens.size() == 5;
	uint64_t paletteHash = 0;
	if(hasPalette) {
		if(tokens[4].size() != 4 * ((1u << bpp) - 1)) {
			MessageManager::Log("[HDPack] Invalid palette (expected " + std::to_string(4 * ((1u << bpp) - 1)) + " hex digits): " + tokens[4]);
			return false;
		}
		//HashPalette skips color 0, so put a placeholder first
		vector<uint16_t> colors = ParseWords(tokens[4]);
		colors.insert(colors.begin(), 0);
		paletteHash = HashPalette(colors.data(), 0, bpp);
	}

	SnesHdTile tile;
	tile.Pixels.resize(size * size);
	uint32_t imageWidth = imageSizes[imageIndex].first;
	for(uint32_t row = 0; row < size; row++) {
		memcpy(tile.Pixels.data() + row * size, images[imageIndex].data() + (y + row) * imageWidth + x, size * sizeof(uint32_t));
	}
	Tiles.push_back(std::move(tile));
	uint16_t tileId = (uint16_t)Tiles.size();

	TileEntry& entry = _entries[tileHash];
	if(hasPalette) {
		entry.Variants.push_back({ paletteHash, tileId });
	} else {
		entry.DefaultId = tileId;
	}
	return true;
}

unique_ptr<SnesHdRecorder> SnesHdRecorder::Create()
{
	const char* path = std::getenv("MESEN_SNES_HD_RECORD");
	if(!path || !*path) {
		return nullptr;
	}

	unique_ptr<SnesHdRecorder> recorder(new SnesHdRecorder());
	recorder->_path = path;
	return recorder;
}

static string ToHex(const uint16_t* words, size_t count, bool bytes)
{
	static const char digits[] = "0123456789abcdef";
	string result;
	for(size_t i = 0; i < count; i++) {
		if(bytes) {
			result += digits[(words[i] >> 4) & 0x0F];
			result += digits[words[i] & 0x0F];
			result += digits[(words[i] >> 12) & 0x0F];
			result += digits[(words[i] >> 8) & 0x0F];
		} else {
			for(int shift = 12; shift >= 0; shift -= 4) {
				result += digits[(words[i] >> shift) & 0x0F];
			}
		}
	}
	return result;
}

void SnesHdRecorder::Record(bool sprite, uint8_t bpp, const uint16_t* vram, uint16_t wordAddress, const uint16_t* cgram, uint16_t paletteBase)
{
	auto key = std::make_pair(SnesHdPack::HashTile(vram, wordAddress, bpp) ^ (sprite ? 0 : 0x9E3779B97F4A7C15ULL), SnesHdPack::HashPalette(cgram, paletteBase, bpp));
	if(!_seen.insert(key).second) {
		return;
	}

	uint16_t tile[32];
	for(int i = 0; i < bpp * 4; i++) {
		tile[i] = vram[(wordAddress + i) & 0x7FFF];
	}
	_lines.push_back(string(sprite ? "S " : "B ") + std::to_string(bpp) + " " + ToHex(tile, bpp * 4, true) + " " + ToHex(cgram + ((paletteBase + 1) & 0xFF), (1 << bpp) - 1, false));
	_dirty = true;
}

void SnesHdRecorder::Flush()
{
	if(!_dirty) {
		return;
	}

	ofstream file(_path, std::ios::out | std::ios::trunc);
	for(string& line : _lines) {
		file << line << "\n";
	}
	_dirty = false;
}
