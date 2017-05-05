/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2007, Tom Charlesworth, Michael Pohoreski

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

/* Description: Memory emulation
 *
 * Author: Various
 *
 * In comments, UTA2E is an abbreviation for a reference to "Understanding the Apple //e" by James Sather
 */

#include "StdAfx.h"

#include "AppleWin.h"
#include "CPU.h"
#include "Disk.h"
#include "Frame.h"
#include "Harddisk.h"
#include "Joystick.h"
#include "Keyboard.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "MouseInterface.h"
#include "NTSC.h"
#include "NoSlotClock.h"
#include "ParallelPrinter.h"
#include "Registry.h"
#include "SAM.h"
#include "SerialComms.h"
#include "Speaker.h"
#include "Tape.h"
#include "Video.h"

#include "z80emu.h"
#include "Z80VICE\z80.h"
#include "..\resource\resource.h"
#include "Configuration\PropertySheet.h"
#include "Debugger\DebugDefs.h"
#include "YamlHelper.h"

#define DEBUG_LANGUAGE_CARD 1

#define  SW_80STORE    (memmode & MF_80STORE)
#define  SW_ALTZP      (memmode & MF_ALTZP)
#define  SW_AUXREAD    (memmode & MF_AUXREAD)
#define  SW_AUXWRITE   (memmode & MF_AUXWRITE)
#define  SW_BANK2      (memmode & MF_BANK2)
#define  SW_HIGHRAM    (memmode & MF_HIGHRAM)
#define  SW_HIRES      (memmode & MF_HIRES)
#define  SW_PAGE2      (memmode & MF_PAGE2)
#define  SW_SLOTC3ROM  (memmode & MF_SLOTC3ROM)
#define  SW_SLOTCXROM  (memmode & MF_SLOTCXROM)
#define  SW_WRITERAM   (memmode & MF_WRITERAM)

/*
MEMORY MANAGEMENT SOFT SWITCHES
 $C000   W       80STOREOFF      Allow page2 to switch video page1 page2
 $C001   W       80STOREON       Allow page2 to switch main & aux video memory
 $C002   W       RAMRDOFF        Read enable main memory from $0200-$BFFF
 $C003   W       RAMDRON         Read enable aux memory from $0200-$BFFF
 $C004   W       RAMWRTOFF       Write enable main memory from $0200-$BFFF
 $C005   W       RAMWRTON        Write enable aux memory from $0200-$BFFF
 $C006   W       INTCXROMOFF     Enable slot ROM from $C100-$CFFF
 $C007   W       INTCXROMON      Enable main ROM from $C100-$CFFF
 $C008   W       ALTZPOFF        Enable main memory from $0000-$01FF & avl BSR
 $C009   W       ALTZPON         Enable aux memory from $0000-$01FF & avl BSR
 $C00A   W       SLOTC3ROMOFF    Enable main ROM from $C300-$C3FF
 $C00B   W       SLOTC3ROMON     Enable slot ROM from $C300-$C3FF

VIDEO SOFT SWITCHES
 $C00C   W       80COLOFF        Turn off 80 column display
 $C00D   W       80COLON         Turn on 80 column display
 $C00E   W       ALTCHARSETOFF   Turn off alternate characters
 $C00F   W       ALTCHARSETON    Turn on alternate characters
 $C050   R/W     TEXTOFF         Select graphics mode
 $C051   R/W     TEXTON          Select text mode
 $C052   R/W     MIXEDOFF        Use full screen for graphics
 $C053   R/W     MIXEDON         Use graphics with 4 lines of text
 $C054   R/W     PAGE2OFF        Select panel display (or main video memory)
 $C055   R/W     PAGE2ON         Select page2 display (or aux video memory)
 $C056   R/W     HIRESOFF        Select low resolution graphics
 $C057   R/W     HIRESON         Select high resolution graphics

SOFT SWITCH STATUS FLAGS
 $C010   R7      AKD             1=key pressed   0=keys free    (clears strobe)
 $C011   R7      BSRBANK2        1=bank2 available    0=bank1 available
 $C012   R7      BSRREADRAM      1=BSR active for read   0=$D000-$FFFF active
 $C013   R7      RAMRD           0=main $0200-$BFFF active reads  1=aux active
 $C014   R7      RAMWRT          0=main $0200-$BFFF active writes 1=aux writes
 $C015   R7      INTCXROM        1=main $C100-$CFFF ROM active   0=slot active
 $C016   R7      ALTZP           1=aux $0000-$1FF+auxBSR    0=main available
 $C017   R7      SLOTC3ROM       1=slot $C3 ROM active   0=main $C3 ROM active
 $C018   R7      80STORE         1=page2 switches main/aux   0=page2 video
 $C019   R7      VERTBLANK       1=vertical retrace on   0=vertical retrace off
 $C01A   R7      TEXT            1=text mode is active   0=graphics mode active
 $C01B   R7      MIXED           1=mixed graphics & text    0=full screen
 $C01C   R7      PAGE2           1=video page2 selected or aux
 $C01D   R7      HIRES           1=high resolution graphics   0=low resolution
 $C01E   R7      ALTCHARSET      1=alt character set on    0=alt char set off
 $C01F   R7      80COL           1=80 col display on     0=80 col display off
*/



//-----------------------------------------------------------------------------

// Notes
// -----
//
// mem
// - a copy of the memimage ptr
//
// memimage
// - 64KB
// - reflects the current readable memory in the 6502's 64K address space
//		. excludes $Cxxx I/O memory
//		. could be a mix of RAM/ROM, main/aux, etc
//
// memmain, memaux
// - physical contiguous 64KB RAM for main & aux respectively
//
// memwrite
// - 1 ptr entry per 256-byte page
// - used to write to a page
//
// memdirty
// - 1 byte entry per 256-byte page
// - set when a write occurs to a 256-byte page
//
// memshadow
// - 1 ptr entry per 256-byte page
// - reflects how 'mem' is setup
//		. EG: if ALTZP=1, then:
//			. mem will have copies of memaux's ZP & stack
//			. memshadow[0] = &memaux[0x0000]
//			. memshadow[1] = &memaux[0x0100]
//

static LPBYTE  memshadow[0x100];
LPBYTE         memwrite[0x100];

iofunction		IORead[256];
iofunction		IOWrite[256];
static LPVOID	SlotParameters[NUM_SLOTS];

static BOOL    g_bLastWriteRam = 0;

LPBYTE         mem          = NULL;

//

static LPBYTE  memaux       = NULL;
static LPBYTE  memmain      = NULL;

LPBYTE         memdirty     = NULL;
static LPBYTE  memrom       = NULL;

static LPBYTE  memimage     = NULL;

static LPBYTE	pCxRomInternal		= NULL;
static LPBYTE	pCxRomPeripheral	= NULL;

       DWORD   memmode      = MF_BANK2 | MF_SLOTCXROM | MF_WRITERAM; // 2.9.0.4 now global as Debugger needs access for LC status info in DrawSoftSwitches()
static BOOL    modechanging = 0;				// An Optimisation: means delay calling UpdatePaging() for 1 instruction
static BOOL    Pravets8charmode = 0;

static CNoSlotClock g_NoSlotClock;

#ifdef RAMWORKS
UINT			g_uMaxExPages = 1;				// user requested ram pages (default to 1 aux bank: so total = 128KB)
UINT			g_uActiveBank = 0;				// 0 = aux 64K for: //e extended 80 Col card, or //c -- ALSO RAMWORKS
static LPBYTE	RWpages[kMaxExMemoryBanks];		// pointers to RW memory banks
#endif

#ifdef SATURN
UINT			g_uSaturnTotalBanks = 0;		// Will be > 0 if Saturn card is "installed"
UINT			g_uSaturnActivePage = 0;		// Saturn 128K Language Card Bank 0 .. 7
static LPBYTE	g_aSaturnPages[8];
#endif // SATURN

MemoryType_e	g_eMemType = MEM_TYPE_NATIVE;		// 0 = Native memory, 1=RAMWORKS, 2 = SATURN

BYTE __stdcall IO_Annunciator(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCycles);

//=============================================================================

static BYTE __stdcall IORead_C00x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return KeybReadData(pc, addr, bWrite, d, nCyclesLeft);
}

static BYTE __stdcall IOWrite_C00x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	if ((addr & 0xf) <= 0xB)
		return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	else
		return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
}

//-------------------------------------

static BYTE __stdcall IORead_C01x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	return KeybReadFlag(pc, addr, bWrite, d, nCyclesLeft);
	case 0x1:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x2:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x4:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x5:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x6:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x7:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x8:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x9:	return VideoCheckVbl(nCyclesLeft);
	case 0xA:	return VideoCheckMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0xB:	return VideoCheckMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0xC:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0xD:	return MemCheckPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0xE:	return VideoCheckMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0xF:	return VideoCheckMode(pc, addr, bWrite, d, nCyclesLeft);
	}

	return 0;
}

static BYTE __stdcall IOWrite_C01x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return KeybReadFlag(pc, addr, bWrite, d, nCyclesLeft);
}

//-------------------------------------

static BYTE __stdcall IORead_C02x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
}

static BYTE __stdcall IOWrite_C02x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);	// $C020 TAPEOUT
}

//-------------------------------------

static BYTE __stdcall IORead_C03x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return SpkrToggle(pc, addr, bWrite, d, nCyclesLeft);
}

static BYTE __stdcall IOWrite_C03x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return SpkrToggle(pc, addr, bWrite, d, nCyclesLeft);
}

//-------------------------------------

static BYTE __stdcall IORead_C04x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
}

static BYTE __stdcall IOWrite_C04x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
}

//-------------------------------------

static BYTE __stdcall IORead_C05x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x1:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x2:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x4:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x5:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x6:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x7:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x8:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0x9:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xA:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xB:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xC:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xD:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xE:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0xF:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	}

	return 0;
}

static BYTE __stdcall IOWrite_C05x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x1:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x2:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0x4:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x5:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x6:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x7:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);
	case 0x8:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0x9:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xA:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xB:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xC:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xD:	return IO_Annunciator(pc, addr, bWrite, d, nCyclesLeft);
	case 0xE:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	case 0xF:	return VideoSetMode(pc, addr, bWrite, d, nCyclesLeft);
	}

	return 0;
}

//-------------------------------------

