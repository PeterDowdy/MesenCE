#include "pch.h"
#include "SNES/HdPacks/SnesHdPack.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/StringUtilities.h"

uint64_t SnesHdPack::HashTile(const uint16_t* vram, uint16_t wordAddress)
{
	//FNV-1a over the tile's 32 bytes (low byte of each word first). The address wraps within the 32k words of VRAM
	uint64_t hash = 0xCBF29CE484222325ULL;
	for(int i = 0; i < 16; i++) {
		uint16_t word = vram[(wordAddress + i) & 0x7FFF];
		hash = (hash ^ (word & 0xFF)) * 0x100000001B3ULL;
		hash = (hash ^ (word >> 8)) * 0x100000001B3ULL;
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
			if(!LoadTile(tokens, folder, images, imageSizes)) {
				return false;
			}
		}
	}

	return !Tiles.empty();
}

bool SnesHdPack::LoadTile(vector<string>& tokens, const string& folder, vector<vector<uint32_t>>& images, vector<std::pair<uint32_t, uint32_t>>& imageSizes)
{
	if(tokens.size() != 4 || tokens[1].size() != 64) {
		MessageManager::Log("[HDPack] Invalid tile tag (expected imageIndex,tileData(64 hex digits),x,y)");
		return false;
	}

	uint32_t imageIndex = (uint32_t)std::stoi(tokens[0]);
	uint32_t x = (uint32_t)std::stoi(tokens[2]);
	uint32_t y = (uint32_t)std::stoi(tokens[3]);
	uint32_t size = 8 * Scale;
	if(imageIndex >= images.size() || x + size > imageSizes[imageIndex].first || y + size > imageSizes[imageIndex].second) {
		MessageManager::Log("[HDPack] Tile outside its image: " + tokens[1]);
		return false;
	}

	//The key is the same hash the PPU computes from VRAM: 16 words, low byte first
	uint16_t words[16];
	for(int i = 0; i < 16; i++) {
		words[i] = HexUtilities::FromHex(tokens[1].substr(i * 4, 2)) | (HexUtilities::FromHex(tokens[1].substr(i * 4 + 2, 2)) << 8);
	}

	if(Tiles.size() >= 0xFFFF) {
		MessageManager::Log("[HDPack] Too many tiles");
		return false;
	}

	SnesHdTile tile;
	tile.Pixels.resize(size * size);
	uint32_t imageWidth = imageSizes[imageIndex].first;
	for(uint32_t row = 0; row < size; row++) {
		memcpy(tile.Pixels.data() + row * size, images[imageIndex].data() + (y + row) * imageWidth + x, size * sizeof(uint32_t));
	}

	Tiles.push_back(std::move(tile));
	TileIdByHash[HashTile(words, 0)] = (uint16_t)Tiles.size();
	return true;
}
