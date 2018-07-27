/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2015, Tom Charlesworth, Michael Pohoreski

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

/* Description: Hard drive emulation
 *
 * Author: Copyright (c) 2005, Robert Hoem
 */

/* Liron Controller - SmartPort with 4 drives 
 *
 * Modifications Copyright (c) 2018, Paul Harrison
 * with additions form GSport, Copyright (c) 2010 - 2012 by GSport contributors
 * Based on the KEGS emulator written by and Copyright (C) 2003 Kent Dickey
 *
 */
 
#include "StdAfx.h"

#include "Applewin.h"
#include "DiskImage.h"	// ImageError_e, Disk_Status_e
#include "DiskImageHelper.h"
#include "Frame.h"
#include "Liron.h"
#include "Memory.h"
#include "Registry.h"
#include "YamlHelper.h"

#include "../resource/resource.h"

/*
Memory map:

    C0F0	 (r)	EXECUTE PRODOS
	C0F1	 (r)	EXECUTE SMARTPORT
	C0FC	 (r)	FLAGS
	C0FD (r)	Y
	C0FE	 (r)	X
	C0FF	 (r)	A
*/

struct LironDrive
{
	LironDrive()
	{
		clear();
	}

	void clear()
	{
		// This is not a POD (there is a std::string)
		// ZeroMemory does not work
		ZeroMemory(Name, sizeof(Name));
		ZeroMemory(FullName, sizeof(FullName));
		zipUnused.clear();
		hImage = NULL;
		bWriteProtected = false;
		bImageLoaded = false;
	}

	// From Disk_t
	TCHAR	Name[ MAX_DISK_IMAGE_NAME + 1 ];	// <FILENAME> (ie. no extension)    [not used]
	TCHAR	FullName[ MAX_DISK_FULL_NAME  + 1 ];	// <FILENAME.EXT> or <FILENAME.zip>
	std::string zipUnused;					// ""             or <FILENAME.EXT> [not used]
	ImageInfo*	hImage;			// Init'd by Liron_Insert() -> ImageOpen()
	bool	bWriteProtected;			// Needed for ImageOpen() [otherwise not used]
	bool	bImageLoaded;
};

#if Liron_LED
Disk_Status_e g_Liron_status_next = DISK_STATUS_OFF;
Disk_Status_e g_Liron_status_prev = DISK_STATUS_OFF;
#endif
static bool	g_bLiron_RomLoaded = false;
static bool g_bLiron_Enabled = false;
static LironDrive g_LironUnit[Num_Lirons];
static bool g_bSaveLironImage = true;	// Save the DiskImage name to Registry
static UINT g_LironSlot = 5;
BYTE g_Flags = 0;
BYTE g_Y = 0;
BYTE g_X = 0;
BYTE g_A = 0;

//===========================================================================

static void Liron_SaveLastDiskImage(const int Unit);

static void Liron_CleanupDrive(const int Unit)
{
	if (g_LironUnit[Unit].hImage)
	{
		ImageClose(g_LironUnit[Unit].hImage);
		g_LironUnit[Unit].hImage = NULL;
	}
	g_LironUnit[Unit].bImageLoaded = false;
	g_LironUnit[Unit].Name[0] = 0;
	g_LironUnit[Unit].FullName[0] = 0;
	g_LironUnit[Unit].zipUnused = "";
	Liron_SaveLastDiskImage(Unit);
}

//-----------------------------------------------------------------------------

static void LironImageInvalid(TCHAR* FileName)
{
	// TC: TO DO
}

//===========================================================================

BOOL Liron_Insert(const int Unit, LPCTSTR FileName);