static BYTE __stdcall IORead_C06x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{	
	static byte CurrentKestroke = 0;
	CurrentKestroke = KeybGetKeycode();
	switch (addr & 0x7) // address bit 4 is ignored (UTA2E page 7-5)
	{
	//In Pravets8A/C if SETMODE (8bit character encoding) is enabled, bit6 in $C060 is 0; Else it is 1
	//If (CAPS lOCK of Pravets8A/C is on or Shift is pressed) and (MODE is enabled), bit7 in $C000 is 1; Else it is 0
	//Writing into $C060 sets MODE on and off. If bit 0 is 0 the the MODE is set 0, if bit 0 is 1 then MODE is set to 1 (8-bit)

	case 0x0:	return TapeRead(pc, addr, bWrite, d, nCyclesLeft);		// $C060 TAPEIN
	case 0x1:	return JoyReadButton(pc, addr, bWrite, d, nCyclesLeft);  //$C061 Digital input 0 (If bit 7=1 then JoyButton 0 or OpenApple is pressed)
	case 0x2:	return JoyReadButton(pc, addr, bWrite, d, nCyclesLeft);  //$C062 Digital input 1 (If bit 7=1 then JoyButton 1 or ClosedApple is pressed)
	case 0x3:	return JoyReadButton(pc, addr, bWrite, d, nCyclesLeft);  //$C063 Digital input 2
	case 0x4:	return JoyReadPosition(pc, addr, bWrite, d, nCyclesLeft); //$C064 Analog input 0
	case 0x5:	return JoyReadPosition(pc, addr, bWrite, d, nCyclesLeft); //$C065 Analog input 1
	case 0x6:	return JoyReadPosition(pc, addr, bWrite, d, nCyclesLeft); //$C066 Analog input 2
	case 0x7:	return JoyReadPosition(pc, addr, bWrite, d, nCyclesLeft); //$C067 Analog input 3
	}

	return 0;
}

static BYTE __stdcall IOWrite_C06x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	
		if (g_Apple2Type == A2TYPE_PRAVETS8A )
			return TapeWrite (pc, addr, bWrite, d, nCyclesLeft);			
		else
			return IO_Null(pc, addr, bWrite, d, nCyclesLeft); //Apple2 value
	}
	return IO_Null(pc, addr, bWrite, d, nCyclesLeft); //Apple2 value
}

//-------------------------------------

static BYTE __stdcall IORead_C07x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	return JoyResetPosition(pc, addr, bWrite, d, nCyclesLeft);  //$C070 Analog input reset
	case 0x1:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x2:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x4:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x5:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x6:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x7:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x8:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x9:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xA:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xB:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xC:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xD:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xE:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xF:	return VideoCheckMode(pc, addr, bWrite, d, nCyclesLeft);
	}

	return 0;
}

static BYTE __stdcall IOWrite_C07x(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nCyclesLeft)
{
	switch (addr & 0xf)
	{
	case 0x0:	return JoyResetPosition(pc, addr, bWrite, d, nCyclesLeft);
#ifdef RAMWORKS
	case 0x1:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);	// extended memory card set page
	case 0x2:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return MemSetPaging(pc, addr, bWrite, d, nCyclesLeft);	// Ramworks III set page
#else
	case 0x1:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x2:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x3:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
#endif
	case 0x4:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x5:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x6:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x7:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x8:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0x9:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xA:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xB:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xC:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);
	case 0xD:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft);

	//http://www.kreativekorp.com/miscpages/a2info/iomemory.shtml
	//- Apparently Apple//e & //c (but maybe enhanced//e not //e?)
	//IOUDISON  (W): $C07E  Disable IOU
	//IOUDISOFF (W): $C07F  Enable IOU
	//RDIOUDIS (R7): $C07E  Status of IOU Disabling
	//RDDHIRES (R7): $C07F  Status of Double HiRes
	case 0xE:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft); // TODO: IOUDIS
	case 0xF:	return IO_Null(pc, addr, bWrite, d, nCyclesLeft); // TODO: IOUDIS
	}

	return 0;
}

//-----------------------------------------------------------------------------

static iofunction IORead_C0xx[8] =
{
	IORead_C00x,		// Keyboard
	IORead_C01x,		// Memory/Video
	IORead_C02x,		// Cassette
	IORead_C03x,		// Speaker
	IORead_C04x,
	IORead_C05x,		// Video
	IORead_C06x,		// Joystick
	IORead_C07x,		// Joystick/Video
};

static iofunction IOWrite_C0xx[8] =
{
	IOWrite_C00x,		// Memory/Video
	IOWrite_C01x,		// Keyboard
	IOWrite_C02x,		// Cassette
	IOWrite_C03x,		// Speaker
	IOWrite_C04x,
	IOWrite_C05x,		// Video/Memory
	IOWrite_C06x,
	IOWrite_C07x,		// Joystick/Ramworks
};

static BYTE IO_SELECT;
static BYTE IO_SELECT_InternalROM;

static BYTE* ExpansionRom[NUM_SLOTS];

enum eExpansionRomType {eExpRomNull=0, eExpRomInternal, eExpRomPeripheral};
static eExpansionRomType g_eExpansionRomType = eExpRomNull;
static UINT	g_uPeripheralRomSlot = 0;

//=============================================================================

BYTE __stdcall IO_Null(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCyclesLeft)
{
	if (!write)
		return MemReadFloatingBus(nCyclesLeft);
	else
		return 0;
}

BYTE __stdcall IO_Annunciator(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCyclesLeft)
{
	// Apple//e ROM:
	// . PC=FA6F: LDA $C058 (SETAN0)
	// . PC=FA72: LDA $C05A (SETAN1)
	// . PC=C2B5: LDA $C05D (CLRAN2)

	// NB. AN3: For //e & //c these locations are now used to enabled/disabled DHIRES

	if (address >= 0xC058 && address <= 0xC05B)
	{
		JoyportControl(address & 0x3);	// AN0 and AN1 control
	}

	if (!write)
		return MemReadFloatingBus(nCyclesLeft);
	else
		return 0;
}

inline bool IsPotentialNoSlotClockAccess(const WORD address)
{
	// Ref: Sather UAIIe 5-28
	const BYTE AddrHi = address >>  8;
	return ( ((!SW_SLOTCXROM || !SW_SLOTC3ROM) && (AddrHi == 0xC3)) ||	// Internal ROM at [$C100-CFFF or $C300-C3FF] && AddrHi == $C3
			  (!SW_SLOTCXROM && (AddrHi == 0xC8)) );					// Internal ROM at [$C100-CFFF]               && AddrHi == $C8
}

static bool IsCardInSlot(const UINT uSlot);

// Enabling expansion ROM ($C800..$CFFF]:
// . Enable if: Enable1 && Enable2
// . Enable1 = I/O SELECT' (6502 accesses $Csxx)
//   - Reset when 6502 accesses $CFFF
// . Enable2 = I/O STROBE' (6502 accesses [$C800..$CFFF])

// TODO:
// . IO_SELECT and IO_SELECT_InternalROM are sticky - they only getting reset by $CFFF and MemReset()
// . Check Sather UAIIe, but I assume that a 6502 access to a non-$Csxx (and non-expansion ROM) location will clear IO_SELECT

// NB. ProDOS boot sets IO_SELECT=0x04 (its scan for boot devices?), as slot2 contains a card (ie. SSC) with an expansion ROM.

BYTE __stdcall IORead_Cxxx(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCyclesLeft)
{
	if (address == 0xCFFF)
	{
		// Disable expansion ROM at [$C800..$CFFF]
		// . SSC will disable on an access to $CFxx - but ROM only access $CFFF, so it doesn't matter
		IO_SELECT = 0;
		IO_SELECT_InternalROM = 0;
		g_uPeripheralRomSlot = 0;

		if (SW_SLOTCXROM)
		{
			// NB. SW_SLOTCXROM==0 ensures that internal rom stays switched in
			memset(pCxRomPeripheral+0x800, 0, FIRMWARE_EXPANSION_SIZE);
			memset(mem+FIRMWARE_EXPANSION_BEGIN, 0, FIRMWARE_EXPANSION_SIZE);
			g_eExpansionRomType = eExpRomNull;
		}

		// NB. IO_SELECT won't get set, so ROM won't be switched back in...
	}

	//

	BYTE IO_STROBE = 0;

	if (IS_APPLE2 || SW_SLOTCXROM)
	{
		if ((address >= APPLE_SLOT_BEGIN) && (address <= APPLE_SLOT_END))
		{
			const UINT uSlot = (address>>8)&0x7;
			if (uSlot != 3)
			{
				if (ExpansionRom[uSlot])
					IO_SELECT |= 1<<uSlot;
			}
			else	// slot3
			{
				if ((SW_SLOTC3ROM) && ExpansionRom[uSlot])
					IO_SELECT |= 1<<uSlot;		// Slot3 & Peripheral ROM
				else if (!SW_SLOTC3ROM)
					IO_SELECT_InternalROM = 1;	// Slot3 & Internal ROM
			}
		}
		else if ((address >= FIRMWARE_EXPANSION_BEGIN) && (address <= FIRMWARE_EXPANSION_END))
		{
			IO_STROBE = 1;
		}

		//

		if (IO_SELECT && IO_STROBE)
		{
			// Enable Peripheral Expansion ROM
			UINT uSlot=1;
			for (; uSlot<NUM_SLOTS; uSlot++)
			{
				if (IO_SELECT & (1<<uSlot))
				{
					BYTE RemainingSelected = IO_SELECT & ~(1<<uSlot);
					_ASSERT(RemainingSelected == 0);
					break;
				}
			}

			if (ExpansionRom[uSlot] && (g_uPeripheralRomSlot != uSlot))
			{
				memcpy(pCxRomPeripheral+0x800, ExpansionRom[uSlot], FIRMWARE_EXPANSION_SIZE);
				memcpy(mem+FIRMWARE_EXPANSION_BEGIN, ExpansionRom[uSlot], FIRMWARE_EXPANSION_SIZE);
				g_eExpansionRomType = eExpRomPeripheral;
				g_uPeripheralRomSlot = uSlot;
			}
		}
		else if (IO_SELECT_InternalROM && IO_STROBE && (g_eExpansionRomType != eExpRomInternal))
		{
			// Enable Internal ROM
			// . Get this for PR#3
			memcpy(mem+FIRMWARE_EXPANSION_BEGIN, pCxRomInternal+0x800, FIRMWARE_EXPANSION_SIZE);
			g_eExpansionRomType = eExpRomInternal;
			g_uPeripheralRomSlot = 0;
		}
	}

	if (IsPotentialNoSlotClockAccess(address))
	{
		int data = 0;
		if (g_NoSlotClock.Read(address, data))
			return (BYTE) data;
	}

	if (!IS_APPLE2 && !SW_SLOTCXROM)
	{
		// !SW_SLOTC3ROM = Internal ROM: $C300-C3FF
		// !SW_SLOTCXROM = Internal ROM: $C100-CFFF

		if ((address >= APPLE_SLOT_BEGIN) && (address <= APPLE_SLOT_END))	// Don't care about state of SW_SLOTC3ROM
			IO_SELECT_InternalROM = 1;
		else if ((address >= FIRMWARE_EXPANSION_BEGIN) && (address <= FIRMWARE_EXPANSION_END))
			IO_STROBE = 1;

		if (!SW_SLOTCXROM && IO_SELECT_InternalROM && IO_STROBE && (g_eExpansionRomType != eExpRomInternal))
		{
			// Enable Internal ROM
			memcpy(mem+FIRMWARE_EXPANSION_BEGIN, pCxRomInternal+0x800, FIRMWARE_EXPANSION_SIZE);
			g_eExpansionRomType = eExpRomInternal;
			g_uPeripheralRomSlot = 0;
		}
	}

	if (address >= APPLE_SLOT_BEGIN && address <= APPLE_SLOT_END)
	{
		const UINT uSlot = (address>>8)&0x7;
		const bool bPeripheralSlotRomEnabled = IS_APPLE2 ? true	// A][
													     :		// A//e or above
			  ( (SW_SLOTCXROM) &&					// Peripheral (card) ROMs enabled in $C100..$C7FF
		      !(!SW_SLOTC3ROM  && uSlot == 3) );	// Internal C3 ROM disabled in $C300 when slot == 3

		// Fix for GH#149 and GH#164
		if (bPeripheralSlotRomEnabled && !IsCardInSlot(uSlot))	// Slot is empty
		{
			return IO_Null(programcounter, address, write, value, nCyclesLeft);
		}
	}

	if ((g_eExpansionRomType == eExpRomNull) && (address >= FIRMWARE_EXPANSION_BEGIN))
		return IO_Null(programcounter, address, write, value, nCyclesLeft);

	return mem[address];
}

