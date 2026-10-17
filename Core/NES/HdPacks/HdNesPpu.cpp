#include "pch.h"
#include "NES/HdPacks/HdNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/HdPacks/HdPackConditions.h"
#include "NES/NesMemoryManager.h"
#include "NES/BaseMapper.h"
#include "NES/HdPacks/HdData.h"

HdNesPpu::HdNesPpu(NesConsole* console, HdPackData* hdData) : NesPpu(console)
{
	_hdData = hdData;
	_version = _hdData ? _hdData->Version : 100;
	_isChrRam = !_console->GetMapper()->HasChrRom();
	_screenInfo[0] = new HdScreenInfo(_isChrRam);
	_screenInfo[1] = new HdScreenInfo(_isChrRam);
	_info = _screenInfo[0];
	if(NesHdRecorders::Requested()) {
		_hdLayer.reset(new NesHdLayer(_console->GetMapper(), nullptr));
		_hdRecorders.reset(new NesHdRecorders(_hdLayer.get(), _console->GetMapper(), _console->GetNesConfig()));
	}
	_forceRemoveSpriteLimit = _hdData && (_hdData->OptionFlags & (int)HdPackOptions::NoSpriteLimit) != 0;
}

HdNesPpu::~HdNesPpu()
{
	delete _screenInfo[0];
	delete _screenInfo[1];
}

void* HdNesPpu::OnBeforeSendFrame()
{
	HdScreenInfo* info = _info;
	info->FrameNumber = _frameCount;
	info->WatchedAddressValues.clear();
	memcpy(info->Oam, _spriteRam, sizeof(info->Oam));
	for(int i = 0; i < 32; i++) {
		info->PaletteRam[i] = _paletteRam[i];
	}
	info->BackgroundEnabled = _mask.BackgroundEnabled;
	info->BackgroundPattern = _control.BackgroundPatternAddr;
	info->SpritesEnabled = false;
	for(int row = 0; row < 240; row++) {
		info->SpritesEnabled |= info->SpritesRow[row];
	}
	info->SpriteLeftMask = !_mask.SpriteMask;

	if(_hdRecorders) {
		_hdRecorders->Record(*info, _currentOutputBuffer);
		if(_frameCount % 300 == 0) {
			_hdRecorders->Flush();
		}
	}

	static const unordered_set<uint32_t> noAddresses;
	for(uint32_t address : (_hdData ? _hdData->WatchedMemoryAddresses : noAddresses)) {
		if(address & HdPackBaseMemoryCondition::PpuMemoryMarker) {
			if((address & 0x3FFF) >= 0x3F00) {
				info->WatchedAddressValues[address] = ReadPaletteRam(address);
			} else {
				info->WatchedAddressValues[address] = _console->GetMapper()->DebugReadVram(address & 0x3FFF, true);
			}
		} else {
			info->WatchedAddressValues[address] = _console->GetMemoryManager()->DebugRead(address);
		}
	}

	_info = (_info == _screenInfo[0]) ? _screenInfo[1] : _screenInfo[0];
	memset(_info->SpriteTile, 0xFF, sizeof(_info->SpriteTile));
	memset(_info->SpritesRow, 0, sizeof(_info->SpritesRow));

	return info;
}
