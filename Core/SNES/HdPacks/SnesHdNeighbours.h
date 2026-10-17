#pragma once
#include "pch.h"
#include <functional>

//The screens the background tile graphics are drawn in, as grids of tiles, for restyling a tileset with its neighbours around it
//(MESEN_SNES_HD_NEIGHBOURS=<file>, MESEN_NES_HD_NEIGHBOURS=<file>). A tile on screen is found from the pixels the PPU drew (the
//winning background pixel of each tile says where its tile's corner is). Tiles whose corners are at the same offset from the 8
//pixel grid make a grid (a layer scrolled by a different amount, a parallax band, is another grid), and every third frame is
//counted; a grid of fewer than 12 tiles is left out. The file is JSON written as the game plays:
//
//  {"every": 3, "frames": <counted frames>,
//   "tiles": [[<bpp>, "<tile data, as the <tile> tag writes it>"], ...],       the tile graphics: the same tile in another palette is one
//   "screens": [{"weight": <counted frames it was shown in>, "cols": c, "rows": r,
//                "cells": [n, ...]}, ...]}      in reading order, one integer per cell: 0 = no tile drawn there, otherwise
//                                               (tile + 1) * 4 + flipH * 2 + flipV, where tile is an index into "tiles"
//                                               and the flips are as drawn
//
//Identical grids are one screen with a weight. At most 4000 different screens are kept (the first seen).
struct SnesHdNeighbourTile
{
	int16_t X = 0; //Where the tile's top left corner is on screen (it may be off the screen: a tile scrolled in)
	int16_t Y = 0;
	uint32_t Tile = 0; //From TileNumber, 1 and up
	bool FlipH = false; //Drawn mirrored
	bool FlipV = false;
};

class SnesHdNeighbourRecorder
{
private:
	string _path;
	std::unordered_map<uint64_t, uint32_t> _numbers; //Tile hash -> number
	vector<string> _tiles; //"bpp hex", by number - 1
	struct Screen
	{
		uint32_t Weight = 0;
		uint16_t Columns = 0;
		uint16_t Rows = 0;
		vector<uint32_t> Cells; //tile number (0 = none) | flipH << 16 | flipV << 17
	};
	std::unordered_map<uint64_t, size_t> _screenIndex; //Hash of a grid -> position in _screens
	vector<Screen> _screens;
	uint32_t _frames = 0;
	bool _dirty = false;

public:
	static unique_ptr<SnesHdNeighbourRecorder> Create(bool nes);

	//The number of a tile (1 and up, 0 when there are too many to number), described by "describe" the first time it is seen
	uint32_t TileNumber(uint64_t hash, const std::function<string()>& describe);

	bool Due(uint32_t frame) const { return frame % 3 == 0; }

	//The tiles of a frame of SnesHdFrameData cells (see SnesHdCell): only those drawn as a background tile
	//The tiles of a frame of SnesHdFrameData cells (see SnesHdCell): only those drawn as a background tile
	void FrameFromCells(const uint32_t* cells, uint32_t width, uint32_t height);
	void Frame(const vector<SnesHdNeighbourTile>& tiles);
	void Flush();
};