BYTE __stdcall IOWrite_Cxxx(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCyclesLeft)
{
	return IORead_Cxxx(programcounter, address, write, value, nCyclesLeft);	// GH#392
}

//===========================================================================

static struct SlotInfo
{
	bool bHasCard;
	iofunction IOReadCx;
	iofunction IOWriteCx;
} g_SlotInfo[NUM_SLOTS] = {0};

static void InitIoHandlers()
{
	UINT i=0;

	for (; i<8; i++)	// C00x..C07x
	{
		IORead[i]	= IORead_C0xx[i];
		IOWrite[i]	= IOWrite_C0xx[i];
	}

	for (; i<16; i++)	// C08x..C0Fx
	{
		IORead[i]	= IO_Null;
		IOWrite[i]	= IO_Null;
	}

	//

	for (; i<256; i++)	// C10x..CFFx
	{
		IORead[i]	= IORead_Cxxx;
		IOWrite[i]	= IOWrite_Cxxx;
	}

	//

	for (i=0; i<NUM_SLOTS; i++)
	{
		g_SlotInfo[i].bHasCard = false;
		g_SlotInfo[i].IOReadCx = IORead_Cxxx;
		g_SlotInfo[i].IOWriteCx = IOWrite_Cxxx;
		ExpansionRom[i] = NULL;
	}
}

// All slots [0..7] must register their handlers
void RegisterIoHandler(UINT uSlot, iofunction IOReadC0, iofunction IOWriteC0, iofunction IOReadCx, iofunction IOWriteCx, LPVOID lpSlotParameter, BYTE* pExpansionRom)
{
	_ASSERT(uSlot < NUM_SLOTS);
	SlotParameters[uSlot] = lpSlotParameter;

	IORead[uSlot+8]		= IOReadC0;
	IOWrite[uSlot+8]	= IOWriteC0;

	if (uSlot == 0)		// Don't trash C0xx handlers
		return;

	if (IOReadCx == NULL)	IOReadCx = IORead_Cxxx;
	if (IOWriteCx == NULL)	IOWriteCx = IOWrite_Cxxx;

	for (UINT i=0; i<16; i++)
	{
		IORead[uSlot*16+i]	= IOReadCx;
		IOWrite[uSlot*16+i]	= IOWriteCx;
	}

	g_SlotInfo[uSlot].bHasCard = true;
	g_SlotInfo[uSlot].IOReadCx = IOReadCx;
	g_SlotInfo[uSlot].IOWriteCx = IOWriteCx;

	// What about [$C80x..$CFEx]? - Do any cards use this as I/O memory?
	ExpansionRom[uSlot] = pExpansionRom;
}

// TODO: Support SW_SLOTC3ROM?
static void IoHandlerCardsOut(void)
{
	for (UINT uSlot=1; uSlot<NUM_SLOTS; uSlot++)
	{
		for (UINT i=0; i<16; i++)
		{
			IORead[uSlot*16+i]	= IORead_Cxxx;
			IOWrite[uSlot*16+i]	= IOWrite_Cxxx;
		}
	}
}

// TODO: Support SW_SLOTC3ROM?
static void IoHandlerCardsIn(void)
{
	for (UINT uSlot=1; uSlot<NUM_SLOTS; uSlot++)
	{
		for (UINT i=0; i<16; i++)
		{
			IORead[uSlot*16+i]	= g_SlotInfo[uSlot].IOReadCx;
			IOWrite[uSlot*16+i]	= g_SlotInfo[uSlot].IOWriteCx;
		}
	}
}

static bool IsCardInSlot(const UINT uSlot)
{
	return g_SlotInfo[uSlot].bHasCard;
}

//===========================================================================

static void BackMainImage(void)
{
	for (UINT loop = 0; loop < 256; loop++)
	{
		if (memshadow[loop] && ((*(memdirty+loop) & 1) || (loop <= 1)))
			CopyMemory(memshadow[loop], memimage+(loop << 8), 256);

		*(memdirty+loop) &= ~1;
	}
}

//===========================================================================

static void SetMemMode(const DWORD uNewMemMode)
{
#if defined(_DEBUG) && 0
	static DWORD dwOldDiff = 0;
	DWORD dwDiff = memmode ^ uNewMemMode;
	dwDiff &= ~(MF_SLOTC3ROM | MF_SLOTCXROM);
	if (dwOldDiff != dwDiff)
	{
		dwOldDiff = dwDiff;
		char szStr[100];
		char* psz = szStr;
		psz += sprintf(psz, "diff = %08X ", dwDiff);
		psz += sprintf(psz, "80=%d "   , SW_80STORE   ? 1 : 0);
		psz += sprintf(psz, "ALTZP=%d ", SW_ALTZP     ? 1 : 0);
		psz += sprintf(psz, "AUXR=%d " , SW_AUXREAD   ? 1 : 0);
		psz += sprintf(psz, "AUXW=%d " , SW_AUXWRITE  ? 1 : 0);
		psz += sprintf(psz, "BANK2=%d ", SW_BANK2     ? 1 : 0);
		psz += sprintf(psz, "HIRAM=%d ", SW_HIGHRAM   ? 1 : 0);
		psz += sprintf(psz, "HIRES=%d ", SW_HIRES     ? 1 : 0);
		psz += sprintf(psz, "PAGE2=%d ", SW_PAGE2     ? 1 : 0);
		psz += sprintf(psz, "C3=%d "   , SW_SLOTC3ROM ? 1 : 0);
		psz += sprintf(psz, "CX=%d "   , SW_SLOTCXROM ? 1 : 0);
		psz += sprintf(psz, "WRAM=%d " , SW_WRITERAM  ? 1 : 0);
		psz += sprintf(psz, "\n");
		OutputDebugString(szStr);
	}
#endif
	memmode = uNewMemMode;
}

//===========================================================================

static void ResetPaging(BOOL initialize);
static void UpdatePaging(BOOL initialize);

// Call by:
// . CtrlReset() Soft-reset (Ctrl+Reset)
void MemResetPaging()
{
	ResetPaging(0);		// Initialize=0
}

static void ResetPaging(BOOL initialize)
{
#ifdef SATURN
	// TODO: Reset back to bank 0 ?
#endif
	g_bLastWriteRam = 0;
	SetMemMode(MF_BANK2 | MF_SLOTCXROM | MF_WRITERAM);
	UpdatePaging(initialize);
}

//===========================================================================

void MemUpdatePaging(BOOL initialize)
{
	UpdatePaging(initialize);
}

static void UpdatePaging(BOOL initialize)
{
	// SAVE THE CURRENT PAGING SHADOW TABLE
	LPBYTE oldshadow[256];
	if (!initialize)
		CopyMemory(oldshadow,memshadow,256*sizeof(LPBYTE));

	// UPDATE THE PAGING TABLES BASED ON THE NEW PAGING SWITCH VALUES
	UINT loop;
	if (initialize)
	{
		for (loop = 0x00; loop < 0xC0; loop++)
			memwrite[loop] = mem+(loop << 8);

		for (loop = 0xC0; loop < 0xD0; loop++)
			memwrite[loop] = NULL;
	}

	for (loop = 0x00; loop < 0x02; loop++)
		memshadow[loop] = SW_ALTZP ? memaux+(loop << 8) : memmain+(loop << 8);

	for (loop = 0x02; loop < 0xC0; loop++)
	{
		memshadow[loop] = SW_AUXREAD ? memaux+(loop << 8)
			: memmain+(loop << 8);

		memwrite[loop]  = ((SW_AUXREAD != 0) == (SW_AUXWRITE != 0))
			? mem+(loop << 8)
			: SW_AUXWRITE	? memaux+(loop << 8)
							: memmain+(loop << 8);
	}

	for (loop = 0xC0; loop < 0xC8; loop++)
	{
		const UINT uSlotOffset = (loop & 0x0f) * 0x100;
		if (loop == 0xC3)
			memshadow[loop] = (SW_SLOTC3ROM && SW_SLOTCXROM)	? pCxRomPeripheral+uSlotOffset	// C300..C3FF - Slot 3 ROM (all 0x00's)
																: pCxRomInternal+uSlotOffset;	// C300..C3FF - Internal ROM
		else
			memshadow[loop] = SW_SLOTCXROM	? pCxRomPeripheral+uSlotOffset						// C000..C7FF - SSC/Disk][/etc
											: pCxRomInternal+uSlotOffset;						// C000..C7FF - Internal ROM
	}

	for (loop = 0xC8; loop < 0xD0; loop++)
	{
		const UINT uRomOffset = (loop & 0x0f) * 0x100;
		memshadow[loop] = pCxRomInternal+uRomOffset;											// C800..CFFF - Internal ROM
	}

	for (loop = 0xD0; loop < 0xE0; loop++)
	{
		int bankoffset = (SW_BANK2 ? 0 : 0x1000);
		memshadow[loop] = SW_HIGHRAM ? SW_ALTZP	? memaux+(loop << 8)-bankoffset
												: memmain+(loop << 8)-bankoffset
									: memrom+((loop-0xD0) * 0x100);

		memwrite[loop]  = SW_WRITERAM	? SW_HIGHRAM	? mem+(loop << 8)
														: SW_ALTZP	? memaux+(loop << 8)-bankoffset
																	: memmain+(loop << 8)-bankoffset
										: NULL;
	}

	for (loop = 0xE0; loop < 0x100; loop++)
	{
		memshadow[loop] = SW_HIGHRAM	? SW_ALTZP	? memaux+(loop << 8)
													: memmain+(loop << 8)
										: memrom+((loop-0xD0) * 0x100);

		memwrite[loop]  = SW_WRITERAM	? SW_HIGHRAM	? mem+(loop << 8)
														: SW_ALTZP	? memaux+(loop << 8)
																	: memmain+(loop << 8)
										: NULL;
	}

	if (SW_80STORE)
	{
		for (loop = 0x04; loop < 0x08; loop++)
		{
			memshadow[loop] = SW_PAGE2	? memaux+(loop << 8)
										: memmain+(loop << 8);
			memwrite[loop]  = mem+(loop << 8);
		}

		if (SW_HIRES)
		{
			for (loop = 0x20; loop < 0x40; loop++)
			{
				memshadow[loop] = SW_PAGE2	? memaux+(loop << 8)
											: memmain+(loop << 8);
				memwrite[loop]  = mem+(loop << 8);
			}
		}
	}

	// MOVE MEMORY BACK AND FORTH AS NECESSARY BETWEEN THE SHADOW AREAS AND
	// THE MAIN RAM IMAGE TO KEEP BOTH SETS OF MEMORY CONSISTENT WITH THE NEW
	// PAGING SHADOW TABLE
	//
	// NB. the condition 'loop <= 1' is there because:
	// . Page0 (ZP)    : memdirty[0] is set when the 6502 CPU does a ZP-write, but perhaps older versions didn't set this flag (eg. the asm version?).
	// . Page1 (stack) : memdirty[1] is NOT set when the 6502 CPU writes to this page with JSR, etc.

	for (loop = 0x00; loop < 0x100; loop++)
	{
		if (initialize || (oldshadow[loop] != memshadow[loop]))
		{
			if (!initialize &&
				((*(memdirty+loop) & 1) || (loop <= 1)))
			{
				*(memdirty+loop) &= ~1;
				CopyMemory(oldshadow[loop],mem+(loop << 8),256);
			}

			CopyMemory(mem+(loop << 8),memshadow[loop],256);
		}
	}
}

