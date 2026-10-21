#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// ─── enums ────────────────────────────────────────────────────────────────────
enum class ResultView { Text, Table };

// ─── settings struct (defaults = first-run behaviour) ────────────────────────
struct QuerySettings {
    // DuckDB tab
    std::string duckdb_path;                 // empty = auto (File Tools' setting, PATH, next to DLL)
    std::string db_file;                     // empty = in-memory, else a .duckdb file
    std::string prelude;                     // extra SQL run before every query

    // Results tab
    ResultView  view          = ResultView::Text;
    int         max_rows      = 1000;        // both views
    std::string text_mode     = "duckbox";   // .mode for the text tab
    int         max_output_mb = 50;
    bool        reuse_tab     = true;
    bool        header_line   = true;
    std::string null_text     = "NULL";      // table view
    bool        right_align   = true;        // table view: numeric columns

    // Behavior tab
    int         timeout_s     = 0;           // 0 = no timeout
};

extern QuerySettings g_settings;

void load_settings();
void save_settings();
void show_settings_dialog(HWND parent);
void show_about_dialog(HWND parent);

// Where duckdb.exe comes from, in order: own setting, File Tools' setting,
// PATH, next to QueryFiles.dll, next to FileTools.dll. Empty if not found;
// `source` (optional) gets a short description for the Settings page.
std::wstring find_duckdb(const std::string& own_path, std::wstring* source = nullptr);
