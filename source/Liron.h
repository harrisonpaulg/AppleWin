#pragma once

/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2010, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define NoDrive 0x28
#define OffLine 0x2F
#define IOError 0x27
#define WriteProt 0x2B
#define NoError	0x00
#define BadUnit 0x21 //???

enum LironUnit_e
{
	Liron_1 = 0,
	Liron_2,
	Liron_3,
	Liron_4,
	Num_Lirons
};

static void Liron_CleanupDrive(const int Unit);
static void LironImageInvalid(TCHAR* FileName);
void Liron_LoadLastDiskImage(const int Unit);
static void Liron_SaveLastDiskImage(const int Unit);
bool Liron_CardIsEnabled(void);
void Liron_SetEnabled(const bool bEnabled);
LPCTSTR Liron_GetFullName(const int Unit);
LPCTSTR Liron_GetFullPathName(const int Unit);
LPCTSTR Liron_DiskGetBaseName(const int Unit);
bool Liron_GetProtect(const int Unit);
void Liron_SetProtect(const int Unit, const bool bWriteProtect);
bool Liron_ImageIsWriteProtected(const int Unit);
void Liron_Reset(void);
void Liron_Load_Rom(const LPBYTE pCxRomPeripheral, const UINT uSlot);
void Liron_Destroy(void);
BOOL Liron_Insert(const int Unit, LPCTSTR FileName);
static bool Liron_SelectImage(const int Unit, LPCSTR pszFilename);
bool Liron_Select(const int Unit);
void Liron_Unplug(const int Unit);
bool Liron_IsDriveUnplugged(const int Unit);
static BYTE __stdcall Liron_IO_Handler(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles);
void do_SmartPort(void);
void do_ProDOS(void);
int do_read(int Unit, unsigned int buffer, unsigned int blk);
int do_write(int Unit, unsigned int buf, int blk);
int do_format(int Unit);
BYTE read_byte(WORD addr);
void write_byte(WORD addr, BYTE data);