//
// ----- ALL GLOBALLY ACCESSIBLE FUNCTIONS ARE BELOW THIS LINE -----
//

//===========================================================================

// TODO: >= Apple2e only?
BYTE __stdcall MemCheckPaging(WORD, WORD address, BYTE, BYTE, ULONG)
{
	address &= 0xFF;
	BOOL result = 0;
	switch (address)
	{
	case 0x11: result = SW_BANK2;       break;
	case 0x12: result = SW_HIGHRAM;     break;
	case 0x13: result = SW_AUXREAD;     break;
	case 0x14: result = SW_AUXWRITE;    break;
	case 0x15: result = !SW_SLOTCXROM;  break;
	case 0x16: result = SW_ALTZP;       break;
	case 0x17: result = SW_SLOTC3ROM;   break;
	case 0x18: result = SW_80STORE;     break;
	case 0x1C: result = SW_PAGE2;       break;
	case 0x1D: result = SW_HIRES;       break;
	}
	return KeybGetKeycode() | (result ? 0x80 : 0);
}

//===========================================================================

void MemDestroy()
{
	VirtualFree(memaux  ,0,MEM_RELEASE);
	VirtualFree(memmain ,0,MEM_RELEASE);
	VirtualFree(memdirty,0,MEM_RELEASE);
	VirtualFree(memrom  ,0,MEM_RELEASE);
	VirtualFree(memimage,0,MEM_RELEASE);

	VirtualFree(pCxRomInternal,0,MEM_RELEASE);
	VirtualFree(pCxRomPeripheral,0,MEM_RELEASE);

#ifdef RAMWORKS
	for (UINT i=1; i<g_uMaxExPages; i++)
	{
		if (RWpages[i])
		{
			VirtualFree(RWpages[i], 0, MEM_RELEASE);
			RWpages[i] = NULL;
		}
	}
	RWpages[0]=NULL;
#endif

	memaux   = NULL;
	memmain  = NULL;
	memdirty = NULL;
	memrom   = NULL;
	memimage = NULL;

	pCxRomInternal		= NULL;
	pCxRomPeripheral	= NULL;

	mem      = NULL;

	ZeroMemory(memwrite, sizeof(memwrite));
	ZeroMemory(memshadow,sizeof(memshadow));
}

//===========================================================================

bool MemCheckSLOTCXROM()
{
	return SW_SLOTCXROM ? true : false;
}

//===========================================================================

static LPBYTE MemGetPtrBANK1(const WORD offset, const LPBYTE pMemBase)
{
	if ((offset & 0xF000) != 0xC000)	// Requesting RAM at physical addr $Cxxx (ie. 4K RAM BANK1)
		return NULL;

	// NB. This works for memaux when set to any RWpages[] value, ie. RamWork III "just works"
	const BYTE bank1page = (offset >> 8) & 0xF;
	return (memshadow[0xD0+bank1page] == pMemBase+(0xC0+bank1page)*256)
		? mem+offset+0x1000				// Return ptr to $Dxxx address - 'mem' has (a potentially dirty) 4K RAM BANK1 mapped in at $D000
		: pMemBase+offset;				// Else return ptr to $Cxxx address
}

//-------------------------------------

LPBYTE MemGetAuxPtr(const WORD offset)
{
	LPBYTE lpMem = MemGetPtrBANK1(offset, memaux);
	if (lpMem)
		return lpMem;

	lpMem = (memshadow[(offset >> 8)] == (memaux+(offset & 0xFF00)))
			? mem+offset				// Return 'mem' copy if possible, as page could be dirty
			: memaux+offset;

#ifdef RAMWORKS
	if ( ((SW_PAGE2 && SW_80STORE) || VideoGetSW80COL()) &&
		( ( ((offset & 0xFF00)>=0x0400) &&
		((offset & 0xFF00)<=0700) ) ||
		( SW_HIRES && ((offset & 0xFF00)>=0x2000) &&
		((offset & 0xFF00)<=0x3F00) ) ) ) {
		lpMem = (memshadow[(offset >> 8)] == (RWpages[0]+(offset & 0xFF00)))
			? mem+offset
			: RWpages[0]+offset;
	}
#endif

	return lpMem;
}

//-------------------------------------

LPBYTE MemGetMainPtr(const WORD offset)
{
	LPBYTE lpMem = MemGetPtrBANK1(offset, memmain);
	if (lpMem)
		return lpMem;

	return (memshadow[(offset >> 8)] == (memmain+(offset & 0xFF00)))
			? mem+offset				// Return 'mem' copy if possible, as page could be dirty
			: memmain+offset;
}

//===========================================================================

LPBYTE MemGetBankPtr(const UINT nBank)
{
	BackMainImage();	// Flush any dirty pages to back-buffer

#ifdef RAMWORKS
	if (nBank > g_uMaxExPages)
		return NULL;

	if (nBank == 0)
		return memmain;

	return RWpages[nBank-1];
#else
	return	(nBank == 0) ? memmain :
			(nBank == 1) ? memaux :
			NULL;
#endif
}

//===========================================================================

LPBYTE MemGetCxRomPeripheral()
{
	return pCxRomPeripheral;
}

//===========================================================================

// Post:
// . true:  code memory
// . false: I/O memory or floating bus
bool MemIsAddrCodeMemory(const USHORT addr)
{
	if (addr < 0xC000 || addr > FIRMWARE_EXPANSION_END)	// Assume all A][ types have at least 48K
		return true;

	if (addr < APPLE_SLOT_BEGIN)		// [$C000..C0FF]
		return false;

	if (!IS_APPLE2 && !SW_SLOTCXROM)	// [$C100..C7FF] //e or Enhanced //e internal ROM
		return true;

	if (!IS_APPLE2 && !SW_SLOTC3ROM && (addr >> 8) == 0xC3)	// [$C300..C3FF] //e or Enhanced //e internal ROM
		return true;

	if (addr <= APPLE_SLOT_END)			// [$C100..C7FF]
	{
		const UINT uSlot = (addr >> 8) & 0x7;
		return IsCardInSlot(uSlot);
	}

	// [$C800..CFFF]

	if (g_eExpansionRomType == eExpRomNull)
	{
		if (IO_SELECT || IO_SELECT_InternalROM)	// Was at $Csxx and now in [$C800..$CFFF]
			return true;

		return false;
	}

	return true;
}

//===========================================================================

const UINT CxRomSize = 4*1024;
const UINT Apple2RomSize = 12*1024;
const UINT Apple2eRomSize = Apple2RomSize+CxRomSize;
//const UINT Pravets82RomSize = 12*1024;
//const UINT Pravets8ARomSize = Pravets82RomSize+CxRomSize;

void MemInitialize()
{
	// ALLOCATE MEMORY FOR THE APPLE MEMORY IMAGE AND ASSOCIATED DATA STRUCTURES
	memaux   = (LPBYTE)VirtualAlloc(NULL,_6502_MEM_END+1,MEM_COMMIT,PAGE_READWRITE);
	memmain  = (LPBYTE)VirtualAlloc(NULL,_6502_MEM_END+1,MEM_COMMIT,PAGE_READWRITE);
	memdirty = (LPBYTE)VirtualAlloc(NULL,0x100  ,MEM_COMMIT,PAGE_READWRITE);
	memrom   = (LPBYTE)VirtualAlloc(NULL,0x5000 ,MEM_COMMIT,PAGE_READWRITE);
	memimage = (LPBYTE)VirtualAlloc(NULL,_6502_MEM_END+1,MEM_RESERVE,PAGE_NOACCESS);

	pCxRomInternal		= (LPBYTE) VirtualAlloc(NULL, CxRomSize, MEM_COMMIT, PAGE_READWRITE);
	pCxRomPeripheral	= (LPBYTE) VirtualAlloc(NULL, CxRomSize, MEM_COMMIT, PAGE_READWRITE);

	if (!memaux || !memdirty || !memimage || !memmain || !memrom || !pCxRomInternal || !pCxRomPeripheral)
	{
		MessageBox(
			GetDesktopWindow(),
			TEXT("The emulator was unable to allocate the memory it ")
			TEXT("requires.  Further execution is not possible."),
			g_pAppTitle,
			MB_ICONSTOP | MB_SETFOREGROUND);
		ExitProcess(1);
	}

	LPVOID newloc = VirtualAlloc(memimage,_6502_MEM_END+1,MEM_COMMIT,PAGE_READWRITE);
	if (newloc != memimage)
		MessageBox(
			GetDesktopWindow(),
			TEXT("The emulator has detected a bug in your operating ")
			TEXT("system.  While changing the attributes of a memory ")
			TEXT("object, the operating system also changed its ")
			TEXT("location."),
			g_pAppTitle,
			MB_ICONEXCLAMATION | MB_SETFOREGROUND);

#ifdef RAMWORKS
	// allocate memory for RAMWorks III - up to 8MB
	g_uActiveBank = 0;
	RWpages[g_uActiveBank] = memaux;

	UINT i = 1;
	while ((i < g_uMaxExPages) && (RWpages[i] = (LPBYTE) VirtualAlloc(NULL,_6502_MEM_END+1,MEM_COMMIT,PAGE_READWRITE)))
		i++;

	// We can't set
	//  g_eMemType = MEM_TYPE_RAMWORKS;
	// Since that would blow away if the user has selected the Saturn 128K
#endif

#ifdef SATURN
	for( UINT iPage = 0; iPage < g_uSaturnTotalBanks; iPage++ )
		g_aSaturnPages[ iPage ] = (LPBYTE) VirtualAlloc( NULL, 1024 * 16,MEM_COMMIT,PAGE_READWRITE); // Saturn Pages are 16K / bank, Max 8 Banks/Card
#endif // SATURN

	MemInitializeROM();
	MemInitializeCustomF8ROM();
	MemInitializeIO();
	MemReset();
}

