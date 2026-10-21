#pragma once
// The table view: a panel docked in Notepad++ with one virtual ListView per
// result set (tab strip when a run returns more than one).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "sql_text.h"

struct GridPage {
    std::wstring title;        // tab caption, "Result 1"
    qf::Table    table;
    bool         capped = false;
};

struct GridHost {
    HWND npp = nullptr;
    int  func_index = 0;       // g_funcs index of "Show result panel" (docking dlgID)
    int  cmd_id = 0;           // its menu command id, for the checkmark
    void (*open_as_text)(const std::string& utf8) = nullptr;
};

void grid_init(const GridHost& host);

// Show result pages (last one selected); focus goes to its first row.
void grid_show_results(std::vector<GridPage>&& pages, const std::wstring& status);

// Show a message (error or "no result set") in place of the table, focused.
void grid_show_message(const std::wstring& status, const std::wstring& text);

void grid_show();              // menu "Show result panel"
void grid_dark_mode_changed();