void Liron_LoadLastDiskImage(const int Unit)
{
	_ASSERT(Unit == Liron_1 || Unit == Liron_2 || Unit == Liron_3 || Unit == Liron_4);

	char sFilePath[ MAX_PATH + 1];
	sFilePath[0] = 0;

	const char *pRegKey = 	(Unit > Liron_1) ?
										((Unit > Liron_2) ?
										((Unit > Liron_3) ?
										REGVALUE_PREF_LAST_LIRON_4
										: REGVALUE_PREF_LAST_LIRON_3)
										: REGVALUE_PREF_LAST_LIRON_2)
										: REGVALUE_PREF_LAST_LIRON_1;
	if (RegLoadString(TEXT(REG_PREFS), pRegKey, 1, sFilePath, MAX_PATH))
	{
		sFilePath[ MAX_PATH ] = 0;
		g_bSaveLironImage = false;
		Liron_Insert(Unit, sFilePath);
		g_bSaveLironImage = true;
	}
}

//===========================================================================

static void Liron_SaveLastDiskImage(const int Unit)
{
	_ASSERT(Unit == Liron_1 || Unit == Liron_2 || Unit == Liron_3 || Unit == Liron_4);

	if (!g_bSaveLironImage)
	{
		return;
	}
	const char *pFileName = g_LironUnit[Unit].FullName;

	if (Unit == Liron_1)
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_LIRON_1, TRUE, pFileName);
	else if (Unit == Liron_2)
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_LIRON_2, TRUE, pFileName);
	else if (Unit == Liron_3)
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_LIRON_3, TRUE, pFileName);
	else
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_LIRON_4, TRUE, pFileName);

	char szPathName[MAX_PATH];
	strcpy(szPathName, Liron_GetFullPathName(Unit));

	if (_tcsrchr(szPathName, TEXT('\\')))
	{
		char* pPathEnd = _tcsrchr(szPathName, TEXT('\\'))+1;
		*pPathEnd = 0;
		RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LIRON_START_DIR), 1, szPathName);
	}
}

//===========================================================================

// (Nearly) everything below is global

static BYTE __stdcall Liron_IO_Handler(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles);

static const DWORD LironROM_Size = APPLE_SLOT_SIZE;

bool Liron_CardIsEnabled(void)
{
	return g_bLiron_RomLoaded && g_bLiron_Enabled;
}

// Called by:
// . LoadConfiguration() - Done at each restart
// . RestoreCurrentConfig() - Done when Config dialog is cancelled
// . Snapshot_LoadState_v2() - Done to default to disabled state
void Liron_SetEnabled(const bool bEnabled)
{
	g_bLiron_Enabled = bEnabled;
}

//-------------------------------------

LPCTSTR Liron_GetFullName(const int Unit)
{
	return g_LironUnit[Unit].FullName;
}

LPCTSTR Liron_GetFullPathName(const int Unit)
{
	return ImageGetPathname(g_LironUnit[Unit].hImage);
}

LPCTSTR Liron_DiskGetBaseName(const int Unit)
{
	return g_LironUnit[Unit].Name;
}

bool Liron_GetProtect(const int Unit)
{
	return (Liron_IsDriveUnplugged(Unit)) ? false : g_LironUnit[Unit].bWriteProtected;
}

//===========================================================================

void Liron_SetProtect(const int Unit, const bool bWriteProtect)
{
	if (!Liron_IsDriveUnplugged(Unit))
	{
		g_LironUnit[Unit].bWriteProtected = bWriteProtect;
	}
}

//===========================================================================

bool Liron_ImageIsWriteProtected(const int Unit)
{
	return (Liron_IsDriveUnplugged(Unit)) ? true : ImageIsWriteProtected(g_LironUnit[Unit].hImage);
}

//-------------------------------------

void Liron_Reset(void)
{
// Nothing to do
}

//-------------------------------------

