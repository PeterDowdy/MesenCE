#include "pch.h"
#include <filesystem>
#include <climits>
#include <functional>
#include <map>
#include "SNES/HdPacks/SnesHdObjects.h"
#include "Utilities/PNGHelper.h"

void SnesHdObjectMatcher::Match(const SnesHdPack& pack, const vector<SnesHdOamEntry>& entries, vector<SnesHdObjectInstance>& instances, bool consumed[128])
{
	struct Candidate
	{
		uint16_t Object;
		uint16_t Entry; //Position in "entries" of the OAM entry that has the object's first entry's tiles
	};

	vector<Candidate> candidates;
	for(uint16_t i = 0; i < entries.size(); i++) {
		auto anchor = pack.ObjectsByAnchor.find(entries[i].Signature);
		if(anchor != pack.ObjectsByAnchor.end()) {
			for(uint16_t object : anchor->second) {
				candidates.push_back({ object, i });
			}
		}
	}

	//The largest objects first, so a small one never takes the entries of a bigger one
	std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
		return pack.Objects[a.Object].Entries.size() > pack.Objects[b.Object].Entries.size();
	});

	auto positionKey = [](int x, int y) { return ((uint32_t)(x + 4096) << 16) | (uint32_t)(y + 4096); };
	std::unordered_multimap<uint32_t, uint16_t> byPosition;
	for(uint16_t i = 0; i < entries.size(); i++) {
		byPosition.insert({ positionKey(entries[i].X, entries[i].Y), i });
	}

	for(Candidate& candidate : candidates) {
		const SnesHdObject& object = pack.Objects[candidate.Object];
		const SnesHdOamEntry& first = entries[candidate.Entry];
		if(consumed[first.Index]) {
			continue;
		}

		for(uint8_t orientation = 0; orientation < 4; orientation++) {
			bool mirrorX = orientation & 1;
			bool mirrorY = orientation & 2;

			const SnesHdEntrySpec& anchor = object.Entries[0];
			if(first.FlipH != (anchor.FlipH != mirrorX) || first.FlipV != (anchor.FlipV != mirrorY)) {
				continue;
			}

			int originX = first.X - SnesHdMirror(anchor.X, anchor.Width, object.Width, mirrorX);
			int originY = first.Y - SnesHdMirror(anchor.Y, anchor.Height, object.Height, mirrorY);

			int found[SnesHdObject::MaxEntries];
			bool matched = true;
			for(size_t k = 0; k < object.Entries.size() && matched; k++) {
				const SnesHdEntrySpec& spec = object.Entries[k];
				int x = originX + SnesHdMirror(spec.X, spec.Width, object.Width, mirrorX);
				int y = originY + SnesHdMirror(spec.Y, spec.Height, object.Height, mirrorY);

				found[k] = -1;
				auto range = byPosition.equal_range(positionKey(x, y));
				for(auto it = range.first; it != range.second; ++it) {
					const SnesHdOamEntry& entry = entries[it->second];
					bool alreadyUsed = false;
					for(size_t j = 0; j < k; j++) {
						alreadyUsed |= found[j] == it->second;
					}
					if(!consumed[entry.Index] && !alreadyUsed && entry.Signature == spec.Signature && entry.Width == spec.Width && entry.Height == spec.Height &&
						entry.FlipH == (spec.FlipH != mirrorX) && entry.FlipV == (spec.FlipV != mirrorY)) {
						found[k] = it->second;
						break;
					}
				}
				matched = found[k] >= 0;
			}

			if(!matched) {
				continue;
			}

			SnesHdObjectInstance instance;
			instance.ObjectId = candidate.Object;
			instance.X = (int16_t)originX;
			instance.Y = (int16_t)originY;
			instance.Orientation = orientation;
			instance.SpritePriority = entries[found[0]].Priority;
			instance.OamIndex = 0x7F;
			for(size_t k = 0; k < object.Entries.size(); k++) {
				const SnesHdOamEntry& entry = entries[found[k]];
				consumed[entry.Index] = true;
				instance.Palettes[k] = entry.Palette;
				instance.OamIndex = std::min(instance.OamIndex, entry.Index);
			}
			instances.push_back(instance);
			break;
		}
	}
}