void MemInitializeROM(void)
{
	// READ THE APPLE FIRMWARE ROMS INTO THE ROM IMAGE
	UINT ROM_SIZE = 0;
	HRSRC hResInfo = NULL;
	switch (g_Apple2Type)
	{
	case A2TYPE_APPLE2:         hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_APPLE2_ROM          ), "ROM"); ROM_SIZE = Apple2RomSize ; break;
	case A2TYPE_APPLE2PLUS:     hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_APPLE2_PLUS_ROM     ), "ROM"); ROM_SIZE = Apple2RomSize ; break;
	case A2TYPE_APPLE2E:        hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_APPLE2E_ROM         ), "ROM"); ROM_SIZE = Apple2eRomSize; break;
	case A2TYPE_APPLE2EENHANCED:hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_APPLE2E_ENHANCED_ROM), "ROM"); ROM_SIZE = Apple2eRomSize; break;
	case A2TYPE_PRAVETS82:      hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_PRAVETS_82_ROM      ), "ROM"); ROM_SIZE = Apple2RomSize ; break;
	case A2TYPE_PRAVETS8M:      hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_PRAVETS_8M_ROM      ), "ROM"); ROM_SIZE = Apple2RomSize ; break;
	case A2TYPE_PRAVETS8A:      hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_PRAVETS_8C_ROM      ), "ROM"); ROM_SIZE = Apple2eRomSize; break;
	case A2TYPE_TK30002E:       hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_TK3000_2E_ROM       ), "ROM"); ROM_SIZE = Apple2eRomSize; break;
	}

	if(hResInfo == NULL)
	{
		TCHAR sRomFileName[ MAX_PATH ];
		switch (g_Apple2Type)
		{
		case A2TYPE_APPLE2:         _tcscpy(sRomFileName, TEXT("APPLE2.ROM"          )); break;
		case A2TYPE_APPLE2PLUS:     _tcscpy(sRomFileName, TEXT("APPLE2_PLUS.ROM"     )); break;
		case A2TYPE_APPLE2E:        _tcscpy(sRomFileName, TEXT("APPLE2E.ROM"         )); break;
		case A2TYPE_APPLE2EENHANCED:_tcscpy(sRomFileName, TEXT("APPLE2E_ENHANCED.ROM")); break;
		case A2TYPE_PRAVETS82:      _tcscpy(sRomFileName, TEXT("PRAVETS82.ROM"       )); break;
		case A2TYPE_PRAVETS8M:      _tcscpy(sRomFileName, TEXT("PRAVETS8M.ROM"       )); break;
		case A2TYPE_PRAVETS8A:      _tcscpy(sRomFileName, TEXT("PRAVETS8C.ROM"       )); break;
		case A2TYPE_TK30002E:       _tcscpy(sRomFileName, TEXT("TK3000e.ROM"         )); break;
		default:
			{
				_tcscpy(sRomFileName, TEXT("Unknown type!"));
				REGSAVE(TEXT(REGVALUE_APPLE2_TYPE), A2TYPE_APPLE2EENHANCED);
			}
		}

		TCHAR sText[ MAX_PATH ];
		wsprintf( sText, TEXT("Unable to open the required firmware ROM data file.\n\nFile: %s"), sRomFileName );

		MessageBox(
			GetDesktopWindow(),
			sText,
			g_pAppTitle,
			MB_ICONSTOP | MB_SETFOREGROUND);
		ExitProcess(1);
	}

	DWORD dwResSize = SizeofResource(NULL, hResInfo);
	if(dwResSize != ROM_SIZE)
		return;

	HGLOBAL hResData = LoadResource(NULL, hResInfo);
	if(hResData == NULL)
		return;

	BYTE* pData = (BYTE*) LockResource(hResData);	// NB. Don't need to unlock resource
	if (pData == NULL)
		return;

	memset(pCxRomInternal,0,CxRomSize);
	memset(pCxRomPeripheral,0,CxRomSize);

	if (ROM_SIZE == Apple2eRomSize)
	{
		memcpy(pCxRomInternal, pData, CxRomSize);
		pData += CxRomSize;
		ROM_SIZE -= CxRomSize;
	}

	_ASSERT(ROM_SIZE == Apple2RomSize);
	memcpy(memrom, pData, Apple2RomSize);			// ROM at $D000...$FFFF
}

void MemInitializeCustomF8ROM(void)
{
	const UINT F8RomSize = 0x800;
	if (g_hCustomRomF8 != INVALID_HANDLE_VALUE)
	{
		BYTE OldRom[Apple2RomSize];	// NB. 12KB on stack
		memcpy(OldRom, memrom, Apple2RomSize);

		SetFilePointer(g_hCustomRomF8, 0, NULL, FILE_BEGIN);
		DWORD uNumBytesRead;
		BOOL bRes = ReadFile(g_hCustomRomF8, memrom+Apple2RomSize-F8RomSize, F8RomSize, &uNumBytesRead, NULL);
		if (uNumBytesRead != F8RomSize)
		{
			memcpy(memrom, OldRom, Apple2RomSize);	// ROM at $D000...$FFFF
			bRes = FALSE;
		}

		if (!bRes)
		{
			MessageBox( g_hFrameWindow, "Failed to read custom F8 rom", TEXT("AppleWin Error"), MB_OK );
			CloseHandle(g_hCustomRomF8);
			g_hCustomRomF8 = INVALID_HANDLE_VALUE;
			// Failed, so use default rom...
		}
	}

	if (sg_PropertySheet.GetTheFreezesF8Rom() && IS_APPLE2)
	{
		HGLOBAL hResData = NULL;
		BYTE* pData = NULL;

		HRSRC hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_FREEZES_F8_ROM), "ROM");

		if (hResInfo && (SizeofResource(NULL, hResInfo) == 0x800) && (hResData = LoadResource(NULL, hResInfo)) && (pData = (BYTE*) LockResource(hResData)))
		{
			memcpy(memrom+Apple2RomSize-F8RomSize, pData, F8RomSize);
		}
	}
}

// Called by:
// . MemInitialize()
// . Snapshot_LoadState_v2()
//
// Since called by LoadState(), then this must not init any cards
// - it should only init the card I/O hooks
void MemInitializeIO(void)
{
	InitIoHandlers();

	const UINT uSlot = 0;
	RegisterIoHandler(uSlot, MemSetPaging, MemSetPaging, NULL, NULL, NULL, NULL);

	// TODO: Cleanup peripheral setup!!!
	PrintLoadRom(pCxRomPeripheral, 1);				// $C100 : Parallel printer f/w

	sg_SSC.CommInitialize(pCxRomPeripheral, 2);		// $C200 : SSC

	// Slot 3 contains the Uthernet card (which can coexist with an 80-col+Ram card in AUX slot)
	// . Uthernet card has no ROM and only IO mapped at $C0Bx

	// Apple//e: Auxilary slot contains Extended 80 Column card or RamWorksIII card

	if (g_Slot4 == CT_MouseInterface)
	{
		sg_Mouse.Initialize(pCxRomPeripheral, 4);	// $C400 : Mouse f/w
	}
	else if (g_Slot4 == CT_MockingboardC || g_Slot4 == CT_Phasor)
	{
		const UINT uSlot4 = 4;
		const UINT uSlot5 = 5;
		MB_InitializeIO(pCxRomPeripheral, uSlot4, uSlot5);
	}
	else if (g_Slot4 == CT_Z80)
	{
		ConfigureSoftcard(pCxRomPeripheral, 4);		// $C400 : Z80 card
	}
//	else if (g_Slot4 == CT_GenericClock)
//	{
//		LoadRom_Clock_Generic(pCxRomPeripheral, 4);
//	}

	if (g_Slot5 == CT_Z80)
	{
		ConfigureSoftcard(pCxRomPeripheral, 5);		// $C500 : Z80 card
	}
        else
         if (g_Slot5 == CT_SAM)
          ConfigureSAM(pCxRomPeripheral, 5);		// $C500 : Z80 card

	DiskLoadRom(pCxRomPeripheral, 6);				// $C600 : Disk][ f/w
	HD_Load_Rom(pCxRomPeripheral, 7);				// $C700 : HDD f/w

	//

	// Finally remove the cards' ROMs at $Csnn if internal ROM is enabled
	// . required when restoring saved-state
	if (!SW_SLOTCXROM)
		IoHandlerCardsOut();
}

inline DWORD getRandomTime()
{
	return rand() ^ timeGetTime(); // We can't use g_nCumulativeCycles as it will be zero on a fresh execution.
}

//===========================================================================