void Liron_Load_Rom(const LPBYTE pCxRomPeripheral, const UINT uSlot)
{
	Liron_SetEnabled(true);
	
	HRSRC hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_LIRON_FW), "FIRMWARE");
	if(hResInfo == NULL)
		return;

	DWORD dwResSize = SizeofResource(NULL, hResInfo);
	if(dwResSize != LironROM_Size)
		return;

	HGLOBAL hResData = LoadResource(NULL, hResInfo);
	if(hResData == NULL)
		return;

	BYTE* pData = (BYTE*) LockResource(hResData);	// NB. Don't need to unlock resource
	if(pData == NULL)
		return;

	g_LironSlot = uSlot;
	memcpy(pCxRomPeripheral + uSlot*256, pData, LironROM_Size);
	g_bLiron_RomLoaded = true;

	RegisterIoHandler(g_LironSlot, Liron_IO_Handler, Liron_IO_Handler, NULL, NULL, NULL, NULL);
}

void Liron_Destroy(void)
{
	g_bSaveLironImage = false;
	Liron_CleanupDrive(Liron_1);
	g_bSaveLironImage = false;
	Liron_CleanupDrive(Liron_2);
	g_bSaveLironImage = false;
	Liron_CleanupDrive(Liron_3);
	g_bSaveLironImage = false;
	Liron_CleanupDrive(Liron_4);
	g_bSaveLironImage = true;
}

// Pre: FileName is qualified with path
BOOL Liron_Insert(const int Unit, LPCTSTR FileName)
{
	if (*FileName == 0x00)
	{
		return FALSE;
	}
	if (g_LironUnit[Unit].bImageLoaded)
	{
		Liron_Unplug(Unit);
	}
	// Check if image is being used by the other LironDrive, and unplug it in order to be swapped
	for(int i =0; i < Num_Lirons; i++)
	{
		if (i != Unit)
		{
			const char* pszOtherPathname = Liron_GetFullPathName(i);
			char szCurrentPathname[MAX_PATH]; 
			DWORD uNameLen = GetFullPathName(FileName, MAX_PATH, szCurrentPathname, NULL);
			if (uNameLen == 0 || uNameLen >= MAX_PATH)
			{
				strcpy_s(szCurrentPathname, MAX_PATH, FileName);
			}
			if (!strcmp(pszOtherPathname, szCurrentPathname))
			{
				Liron_Unplug(!Unit);
				FrameRefreshStatus(DRAW_LEDS);
			}
		}
	}
	const bool bCreateIfNecessary = false;
	const bool bExpectFloppy = false;
	const bool bIsHarddisk = true;
	ImageError_e Error = ImageOpen(FileName,
		&g_LironUnit[Unit].hImage,
		&g_LironUnit[Unit].bWriteProtected,
		bCreateIfNecessary,
		g_LironUnit[Unit].zipUnused,
		bExpectFloppy);
	g_LironUnit[Unit].bImageLoaded = (Error == eIMAGE_ERROR_NONE);
#if Liron_LED
	g_LironUnit[Unit].Liron_status_next = DISK_STATUS_OFF;
	g_LironUnit[Unit].Liron_status_prev = DISK_STATUS_OFF;
#endif
	if (Error == eIMAGE_ERROR_NONE)
	{
		GetImageTitle(FileName, g_LironUnit[Unit].Name, g_LironUnit[Unit].FullName);
	}
	Liron_SaveLastDiskImage(Unit);
	return g_LironUnit[Unit].bImageLoaded;
}

