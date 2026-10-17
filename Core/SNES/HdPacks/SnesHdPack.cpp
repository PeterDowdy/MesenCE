#include "pch.h"
#include <filesystem>
#include "SNES/HdPacks/SnesHdPack.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/StringUtilities.h"
#include <deque>

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

uint64_t SnesHdPack::HashMode7Tile(const uint16_t* vram, uint8_t tile)
{
	uint64_t hash = 0x4D4F444537A5A5A5ULL; //Not the seed of a planar tile's hash, so the two can't be mistaken for each other
	for(int i = 0; i < 64; i++) {
		hash = (hash ^ (vram[(tile << 6) | i] >> 8)) * 0x100000001B3ULL;
	}
	return hash;
}

string SnesHdPack::TileHex(const uint16_t* vram, uint16_t wordAddress, uint8_t bpp)
{
	static const char digits[] = "0123456789abcdef";
	string result;
	for(int i = 0; i < bpp * 4; i++) {
		uint16_t word = vram[(wordAddress + i) & 0x7FFF];
		result += digits[(word >> 4) & 0x0F];
		result += digits[word & 0x0F];
		result += digits[(word >> 12) & 0x0F];
		result += digits[(word >> 8) & 0x0F];
	}
	return result;
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

	MessageManager::Log("[HDPack] SNES HD pack loaded: " + definitionFile + " (scale " + std::to_string(pack->Scale) + ", " + std::to_string(pack->Tiles.size()) + " tiles, " + std::to_string(pack->Objects.size()) + " objects)");
	return pack;
}

bool SnesHdPack::LoadDefinition(const string& definitionFile)
{
	string folder = FolderUtilities::GetFolderName(definitionFile);

	LoadState state;
	bool hasObject = false;

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

		if(tag != "entry" && hasObject) {
			if(!FinishObject(state)) {
				return false;
			}
			hasObject = false;
		}

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
			state.Images.push_back(std::move(pixels));
			state.ImageSizes.push_back({ width, height });
		} else if(tag == "tile" || tag == "variant") {
			vector<string> tokens = StringUtilities::Split(value, ',');
			if(!LoadTile(tokens, state, tag == "variant")) {
				return false;
			}
		} else if(tag == "object") {
			vector<string> tokens = StringUtilities::Split(value, ',');
			if(!LoadObject(tokens, state)) {
				return false;
			}
			hasObject = true;
		} else if(tag == "entry") {
			vector<string> tokens = StringUtilities::Split(value, ',');
			if(!hasObject || !LoadEntry(tokens, state)) {
				MessageManager::Log("[HDPack] Invalid or misplaced <entry> tag");
				return false;
			}
		}
	}

	if(hasObject && !FinishObject(state)) {
		return false;
	}

	//Match the largest objects first, so a small one never takes an entry that belongs to a bigger one
	for(auto& anchor : ObjectsByAnchor) {
		std::stable_sort(anchor.second.begin(), anchor.second.end(), [this](uint16_t a, uint16_t b) {
			return Objects[a].Entries.size() > Objects[b].Entries.size();
		});
	}

	return !Tiles.empty() || !Objects.empty();
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

//Reads the 4 hex digits of each color as written by the recorder (most significant digit first)
static vector<uint16_t> ParseColors(const string& hex)
{
	vector<uint16_t> colors(hex.size() / 4);
	for(size_t i = 0; i < colors.size(); i++) {
		colors[i] = (uint16_t)std::stoi(hex.substr(i * 4, 4), nullptr, 16);
	}
	return colors;
}

static void DecodeTile(const vector<uint16_t>& words, uint8_t bpp, uint8_t* index)
{
	for(int y = 0; y < 8; y++) {
		for(int x = 0; x < 8; x++) {
			int shift = 7 - x;
			uint8_t color = 0;
			for(int pair = 0; pair < bpp / 2; pair++) {
				uint16_t word = words[pair * 8 + y];
				color |= (((word & 0xFF) >> shift) & 1) << (pair * 2);
				color |= (((word >> 8) >> shift) & 1) << (pair * 2 + 1);
			}
			index[y * 8 + x] = color;
		}
	}
}

