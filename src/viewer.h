// viewer.h - the plugin's child window (custom window class).
//
// The POC window exists only to prove that the ABI works: it accepts the
// messages Opus sends, logs every one of them, paints itself with the Lister
// background colour, and shows a STATIC label with the file Opus handed over.
#pragma once

#include <windows.h>

// Registers the viewer window class once per process. Returns false on failure
// (already-logged); DVP_CreateViewer turns that into a NULL return.
bool ViewerRegisterClass(HINSTANCE hInstance);

// Creates a viewer child window. lpRc may be NULL. Ownership of the window
// stays with Opus; the plugin only ever creates and lays out children.
HWND ViewerCreate(HWND hWndParent, const RECT* lpRc, DWORD dwFlags);

// Number of viewer windows currently alive (incremented on create, decremented
// on WM_NCDESTROY). DVP_Uninit logs it so a leaked viewer is visible.
LONG ViewerLiveCount(void);