static bool Liron_SelectImage(const int Unit, LPCSTR pszFilename)
{
	TCHAR directory[MAX_PATH] = TEXT("");
	TCHAR filename[MAX_PATH]  = TEXT("");
	TCHAR title[40];

	strcpy(filename, pszFilename);
	RegLoadString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LIRON_START_DIR), 1, directory, MAX_PATH);
	_tcscpy(title, TEXT("Select Image For LironDrive "));
	_tcscat(title, (Unit > 0) ? ((Unit > 1) ? ((Unit > 2) ? TEXT("4") : TEXT("3")) : TEXT("2")) : TEXT("1"));

	_ASSERT(sizeof(OPENFILENAME) == sizeof(OPENFILENAME_NT4));	// Required for Win98/ME support (selected by _WIN32_WINNT=0x0400 in stdafx.h)

	OPENFILENAME ofn;
	ZeroMemory(&ofn,sizeof(OPENFILENAME));
	ofn.lStructSize     = sizeof(OPENFILENAME);
	ofn.hwndOwner       = g_hFrameWindow;
	ofn.hInstance       = g_hInstance;
	ofn.lpstrFilter     = TEXT("3.5\" Disk Images (*.hdv,*.po,*.2mg,*.2img,*.gz,*.zip)\0*.hdv;*.po;*.2mg;*.2img;*.gz;*.zip\0")
						  TEXT("All Files\0*.*\0");
	ofn.lpstrFile       = filename;
	ofn.nMaxFile        = MAX_PATH;
	ofn.lpstrInitialDir = directory;
	ofn.Flags           = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;	// Don't allow creation & hide the read-only checkbox
	ofn.lpstrTitle      = title;

	bool bRes = false;

	if (GetOpenFileName(&ofn))
	{
		if ((!ofn.nFileExtension) || !filename[ofn.nFileExtension])
		{
			_tcscat(filename,TEXT(".po"));
		}
		if (Liron_Insert(Unit, filename))
		{
			bRes = true;
		}
		else
		{
			LironImageInvalid(filename);
		}
	}
	return bRes;
}

bool Liron_Select(const int Unit)
{
	return Liron_SelectImage(Unit, TEXT(""));
}

void Liron_Unplug(const int Unit)
{
	if (g_LironUnit[Unit].bImageLoaded)
		Liron_CleanupDrive(Unit);
}

bool Liron_IsDriveUnplugged(const int Unit)
{
	return g_LironUnit[Unit].bImageLoaded == false;
}

//-----------------------------------------------------------------------------

static BYTE __stdcall Liron_IO_Handler(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	addr &= 0xF;
	if (bWrite == 0) // read
	{
#if Liron_LED
		g_Liron_status_next = DISK_STATUS_READ;
		if( g_Liron_status_prev != g_Liron_status_next ) // Update LEDs if state changes
		{
			g_Liron_status_prev = g_Liron_status_next;
			FrameRefreshStatus(DRAW_LEDS);
		}
#endif
		switch (addr)
		{
			case 0x0:
				do_ProDOS();
				return 0;
			case 0x1:
				do_SmartPort();
				return 0;
			case 0xC:
				return g_Flags;
			case 0xD:
				return g_Y;
			case 0xE:
				return g_X;
			case 0xF:
#if Liron_LED
				g_Liron_status_next = DISK_STATUS_OFF;
				if( g_Liron_status_prev != g_Liron_status_next ) // Update LEDs if state changes
				{
					g_Liron_status_prev = g_Liron_status_next;
					FrameRefreshStatus(DRAW_LEDS);
				}
#endif
				return g_A;
			default:
#if Liron_LED
				g_Liron_status_next = DISK_STATUS_OFF;
				if( g_Liron_status_prev != g_Liron_status_next ) // Update LEDs if state changes
				{
					g_Liron_status_prev = g_Liron_status_next;
					FrameRefreshStatus(DRAW_LEDS);
				}
#endif
				return IO_Null(pc, addr, bWrite, d, nExecutedCycles);
		}
	}
	return IO_Null(pc, addr, bWrite, d, nExecutedCycles);
}

//===========================================================================