bool SnesHdPack::LoadTile(vector<string>& tokens, LoadState& state, bool variant)
{
	if(!variant && tokens.size() >= 4 && tokens[1].rfind("m7:", 0) == 0) {
		//A tile of Mode 7's plane
		string data = tokens[1].substr(3);
		uint32_t imageIndex = (uint32_t)std::stoi(tokens[0]);
		uint32_t x = (uint32_t)std::stoi(tokens[2]);
		uint32_t y = (uint32_t)std::stoi(tokens[3]);
		uint32_t size = 8 * Scale;
		if(data.size() != 128 || imageIndex >= state.Images.size() || x + size > state.ImageSizes[imageIndex].first || y + size > state.ImageSizes[imageIndex].second || Tiles.size() >= 0xFFFF) {
			MessageManager::Log("[HDPack] Invalid Mode 7 tile: " + tokens[1]);
			return false;
		}

		uint64_t hash = 0x4D4F444537A5A5A5ULL;
		for(int i = 0; i < 64; i++) {
			hash = (hash ^ (uint64_t)HexUtilities::FromHex(data.substr(i * 2, 2))) * 0x100000001B3ULL;
		}

		SnesHdTile tile;
		tile.Pixels.resize(size * size);
		uint32_t imageWidth = state.ImageSizes[imageIndex].first;
		for(uint32_t row = 0; row < size; row++) {
			memcpy(tile.Pixels.data() + row * size, state.Images[imageIndex].data() + (y + row) * imageWidth + x, size * sizeof(uint32_t));
		}
		Tiles.push_back(std::move(tile));
		_entries[hash].DefaultId = (uint16_t)Tiles.size();
		_hasMode7Tiles = true;
		return true;
	}

	size_t digits = tokens.size() >= 2 ? tokens[1].size() : 0;
	bool validCount = variant ? tokens.size() == 5 : (tokens.size() == 4 || tokens.size() == 5);
	if(!validCount || (digits != 32 && digits != 64 && digits != 128)) {
		MessageManager::Log("[HDPack] Invalid tile tag (expected imageIndex,tileData(32, 64 or 128 hex digits),x,y[,palette])");
		return false;
	}

	uint8_t bpp = (uint8_t)(digits / 16);
	uint32_t imageIndex = (uint32_t)std::stoi(tokens[0]);
	uint32_t x = (uint32_t)std::stoi(tokens[2]);
	uint32_t y = (uint32_t)std::stoi(tokens[3]);
	uint32_t size = 8 * Scale;
	if(imageIndex >= state.Images.size() || x + size > state.ImageSizes[imageIndex].first || y + size > state.ImageSizes[imageIndex].second) {
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

	vector<uint16_t> colors;
	if(tokens.size() == 5) {
		if(tokens[4].size() != 4 * ((1u << bpp) - 1)) {
			MessageManager::Log("[HDPack] Invalid palette (expected " + std::to_string(4 * ((1u << bpp) - 1)) + " hex digits): " + tokens[4]);
			return false;
		}
		colors = ParseColors(tokens[4]);
	}

	SnesHdTile tile;
	tile.Pixels.resize(size * size);
	uint32_t imageWidth = state.ImageSizes[imageIndex].first;
	for(uint32_t row = 0; row < size; row++) {
		memcpy(tile.Pixels.data() + row * size, state.Images[imageIndex].data() + (y + row) * imageWidth + x, size * sizeof(uint32_t));
	}

	if(!variant && !colors.empty() && bpp <= 4) {
		tile.HasReference = true;
		DecodeTile(tileWords, bpp, tile.Index);
		for(size_t i = 0; i < colors.size(); i++) {
			tile.Reference[i + 1] = colors[i];
		}
	}

	Tiles.push_back(std::move(tile));
	uint16_t tileId = (uint16_t)Tiles.size();

	TileEntry& entry = _entries[tileHash];
	if(variant) {
		//HashPalette skips color 0, so put a placeholder first
		colors.insert(colors.begin(), 0);
		entry.Variants.push_back({ HashPalette(colors.data(), 0, bpp), tileId });
	} else {
		entry.DefaultId = tileId;
	}
	return true;
}

bool SnesHdPack::LoadObject(vector<string>& tokens, LoadState& state)
{
	if(tokens.size() != 11) {
		MessageManager::Log("[HDPack] Invalid object tag (expected name,artImage,artX,artY,artW,artH,offsetX,offsetY,indexImage,indexX,indexY)");
		return false;
	}

	SnesHdObject object;
	object.Name = tokens[0];

	uint32_t artImage = (uint32_t)std::stoi(tokens[1]);
	uint32_t artX = (uint32_t)std::stoi(tokens[2]);
	uint32_t artY = (uint32_t)std::stoi(tokens[3]);
	object.ArtWidth = (uint32_t)std::stoi(tokens[4]);
	object.ArtHeight = (uint32_t)std::stoi(tokens[5]);
	object.OffsetX = std::stoi(tokens[6]);
	object.OffsetY = std::stoi(tokens[7]);
	state.IndexImage = (uint32_t)std::stoi(tokens[8]);
	state.IndexX = (uint32_t)std::stoi(tokens[9]);
	state.IndexY = (uint32_t)std::stoi(tokens[10]);

	if(artImage >= state.Images.size() || state.IndexImage >= state.Images.size() ||
		artX + object.ArtWidth > state.ImageSizes[artImage].first || artY + object.ArtHeight > state.ImageSizes[artImage].second) {
		MessageManager::Log("[HDPack] Object art outside its image: " + object.Name);
		return false;
	}

	object.Pixels.resize(object.ArtWidth * object.ArtHeight);
	uint32_t imageWidth = state.ImageSizes[artImage].first;
	for(uint32_t row = 0; row < object.ArtHeight; row++) {
		memcpy(object.Pixels.data() + row * object.ArtWidth, state.Images[artImage].data() + (artY + row) * imageWidth + artX, object.ArtWidth * sizeof(uint32_t));
	}

	state.Current = std::move(object);
	return true;
}

bool SnesHdPack::LoadEntry(vector<string>& tokens, LoadState& state)
{
	if(tokens.size() != 7 && tokens.size() != 8) {
		return false;
	}

	SnesHdEntrySpec entry;
	entry.X = (int16_t)std::stoi(tokens[0]);
	entry.Y = (int16_t)std::stoi(tokens[1]);
	entry.Width = (uint8_t)std::stoi(tokens[2]);
	entry.Height = (uint8_t)std::stoi(tokens[3]);
	entry.FlipH = std::stoi(tokens[4]) != 0;
	entry.FlipV = std::stoi(tokens[5]) != 0;
	entry.Signature = std::stoull(tokens[6], nullptr, 16);
	if(tokens.size() == 8) {
		if(tokens[7].size() != 60) {
			return false;
		}
		entry.HasReference = true;
		vector<uint16_t> colors = ParseColors(tokens[7]);
		for(size_t i = 0; i < colors.size(); i++) {
			entry.Reference[i + 1] = colors[i];
		}
	}

	if(state.Current.Entries.size() >= SnesHdObject::MaxEntries || entry.X < 0 || entry.Y < 0) {
		return false;
	}
	state.Current.Entries.push_back(entry);
	return true;
}

bool SnesHdPack::FinishObject(LoadState& state)
{
	SnesHdObject& object = state.Current;
	if(object.Entries.empty()) {
		MessageManager::Log("[HDPack] Object without entries: " + object.Name);
		return false;
	}

	for(SnesHdEntrySpec& entry : object.Entries) {
		object.Width = std::max<uint16_t>(object.Width, entry.X + entry.Width);
		object.Height = std::max<uint16_t>(object.Height, entry.Y + entry.Height);
	}

	uint32_t imageWidth = state.ImageSizes[state.IndexImage].first;
	if(state.IndexX + object.Width > imageWidth || state.IndexY + object.Height > state.ImageSizes[state.IndexImage].second) {
		MessageManager::Log("[HDPack] Object index map outside its image: " + object.Name);
		return false;
	}

	//Read the index map: R = entry, G = color index. Pixels the original didn't draw take the nearest one that it did
	uint32_t width = object.Width;
	uint32_t height = object.Height;
	object.Source.assign(width * height, 0);
	std::deque<uint32_t> queue;
	for(uint32_t y = 0; y < height; y++) {
		for(uint32_t x = 0; x < width; x++) {
			uint32_t pixel = state.Images[state.IndexImage][(state.IndexY + y) * imageWidth + state.IndexX + x];
			uint16_t source = (uint16_t)((((pixel >> 16) & 0xFF) << 8) | ((pixel >> 8) & 0xFF));
			if((source & 0xFF) != 0) {
				object.Source[y * width + x] = source;
				queue.push_back(y * width + x);
			}
		}
	}
	while(!queue.empty()) {
		uint32_t index = queue.front();
		queue.pop_front();
		uint32_t x = index % width;
		uint32_t y = index / width;
		const int neighbours[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for(auto& n : neighbours) {
			int nx = (int)x + n[0];
			int ny = (int)y + n[1];
			if(nx >= 0 && ny >= 0 && nx < (int)width && ny < (int)height && object.Source[ny * width + nx] == 0) {
				object.Source[ny * width + nx] = object.Source[index];
				queue.push_back(ny * width + nx);
			}
		}
	}

	if(Objects.size() >= 0xFFFF) {
		MessageManager::Log("[HDPack] Too many objects");
		return false;
	}
	ObjectsByAnchor[object.Entries[0].Signature].push_back((uint16_t)Objects.size());
	Objects.push_back(std::move(object));
	state.Current = SnesHdObject();
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

void SnesHdRecorder::Record(bool sprite, uint8_t bpp, const uint16_t* vram, uint16_t wordAddress, const uint16_t* cgram, uint16_t paletteBase, uint32_t frame)
{
	auto key = std::make_pair(SnesHdPack::HashTile(vram, wordAddress, bpp) ^ (sprite ? 0 : 0x9E3779B97F4A7C15ULL), SnesHdPack::HashPalette(cgram, paletteBase, bpp));
	auto known = _index.find(key);
	if(known != _index.end()) {
		_rows[known->second].Fetches++;
		_dirty = true;
		return;
	}

	uint16_t tile[32];
	for(int i = 0; i < bpp * 4; i++) {
		tile[i] = vram[(wordAddress + i) & 0x7FFF];
	}
	_index[key] = _rows.size();
	_rows.push_back({ string(sprite ? "S " : "B ") + std::to_string(bpp) + " " + ToHex(tile, bpp * 4, true) + " " + ToHex(cgram + ((paletteBase + 1) & 0xFF), (1 << bpp) - 1, false), frame, 1 });
	_dirty = true;
}

void SnesHdRecorder::Flush()
{
	if(!_dirty) {
		return;
	}

	ofstream file(_path, std::ios::out | std::ios::trunc);
	for(Row& row : _rows) {
		file << row.Text << " " << row.FirstFrame << " " << row.Fetches << "\n";
	}
	_dirty = false;
}

unique_ptr<SnesHdPlaneRecorder> SnesHdPlaneRecorder::Create()
{
	const char* folder = std::getenv("MESEN_SNES_HD_PLANES");
	if(!folder || !*folder) {
		return nullptr;
	}

	unique_ptr<SnesHdPlaneRecorder> recorder(new SnesHdPlaneRecorder());
	recorder->_folder = folder;
	std::filesystem::create_directories(folder);
	return recorder;
}

bool SnesHdPlaneRecorder::Seen(uint64_t key, uint8_t layer, uint8_t bpp, uint16_t width, uint16_t height, uint32_t frame, int spread)
{
	Plane& plane = _planes[key];
	bool isNew = plane.Frames == 0;
	if(isNew) {
		plane.Layer = layer;
		plane.Bpp = bpp;
		plane.Width = width;
		plane.Height = height;
		plane.FirstFrame = frame;
	}
	plane.Frames++;
	_dirty = true;
	if(isNew || spread > plane.Spread + 8) {
		plane.Spread = spread;
		return true;
	}
	return false;
}

void SnesHdPlaneRecorder::Write(uint64_t key, const vector<uint32_t>& pixels, uint16_t width, uint16_t height, const string& cells)
{
	char name[48];
	snprintf(name, sizeof(name), "plane_%016llx", (unsigned long long)key);
	vector<uint32_t> copy(pixels);
	PNGHelper::WritePNG((std::filesystem::path(_folder) / (string(name) + ".png")).string(), copy.data(), width, height, 32);
	ofstream file(std::filesystem::path(_folder) / (string(name) + ".json"), std::ios::out | std::ios::trunc);
	file << cells;
}

void SnesHdPlaneRecorder::Flush()
{
	if(!_dirty) {
		return;
	}

	ofstream file(std::filesystem::path(_folder) / "planes.txt", std::ios::out | std::ios::trunc);
	for(auto& pair : _planes) {
		const Plane& plane = pair.second;
		char line[160];
		snprintf(line, sizeof(line), "P %016llx %u %u %u %u %u %u %d\n", (unsigned long long)pair.first, plane.Layer, plane.Bpp, plane.Width, plane.Height, plane.FirstFrame, plane.Frames, plane.Spread);
		file << line;
	}
	_dirty = false;
}
