#include "pch.h"
#include "SNES/HdPacks/SnesHdNeighbours.h"
#include <climits>
#include <filesystem>
#include <map>
#include "SNES/HdPacks/SnesHdPack.h"

unique_ptr<SnesHdNeighbourRecorder> SnesHdNeighbourRecorder::Create(bool nes)
{
	const char* path = std::getenv(nes ? "MESEN_NES_HD_NEIGHBOURS" : "MESEN_SNES_HD_NEIGHBOURS");
	if(!path || !*path) {
		return nullptr;
	}

	unique_ptr<SnesHdNeighbourRecorder> recorder(new SnesHdNeighbourRecorder());
	recorder->_path = path;
	return recorder;
}

uint32_t SnesHdNeighbourRecorder::TileNumber(uint64_t hash, const std::function<string()>& describe)
{
	auto known = _numbers.find(hash);
	if(known != _numbers.end()) {
		return known->second;
	}
	if(_tiles.size() >= 0xFFFF) {
		return 0;
	}

	_tiles.push_back(describe());
	_numbers[hash] = (uint32_t)_tiles.size();
	return (uint32_t)_tiles.size();
}

void SnesHdNeighbourRecorder::FrameFromCells(const uint32_t* cells, uint32_t width, uint32_t height)
{
	vector<SnesHdNeighbourTile> tiles;
	std::unordered_set<uint32_t> seen;
	for(uint32_t y = 0; y < height; y++) {
		for(uint32_t x = 0; x < width; x++) {
			uint32_t cell = cells[y * width + x];
			if(!cell || !SnesHdCell::IsBackground(cell)) {
				continue;
			}

			//The stored x and y of the pixel in its tile, and where that puts the tile's corner when the tile is drawn flipped
			int column = SnesHdCell::FlipH(cell) ? 7 - SnesHdCell::X(cell) : SnesHdCell::X(cell);
			int row = SnesHdCell::FlipV(cell) ? 7 - SnesHdCell::Y(cell) : SnesHdCell::Y(cell);
			int cornerX = (int)x - column;
			int cornerY = (int)y - row;
			if(seen.insert(((uint32_t)(cornerX + 64) << 16) | (uint32_t)(cornerY + 64)).second) {
				tiles.push_back({ (int16_t)cornerX, (int16_t)cornerY, SnesHdCell::TileId(cell), SnesHdCell::FlipH(cell), SnesHdCell::FlipV(cell) });
			}
		}
	}
	Frame(tiles);
}

void SnesHdNeighbourRecorder::Frame(const vector<SnesHdNeighbourTile>& tiles)
{
	_frames++;
	_dirty = true;
	for(const SnesHdNeighbourTile& tile : tiles) {
		_drawn.insert(tile.Tile - 1);
	}

	//Tiles at the same offset from the 8 pixel grid are in one grid
	std::map<int, vector<const SnesHdNeighbourTile*>> bands;
	for(const SnesHdNeighbourTile& tile : tiles) {
		bands[(tile.X & 7) * 8 + (tile.Y & 7)].push_back(&tile);
	}

	for(auto& band : bands) {
		if(band.second.size() < 12) {
			continue;
		}

		int minColumn = INT_MAX, minRow = INT_MAX, maxColumn = INT_MIN, maxRow = INT_MIN;
		for(const SnesHdNeighbourTile* tile : band.second) {
			int column = (tile->X - (tile->X & 7)) / 8;
			int row = (tile->Y - (tile->Y & 7)) / 8;
			minColumn = std::min(minColumn, column);
			maxColumn = std::max(maxColumn, column);
			minRow = std::min(minRow, row);
			maxRow = std::max(maxRow, row);
		}

		Screen screen;
		screen.Columns = (uint16_t)(maxColumn - minColumn + 1);
		screen.Rows = (uint16_t)(maxRow - minRow + 1);
		screen.Cells.assign(screen.Columns * screen.Rows, 0);
		for(const SnesHdNeighbourTile* tile : band.second) {
			int column = (tile->X - (tile->X & 7)) / 8 - minColumn;
			int row = (tile->Y - (tile->Y & 7)) / 8 - minRow;
			screen.Cells[row * screen.Columns + column] = tile->Tile | ((uint32_t)tile->FlipH << 16) | ((uint32_t)tile->FlipV << 17);
		}

		uint64_t hash = 0xCBF29CE484222325ULL ^ (screen.Columns << 8) ^ screen.Rows;
		for(uint32_t cell : screen.Cells) {
			hash = (hash ^ cell) * 0x100000001B3ULL;
		}

		auto known = _screenIndex.find(hash);
		if(known != _screenIndex.end()) {
			_screens[known->second].Weight++;
		} else if(_screens.size() < 4000) {
			screen.Weight = 1;
			_screenIndex[hash] = _screens.size();
			_screens.push_back(std::move(screen));
		}
	}
}

void SnesHdNeighbourRecorder::Flush()
{
	if(!_dirty) {
		return;
	}

	//Written beside and renamed, so a run that ends mid-write leaves the last complete file
	string temporary = _path + ".tmp";
	ofstream file(temporary, std::ios::out | std::ios::trunc);
	file << "{\"every\":3,\"frames\":" << _frames << ",\"tiles\":[";
	for(size_t i = 0; i < _tiles.size(); i++) {
		size_t space = _tiles[i].find(' ');
		file << (i ? "," : "") << "[" << _tiles[i].substr(0, space) << ",\"" << _tiles[i].substr(space + 1) << "\"]";
	}
	file << "],\"drawn\":[";
	bool first = true;
	for(uint32_t tile : _drawn) {
		file << (first ? "" : ",") << tile;
		first = false;
	}
	file << "],\"screens\":[";
	for(size_t i = 0; i < _screens.size(); i++) {
		const Screen& screen = _screens[i];
		file << (i ? ",\n" : "\n") << "{\"weight\":" << screen.Weight << ",\"cols\":" << screen.Columns << ",\"rows\":" << screen.Rows << ",\"cells\":[";
		for(size_t c = 0; c < screen.Cells.size(); c++) {
			uint32_t cell = screen.Cells[c];
			uint32_t tile = cell & 0xFFFF;
			file << (c ? "," : "") << (tile ? (tile << 2) | (((cell >> 16) & 1) << 1) | ((cell >> 17) & 1) : 0);
		}
		file << "]}";
	}
	file << "]}\n";
	file.close();
	std::filesystem::rename(temporary, _path);
	_dirty = false;
}