void do_SmartPort(void)
{
	unsigned int	cmd_list;
	byte	cmd;
	byte	Unit;
	unsigned int	status_ptr;
	int	buf_ptr;
	int	block;
	byte	status_code;
	byte	ctl_code;
	int	stat_val;
	int	size;

	cmd_list = 0x42;
	cmd = read_byte(cmd_list + 0);
	switch(cmd) {
	case 0x00:	/* Status */
		Unit = read_byte(cmd_list+1);
		status_code = read_byte(cmd_list+4);
		status_ptr = (read_byte(cmd_list+3) << 8) + read_byte(cmd_list+2);

		if(Unit == 0 && status_code == 0) {
			/* Smartport driver status */
			/* see technotes/smpt/tn-smpt-002 */
			write_byte(status_ptr+0, 0x04); /* number of Units*/
			write_byte(status_ptr+1, 0xFF); /* interrupt stat*/
			write_byte(status_ptr+2, 0x02); /* vendor id */
			write_byte(status_ptr+3, 0x00); /* vendor id */
			write_byte(status_ptr+4, 0x00); /* version */
			write_byte(status_ptr+5, 0x10); /* version */
			write_byte(status_ptr+6, 0x00);
			write_byte(status_ptr+7, 0x00);
			g_X = 8;
			g_Y = 0;
			g_A = 0;
			g_Flags = 0x30;
			return;
		} else if(Unit > 0 && status_code == 0) {
			/* status for Unit x */
			if(Unit > 4 || (!g_LironUnit[Unit-1].hImage)){
				stat_val = 0x80;
				size = 0;
			} else {
				stat_val = 0xf8;
				size = ImageGetImageSize(g_LironUnit[Unit-1].hImage);
				size = size / 0x200;
			}
			write_byte(status_ptr, stat_val);
			write_byte(status_ptr + 1, size & 0xFF);
			write_byte(status_ptr + 2, (size >> 8) & 0xFF);
			write_byte(status_ptr + 3, (size >> 16) & 0xFF);
			g_X = 4;
			g_Y = 0;
			g_A = 0;
			g_Flags = 0x30;
			return;
		} else if(status_code == 3) {
			if(Unit > 4 || (!g_LironUnit[Unit-1].bImageLoaded)){
				stat_val = 0x80;
				size = 0;
			} else {
				stat_val = 0xf8;
				size = ImageGetImageSize(g_LironUnit[Unit-1].hImage) / 0x200;
			}
			/* DIB for Unit 1 */
			write_byte(status_ptr, stat_val);
			write_byte(status_ptr + 1, size & 0xFF);
			write_byte(status_ptr + 2, (size >> 8) & 0xFF);
			write_byte(status_ptr + 3, (size >> 16) & 0xFF);
			write_byte(status_ptr + 4, 16);
			write_byte(status_ptr + 5, 'M');
			write_byte(status_ptr + 6, 'U');
			write_byte(status_ptr + 7, 'L');
			write_byte(status_ptr + 8, 'T');
			write_byte(status_ptr + 9, 'I');
			write_byte(status_ptr + 10, 'D');
			write_byte(status_ptr + 11, 'R');
			write_byte(status_ptr + 12, 'I');
			write_byte(status_ptr + 13, 'V');
			write_byte(status_ptr + 14, 'E');
			write_byte(status_ptr + 15, ' ');
			write_byte(status_ptr + 16, 'I');
			write_byte(status_ptr + 17, 'I');
			write_byte(status_ptr + 18, 'P');
			write_byte(status_ptr + 19, 'R');
			write_byte(status_ptr + 20, 'O');
			write_byte(status_ptr + 21, 0x02);
			write_byte(status_ptr + 22, 0xa0);
			write_byte(status_ptr + 23, 0x00);
			write_byte(status_ptr + 24, 0x00);
			g_X = 25;
			g_Y = 0;
			g_A = 0;
			g_Flags = 0x30;

			if(Unit == 0 || Unit > 4) {
				g_A = NoDrive;
				g_Flags = 0x31;
			}
			return;
		}
		return;
	case 0x01:	/* Read Block  */
	case 0x02:	/* Write Block  */
	case 0x03:	/* Format  */
		Unit = read_byte(cmd_list+1);
		buf_ptr = (read_byte(cmd_list+3) << 8) + read_byte(cmd_list+2);
		block = (read_byte(cmd_list+5) << 8) + read_byte(cmd_list+4);
		if(Unit < 1 || Unit > 4) {
			g_A = BadUnit;
			g_Flags = 0x31;
			return;
		}
		switch (cmd) {
			case 1:
				g_A = do_read(Unit - 1, buf_ptr, block);
				break;
			case 2:
				g_A = do_write(Unit - 1, buf_ptr, block);
				break;
			case 3:
				g_A = do_format(Unit - 1);
		g_X = 0;
		g_Y = 2;
		g_Flags = 0x30;
		if(g_A != 0)
		{
			g_Flags = 0x31;
		}
		return;
	case 0x04:	/* Control  */
		Unit = read_byte((cmd_list+1));
		ctl_code = read_byte((cmd_list +4));
		g_X = 0;
		g_Y = 0;
		g_A = 0;
		g_Flags = 0x30;
		// Test if ctl_code is valid for device and adjust A and Flags
		return;
	default:	/* Unknown command! */
		g_X = 0;
		g_Y = 0;
		g_A = 1;
		g_Flags = 0x31;	/* set carry */
		return;
		}
	}
}