// Called by:
// . MemInitialize()
// . ResetMachineState()	eg. Power-cycle ('Apple-Go' button)
// . Snapshot_LoadState_v1()
// . Snapshot_LoadState_v2()
void MemReset()
{
	// INITIALIZE THE PAGING TABLES
	ZeroMemory(memshadow,256*sizeof(LPBYTE));
	ZeroMemory(memwrite ,256*sizeof(LPBYTE));

	// INITIALIZE THE RAM IMAGES
	ZeroMemory(memaux ,0x10000);
	ZeroMemory(memmain,0x10000);

	// Init the I/O ROM vars
	IO_SELECT = 0;
	IO_SELECT_InternalROM = 0;
	g_eExpansionRomType = eExpRomNull;
	g_uPeripheralRomSlot = 0;

	//

	int iByte;

	// Memory is pseudo-initialized across various models of Apple ][ //e //c
	// We chose a random one for nostalgia's sake
	// To inspect:
	//   F2. Ctrl-F2. CALL-151, C050 C053 C057
	// OR
	//   F2, Ctrl-F2, F7, HGR
	DWORD clock = getRandomTime();
	MemoryInitPattern_e eMemoryInitPattern = static_cast<MemoryInitPattern_e>(g_nMemoryClearType);

	if (g_nMemoryClearType < 0)	// random
	{
		eMemoryInitPattern = static_cast<MemoryInitPattern_e>( clock % NUM_MIP );

		// Don't use unless manually specified as a
		// few badly written programs will not work correctly
		// due to buffer overflows or not initializig memory before using.
		if( eMemoryInitPattern == MIP_PAGE_ADDRESS_LOW )
			eMemoryInitPattern = MIP_FF_FF_00_00;
	}

	switch( eMemoryInitPattern )
	{
		case MIP_FF_FF_00_00:
			for( iByte = 0x0000; iByte < 0xC000; iByte += 4 ) // NB. ODD 16-bit words are zero'd above...
			{
				memmain[ iByte+0 ] = 0xFF;
				memmain[ iByte+1 ] = 0xFF;
			}

			// Exceptions: xx28 xx29 xx68 xx69 Apple //e
			for( iByte = 0x0000; iByte < 0xC000; iByte += 512 )
			{
				clock = getRandomTime();
				memmain[ iByte + 0x28 ] = (clock >>  0) & 0xFF;
				memmain[ iByte + 0x29 ] = (clock >>  8) & 0xFF;
				clock = getRandomTime();
				memmain[ iByte + 0x68 ] = (clock >>  0) & 0xFF;
				memmain[ iByte + 0x69 ] = (clock >>  8) & 0xFF;
			}
			break;
		
		case MIP_FF_00_FULL_PAGE:
			// https://github.com/AppleWin/AppleWin/issues/225
			// AppleWin 1.25 RC2 fails to boot Castle Wolfenstein #225
			// This causes Castle Wolfenstein to not boot properly 100% with an error:
			// ?OVERFLOW ERROR IN 10
			// http://mirrors.apple2.org.za/ftp.apple.asimov.net/images/games/action/wolfenstein/castle_wolfenstein-fixed.dsk
			for( iByte = 0x0000; iByte < 0xC000; iByte += 512 )
			{
				memset( &memmain[ iByte ], 0xFF, 256 );

				// Exceptions: xx28: 00  xx68:00  Apple //e Platinum NTSC
				memmain[ iByte + 0x28 ] = 0x00;
				memmain[ iByte + 0x68 ] = 0x00;
			}
			break;

		case MIP_00_FF_HALF_PAGE: 
			for( iByte = 0x0080; iByte < 0xC000; iByte += 256 ) // NB. start = 0x80, delta = 0x100 !
				memset( &memmain[ iByte ], 0xFF, 128 );
			break;

		case MIP_FF_00_HALF_PAGE:
			for( iByte = 0x0000; iByte < 0xC000; iByte += 256 )
				memset( &memmain[ iByte ], 0xFF, 128 );
			break;

		case MIP_RANDOM:
			unsigned char random[ 256 ];
			for( iByte = 0x0000; iByte < 0xC000; iByte += 256 )
			{
				for( int i = 0; i < 256; i++ )
				{
					clock = getRandomTime();
					random[ (i+0) & 0xFF ] ^= (clock >>  0) & 0xFF;
					random[ (i+1) & 0xFF ] ^= (clock >> 11) & 0xFF;
				}

				memcpy( &memmain[ iByte ], random, 256 );
			}
			break;

		case MIP_PAGE_ADDRESS_LOW:
			for( iByte = 0x0000; iByte < 0xC000; iByte++ )
				memmain[ iByte ] = iByte & 0xFF;
			break;

		case MIP_PAGE_ADDRESS_HIGH:
			for( iByte = 0x0000; iByte < 0xC000; iByte += 256 )
				memset( &memmain[ iByte ], (iByte >> 8), 256 );
			break;

		default: // MIP_ZERO -- nothing to do
			break;
	}

	// https://github.com/AppleWin/AppleWin/issues/206
	// Work-around for a cold-booting bug in "Pooyan" which expects RNDL and RNDH to be non-zero.
	clock = getRandomTime();
	memmain[ 0x4E ] = 0x20 | (clock >> 0) & 0xFF;
	memmain[ 0x4F ] = 0x20 | (clock >> 8) & 0xFF;

	// https://github.com/AppleWin/AppleWin/issues/222
	// MIP_PAGE_ADDRESS_LOW breaks a few badly written programs!
	// "Beautiful Boot by Mini Appler" reads past $61FF into $6200
	// - "BeachParty-PoacherWars-DaytonDinger-BombsAway.dsk"
	// - "Dung Beetles, Ms. PacMan, Pooyan, Star Cruiser, Star Thief, Invas. Force.dsk"
	memmain[ 0x620B ] = 0x0;

	// https://github.com/AppleWin/AppleWin/issues/222
	// MIP_PAGE_ADDRESS_LOW
	// "Copy II+ v5.0.dsk"
	// There is a strange memory checker from $1B03 .. $1C25
	// Stuck in loop at $1BC2: JSR $F88E INSDS2 before crashing to $0: 00 BRK
	memmain[ 0xBFFD ] = 0;
	memmain[ 0xBFFE ] = 0;
	memmain[ 0xBFFF ] = 0;

	// SET UP THE MEMORY IMAGE
	mem = memimage;

	// INITIALIZE PAGING, FILLING IN THE 64K MEMORY IMAGE
	ResetPaging(1);		// Initialize=1

	// INITIALIZE & RESET THE CPU
	// . Do this after ROM has been copied back to mem[], so that PC is correctly init'ed from 6502's reset vector
	CpuInitialize();
	//Sets Caps Lock = false (Pravets 8A/C only)

	z80_reset();	// NB. Also called above in CpuInitialize()
}

//===========================================================================

BYTE MemReadFloatingBus(const ULONG uExecutedCycles)
{
//	return mem[ VideoGetScannerAddress(NULL, uExecutedCycles) ];	// NG: ANSI STORY (End Credits) - repro by running from "Turn the disk over"
	return mem[ NTSC_VideoGetScannerAddress() ];					// OK: This does the 2-cycle adjust for ANSI STORY (End Credits)
}

//===========================================================================

BYTE MemReadFloatingBus(const BYTE highbit, const ULONG uExecutedCycles)
{
	BYTE r = MemReadFloatingBus(uExecutedCycles);
	return (r & ~0x80) | ((highbit) ? 0x80 : 0);
}

//===========================================================================

//#define DEBUG_FLIP_TIMINGS

#if defined(_DEBUG) && defined(DEBUG_FLIP_TIMINGS)
static void DebugFlip(WORD address, ULONG nCyclesLeft)
{
	static unsigned __int64 uLastFlipCycle = 0;
	static unsigned int uLastPage = -1;

	if (address != 0x54 && address != 0x55)
		return;

	const unsigned int uNewPage = address & 1;
	if (uLastPage == uNewPage)
		return;
	uLastPage = uNewPage;

	CpuCalcCycles(nCyclesLeft);	// Update g_nCumulativeCycles

	const unsigned int uCyclesBetweenFlips = (unsigned int) (uLastFlipCycle ? g_nCumulativeCycles - uLastFlipCycle : 0);
	uLastFlipCycle = g_nCumulativeCycles;

	if (!uCyclesBetweenFlips)
		return;					// 1st time in func

	const double fFreq = CLK_6502 / (double)uCyclesBetweenFlips;

	char szStr[100];
	sprintf(szStr, "Cycles between flips = %d (%f Hz)\n", uCyclesBetweenFlips, fFreq);
	OutputDebugString(szStr);
}
#endif

