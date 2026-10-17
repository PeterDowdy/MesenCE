#pragma once
#include "pch.h"
#include <map>
#include "SNES/HdPacks/SnesHdPack.h"

//One OAM entry as the HD layer sees it: a block of 8x8 tiles drawn at a position, with flips and a palette
struct SnesHdOamEntry
{
	uint8_t Index = 0;
	int16_t X = 0;
	int16_t Y = 0; //The frame buffer row of its top
	uint8_t Width = 8;
	uint8_t Height = 8;
	uint8_t Palette = 0;
	uint8_t Priority = 0;
	bool FlipH = false;
	bool FlipV = false;
	uint64_t Signature = 0; //Hash of the CHR data of its tiles, in the order they're stored (not flipped)
	uint8_t CellCount = 0;
	uint16_t Cells[64] = {}; //Word address in VRAM of each tile, row by row, as stored
};

//Where an entry's rectangle ends up in an object's box when the box is mirrored
constexpr int SnesHdMirror(int position, int size, int boxSize, bool mirrored)
{
	return mirrored ? boxSize - (position + size) : position;
}

class SnesHdObjectMatcher
{
public:
	//Finds the pack's objects in OAM. Entries that belong to one are marked in "consumed" (by OAM index)
	static void Match(const SnesHdPack& pack, const vector<SnesHdOamEntry>& entries, vector<SnesHdObjectInstance>& instances, bool consumed[128]);
};

//Writes the objects the game draws, as the pack's <object> and <entry> tags need them (MESEN_SNES_HD_OBJECTS=<folder>):
//  objects.txt  one "O" line per distinct object (id, how often it was seen, its box) and one "E" line per entry
//  png/<id>.png the object at the original resolution, in the palette it was first seen with
//  png/<id>_idx.png  R = the entry that drew each pixel, G = its color index (0 = none)
//  frames.txt   "F <frame> <id> <x> <y> <orientation>" for every object on screen in every frame
//Objects are groups of OAM entries that are next to each other in OAM and on screen. An object that is drawn mirrored
//is the same object: the files hold it in whichever orientation sorts first.
class SnesHdObjectRecorder
{
private:
	struct ObjectRecord
	{
		uint32_t Id = 0;
		uint32_t Seen = 0;
		uint32_t FirstFrame = 0;
		uint32_t LastFrame = 0;
		uint16_t Width = 0;
		uint16_t Height = 0;
		vector<SnesHdEntrySpec> Entries;
	};

	string _folder;
	std::map<string, ObjectRecord> _records;
	string _frameLog;
	bool _dirty = false;

public:
	static unique_ptr<SnesHdObjectRecorder> Create();

	void Record(uint32_t frame, const vector<SnesHdOamEntry>& entries, const uint16_t* vram, const uint16_t* cgram);
	void Flush();
};
