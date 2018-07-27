/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2014, Tom Charlesworth, Michael Pohoreski, Nick Westgate

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

#include "StdAfx.h"

#include "../Applewin.h"
#include "../Disk.h"	// Drive_e, Disk_Status_e
#include "../Frame.h"
#include "../Registry.h"
#include "../resource/resource.h"
#include "PageLiron.h"
#include "../Liron.h"
#include "PropertySheetHelper.h"

CPageLiron* CPageLiron::ms_this = 0;	// reinit'd in ctor

const TCHAR CPageLiron::m_defaultLironOptions[] =
				TEXT("Select Disk...\0")
				TEXT("Eject Disk\0");

BOOL CALLBACK CPageLiron::DlgProc(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	// Switch from static func to our instance
	return CPageLiron::ms_this->DlgProcInternal(hWnd, message, wparam, lparam);
}

BOOL CPageLiron::DlgProcInternal(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_NOTIFY:
		{
			// Property Sheet notifications

			switch (((LPPSHNOTIFY)lparam)->hdr.code)
			{
			case PSN_SETACTIVE:
				// About to become the active page
				m_PropertySheetHelper.SetLastPage(m_Page);
				InitOptions(hWnd);
				break;
			case PSN_KILLACTIVE:
				SetWindowLong(hWnd, DWL_MSGRESULT, FALSE);			// Changes are valid
				break;
			case PSN_APPLY:
				DlgOK(hWnd);
				SetWindowLong(hWnd, DWL_MSGRESULT, PSNRET_NOERROR);	// Changes are valid
				break;
			case PSN_QUERYCANCEL:
				// Can use this to ask user to confirm cancel
				break;
			case PSN_RESET:
				DlgCANCEL(hWnd);
				break;
			}
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDC_COMBO_LIRON1:
			if (HIWORD(wparam) == CBN_SELCHANGE)
			{
				HandleLironCombo(hWnd, Liron_1, LOWORD(wparam));
				FrameRefreshStatus(DRAW_BUTTON_DRIVES);
			}
			break;
		case IDC_COMBO_LIRON2:
			if (HIWORD(wparam) == CBN_SELCHANGE)
			{
				HandleLironCombo(hWnd, Liron_2, LOWORD(wparam));
				FrameRefreshStatus(DRAW_BUTTON_DRIVES);
			}
			break;
		case IDC_COMBO_LIRON3:
			if (HIWORD(wparam) == CBN_SELCHANGE)
			{
				HandleLironCombo(hWnd, Liron_3, LOWORD(wparam));
			}
			break;
		case IDC_COMBO_LIRON4:
			if (HIWORD(wparam) == CBN_SELCHANGE)
			{
				HandleLironCombo(hWnd, Liron_4, LOWORD(wparam));
			}
			break;
		case IDC_LIRON_ENABLE:
			const UINT uNewState = IsDlgButtonChecked(hWnd, IDC_LIRON_ENABLE) ? 1 : 0;
			m_PropertySheetHelper.GetConfigNew().m_Slot[5] = uNewState ? CT_Liron : CT_Empty;
			EnableLiron(hWnd, uNewState);
			break;
		}
		break;

	case WM_INITDIALOG:
		{
			m_PropertySheetHelper.FillComboBox(hWnd, IDC_COMBO_LIRON1, m_defaultLironOptions, -1);
			m_PropertySheetHelper.FillComboBox(hWnd, IDC_COMBO_LIRON2, m_defaultLironOptions, -1);
			m_PropertySheetHelper.FillComboBox(hWnd, IDC_COMBO_LIRON3, m_defaultLironOptions, -1);
			m_PropertySheetHelper.FillComboBox(hWnd, IDC_COMBO_LIRON4, m_defaultLironOptions, -1);

			if (strlen(Liron_GetFullName(Liron_1)) > 0)
			{
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON1, CB_INSERTSTRING, 0, (LPARAM)Liron_GetFullName(Liron_1));
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON1, CB_SETCURSEL, 0, 0);
			}

			if (strlen(Liron_GetFullName(Liron_2)) > 0)
			{ 
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON2, CB_INSERTSTRING, 0, (LPARAM)Liron_GetFullName(Liron_2));
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON2, CB_SETCURSEL, 0, 0);
			}

			if (strlen(Liron_GetFullName(Liron_3)) > 0)
			{
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON3, CB_INSERTSTRING, 0, (LPARAM)Liron_GetFullName(Liron_3));
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON3, CB_SETCURSEL, 0, 0);
			}

			if (strlen(Liron_GetFullName(Liron_4)) > 0)
			{
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON4, CB_INSERTSTRING, 0, (LPARAM)Liron_GetFullName(Liron_4));
				SendDlgItemMessage(hWnd, IDC_COMBO_LIRON4, CB_SETCURSEL, 0, 0);
			}

			CheckDlgButton(hWnd, IDC_LIRON_ENABLE, Liron_CardIsEnabled() ? BST_CHECKED : BST_UNCHECKED);

			EnableLiron(hWnd, IsDlgButtonChecked(hWnd, IDC_LIRON_ENABLE));

			InitOptions(hWnd);

			break;
		}

	}

	return FALSE;
}