unique_ptr<SnesHdObjectRecorder> SnesHdObjectRecorder::Create(bool nes)
{
	const char* folder = std::getenv(nes ? "MESEN_NES_HD_OBJECTS" : "MESEN_SNES_HD_OBJECTS");
	if(!folder || !*folder) {
		return nullptr;
	}

	unique_ptr<SnesHdObjectRecorder> recorder(new SnesHdObjectRecorder());
	recorder->_folder = folder;
	std::filesystem::create_directories(std::filesystem::path(folder) / "png");
	return recorder;
}

namespace
{
	struct ObservedEntry
	{
		const SnesHdOamEntry* Oam;
		int RelX;
		int RelY;
	};

	string Encode(const SnesHdEntrySpec& spec)
	{
		char text[96];
		snprintf(text, sizeof(text), "%d,%d,%d,%d,%d,%d,%016llx;", spec.X, spec.Y, spec.Width, spec.Height, spec.FlipH, spec.FlipV, (unsigned long long)spec.Signature);
		return text;
	}

	bool SpecLess(const SnesHdEntrySpec& a, const SnesHdEntrySpec& b)
	{
		if(a.Y != b.Y) {
			return a.Y < b.Y;
		}
		if(a.X != b.X) {
			return a.X < b.X;
		}
		return a.Signature < b.Signature;
	}