BYTE __stdcall MemSetPaging(WORD programcounter, WORD address, BYTE write, BYTE value, ULONG nCyclesLeft)
{
	address &= 0xFF;
	DWORD lastmemmode = memmode;
#if defined(_DEBUG) && defined(DEBUG_FLIP_TIMINGS)
	DebugFlip(address, nCyclesLeft);
#endif

	// DETERMINE THE NEW MEMORY PAGING MODE.
	if ((address >= 0x80) && (address <= 0x8F))
	{
#if defined(DEBUG_LANGUAGE_CARD)
		static char text[ 128 ];
		int isBank2 = (memmode & MF_BANK2   ) ? 1 : 0;
		int isLC_on = (memmode & MF_HIGHRAM ) ? 1 : 0;
		int isWrite = (memmode & MF_WRITERAM) ? 1 : 0;
		sprintf( text, "@$%04X: >LC: $%04X  Bank2:%d  isLC: %d  isWrite:%d  LastWriteRam:%d\n", regs.pc-3, 0xC000 | address, isBank2, isLC_on, isWrite, g_bLastWriteRam & 1 );
		//OutputDebugStringA( text );
#endif

#ifdef SATURN
		if (g_uSaturnTotalBanks)
		{
			/*
					Bin   Addr.
						  $C0N0 4K Bank A, RAM read, Write protect
						  $C0N1 4K Bank A, ROM read, Write enabled
						  $C0N2 4K Bank A, ROM read, Write protect
						  $C0N3 4K Bank A, RAM read, Write enabled
					0100  $C0N4 select 16K Bank 1
					0101  $C0N5 select 16K Bank 2
					0110  $C0N6 select 16K Bank 3
					0111  $C0N7 select 16K Bank 4
						  $C0N8 4K Bank B, RAM read, Write protect
						  $C0N9 4K Bank B, ROM read, Write enabled
						  $C0NA 4K Bank B, ROM read, Write protect
						  $C0NB 4K Bank B, RAM read, Write enabled
					1100  $C0NC select 16K Bank 5
					1101  $C0ND select 16K Bank 6
					1110  $C0NE select 16K Bank 7
					1111  $C0NF select 16K Bank 8
					^ ^^
					3210 Bits
			*/
			if ((address & 7) > 3)
			{
				int iBankOffset = (SW_BANK2 ? 0 : 0x1000);
				int iPrevBank = g_uSaturnActivePage;
				g_uSaturnActivePage = 0 // Saturn 128K Language Card Bank 0 .. 7
					| (address >> 1) & 4
					| (address >> 0) & 3
					;
				LPBYTE pSatAddr = g_aSaturnPages[ g_uSaturnActivePage ];

/*
=== REPRO ===
300:AD 84 C0
303:AD 81 C0
306:AD 81 C0
309:A2 D0
30B:86 FF
30D:A0 00
30F:84 FE
311:B1 FE
313:91 FE
315:C8
316:D0 F9
318:E8
319:D0 F2
31B:AD 83 C0
31E:AD 83 C0
321:60

=== WATCH ===
*(memmain + 0xE000),x
*((g_aSaturnPages[0]) + 0xE000 - 0xC000),x
*((g_aSaturnPages[1]) + 0xE000 - 0xC000),x


*/
#if defined(DEBUG_LANGUAGE_CARD)
				BYTE prevByte, thisByte, nextByte;

				sprintf( text, "        SATURN:  Bank: %04X -> %04X\n", iPrevBank, g_uSaturnActivePage );
				OutputDebugStringA( text );

				prevByte = *(g_aSaturnPages[ 0 ] + 0x0000); // D000 Bank A
				thisByte = *(g_aSaturnPages[ 0 ] + 0x1000); // D000 Bank B
				nextByte = *(g_aSaturnPages[ 0 ] + 0x2000); // E000
				sprintf( text, "        SATURN[0]: $C000: %02X,  $D000: %02X  $E000: %02X\n", prevByte, thisByte, nextByte );
				OutputDebugStringA( text );

				prevByte = *(g_aSaturnPages[ 1 ] + 0x0000); // D000 Bank A
				thisByte = *(g_aSaturnPages[ 1 ] + 0x1000); // D000 Bank B
				nextByte = *(g_aSaturnPages[ 1 ] + 0x2000); // E000
				sprintf( text, "        SATURN[1]: $C000: %02X,  $D000: %02X  $E000: %02X\n", prevByte, thisByte, nextByte );
				OutputDebugStringA( text );

				prevByte = *(mem + 0xD000);
				nextByte = *(g_aSaturnPages[ g_uSaturnActivePage ] + 0x0000);
				sprintf( text, "        SATURN: $C000: %02X <- SATURN: %02X\n", prevByte, nextByte );
				OutputDebugStringA( text );

				prevByte = *(mem + 0xD000);
				nextByte = *(g_aSaturnPages[ g_uSaturnActivePage ] + 0x1000);
				sprintf( text, "        SATURN: $D000: %02X <- SATURN: %02X\n", prevByte, nextByte );
				OutputDebugStringA( text );

				prevByte = *(mem + 0xE000);
				nextByte = *(g_aSaturnPages[ g_uSaturnActivePage ] + 0x2000);
				sprintf( text, "        SATURN: $E000: %02X <- SATURN: %02X\n", prevByte, nextByte );
				OutputDebugStringA( text );
#endif

				// Sat  Mem
				// 0000 D000 Slot # Bank A
				// 1000 D000 Slot # Bank B
				// 2000 E000 Slot #
				// 3000 F000 Slot #

				// Bank out the old slot
//				if (iPrevBank != g_uSaturnActivePage)
				{
					modechanging = true;

					memcpy( g_aSaturnPages[iPrevBank] + iBankOffset, mem + 0xD000, 0x1000 );
					memcpy( g_aSaturnPages[iPrevBank] + 0x2000     , mem + 0xE000, 0x2000 ); // RAM  -> LC slot # old
				}

				// Bank in the new slot
				if ( SW_HIGHRAM )
				{
					memcpy( mem     + 0xD000, pSatAddr + iBankOffset, 0x1000 );
					memcpy( mem     + 0xE000, pSatAddr + 0x2000     , 0x2000 ); // mem  <- LC slot # new
				}
				else
				{
					memcpy( mem     + 0xD000, memrom                , 0x3000 ); // TODO: This is probably a HACK -- setup memshadow?
				}

				// NOTE: Do NOT mark memdirty

				goto _done_saturn;
			} // C084..C087  C08C..C08F

#define SATURN_POKE 0

#if SATURN_POKE == 0
			int bank2 = memmode &   MF_BANK2; // prev flags
			int flags = memmode & ~(MF_BANK2 | MF_HIGHRAM | MF_WRITERAM); // next flags

			if ((address & 3) == 0) flags |= MF_HIGHRAM;
			if ((address & 3) == 3) flags |= MF_HIGHRAM;
			if ((address &15) <  8) flags |= MF_BANK2;

			if (address & 1)    // GH#392
				if ( g_bLastWriteRam ) // SATURN -- this differs from Apple's 16K LC???
					flags |= MF_WRITERAM; // UTAIIe:5-23

			g_bLastWriteRam = (address & 1); // SATURN -- this differs from Apple's 16K LC???

			SetMemMode( flags );
			
			// Did banks change?
			if (MF_HIGHRAM
			&& (bank2 != (flags & MF_BANK2)))
			{
				int iBankOffsetPrev = (bank2    ? 0 : 0x1000);
				int iBankOffsetNext = (SW_BANK2 ? 0 : 0x1000);

#if defined(DEBUG_LANGUAGE_CARD)
		sprintf( text, "        *** LC: Prev Bank: %c\n", 'B' - (bank2 & MF_BANK2 ? 1 : 0) );
		OutputDebugStringA( text );
		sprintf( text, "        *** LC: Next Bank: %c\n", 'B' - (flags & MF_BANK2 ? 1 : 0) );
		OutputDebugStringA( text );
#endif
				memcpy(               g_aSaturnPages[ g_uSaturnActivePage ] + iBankOffsetPrev, mem + 0xD000, 0x1000 ); // Save prev bank
				memcpy( mem + 0xD000, g_aSaturnPages[ g_uSaturnActivePage ] + iBankOffsetNext,               0x1000 ); // Load next bank
			}
#endif

#if SATURN_POKE == 1
			int flags = memmode & ~MF_WRITERAM;

			switch( address )
			{
				case 0x80: flags |= MF_HIGHRAM; flags |= MF_BANK2; break;
				case 0x81: flags &=~MF_HIGHRAM; flags |= MF_BANK2; break; // 2 access to enable MF_WRITERAM
				case 0x82: flags &=~MF_HIGHRAM; flags |= MF_BANK2; break;
				case 0x83: flags |= MF_HIGHRAM; flags |= MF_BANK2; break; // 2 access to enable MF_WRITERAM

				case 0x88: flags |= MF_HIGHRAM; flags &=~MF_BANK2; break;
				case 0x89: flags &=~MF_HIGHRAM; flags &=~MF_BANK2; break; // 2 access to enable MF_WRITERAM
				case 0x8A: flags &=~MF_HIGHRAM; flags &=~MF_BANK2; break;
				case 0x8B: flags |= MF_HIGHRAM; flags &=~MF_BANK2; break; // 2 access to enable MF_WRITERAM

				default: // Handled above
					break;
			}

			if (address & 1)    // GH#392
				if ( g_bLastWriteRam ) // SATURN -- this differs from Apple's 16K LC???
					flags |= MF_WRITERAM; // UTAIIe:5-23

			SetMemMode( flags );
			g_bLastWriteRam = (address & 1); // SATURN -- this differs from Apple's 16K LC???
#endif // SATURN_POKE

#if SATURN_POKE == 2
			SetMemMode(memmode & ~(MF_BANK2 | MF_HIGHRAM));

			// Apple 16K Language Card
			// C080 .. C087 Bank 2
			// C088 .. C08F Bank 1
			if (!(address & 8))
				SetMemMode(memmode | MF_BANK2);

			// C080 == C088    Read RAM   MF_HIGHRAM
			// C081 == C089    Read ROM
			// C082 == C08A    Read ROM
			// C083 == C08B    Read RAM   MF_HIGHRAM
			if (((address & 2) >> 1) == (address & 1))
				SetMemMode(memmode | MF_HIGHRAM);

			if (address & 1)	// GH#392
			{
				if ( g_bLastWriteRam ) // SATURN -- this differs from Apple's 16K LC???
						SetMemMode(memmode | MF_WRITERAM);
			}
			else
			{
				SetMemMode(memmode & ~(MF_WRITERAM)); // UTAIIe:5-23
			}

			g_bLastWriteRam = (address & 1); // SATURN -- this differs from Apple's 16K LC???
#endif // SATURN_POKE
		}
		else
#endif // SATURN
		{
			SetMemMode(memmode & ~(MF_BANK2 | MF_HIGHRAM));

			// Apple 16K Language Card
			// C080 .. C087 Bank 2
			// C088 .. C08F Bank 1
			if (!(address & 8))
				SetMemMode(memmode | MF_BANK2);

			// C080 == C088    Read RAM   MF_HIGHRAM
			// C081 == C089    Read ROM
			// C082 == C08A    Read ROM
			// C083 == C08B    Read RAM   MF_HIGHRAM
			if (((address & 2) >> 1) == (address & 1))
				SetMemMode(memmode | MF_HIGHRAM);

			// C080 == C088    Write protect
			// C081 == C089    Write enabled  MF_WRITERAM
			// C082 == C08A    Write protect
			// C083 == C08B    Write enabled  MF_WRITERAM
			if (address & 1)	// GH#392
			{
				if (!write && g_bLastWriteRam)
				{
					SetMemMode(memmode | MF_WRITERAM); // UTAIIe:5-23
				}
			}
			else
			{
				SetMemMode(memmode & ~(MF_WRITERAM)); // UTAIIe:5-23
			}

			g_bLastWriteRam = (address & 1) && (!write); // UTAIIe:5-23
		}

#if defined(DEBUG_LANGUAGE_CARD)
		isBank2 = (memmode & MF_BANK2   ) ? 1 : 0;
		isLC_on = (memmode & MF_HIGHRAM ) ? 1 : 0;
		isWrite = (memmode & MF_WRITERAM) ? 1 : 0;
		sprintf( text, "        <LC: $%04X  Bank2:%d  isLC: %d  isWrite:%d\n", 0xC000 | address, isBank2, isLC_on, isWrite );
		//OutputDebugStringA( text );
#endif
	} // IO $C080 .. $C08F
	else if (!IS_APPLE2)
	{
		switch (address)
		{
			case 0x00: SetMemMode(memmode & ~MF_80STORE);    break;
			case 0x01: SetMemMode(memmode |  MF_80STORE);    break;
			case 0x02: SetMemMode(memmode & ~MF_AUXREAD);    break;
			case 0x03: SetMemMode(memmode |  MF_AUXREAD);    break;
			case 0x04: SetMemMode(memmode & ~MF_AUXWRITE);   break;
			case 0x05: SetMemMode(memmode |  MF_AUXWRITE);   break;
			case 0x06: SetMemMode(memmode |  MF_SLOTCXROM);  break;
			case 0x07: SetMemMode(memmode & ~MF_SLOTCXROM);  break;
			case 0x08: SetMemMode(memmode & ~MF_ALTZP);      break;
			case 0x09: SetMemMode(memmode |  MF_ALTZP);      break;
			case 0x0A: SetMemMode(memmode & ~MF_SLOTC3ROM);  break;
			case 0x0B: SetMemMode(memmode |  MF_SLOTC3ROM);  break;
			case 0x54: SetMemMode(memmode & ~MF_PAGE2);      break;
			case 0x55: SetMemMode(memmode |  MF_PAGE2);      break;
			case 0x56: SetMemMode(memmode & ~MF_HIRES);      break;
			case 0x57: SetMemMode(memmode |  MF_HIRES);      break;
#ifdef RAMWORKS
			case 0x71: // extended memory aux page number
			case 0x73: // Ramworks III set aux page number
				if ((value < g_uMaxExPages) && RWpages[value])
				{
					// NOTE: Do NOT set: g_eMemType = MEM_TYPE_RAMWORKS
					g_uActiveBank = value;
					memaux = RWpages[g_uActiveBank];
					UpdatePaging(0);	// Initialize=0
				}
				break;
#endif
		}
	}

#ifdef SATURN
_done_saturn:
#endif // SATURN

	// IF THE EMULATED PROGRAM HAS JUST UPDATE THE MEMORY WRITE MODE AND IS
	// ABOUT TO UPDATE THE MEMORY READ MODE, HOLD OFF ON ANY PROCESSING UNTIL
	// IT DOES SO.
	//
	// NB. A 6502 interrupt occurring between these memory write & read updates could lead to incorrect behaviour.
	// - although any date-race is probably a bug in the 6502 code too.
	if ((address >= 4) && (address <= 5) &&
		((*(LPDWORD)(mem+programcounter) & 0x00FFFEFF) == 0x00C0028D)) {
			modechanging = 1;
			return write ? 0 : MemReadFloatingBus(1, nCyclesLeft);
	}
	if ((address >= 0x80) && (address <= 0x8F) && (programcounter < 0xC000) &&
		(((*(LPDWORD)(mem+programcounter) & 0x00FFFEFF) == 0x00C0048D) ||
		 ((*(LPDWORD)(mem+programcounter) & 0x00FFFEFF) == 0x00C0028D))) {
			modechanging = 1;
			return write ? 0 : MemReadFloatingBus(1, nCyclesLeft);
	}

	// IF THE MEMORY PAGING MODE HAS CHANGED, UPDATE OUR MEMORY IMAGES AND
	// WRITE TABLES.
	if ((lastmemmode != memmode) || modechanging)
	{
		modechanging = 0;

		if ((lastmemmode & MF_SLOTCXROM) != (memmode & MF_SLOTCXROM))
		{
			if (SW_SLOTCXROM)
			{
				// Disable Internal ROM
				// . Similar to $CFFF access
				// . None of the peripheral cards can be driving the bus - so use the null ROM
				IO_SELECT_InternalROM = 0;	// GH#392
				memset(pCxRomPeripheral+0x800, 0, FIRMWARE_EXPANSION_SIZE);
				memset(mem+FIRMWARE_EXPANSION_BEGIN, 0, FIRMWARE_EXPANSION_SIZE);
				g_eExpansionRomType = eExpRomNull;
				g_uPeripheralRomSlot = 0;
				IoHandlerCardsIn();
			}
			else
			{
				// Enable Internal ROM
				memcpy(mem+0xC800, pCxRomInternal+0x800, FIRMWARE_EXPANSION_SIZE);
				g_eExpansionRomType = eExpRomInternal;
				g_uPeripheralRomSlot = 0;
				IoHandlerCardsOut();
			}
		}

		UpdatePaging(0);	// Initialize=0
	}

	if ((address <= 1) || ((address >= 0x54) && (address <= 0x57)))
		return VideoSetMode(programcounter,address,write,value,nCyclesLeft);

	return write ? 0 : MemReadFloatingBus(nCyclesLeft);
}

