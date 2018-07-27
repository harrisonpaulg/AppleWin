#pragma once

#include "IPropertySheetPage.h"
#include "PropertySheetDefs.h"
class CPropertySheetHelper;

class CPageLiron : private IPropertySheetPage
{
public:
	CPageLiron(CPropertySheetHelper& PropertySheetHelper) :
		m_Page(PG_DISK),
		m_PropertySheetHelper(PropertySheetHelper)
	{
		CPageLiron::ms_this = this;
	}
	virtual ~CPageLiron(){}

	static BOOL CALLBACK DlgProc(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam);

protected:
	// IPropertySheetPage
	virtual BOOL DlgProcInternal(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam);
	virtual void DlgOK(HWND hWnd);
	virtual void DlgCANCEL(HWND hWnd){}

private:
	void InitOptions(HWND hWnd);
	void EnableLiron(HWND hWnd, BOOL bEnable);
	void HandleLironCombo(HWND hWnd, UINT driveSelected, UINT comboSelected);
	UINT RemovalConfirmation(UINT uCommand);

	static CPageLiron* ms_this;
	static const TCHAR m_defaultLironOptions[];

	const PAGETYPE m_Page;
	CPropertySheetHelper& m_PropertySheetHelper;
};