	uint32_t ToArgb(uint16_t bgr555)
	{
		uint32_t r = (bgr555 & 0x1F) * 255 / 31;
		uint32_t g = ((bgr555 >> 5) & 0x1F) * 255 / 31;
		uint32_t b = ((bgr555 >> 10) & 0x1F) * 255 / 31;
		return 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

void SnesHdObjectRecorder::Record(uint32_t frame, const vector<SnesHdOamEntry>& entries, const uint16_t* vram, const uint16_t* cgram)
{
	SnesHdRecordSource source;
	source.ColorIndex = [vram](const SnesHdOamEntry& e, int sx, int sy) {
		int cellX = e.FlipH ? (e.Width / 8 - 1 - sx / 8) : sx / 8;
		int cellY = e.FlipV ? (e.Height / 8 - 1 - sy / 8) : sy / 8;
		int px = e.FlipH ? 7 - (sx % 8) : sx % 8;
		int py = e.FlipV ? 7 - (sy % 8) : sy % 8;

		uint16_t address = e.Cells[cellY * (e.Width / 8) + cellX];
		uint16_t lowWord = vram[(address + py) & 0x7FFF];
		uint16_t highWord = vram[(address + 8 + py) & 0x7FFF];
		int shift = 7 - px;
		return (uint8_t)(((lowWord >> shift) & 1) | (((lowWord >> (8 + shift)) & 1) << 1) | (((highWord >> shift) & 1) << 2) | (((highWord >> (8 + shift)) & 1) << 3));
	};
	source.Argb = [cgram](const SnesHdOamEntry& e, uint8_t color) { return ToArgb(cgram[128 + e.Palette * 16 + color]); };
	source.Reference = [cgram](const SnesHdOamEntry& e, SnesHdEntrySpec& spec) {
		spec.HasReference = true;
		for(int c = 1; c < 16; c++) {
			spec.Reference[c] = cgram[128 + e.Palette * 16 + c];
		}
	};
	Record(frame, entries, source);
}

void SnesHdObjectRecorder::Record(uint32_t frame, const vector<SnesHdOamEntry>& entries, const SnesHdRecordSource& source)
{
	_colorsPerPalette = source.ColorsPerPalette;
	_digitsPerColor = source.DigitsPerColor;

	//Group entries that touch on screen. A group that's too big to be one object is split into runs of entries that are
	//next to each other in OAM
	auto touches = [](const SnesHdOamEntry& a, const SnesHdOamEntry& b) {
		return a.X < b.X + b.Width + 1 && b.X < a.X + a.Width + 1 && a.Y < b.Y + b.Height + 1 && b.Y < a.Y + a.Height + 1;
	};

	vector<int> parent(entries.size());
	for(size_t i = 0; i < entries.size(); i++) {
		parent[i] = (int)i;
	}
	std::function<int(int)> find = [&](int i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
	for(size_t i = 0; i < entries.size(); i++) {
		for(size_t j = i + 1; j < entries.size(); j++) {
			if(touches(entries[i], entries[j])) {
				parent[find((int)i)] = find((int)j);
			}
		}
	}

	std::map<int, vector<const SnesHdOamEntry*>> groups;
	for(size_t i = 0; i < entries.size(); i++) {
		groups[find((int)i)].push_back(&entries[i]);
	}

	vector<vector<const SnesHdOamEntry*>> clusters;
	for(auto& group : groups) {
		if(group.second.size() <= SnesHdObject::MaxEntries) {
			clusters.push_back(group.second);
			continue;
		}
		for(const SnesHdOamEntry* entry : group.second) {
			bool joined = false;
			if(!clusters.empty() && clusters.back().size() < SnesHdObject::MaxEntries) {
				for(const SnesHdOamEntry* member : clusters.back()) {
					joined |= touches(*entry, *member) && entry->Index <= member->Index + 2 && member->Index <= entry->Index + 2;
				}
			}
			if(joined) {
				clusters.back().push_back(entry);
			} else {
				clusters.push_back({ entry });
			}
		}
	}

	for(auto& cluster : clusters) {
		int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
		for(const SnesHdOamEntry* e : cluster) {
			minX = std::min<int>(minX, e->X);
			minY = std::min<int>(minY, e->Y);
			maxX = std::max<int>(maxX, e->X + e->Width);
			maxY = std::max<int>(maxY, e->Y + e->Height);
		}
		int width = maxX - minX;
		int height = maxY - minY;
		if(width > 256 || height > 256) {
			continue;
		}

		//Pick the orientation whose description sorts first: the same object mirrored is one object
		string bestKey;
		uint8_t bestOrientation = 0;
		vector<SnesHdEntrySpec> bestSpecs;
		for(uint8_t orientation = 0; orientation < 4; orientation++) {
			bool mirrorX = orientation & 1;
			bool mirrorY = orientation & 2;

			vector<SnesHdEntrySpec> specs;
			for(const SnesHdOamEntry* e : cluster) {
				SnesHdEntrySpec spec;
				spec.X = (int16_t)SnesHdMirror(e->X - minX, e->Width, width, mirrorX);
				spec.Y = (int16_t)SnesHdMirror(e->Y - minY, e->Height, height, mirrorY);
				spec.Width = e->Width;
				spec.Height = e->Height;
				spec.FlipH = e->FlipH != mirrorX;
				spec.FlipV = e->FlipV != mirrorY;
				spec.Signature = e->Signature;
				specs.push_back(spec);
			}
			std::sort(specs.begin(), specs.end(), SpecLess);

			string key = std::to_string(width) + "x" + std::to_string(height) + ":";
			for(auto& spec : specs) {
				key += Encode(spec);
			}
			if(orientation == 0 || key < bestKey) {
				bestKey = key;
				bestOrientation = orientation;
				bestSpecs = specs;
			}
		}

		auto existing = _records.find(bestKey);
		if(existing == _records.end()) {
			ObjectRecord record;
			record.Id = (uint32_t)_records.size() + 1;
			record.FirstFrame = frame;
			record.Width = (uint16_t)width;
			record.Height = (uint16_t)height;
			record.Entries = bestSpecs;

			//Render it as it is now, in the orientation it's stored in
			bool mirrorX = bestOrientation & 1;
			bool mirrorY = bestOrientation & 2;
			vector<uint32_t> image(width * height, 0);
			vector<uint32_t> index(width * height, 0xFF000000);

			vector<const SnesHdOamEntry*> byIndex(cluster.begin(), cluster.end());
			std::sort(byIndex.begin(), byIndex.end(), [](const SnesHdOamEntry* a, const SnesHdOamEntry* b) { return a->Index > b->Index; });
			for(const SnesHdOamEntry* e : byIndex) {
				//Which entry of the stored description this one is
				int stored = -1;
				for(size_t k = 0; k < bestSpecs.size(); k++) {
					const SnesHdEntrySpec& spec = bestSpecs[k];
					if(spec.X == SnesHdMirror(e->X - minX, e->Width, width, mirrorX) && spec.Y == SnesHdMirror(e->Y - minY, e->Height, height, mirrorY) && spec.Signature == e->Signature) {
						stored = (int)k;
					}
				}
				if(stored < 0) {
					continue;
				}

				if(!record.Entries[stored].HasReference) {
					//The palette the art is authored against: the one the entry is first seen with
					source.Reference(*e, record.Entries[stored]);
				}

				for(int sy = 0; sy < e->Height; sy++) {
					for(int sx = 0; sx < e->Width; sx++) {
						uint8_t color = source.ColorIndex(*e, sx, sy);
						if(color == 0) {
							continue;
						}

						int bx = e->X - minX + sx;
						int by = e->Y - minY + sy;
						int ix = mirrorX ? width - 1 - bx : bx;
						int iy = mirrorY ? height - 1 - by : by;
						image[iy * width + ix] = source.Argb(*e, color);
						index[iy * width + ix] = 0xFF000000 | (stored << 16) | (color << 8);
					}
				}
			}

			char name[32];
			snprintf(name, sizeof(name), "o%05u", record.Id);
			PNGHelper::WritePNG((std::filesystem::path(_folder) / "png" / (string(name) + ".png")).string(), image.data(), width, height, 32);
			PNGHelper::WritePNG((std::filesystem::path(_folder) / "png" / (string(name) + "_idx.png")).string(), index.data(), width, height, 32);

			existing = _records.insert({ bestKey, record }).first;
		}

		ObjectRecord& record = existing->second;
		record.Seen++;
		record.LastFrame = frame;

		//Where the stored orientation's box sits on screen (its top left corner), then the sprite palette and the OAM
		//index of the entry with the lowest one: a character keeps its palette and, mostly, its place in OAM
		const SnesHdOamEntry* first = *std::min_element(cluster.begin(), cluster.end(), [](const SnesHdOamEntry* a, const SnesHdOamEntry* b) { return a->Index < b->Index; });
		char line[112];
		snprintf(line, sizeof(line), "F %u o%05u %d %d %d %d %d\n", frame, record.Id, minX, minY, bestOrientation, first->Palette, first->Index);
		_frameLog += line;
		_dirty = true;
	}
}

void SnesHdObjectRecorder::Flush()
{
	if(!_dirty) {
		return;
	}

	static const char digits[] = "0123456789abcdef";
	ofstream objects(std::filesystem::path(_folder) / "objects.txt", std::ios::out | std::ios::trunc);
	vector<const ObjectRecord*> ordered;
	for(auto& pair : _records) {
		ordered.push_back(&pair.second);
	}
	std::sort(ordered.begin(), ordered.end(), [](const ObjectRecord* a, const ObjectRecord* b) { return a->Id < b->Id; });
	for(const ObjectRecord* record : ordered) {
		char line[160];
		snprintf(line, sizeof(line), "O o%05u seen=%u first=%u last=%u w=%u h=%u\n", record->Id, record->Seen, record->FirstFrame, record->LastFrame, record->Width, record->Height);
		objects << line;
		for(const SnesHdEntrySpec& spec : record->Entries) {
			string palette;
			for(int c = 1; c <= _colorsPerPalette; c++) {
				for(int shift = (_digitsPerColor - 1) * 4; shift >= 0; shift -= 4) {
					palette += digits[(spec.Reference[c] >> shift) & 0x0F];
				}
			}
			snprintf(line, sizeof(line), "E %d %d %d %d %d %d %016llx ", spec.X, spec.Y, spec.Width, spec.Height, spec.FlipH, spec.FlipV, (unsigned long long)spec.Signature);
			objects << line << palette << "\n";
		}
	}

	ofstream frames(std::filesystem::path(_folder) / "frames.txt", std::ios::out | std::ios::trunc);
	frames << _frameLog;
	_dirty = false;
}
