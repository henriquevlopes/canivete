// Light and dark look for every Canivete window. The choice is the Appearance setting (General):
// "system" follows Windows (Settings > Personalization > Colors > app mode), or "light" / "dark".
// Windows built from standard controls call Theme_Window once they are built; the module then
// colors their title bar, edits, buttons, check boxes, lists and scroll bars, and repaints them
// when the choice or the Windows mode changes. Custom-painted windows read their colors from Pal().
#pragma once
#include <windows.h>

struct Palette {
    bool dark;
    // ordinary windows
    COLORREF window;      // content background
    COLORREF text;        // normal text
    COLORREF grey;        // hints, descriptions
    COLORREF red;         // problems
    COLORREF nav, navSel; // the tab list on the left and its selected tab
    COLORREF footer;      // the OK / Cancel strip
    COLORREF line;        // thin separator lines
    COLORREF accent;      // selected-tab bar, On switches, links
    COLORREF onAccent;    // knob or text drawn on the accent color
    COLORREF switchLine, switchKnob;  // an Off switch
    COLORREF focusBg;     // a hotkey box waiting for keys
    COLORREF edit;        // text box background
    // picture windows (editor, Make alpha)
    COLORREF canvas;      // behind the picture
    COLORREF bar;         // toolbar, hint and status strips
    COLORREF barLine;     // line under the toolbar
    COLORREF icon, iconOff;            // tool icons, disabled ones
    COLORREF toolSel, toolHover;       // selected / hovered tool
    COLORREF ring;                     // outline around color dots
    COLORREF hint, status, statusOk;   // strip texts: main, secondary, "saved" message
    COLORREF btn, btnHover, btnOn, btnOnHover, btnText;  // Make alpha buttons
    COLORREF hist, histBar, frame;     // levels box, its bars, frame around the result
    COLORREF numText;                  // the "1" on the number tool icon
};

const Palette& Pal();
bool Theme_Dark();
HBRUSH Theme_Brush(COLORREF c);  // shared solid brush, never delete it

void Theme_Init(HWND mainWnd);  // once at start: menus and following the Windows setting
void Theme_Window(HWND top);    // a top-level window built from controls: theme it now and on changes
void Theme_Update();            // the setting or Windows changed: repaint every open window if needed
void Theme_SettingsLoaded();    // called by LoadSettings (settings.ini may have been edited by hand)