void do_ProDOS(void)
{
	int	cmd, Unit;
	int	blk, buf;
	int	prodos_Unit;
	int	size;

	cmd = read_byte(0x42);
	prodos_Unit = read_byte(0x43);
	blk = (read_byte(0x47) << 8) + read_byte(0x46);
	buf = (read_byte(0x45) << 8) + read_byte(0x44);
	if((prodos_Unit & 0x7f) == 0x50) {
		Unit = 0 + (prodos_Unit >> 7);
	} else if((prodos_Unit & 0x7f) == 0x20) {
		Unit = 2 + (prodos_Unit >> 7);
	} else {
		g_A = BadUnit;
		g_Flags = 0x31;
		return;
	}
	g_A = IOError;
	if(cmd == 0x00) {
		size = ImageGetImageSize(g_LironUnit[Unit].hImage) / 0x200;
		g_A = 0;
		g_X = size & 0xff;
		g_Y = size >> 8;
	} else if(cmd == 0x01) {
		g_A = do_read(Unit, buf, blk);
	} else if(cmd == 0x02) {
		g_A = do_write(Unit, buf, blk);
	} else if(cmd == 0x03) {
		g_A = do_format(Unit);
	}
	if(g_A != 0) {
		g_Flags = 0x31;
	}
	return;
}

int do_read(int Unit, unsigned int buffer, unsigned int blk)
{
	byte	lBuffer[0x200];
	int	i;

	if(Unit < 0 || Unit > 3)
	{
		return NoDrive;
	}
	if(!g_LironUnit[Unit].hImage)
	{
		return OffLine;
	}
	if((blk * 0x200) >= ImageGetImageSize(g_LironUnit[Unit].hImage))
	{
		return IOError;
	}
	if(!ImageReadBlock(g_LironUnit[Unit].hImage, blk, lBuffer))
	{
		return IOError;
	}
	for(i = 0; i < 0x200; i ++)
	{
		write_byte(buffer + i, lBuffer[i]);
	}
	return 0;
}

int do_write(int Unit, unsigned int buf, int blk)
{
	BYTE	lBuffer[0x200];
	BYTE	*ptr;
	int	i;

	if(Unit < 0 || Unit > 3)
	{
		return NoDrive;
	}
	if(!g_LironUnit[Unit].bImageLoaded) {
		return NoDrive;
	}
	if((blk * 0x200) >= ImageGetImageSize(g_LironUnit[Unit].hImage))
	{
		return IOError;
	}
	if(g_LironUnit[Unit].bWriteProtected)
	{
		return WriteProt;
	}
	ptr = &(lBuffer[0]);
	for(i = 0; i < 0x200; i ++)
	{
		*ptr++ = read_byte(buf + i);
	}
	if(!ImageWriteBlock(g_LironUnit[Unit].hImage, blk, lBuffer))
	{
		return IOError;
	}
	return 0;
}