//===========================================================================

LPVOID MemGetSlotParameters(UINT uSlot)
{
	_ASSERT(uSlot < NUM_SLOTS);
	return SlotParameters[uSlot];
}

//===========================================================================

// NB. Don't need to save 'modechanging', as this is just an optimisation to save calling UpdatePaging() twice.
// . If we were to save the state when 'modechanging' is set, then on restoring the state, the 6502 code will immediately update the read memory mode.
// . This will work correctly.

void MemSetSnapshot_v1(const DWORD MemMode, const BOOL LastWriteRam, const BYTE* const pMemMain, const BYTE* const pMemAux)
{
	SetMemMode(MemMode);
	g_bLastWriteRam = LastWriteRam;

	memcpy(memmain, pMemMain, nMemMainSize);
	memcpy(memaux, pMemAux, nMemAuxSize);
	memset(memdirty, 0, 0x100);

	//

	modechanging = 0;
	// NB. MemUpdatePaging(TRUE) called at end of Snapshot_LoadState_v1()
	UpdatePaging(1);	// Initialize=1
}

//

#define UNIT_AUXSLOT_VER 1

#define SS_YAML_KEY_MEMORYMODE "Memory Mode"
#define SS_YAML_KEY_LASTRAMWRITE "Last RAM Write"
#define SS_YAML_KEY_IOSELECT "IO_SELECT"
#define SS_YAML_KEY_IOSELECT_INT "IO_SELECT_InternalROM"
#define SS_YAML_KEY_EXPANSIONROMTYPE "Expansion ROM Type"
#define SS_YAML_KEY_PERIPHERALROMSLOT "Peripheral ROM Slot"

#define SS_YAML_VALUE_CARD_80COL "80 Column"
#define SS_YAML_VALUE_CARD_EXTENDED80COL "Extended 80 Column"
#define SS_YAML_VALUE_CARD_RAMWORKSIII "RamWorksIII"

#define SS_YAML_KEY_NUMAUXBANKS "Num Aux Banks"
#define SS_YAML_KEY_ACTIVEAUXBANK "Active Aux Bank"

static std::string MemGetSnapshotStructName(void)
{
	static const std::string name("Memory");
	return name;
}

std::string MemGetSnapshotUnitAuxSlotName(void)
{
	static const std::string name("Auxiliary Slot");
	return name;
}

static std::string MemGetSnapshotMainMemStructName(void)
{
	static const std::string name("Main Memory");
	return name;
}

static std::string MemGetSnapshotAuxMemStructName(void)
{
	static const std::string name("Auxiliary Memory Bank");
	return name;
}

static void MemSaveSnapshotMemory(YamlSaveHelper& yamlSaveHelper, bool bIsMainMem, UINT bank=0)
{
	LPBYTE pMemBase = MemGetBankPtr(bank);

	if (bIsMainMem)
	{
		YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", MemGetSnapshotMainMemStructName().c_str());
		yamlSaveHelper.SaveMemory(pMemBase, 64*1024);
	}
	else
	{
		YamlSaveHelper::Label state(yamlSaveHelper, "%s%02X:\n", MemGetSnapshotAuxMemStructName().c_str(), bank-1);
		yamlSaveHelper.SaveMemory(pMemBase, 64*1024);
	}
}

void MemSaveSnapshot(YamlSaveHelper& yamlSaveHelper)
{
	// Scope so that "Memory" & "Main Memory" are at same indent level
	{
		YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", MemGetSnapshotStructName().c_str());
		yamlSaveHelper.SaveHexUint32(SS_YAML_KEY_MEMORYMODE, memmode);
		yamlSaveHelper.SaveUint(SS_YAML_KEY_LASTRAMWRITE, g_bLastWriteRam ? 1 : 0);
		yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_IOSELECT, IO_SELECT);
		yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_IOSELECT_INT, IO_SELECT_InternalROM);
		yamlSaveHelper.SaveUint(SS_YAML_KEY_EXPANSIONROMTYPE, (UINT) g_eExpansionRomType);
		yamlSaveHelper.SaveUint(SS_YAML_KEY_PERIPHERALROMSLOT, g_uPeripheralRomSlot);
	}

	MemSaveSnapshotMemory(yamlSaveHelper, true);
}

bool MemLoadSnapshot(YamlLoadHelper& yamlLoadHelper)
{
	if (!yamlLoadHelper.GetSubMap(MemGetSnapshotStructName()))
		return false;

	SetMemMode( yamlLoadHelper.LoadUint(SS_YAML_KEY_MEMORYMODE) );
	g_bLastWriteRam = yamlLoadHelper.LoadUint(SS_YAML_KEY_LASTRAMWRITE) ? TRUE : FALSE;
	IO_SELECT = (BYTE) yamlLoadHelper.LoadUint(SS_YAML_KEY_IOSELECT);
	IO_SELECT_InternalROM = (BYTE) yamlLoadHelper.LoadUint(SS_YAML_KEY_IOSELECT_INT);
	g_eExpansionRomType = (eExpansionRomType) yamlLoadHelper.LoadUint(SS_YAML_KEY_EXPANSIONROMTYPE);
	g_uPeripheralRomSlot = yamlLoadHelper.LoadUint(SS_YAML_KEY_PERIPHERALROMSLOT);

	yamlLoadHelper.PopMap();

	//

	if (!yamlLoadHelper.GetSubMap( MemGetSnapshotMainMemStructName() ))
		throw std::string("Card: Expected key: ") + MemGetSnapshotMainMemStructName();

	yamlLoadHelper.LoadMemory(memmain, _6502_MEM_END+1);
	memset(memdirty, 0, 0x100);

	yamlLoadHelper.PopMap();

	//

	modechanging = 0;
	// NB. MemUpdatePaging(TRUE) called at end of Snapshot_LoadState_v2()
	UpdatePaging(1);	// Initialize=1 (Still needed, even with call to MemUpdatePaging() - why?)

	return true;
}

void MemSaveSnapshotAux(YamlSaveHelper& yamlSaveHelper)
{
	if (IS_APPLE2)
	{
		return;	// No Aux slot for AppleII
	}

	if (IS_APPLE2C)
	{
		_ASSERT(g_uMaxExPages == 1);
	}

	yamlSaveHelper.UnitHdr(MemGetSnapshotUnitAuxSlotName(), UNIT_AUXSLOT_VER);
	YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);

	std::string card = 	g_uMaxExPages == 0 ?	SS_YAML_VALUE_CARD_80COL :			// todo: support empty slot
						g_uMaxExPages == 1 ?	SS_YAML_VALUE_CARD_EXTENDED80COL :
												SS_YAML_VALUE_CARD_RAMWORKSIII;
	yamlSaveHelper.SaveString(SS_YAML_KEY_CARD, card.c_str());
	yamlSaveHelper.Save("%s: %02X   # [0,1..7F] 0=no aux mem, 1=128K system, etc\n", SS_YAML_KEY_NUMAUXBANKS, g_uMaxExPages);
	yamlSaveHelper.Save("%s: %02X # [  0..7E] 0=memaux\n", SS_YAML_KEY_ACTIVEAUXBANK, g_uActiveBank);

	for(UINT uBank = 1; uBank <= g_uMaxExPages; uBank++)
	{
		MemSaveSnapshotMemory(yamlSaveHelper, false, uBank);
	}
}

bool MemLoadSnapshotAux(YamlLoadHelper& yamlLoadHelper, UINT version)
{
	if (version != UNIT_AUXSLOT_VER)
		throw std::string(SS_YAML_KEY_UNIT ": AuxSlot: Version mismatch");

	// "State"
	UINT numAuxBanks   = yamlLoadHelper.LoadUint(SS_YAML_KEY_NUMAUXBANKS);
	UINT activeAuxBank = yamlLoadHelper.LoadUint(SS_YAML_KEY_ACTIVEAUXBANK);

	std::string card = yamlLoadHelper.LoadString(SS_YAML_KEY_CARD);
	if (card == SS_YAML_VALUE_CARD_80COL)
	{
		if (numAuxBanks != 0 || activeAuxBank != 0)
			throw std::string(SS_YAML_KEY_UNIT ": AuxSlot: Bad aux slot card state");
	}
	else if (card == SS_YAML_VALUE_CARD_EXTENDED80COL)
	{
		if (numAuxBanks != 1 || activeAuxBank != 0)
			throw std::string(SS_YAML_KEY_UNIT ": AuxSlot: Bad aux slot card state");
	}
	else if (card == SS_YAML_VALUE_CARD_RAMWORKSIII)
	{
		if (numAuxBanks < 2 || numAuxBanks > 0x7F || (activeAuxBank+1) > numAuxBanks)
			throw std::string(SS_YAML_KEY_UNIT ": AuxSlot: Bad aux slot card state");
	}
	else
	{
		// todo: support empty slot
		throw std::string(SS_YAML_KEY_UNIT ": AuxSlot: Unknown card: " + card);
	}

	g_uMaxExPages = numAuxBanks;
	g_uActiveBank = activeAuxBank;

	//

	for(UINT uBank = 1; uBank <= g_uMaxExPages; uBank++)
	{
		LPBYTE pBank = MemGetBankPtr(uBank);
		if (!pBank)
		{
			pBank = RWpages[uBank-1] = (LPBYTE) VirtualAlloc(NULL,_6502_MEM_END+1,MEM_COMMIT,PAGE_READWRITE);
			if (!pBank)
				throw std::string("Card: mem alloc failed");
		}

		// "Auxiliary Memory Bankxx"
		char szBank[3];
		sprintf(szBank, "%02X", uBank-1);
		std::string auxMemName = MemGetSnapshotAuxMemStructName() + szBank;

		if (!yamlLoadHelper.GetSubMap(auxMemName))
			throw std::string("Memory: Missing map name: " + auxMemName);

		yamlLoadHelper.LoadMemory(pBank, _6502_MEM_END+1);

		yamlLoadHelper.PopMap();
	}

	memaux = RWpages[g_uActiveBank];
	// NB. MemUpdatePaging(TRUE) called at end of Snapshot_LoadState_v2()

	return true;
}
