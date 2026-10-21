#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include "settings.h"
#include "resource.h"
#include "duck_runner.h"

extern HINSTANCE g_module;

QuerySettings g_settings;

static const wchar_t* const TITLE = L"Datamodder Query Files";

// ─── paths ────────────────────────────────────────────────────────────────────

static std::wstring config_dir() {
    wchar_t appdata[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = std::wstring(appdata) + L"\\Notepad++\\plugins\\config";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring get_ini_path() { return config_dir() + L"\\QueryFiles.ini"; }

// ─── duckdb.exe lookup ────────────────────────────────────────────────────────

std::wstring find_duckdb(const std::string& own_path, std::wstring* source) {
    auto found = [&](const std::wstring& p, const wchar_t* why) {
        if (source) *source = why;
        return p;
    };

    // 1. own setting
    if (!own_path.empty()) {
        std::wstring w = widen(own_path);
        if (file_exists(w)) return found(w, L"Query Files' setting");
    }
    // 2. File Tools' setting
    {
        wchar_t buf[MAX_PATH] = {};
        std::wstring ft_ini = config_dir() + L"\\FileTools.ini";
        GetPrivateProfileStringW(L"Parquet", L"DuckdbPath", L"", buf, MAX_PATH, ft_ini.c_str());
        if (buf[0] && file_exists(buf)) return found(buf, L"File Tools' setting");
    }
    // 3. PATH
    {
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"duckdb", L".exe", MAX_PATH, buf, nullptr)) return found(buf, L"PATH");
    }
    // 4. next to QueryFiles.dll, 5. next to FileTools.dll (sibling plugin folder)
    wchar_t here[MAX_PATH] = {};
    GetModuleFileNameW(g_module, here, MAX_PATH);
    std::wstring dir = dir_of(here);
    if (file_exists(dir + L"\\duckdb.exe")) return found(dir + L"\\duckdb.exe", L"the Query Files plugin folder");
    std::wstring ft = dir_of(dir) + L"\\FileTools\\duckdb.exe";
    if (file_exists(ft)) return found(ft, L"the File Tools plugin folder");

    if (source) source->clear();
    return L"";
}

// ─── escaping for multi-line values (INI and JSON) ────────────────────────────

static std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\')      o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '"')  o += "\\\"";
        else if (c != '\r') o += c;
    }
    return o;
}
static std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += (n == 'n') ? '\n' : n;
        } else o += s[i];
    }
    return o;
}

// ─── INI persistence ──────────────────────────────────────────────────────────

static const wchar_t* bool_str(bool v) { return v ? L"1" : L"0"; }

static const char* view_str(ResultView v) { return v == ResultView::Table ? "table" : "text"; }
static ResultView  view_from(const std::string& s) { return s == "table" ? ResultView::Table : ResultView::Text; }

void save_settings() {
    std::wstring p = get_ini_path();
    const wchar_t* f = p.c_str();
    auto ws = [&](LPCWSTR sec, LPCWSTR key, const std::string& val) {
        WritePrivateProfileStringW(sec, key, widen(val).c_str(), f);
    };
    auto wb = [&](LPCWSTR sec, LPCWSTR key, bool val) { WritePrivateProfileStringW(sec, key, bool_str(val), f); };
    auto wi = [&](LPCWSTR sec, LPCWSTR key, int val) {
        wchar_t buf[16]; wsprintfW(buf, L"%d", val);
        WritePrivateProfileStringW(sec, key, buf, f);
    };

    ws(L"DuckDB", L"DuckdbPath", g_settings.duckdb_path);
    ws(L"DuckDB", L"DatabaseFile", g_settings.db_file);
    ws(L"DuckDB", L"Prelude", escape(g_settings.prelude));

    ws(L"Results", L"View", view_str(g_settings.view));
    wi(L"Results", L"MaxRows", g_settings.max_rows);
    ws(L"Results", L"TextMode", g_settings.text_mode);
    wi(L"Results", L"MaxOutputMB", g_settings.max_output_mb);
    wb(L"Results", L"ReuseTab", g_settings.reuse_tab);
    wb(L"Results", L"HeaderLine", g_settings.header_line);
    ws(L"Results", L"NullText", g_settings.null_text);
    wb(L"Results", L"RightAlign", g_settings.right_align);

    wi(L"General", L"TimeoutSeconds", g_settings.timeout_s);
}