int do_format(int Unit)
{
	byte	lBuffer[0x200];
	int	len;
	int	i;

	if(Unit < 0 || Unit > 3)
	{
		return NoDrive;
	}
	if(!g_LironUnit[Unit].bImageLoaded)
	{
		return NoDrive;
	}
	if(g_LironUnit[Unit].bWriteProtected)
	{
		return WriteProt;
	}
	len = (ImageGetImageSize(g_LironUnit[Unit].hImage)) / 0x200;
	for(i = 0; i < 0x200; i++)
	{
		lBuffer[i] = 0;
	}
	for(i=0; i < len; i++)
	{
		if(!ImageWriteBlock(g_LironUnit[Unit].hImage, i, lBuffer))
		{
			return IOError;
		}
	}
	return 0;
}

BYTE read_byte(WORD addr)
{
	return (((addr & 0xF000) == 0xC000) ?
		IORead[(addr >> 4) & 0xFF](0xC555, addr, 0, 0, 0) :
		*(mem + addr));
}

void write_byte(WORD addr, BYTE data)
{
   memdirty[addr >> 8] = 0xFF;
   LPBYTE page = memwrite[addr >> 8];
   if (page)
     *(page+(addr & 0xFF)) = (BYTE)(data);
   else if ((addr & 0xF000) == 0xC000)
     IOWrite[(addr>>4) & 0xFF](0xC555,addr,1,(BYTE)(data),0);
}
//===========================================================================

#define SS_YAML_VALUE_CARD_LIRONDRIVE "Generic LironDrive"

#define SS_YAML_KEY_CURRENT_UNIT "Current Unit"
#define SS_YAML_KEY_COMMAND "Command"

#define SS_YAML_KEY_LIRONDRIVEUNIT "Unit"
#define SS_YAML_KEY_FILENAME "Filename"
#define SS_YAML_KEY_IMAGELOADED "ImageLoaded"
#define SS_YAML_KEY_STATUS_NEXT "Status Next"
#define SS_YAML_KEY_STATUS_PREV "Status Prev"

std::string Liron_GetSnapshotCardName(void)
{
	static const std::string name(SS_YAML_VALUE_CARD_LIRONDRIVE);
	return name;
}

static void Liron_SaveSnapshotLironDriveUnit(YamlSaveHelper& yamlSaveHelper, UINT unit)
{
	YamlSaveHelper::Label label(yamlSaveHelper, "%s%d:\n", SS_YAML_KEY_LIRONDRIVEUNIT, unit);
	yamlSaveHelper.SaveString(SS_YAML_KEY_FILENAME, g_LironUnit[unit].FullName);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_IMAGELOADED, g_LironUnit[unit].bImageLoaded);
#if Liron_LED
	yamlSaveHelper.SaveUint(SS_YAML_KEY_STATUS_NEXT, g_LironUnit[unit].Liron_status_next);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_STATUS_PREV, g_LironUnit[unit].Liron_status_prev);
#endif
}

void Liron_SaveSnapshot(YamlSaveHelper& yamlSaveHelper)
{
	if (!Liron_CardIsEnabled())
	{
		return;
	}
	YamlSaveHelper::Slot slot(yamlSaveHelper, Liron_GetSnapshotCardName(), g_LironSlot, 1);
	YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);
	Liron_SaveSnapshotLironDriveUnit(yamlSaveHelper, Liron_1);
	Liron_SaveSnapshotLironDriveUnit(yamlSaveHelper, Liron_2);
	Liron_SaveSnapshotLironDriveUnit(yamlSaveHelper, Liron_3);
	Liron_SaveSnapshotLironDriveUnit(yamlSaveHelper, Liron_4);
}