void CPageLiron::DlgOK(HWND hWnd)
{
	const bool bNewLironIsEnabled = IsDlgButtonChecked(hWnd, IDC_LIRON_ENABLE) ? true : false;
	if (bNewLironIsEnabled != Liron_CardIsEnabled())
	{
		m_PropertySheetHelper.GetConfigNew().m_bEnableLiron = bNewLironIsEnabled;
	}

	RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LAST_LIRON_1), 1, Liron_GetFullPathName(Liron_1));
	RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LAST_LIRON_2), 1, Liron_GetFullPathName(Liron_2));
	RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LAST_LIRON_3), 1, Liron_GetFullPathName(Liron_3));
	RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_LAST_LIRON_4), 1, Liron_GetFullPathName(Liron_4));

	m_PropertySheetHelper.PostMsgAfterClose(hWnd, m_Page);
}

void CPageLiron::InitOptions(HWND hWnd)
{
	// Nothing to do:
	// - no changes made on any other pages affect this page
}

void CPageLiron::EnableLiron(HWND hWnd, BOOL bEnable)
{
	EnableWindow(GetDlgItem(hWnd, IDC_COMBO_LIRON1), bEnable);
	EnableWindow(GetDlgItem(hWnd, IDC_COMBO_LIRON2), bEnable);
	EnableWindow(GetDlgItem(hWnd, IDC_COMBO_LIRON3), bEnable);
	EnableWindow(GetDlgItem(hWnd, IDC_COMBO_LIRON4), bEnable);
}

void CPageLiron::HandleLironCombo(HWND hWnd, UINT driveSelected, UINT comboSelected)
{
	if (!IsDlgButtonChecked(hWnd, IDC_LIRON_ENABLE))
	{
		return;
	}
	DWORD dwOpenDialogIndex = (DWORD)SendDlgItemMessage(hWnd, comboSelected, CB_FINDSTRINGEXACT, -1, (LPARAM)&m_defaultLironOptions[0]);
	DWORD dwComboSelection = (DWORD)SendDlgItemMessage(hWnd, comboSelected, CB_GETCURSEL, 0, 0);

	SendDlgItemMessage(hWnd, comboSelected, CB_SETCURSEL, -1, 0);	// Set to "empty" item

	if (dwComboSelection == dwOpenDialogIndex)
	{
		EnableLiron(hWnd, FALSE);	// Prevent multiple Selection dialogs to be triggered
		bool bRes = Liron_Select(driveSelected);
		EnableLiron(hWnd, TRUE);

		if (!bRes)
		{
			if (SendDlgItemMessage(hWnd, comboSelected, CB_GETCOUNT, 0, 0) == 3)	// If there's already a HDD...
				SendDlgItemMessage(hWnd, comboSelected, CB_SETCURSEL, 0, 0);		// then reselect it in the ComboBox
			return;
		}

		// Add hard drive name as item 0 and select it
		if (dwOpenDialogIndex > 0)
		{
			// Remove old item first
			SendDlgItemMessage(hWnd, comboSelected, CB_DELETESTRING, 0, 0);
		}

		SendDlgItemMessage(hWnd, comboSelected, CB_INSERTSTRING, 0, (LPARAM)Liron_GetFullName(driveSelected));
		SendDlgItemMessage(hWnd, comboSelected, CB_SETCURSEL, 0, 0);

		// If the HD was in the other combo, remove now
		for(int i = 0; i < 4; i++)
		{
			DWORD comboOther = (i >0) ? ((i >0) ? ((i >0) ? IDC_COMBO_LIRON4 : IDC_COMBO_LIRON3) : IDC_COMBO_LIRON2) : IDC_COMBO_LIRON1;
			if (comboOther != comboSelected)
			{
				DWORD duplicated = (DWORD)SendDlgItemMessage(hWnd, comboOther, CB_FINDSTRINGEXACT, -1, (LPARAM)Liron_GetFullName(driveSelected));
				if (duplicated != CB_ERR)
				{
					SendDlgItemMessage(hWnd, comboOther, CB_DELETESTRING, duplicated, 0);
					SendDlgItemMessage(hWnd, comboOther, CB_SETCURSEL, -1, 0);
				}
			}
		}
	}
	else if (dwComboSelection == (dwOpenDialogIndex+1))
	{
		if (dwComboSelection > 1)
		{
			UINT uCommand = (driveSelected >0) ? ((driveSelected >0) ? ((driveSelected >0) ? IDC_COMBO_LIRON4 : IDC_COMBO_LIRON3) : IDC_COMBO_LIRON2) : IDC_COMBO_LIRON1;
			if (RemovalConfirmation(uCommand))
			{
				// Unplug selected disk
				Liron_Unplug(driveSelected);
				// Remove drive from list
				SendDlgItemMessage(hWnd, comboSelected, CB_DELETESTRING, 0, 0);
			}
			else
			{
				SendDlgItemMessage(hWnd, comboSelected, CB_SETCURSEL, 0, 0);
			}
		}
	}
}


UINT CPageLiron::RemovalConfirmation(UINT uCommand)
{
	TCHAR szText[100];
	bool bMsgBox = true;

	if (uCommand == IDC_COMBO_LIRON1 || uCommand == IDC_COMBO_LIRON2 || uCommand == IDC_COMBO_LIRON3 || uCommand == IDC_COMBO_LIRON4)
		wsprintf(szText, "Do you really want to eject the disk in drive-%c ?", '1' + uCommand - IDC_COMBO_DISK1);
	else
		bMsgBox = false;

	if (bMsgBox)
	{
		int nRes = MessageBox(g_hFrameWindow, szText, TEXT("Eject/Unplug Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
		if (nRes == IDNO)
			uCommand = 0;
	}

	return uCommand;
}