void load_settings() {
    std::wstring p = get_ini_path();
    const wchar_t* f = p.c_str();
    auto rs = [&](LPCWSTR sec, LPCWSTR key, LPCWSTR def) {
        wchar_t buf[4096] = {};
        GetPrivateProfileStringW(sec, key, def, buf, 4096, f);
        return narrow(buf);
    };
    auto ri = [&](LPCWSTR sec, LPCWSTR key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, f); };

    g_settings.duckdb_path = rs(L"DuckDB", L"DuckdbPath", L"");
    g_settings.db_file     = rs(L"DuckDB", L"DatabaseFile", L"");
    g_settings.prelude     = unescape(rs(L"DuckDB", L"Prelude", L""));

    g_settings.view          = view_from(rs(L"Results", L"View", L"text"));
    g_settings.max_rows      = ri(L"Results", L"MaxRows", 1000);
    g_settings.text_mode     = rs(L"Results", L"TextMode", L"duckbox");
    g_settings.max_output_mb = ri(L"Results", L"MaxOutputMB", 50);
    g_settings.reuse_tab     = ri(L"Results", L"ReuseTab", 1) != 0;
    g_settings.header_line   = ri(L"Results", L"HeaderLine", 1) != 0;
    g_settings.null_text     = rs(L"Results", L"NullText", L"NULL");
    g_settings.right_align   = ri(L"Results", L"RightAlign", 1) != 0;

    g_settings.timeout_s     = ri(L"General", L"TimeoutSeconds", 0);

    if (g_settings.max_rows <= 0)      g_settings.max_rows = 1000;
    if (g_settings.max_output_mb <= 0) g_settings.max_output_mb = 50;
    if (g_settings.text_mode.empty())  g_settings.text_mode = "duckbox";
}

// ─── JSON serialization (profiles) ────────────────────────────────────────────

static std::string jstr(const char* key, const std::string& val) {
    return std::string("  \"") + key + "\": \"" + escape(val) + "\"";
}
static std::string jbool(const char* key, bool val) {
    return std::string("  \"") + key + "\": " + (val ? "true" : "false");
}
static std::string jint(const char* key, int val) {
    return std::string("  \"") + key + "\": " + std::to_string(val);
}

static bool json_has(const std::string& j, const char* key) {
    return j.find(std::string("\"") + key + "\": ") != std::string::npos;
}
static std::string json_get_str(const std::string& j, const char* key, const std::string& def) {
    std::string pat = std::string("\"") + key + "\": \"";
    auto p = j.find(pat);
    if (p == std::string::npos) return def;
    p += pat.size();
    std::string raw;
    for (; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) raw += j[p++];
        raw += j[p];
    }
    return unescape(raw);
}
static bool json_get_bool(const std::string& j, const char* key, bool def) {
    std::string pat = std::string("\"") + key + "\": ";
    auto p = j.find(pat);
    if (p == std::string::npos) return def;
    p += pat.size();
    if (j.compare(p, 4, "true")  == 0) return true;
    if (j.compare(p, 5, "false") == 0) return false;
    return def;
}
static int json_get_int(const std::string& j, const char* key, int def) {
    std::string pat = std::string("\"") + key + "\": ";
    auto p = j.find(pat);
    if (p == std::string::npos) return def;
    p += pat.size();
    if (p < j.size() && (std::isdigit((unsigned char)j[p]) || j[p] == '-'))
        return std::atoi(j.c_str() + p);
    return def;
}

static std::string settings_to_json(const QuerySettings& s) {
    std::vector<std::string> lines = {
        jstr("duckdb_path",    s.duckdb_path),
        jstr("db_file",        s.db_file),
        jstr("prelude",        s.prelude),
        jstr("view",           view_str(s.view)),
        jint("max_rows",       s.max_rows),
        jstr("text_mode",      s.text_mode),
        jint("max_output_mb",  s.max_output_mb),
        jbool("reuse_tab",     s.reuse_tab),
        jbool("header_line",   s.header_line),
        jstr("null_text",      s.null_text),
        jbool("right_align",   s.right_align),
        jint("timeout_s",      s.timeout_s),
    };
    std::string j = "{\n";
    for (size_t i = 0; i < lines.size(); ++i)
        j += lines[i] + (i + 1 < lines.size() ? ",\n" : "\n");
    return j + "}\n";
}