static bool Liron_LoadSnapshotLironDriveUnit(YamlLoadHelper& yamlLoadHelper, UINT unit)
{
	std::string LironUnitName = std::string(SS_YAML_KEY_LIRONDRIVEUNIT) +
	(unit != Liron_1 ? 	(unit != Liron_2 ? 	(unit != Liron_3 ? 	std::string("3") : std::string("2")) : std::string("1")) 	: std::string("0"));
	if (!yamlLoadHelper.GetSubMap(LironUnitName))
	{
		throw std::string("Card: Expected key: ") + LironUnitName;
	}
	g_LironUnit[unit].FullName[0] = 0;
	g_LironUnit[unit].Name[0] = 0;
	g_LironUnit[unit].bImageLoaded = false;	// Default to false (until image is successfully loaded below)
#if Liron_LED
	g_LironUnit[unit].Liron_status_next = DISK_STATUS_OFF;
	g_LironUnit[unit].Liron_status_prev = DISK_STATUS_OFF;
#endif
	std::string filename = yamlLoadHelper.LoadString(SS_YAML_KEY_FILENAME);
	yamlLoadHelper.LoadBool(SS_YAML_KEY_IMAGELOADED);	// Consume
#if Liron_LED
	Disk_Status_e diskStatusNext = (Disk_Status_e) yamlLoadHelper.LoadUint(SS_YAML_KEY_STATUS_NEXT);
	Disk_Status_e diskStatusPrev = (Disk_Status_e) yamlLoadHelper.LoadUint(SS_YAML_KEY_STATUS_PREV);
#endif
	yamlLoadHelper.PopMap();
	yamlLoadHelper.PopMap();

	bool bResSelectImage = false;

	if (!filename.empty())
	{
		DWORD dwAttributes = GetFileAttributes(filename.c_str());
		if (dwAttributes == INVALID_FILE_ATTRIBUTES)
		{
			// Get user to browse for file
			bResSelectImage = Liron_SelectImage(unit, filename.c_str());
			dwAttributes = GetFileAttributes(filename.c_str());
		}
		bool bImageError = (dwAttributes == INVALID_FILE_ATTRIBUTES);
		if (!bImageError)
		{
			if (!Liron_Insert(unit, filename.c_str()))
			{
				bImageError = true;
			}
#if Liron_LED
			g_LironUnit[unit].Liron_status_next = diskStatusNext;
			g_LironUnit[unit].Liron_status_prev = diskStatusPrev;
#endif
		}
	}
	return bResSelectImage;
}

bool Liron_LoadSnapshot(YamlLoadHelper& yamlLoadHelper, UINT slot, UINT version, const std::string strSaveStatePath)
{
	if (slot != 5)
	{
		throw std::string("Card: wrong slot");
	}
	if (version != 1)
	{
		throw std::string("Card: wrong version");
	}
	for (UINT i=0; i<Num_Lirons; i++)
	{
		Liron_Unplug(i);
		g_LironUnit[i].clear();
	}
	bool bResSelectImage1 = Liron_LoadSnapshotLironDriveUnit(yamlLoadHelper, Liron_1);
	bool bResSelectImage2 = Liron_LoadSnapshotLironDriveUnit(yamlLoadHelper, Liron_2);
	bool bResSelectImage3 = Liron_LoadSnapshotLironDriveUnit(yamlLoadHelper, Liron_3);
	bool bResSelectImage4 = Liron_LoadSnapshotLironDriveUnit(yamlLoadHelper, Liron_4);
	if (!bResSelectImage1 && !bResSelectImage2 && !bResSelectImage3 && !bResSelectImage4)
	{
		RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LIRON_START_DIR), 1, strSaveStatePath.c_str());
	}
	Liron_SetEnabled(true);
	FrameRefreshStatus(DRAW_LEDS);
	return true;
}