static QuerySettings settings_from_json(const std::string& j) {
    QuerySettings s;
    s.duckdb_path   = json_get_str(j, "duckdb_path", s.duckdb_path);
    s.db_file       = json_get_str(j, "db_file", s.db_file);
    s.prelude       = json_get_str(j, "prelude", s.prelude);
    if (json_has(j, "view")) s.view = view_from(json_get_str(j, "view", "text"));
    s.max_rows      = json_get_int(j, "max_rows", s.max_rows);
    s.text_mode     = json_get_str(j, "text_mode", s.text_mode);
    s.max_output_mb = json_get_int(j, "max_output_mb", s.max_output_mb);
    s.reuse_tab     = json_get_bool(j, "reuse_tab", s.reuse_tab);
    s.header_line   = json_get_bool(j, "header_line", s.header_line);
    s.null_text     = json_get_str(j, "null_text", s.null_text);
    s.right_align   = json_get_bool(j, "right_align", s.right_align);
    s.timeout_s     = json_get_int(j, "timeout_s", s.timeout_s);
    return s;
}

// ─── profile file operations ──────────────────────────────────────────────────

static std::wstring get_profiles_dir() {
    std::wstring dir = config_dir() + L"\\QueryFiles_profiles";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::vector<std::wstring> list_profiles() {
    std::wstring dir = get_profiles_dir();
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.size() > 5) names.push_back(name.substr(0, name.size() - 5));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

// ─── text-mode choices (combo box) ────────────────────────────────────────────

struct ModeChoice { const char* mode; const wchar_t* label; };
static const ModeChoice MODES[] = {
    { "duckbox",  L"Box with column types (duckbox)" },
    { "box",      L"Box" },
    { "table",    L"ASCII table" },
    { "markdown", L"Markdown" },
    { "csv",      L"CSV" },
    { "json",     L"JSON" },
    { "line",     L"One value per line" },
};
static const int MODE_COUNT = sizeof MODES / sizeof MODES[0];

// ─── tab pages ────────────────────────────────────────────────────────────────

struct TabPage { UINT id; LPCWSTR label; DLGPROC proc; };

static void settings_to_ui();
static void ui_to_settings();
static void refresh_profile_list(HWND hPage);

static INT_PTR CALLBACK page_proc(HWND, UINT msg, WPARAM, LPARAM) {
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

static bool browse_file(HWND owner, const wchar_t* filter, bool save, std::wstring& out) {
    wchar_t fname[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = fname;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = save ? OFN_PATHMUSTEXIST : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (ok) out = fname;
    return ok != FALSE;
}

// Show which duckdb.exe will be used, for what is currently typed in the box.
static void update_in_use(HWND hPage) {
    wchar_t buf[MAX_PATH] = {};
    GetDlgItemTextW(hPage, IDC_DUCKDB_PATH, buf, MAX_PATH);
    std::wstring source;
    std::wstring exe = find_duckdb(narrow(buf), &source);
    std::wstring text;
    if (exe.empty()) {
        text = L"duckdb.exe not found. Download it from https://duckdb.org/ and put it on your PATH "
               L"or next to the plugin, or browse to it above.";
    } else {
        std::string ver = duckdb_version(exe);
        text = L"In use (from " + source + L"):  " + exe;
        if (!ver.empty()) text += L"\nVersion: " + widen(ver);
    }
    SetDlgItemTextW(hPage, IDC_DUCKDB_INUSE, text.c_str());
}

// ── DuckDB page ───────────────────────────────────────────────────────────────

static INT_PTR CALLBACK duckdb_page_proc(HWND hPage, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND: {
        UINT ctrl = LOWORD(wParam), notif = HIWORD(wParam);
        if (ctrl == IDC_DUCKDB_PATH && notif == EN_KILLFOCUS) { update_in_use(hPage); break; }
        if (notif != BN_CLICKED) break;
        std::wstring f;
        if (ctrl == IDC_DUCKDB_BROWSE &&
            browse_file(GetParent(hPage), L"duckdb.exe\0duckdb.exe\0All files\0*.*\0", false, f)) {
            SetDlgItemTextW(hPage, IDC_DUCKDB_PATH, f.c_str());
            update_in_use(hPage);
        }
        if (ctrl == IDC_DB_BROWSE &&
            browse_file(GetParent(hPage), L"DuckDB database\0*.duckdb;*.db\0All files\0*.*\0", true, f))
            SetDlgItemTextW(hPage, IDC_DB_FILE, f.c_str());
        break;
    }
    }
    return FALSE;
}

// ── Results page ──────────────────────────────────────────────────────────────

static INT_PTR CALLBACK results_page_proc(HWND hPage, UINT msg, WPARAM, LPARAM) {
    if (msg == WM_INITDIALOG) {
        HWND combo = GetDlgItem(hPage, IDC_TEXT_MODE);
        for (int i = 0; i < MODE_COUNT; ++i)
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(MODES[i].label));
        return TRUE;
    }
    return FALSE;
}

// ── Behavior & Profiles page ──────────────────────────────────────────────────

static INT_PTR CALLBACK behavior_page_proc(HWND hPage, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        refresh_profile_list(hPage);
        return TRUE;
    case WM_COMMAND: {
        UINT notif = HIWORD(wParam);
        UINT ctrl  = LOWORD(wParam);

        if (ctrl == IDC_PROFILE_LIST && notif == LBN_DBLCLK)
            goto do_load;

        if (notif != BN_CLICKED) break;
        switch (ctrl) {
        case IDC_PROFILE_LOAD: do_load: {
            HWND hList = GetDlgItem(hPage, IDC_PROFILE_LIST);
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) break;
            wchar_t name[512] = {};
            SendMessageW(hList, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(name));
            std::string json = read_file(get_profiles_dir() + L"\\" + name + L".json");
            if (!json.empty()) {
                g_settings = settings_from_json(json);
                settings_to_ui();
                std::wstring m = std::wstring(L"Profile '") + name + L"' loaded.";
                MessageBoxW(hPage, m.c_str(), TITLE, MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        case IDC_PROFILE_SAVE: {
            wchar_t name[512] = {};
            GetDlgItemTextW(hPage, IDC_PROFILE_NAME, name, 512);
            if (!name[0]) break;
            ui_to_settings();
            write_file(get_profiles_dir() + L"\\" + name + L".json", settings_to_json(g_settings));
            refresh_profile_list(hPage);
            break;
        }
        case IDC_PROFILE_DELETE: {
            HWND hList = GetDlgItem(hPage, IDC_PROFILE_LIST);
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) break;
            wchar_t name[512] = {};
            SendMessageW(hList, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(name));
            std::wstring m2 = std::wstring(L"Delete profile \"") + name + L"\"?";
            if (MessageBoxW(GetParent(hPage), m2.c_str(), TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                DeleteFileW((get_profiles_dir() + L"\\" + name + L".json").c_str());
                refresh_profile_list(hPage);
            }
            break;
        }
        case IDC_PROFILE_EXPORT: {
            wchar_t fname[MAX_PATH] = L"settings";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = GetParent(hPage);
            ofn.lpstrFilter = L"JSON files\0*.json\0All files\0*.*\0";
            ofn.lpstrFile   = fname;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = L"json";
            if (GetSaveFileNameW(&ofn)) {
                ui_to_settings();
                write_file(fname, settings_to_json(g_settings));
            }
            break;
        }
        case IDC_PROFILE_IMPORT: {
            std::wstring f;
            if (browse_file(GetParent(hPage), L"JSON files\0*.json\0All files\0*.*\0", false, f)) {
                std::string json = read_file(f);
                if (!json.empty()) { g_settings = settings_from_json(json); settings_to_ui(); }
            }
            break;
        }
        }
        break;
    }
    }
    return FALSE;
}

// ── Tab page array ────────────────────────────────────────────────────────────
// Order: 0=DuckDB 1=Results 2=Behavior & Profiles

static const TabPage PAGES[] = {
    { IDD_TAB_DUCKDB,   L"DuckDB",   duckdb_page_proc   },
    { IDD_TAB_RESULTS,  L"Results",  results_page_proc  },
    { IDD_TAB_BEHAVIOR, L"Behavior", behavior_page_proc },
};
static const int PAGE_COUNT = 3;
static HWND g_pages[PAGE_COUNT] = {};

#define P_DUCKDB   g_pages[0]
#define P_RESULTS  g_pages[1]
#define P_BEHAVIOR g_pages[2]

static void show_page(int idx) {
    for (int i = 0; i < PAGE_COUNT; ++i)
        ShowWindow(g_pages[i], i == idx ? SW_SHOW : SW_HIDE);
}

// ─── control helpers ──────────────────────────────────────────────────────────

static bool chk_set(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }
static int  edit_int(HWND dlg, int id) {
    wchar_t buf[16] = {};
    GetDlgItemTextW(dlg, id, buf, 16);
    return buf[0] ? _wtoi(buf) : 0;
}
static void set_edit(HWND dlg, int id, int v) {
    wchar_t buf[16]; wsprintfW(buf, L"%d", v);
    SetDlgItemTextW(dlg, id, buf);
}
static std::string edit_str(HWND dlg, int id) {
    HWND h = GetDlgItem(dlg, id);
    int n = GetWindowTextLengthW(h);
    std::wstring w(n + 1, L'\0');
    GetWindowTextW(h, &w[0], n + 1);
    w.resize(n);
    return narrow(w);
}
static void set_edit_str(HWND dlg, int id, const std::string& s) {
    // Multi-line edit controls want CRLF.
    std::string t;
    for (char c : s) { if (c == '\n') t += '\r'; t += c; }
    SetDlgItemTextW(dlg, id, widen(t).c_str());
}
static std::string strip_cr(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

static void refresh_profile_list(HWND hPage) {
    HWND hList = GetDlgItem(hPage, IDC_PROFILE_LIST);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    for (const auto& name : list_profiles())
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
}

// ─── settings ↔ UI ────────────────────────────────────────────────────────────

static void settings_to_ui() {
    set_edit_str(P_DUCKDB, IDC_DUCKDB_PATH, g_settings.duckdb_path);
    set_edit_str(P_DUCKDB, IDC_DB_FILE,     g_settings.db_file);
    set_edit_str(P_DUCKDB, IDC_PRELUDE,     g_settings.prelude);
    update_in_use(P_DUCKDB);

    CheckRadioButton(P_RESULTS, IDC_VIEW_TEXT, IDC_VIEW_TABLE,
        g_settings.view == ResultView::Table ? IDC_VIEW_TABLE : IDC_VIEW_TEXT);
    set_edit(P_RESULTS, IDC_MAX_ROWS, g_settings.max_rows);
    int sel = 0;
    for (int i = 0; i < MODE_COUNT; ++i) if (g_settings.text_mode == MODES[i].mode) sel = i;
    SendDlgItemMessageW(P_RESULTS, IDC_TEXT_MODE, CB_SETCURSEL, sel, 0);
    set_edit(P_RESULTS, IDC_MAX_MB, g_settings.max_output_mb);
    CheckDlgButton(P_RESULTS, IDC_REUSE_TAB,   g_settings.reuse_tab   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(P_RESULTS, IDC_HEADER_LINE, g_settings.header_line ? BST_CHECKED : BST_UNCHECKED);
    set_edit_str(P_RESULTS, IDC_NULL_TEXT, g_settings.null_text);
    CheckDlgButton(P_RESULTS, IDC_RIGHT_ALIGN, g_settings.right_align ? BST_CHECKED : BST_UNCHECKED);

    set_edit(P_BEHAVIOR, IDC_TIMEOUT, g_settings.timeout_s);
}

static void ui_to_settings() {
    g_settings.duckdb_path = edit_str(P_DUCKDB, IDC_DUCKDB_PATH);
    g_settings.db_file     = edit_str(P_DUCKDB, IDC_DB_FILE);
    g_settings.prelude     = strip_cr(edit_str(P_DUCKDB, IDC_PRELUDE));

    g_settings.view     = chk_set(P_RESULTS, IDC_VIEW_TABLE) ? ResultView::Table : ResultView::Text;
    g_settings.max_rows = edit_int(P_RESULTS, IDC_MAX_ROWS);
    if (g_settings.max_rows <= 0) g_settings.max_rows = 1000;
    int sel = (int)SendDlgItemMessageW(P_RESULTS, IDC_TEXT_MODE, CB_GETCURSEL, 0, 0);
    g_settings.text_mode = (sel >= 0 && sel < MODE_COUNT) ? MODES[sel].mode : "duckbox";
    g_settings.max_output_mb = edit_int(P_RESULTS, IDC_MAX_MB);
    if (g_settings.max_output_mb <= 0) g_settings.max_output_mb = 50;
    g_settings.reuse_tab   = chk_set(P_RESULTS, IDC_REUSE_TAB);
    g_settings.header_line = chk_set(P_RESULTS, IDC_HEADER_LINE);
    g_settings.null_text   = edit_str(P_RESULTS, IDC_NULL_TEXT);
    g_settings.right_align = chk_set(P_RESULTS, IDC_RIGHT_ALIGN);

    g_settings.timeout_s = edit_int(P_BEHAVIOR, IDC_TIMEOUT);
}

// ─── settings dialog ──────────────────────────────────────────────────────────

static void destroy_pages() {
    for (int i = 0; i < PAGE_COUNT; ++i)
        if (g_pages[i]) { DestroyWindow(g_pages[i]); g_pages[i] = nullptr; }
}

static INT_PTR CALLBACK settings_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {

    case WM_INITDIALOG: {
        HWND hNav = GetDlgItem(hDlg, IDC_NAV);
        for (int i = 0; i < PAGE_COUNT; ++i)
            SendMessageW(hNav, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(PAGES[i].label));
        SendMessageW(hNav, LB_SETCURSEL, 0, 0);

        RECT navRc;
        GetWindowRect(hNav, &navRc);
        MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<POINT*>(&navRc), 2);
        int cx = navRc.right + 5;
        int cy = navRc.top;

        for (int i = 0; i < PAGE_COUNT; ++i) {
            g_pages[i] = CreateDialog(g_module, MAKEINTRESOURCE(PAGES[i].id), hDlg, PAGES[i].proc);
            SetWindowPos(g_pages[i], HWND_TOP, cx, cy, 0, 0, SWP_NOSIZE | SWP_HIDEWINDOW);
        }

        settings_to_ui();
        show_page(0);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_NAV && HIWORD(wParam) == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(GetDlgItem(hDlg, IDC_NAV), LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) show_page(sel);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            ui_to_settings();
            save_settings();
            destroy_pages();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            destroy_pages();
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        destroy_pages();
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void show_settings_dialog(HWND parent) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    QuerySettings before = g_settings;
    if (DialogBox(g_module, MAKEINTRESOURCE(IDD_SETTINGS), parent, settings_proc) != IDOK)
        g_settings = before;       // a profile loaded and then cancelled must not stick
}

// ─── about dialog ─────────────────────────────────────────────────────────────

static INT_PTR CALLBACK about_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION, L"Datamodder Query Files  " QUERYFILES_VERSION_W);
        SetDlgItemTextW(hDlg, IDC_ABOUT_BODY,
            L"Run SQL on CSV, Parquet, JSON and Excel files directly in Notepad++, powered by DuckDB.\n\n"
            L"Features:\n"
            L"- Query this file: the open data file becomes THIS in a new SQL tab\n"
            L"- Run query (toolbar button): selection or the whole document\n"
            L"- Join files on disk: from 'orders.parquet' join 'klanten.csv' ...\n"
            L"- Results as text in a tab, or as a sortable table in a docked panel\n"
            L"- Describe / Summarize THIS: columns, types and statistics in one click\n\n"
            L"Needs duckdb.exe (https://duckdb.org/). The one set up for Datamodder File Tools is reused.\n\n"
            L"This plugin is distributed under the MIT license.\n\n"
            L"For usage tips, see the plugin page. For updates or to report a bug, visit the project repository:");
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if ((hdr->idFrom == IDC_ABOUT_LINK || hdr->idFrom == IDC_ABOUT_LINK_WEBSITE) &&
            (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
            NMLINK* link = reinterpret_cast<NMLINK*>(lParam);
            ShellExecuteW(hDlg, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void show_about_dialog(HWND parent) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    DialogBox(g_module, MAKEINTRESOURCE(IDD_ABOUT), parent, about_proc);
}
