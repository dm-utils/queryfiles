#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <ctime>
#include "sql_text.h"
#include "duck_runner.h"
#include "result_grid.h"
#include "settings.h"
#include "resource.h"

// ─── Notepad++ / Scintilla ABI (only what this plugin uses) ──────────────────

struct NppData {
    HWND _nppHandle;
    HWND _scintillaMainHandle;
    HWND _scintillaSecondHandle;
};
struct ShortcutKey {
    bool  _isCtrl;
    bool  _isAlt;
    bool  _isShift;
    UCHAR _key;
};
typedef void (*PFUNCPLUGINCMD)();
struct FuncItem {
    wchar_t        _menuItemName[64];
    PFUNCPLUGINCMD _pFunc;
    int            _cmdID;
    bool           _init2Check;
    ShortcutKey*   _pShKey;
};
struct SCNotification {
    HWND         hwndFrom;
    uintptr_t    idFrom;
    unsigned int code;
};
struct toolbarIconsWithDarkMode {
    HBITMAP hToolbarBmp;
    HICON   hToolbarIcon;
    HICON   hToolbarIconDarkMode;
};
enum {
    SCI_GETLENGTH         = 2006,
    SCI_GETCURRENTPOS     = 2008,
    SCI_SETSAVEPOINT      = 2014,
    SCI_GETSELECTIONSTART = 2143,
    SCI_GETSELECTIONEND   = 2145,
    SCI_GETMODIFY         = 2159,
    SCI_LINEFROMPOSITION  = 2166,
    SCI_EMPTYUNDOBUFFER   = 2175,
    SCI_SETTEXT           = 2181,
    SCI_GETTEXT           = 2182,
    SCI_DOCUMENTSTART     = 2316,
    SCI_DOCUMENTEND       = 2318,
    SCI_GRABFOCUS         = 2400,
};
enum : UINT {
    NPPMSG                          = WM_USER + 1000,
    NPPM_GETCURRENTSCINTILLA        = NPPMSG + 4,
    NPPM_SETCURRENTLANGTYPE         = NPPMSG + 6,
    NPPM_GETCURRENTDOCINDEX         = NPPMSG + 23,
    NPPM_SETSTATUSBAR               = NPPMSG + 24,
    NPPM_ACTIVATEDOC                = NPPMSG + 28,
    NPPM_SETMENUITEMCHECK           = NPPMSG + 40,
    NPPM_MENUCOMMAND                = NPPMSG + 48,
    NPPM_GETPOSFROMBUFFERID         = NPPMSG + 57,
    NPPM_GETFULLPATHFROMBUFFERID    = NPPMSG + 58,
    NPPM_GETCURRENTBUFFERID         = NPPMSG + 60,
    NPPM_DOOPEN                     = NPPMSG + 77,
    NPPM_ADDTOOLBARICON_FORDARKMODE = NPPMSG + 101,
    IDM_FILE_NEW                    = 41001,
    IDM_FORMAT_AS_UTF_8             = 45008,
    NPPN_READY                      = 1001,
    NPPN_TBMODIFICATION             = 1002,
    NPPN_FILECLOSED                 = 1005,
    NPPN_SHUTDOWN                   = 1009,
    NPPN_DARKMODECHANGED            = 1027,
    STATUSBAR_DOC_TYPE              = 0,
    L_SQL                           = 17,
};

// ─── globals ────────────────────────────────────────────────────────────────

static NppData   g_npp    = {};
HINSTANCE        g_module = nullptr;
static HWND      g_msgwnd = nullptr;

static const wchar_t* const TITLE = L"Datamodder Query Files";

// g_funcs slot map. Notepad++ shows it as one flat menu; no submenus needed.
enum {
    F_RUN,            // Run query (also the toolbar button)
    F_QUERY_THIS,     // Query this file
    F_CANCEL,         // Cancel query
    F_SEP1,
    F_DESCRIBE,       // Describe THIS
    F_SUMMARIZE,      // Summarize THIS
    F_SEP2,
    F_AS_TABLE,       // Results as table (checkmark)
    F_SHOW_PANEL,     // Show result panel (docking dlgID)
    F_SEP3,
    F_SETTINGS,
    F_HELP,
    F_ABOUT,
    NFUNCS
};
static FuncItem g_funcs[NFUNCS] = {};

// SQL tab -> data tab it was opened for with "Query this file".
static std::map<UINT_PTR, UINT_PTR> g_bind;
static UINT_PTR g_result_buf = 0;          // the reusable text result tab

// ─── editor helpers ─────────────────────────────────────────────────────────

static HWND current_editor() {
    int which = 0;
    SendMessage(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
    return which ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}
static std::string get_text(HWND sci) {
    int len = (int)SendMessage(sci, SCI_GETLENGTH, 0, 0);
    if (len <= 0) return {};
    std::string buf(len + 1, '\0');
    SendMessage(sci, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf.data());
    buf.resize(len);
    return buf;
}
static UINT_PTR current_buffer() {
    return (UINT_PTR)SendMessage(g_npp._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0);
}
// Full path of a buffer; for an unsaved one this is its tab name ("new 3").
static std::wstring buffer_path(UINT_PTR buf) {
    LRESULT n = SendMessage(g_npp._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)buf, 0);
    if (n < 0) return L"";
    std::wstring p((size_t)n + 1, L'\0');
    SendMessage(g_npp._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)buf, (LPARAM)&p[0]);
    p.resize((size_t)n);
    return p;
}
static bool buffer_open(UINT_PTR buf) {
    return buf && SendMessage(g_npp._nppHandle, NPPM_GETPOSFROMBUFFERID, (WPARAM)buf, 0) != -1;
}
static bool activate_buffer(UINT_PTR buf) {
    LRESULT pos = SendMessage(g_npp._nppHandle, NPPM_GETPOSFROMBUFFERID, (WPARAM)buf, 0);
    if (pos == -1) return false;
    SendMessage(g_npp._nppHandle, NPPM_ACTIVATEDOC, (WPARAM)(pos >> 30), (LPARAM)(pos & 0x3FFFFFFF));
    return true;
}

// Text + modified flag of any open buffer. A buffer that is not on screen is
// briefly activated (with redraw off) and the original one restored.
static bool read_buffer(UINT_PTR buf, std::string& text, bool& modified) {
    HWND npp = g_npp._nppHandle;
    LRESULT pos = SendMessage(npp, NPPM_GETPOSFROMBUFFERID, (WPARAM)buf, 0);
    if (pos == -1) return false;
    int view = (int)(pos >> 30), idx = (int)(pos & 0x3FFFFFFF);
    if ((int)SendMessage(npp, NPPM_GETCURRENTDOCINDEX, 0, view) == idx) {
        HWND sci = view ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
        text = get_text(sci);
        modified = SendMessage(sci, SCI_GETMODIFY, 0, 0) != 0;
        return true;
    }
    UINT_PTR back = current_buffer();
    SendMessage(npp, WM_SETREDRAW, FALSE, 0);
    SendMessage(npp, NPPM_ACTIVATEDOC, view, idx);
    HWND sci = current_editor();
    text = get_text(sci);
    modified = SendMessage(sci, SCI_GETMODIFY, 0, 0) != 0;
    activate_buffer(back);
    SendMessage(npp, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(npp, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return true;
}

static void msg(const std::wstring& w, UINT icon) {
    MessageBoxW(g_npp._nppHandle, w.c_str(), TITLE, MB_OK | icon);
}

static std::wstring file_name(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

static bool is_data_ext(const std::wstring& e) {
    for (const wchar_t* x : { L".csv", L".tsv", L".tab", L".txt", L".parquet", L".json",
                              L".ndjson", L".jsonl", L".xlsx" })
        if (e == x) return true;
    return false;
}
static bool is_binary_ext(const std::wstring& e) { return e == L".parquet" || e == L".xlsx"; }

// ─── showing results ─────────────────────────────────────────────────────────

// Write into the (reused) result tab and give it the focus.
static void show_text_result(const std::string& text) {
    HWND npp = g_npp._nppHandle;
    if (!(g_settings.reuse_tab && buffer_open(g_result_buf) && activate_buffer(g_result_buf))) {
        SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
        SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FORMAT_AS_UTF_8);
        g_result_buf = current_buffer();
    }
    HWND sci = current_editor();
    SendMessage(sci, SCI_SETTEXT, 0, (LPARAM)text.c_str());
    SendMessage(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessage(sci, SCI_SETSAVEPOINT, 0, 0);      // closing the tab doesn't ask to save
    SendMessage(sci, SCI_DOCUMENTSTART, 0, 0);
    SetFocus(sci);
    SendMessage(sci, SCI_GRABFOCUS, 0, 0);
}

static void open_csv_tab(const std::string& csv) {
    HWND npp = g_npp._nppHandle;
    SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
    SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FORMAT_AS_UTF_8);
    HWND sci = current_editor();
    SendMessage(sci, SCI_SETTEXT, 0, (LPARAM)csv.c_str());
    SendMessage(sci, SCI_DOCUMENTSTART, 0, 0);
    SetFocus(sci);
}

// An error / notice before DuckDB even ran, shown where results go.
static void show_notice(const std::wstring& status, const std::string& text) {
    if (g_settings.view == ResultView::Table) grid_show_message(status, widen(text));
    else show_text_result("-- " + narrow(status) + "\n\n" + text);
}

// ─── running a query ─────────────────────────────────────────────────────────

static const UINT WM_QF_DONE = WM_APP + 1;
static const UINT_PTR TIMER_TIMEOUT = 1;

struct Job {
    // to run
    std::wstring exe, db, script, cwd;
    size_t       cap = 0;
    std::vector<std::wstring> temps;     // deleted afterwards (script included)
    // to present the result
    bool              grid = false;
    std::vector<qf::Stmt> stmts;
    int               base_line = 0;     // 0-based document line of the selection start
    std::wstring      this_label;        // "orders.csv", or "" when THIS isn't used
    size_t            max_rows = 1000;
    // filled by the worker
    DuckRun           run;
};

static Job*  g_job = nullptr;            // the running job, owned by the UI thread
static bool  g_timed_out = false;

static void set_status_bar(const std::wstring& s) {
    SendMessage(g_npp._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, (LPARAM)s.c_str());
}

static DWORD WINAPI worker(LPVOID p) {
    Job* j = static_cast<Job*>(p);
    j->run = run_duckdb(j->exe, j->db, j->script, j->cwd, j->cap);
    PostMessageW(g_msgwnd, WM_QF_DONE, 0, (LPARAM)j);
    return 0;
}

static std::wstring run_status(const Job& j) {
    wchar_t secs[32];
    swprintf(secs, 32, L"%.2f s", j.run.ms / 1000.0);
    std::wstring s = L"Query Files";
    if (!j.this_label.empty()) s += L"  \x00B7  THIS = " + j.this_label;
    s += L"  \x00B7  ";
    s += secs;
    return s;
}

static std::string one_line(const std::string& s, size_t max) {
    std::string o;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = true; continue; }
        if (space && !o.empty()) o += ' ';
        space = false;
        o += c;
        if (o.size() > max) { o.resize(max); o += "..."; break; }
    }
    return o;
}

static void finish_job(Job* j) {
    KillTimer(g_msgwnd, TIMER_TIMEOUT);
    g_job = nullptr;
    for (const auto& t : j->temps) DeleteFileW(t.c_str());

    const DuckRun& r = j->run;
    std::wstring status = run_status(*j);
    std::vector<qf::Section> secs = qf::split_output(r.out);
    bool failed = r.exit_code != 0 && !r.truncated && !r.cancelled;

    // Error text, with the location in the user's document when DuckDB gives one.
    std::string error;
    if (r.cancelled) {
        error = g_timed_out ? "Query stopped: it ran longer than the timeout (Settings \xE2\x86\x92 Behavior)."
                            : "Query cancelled.";
    } else if (failed) {
        const qf::Section& last = secs.back();
        std::string where;
        if (last.stmt < 0) {
            where = j->this_label.empty() ? "Error before the first statement (prelude SQL from Settings?)"
                                          : "Error while reading THIS (" + narrow(j->this_label) + ")";
        } else {
            int l = qf::error_line(last.body);
            int doc_line = j->base_line + j->stmts[last.stmt].first_line + (l > 0 ? l : 1);
            where = "Error in statement " + std::to_string(last.stmt + 1) + ", line " + std::to_string(doc_line);
        }
        error = where + ":\n\n" + qf::unalias_error(last.body);
        if (r.exit_code == -1) error = r.out;
    }
    g_timed_out = false;
    set_status_bar(r.cancelled ? L"Query Files: cancelled" : failed ? L"Query Files: error" : L"Query Files: done");

    if (j->grid) {
        if (!error.empty()) { grid_show_message(status, widen(error)); delete j; return; }
        std::vector<GridPage> pages;
        for (const qf::Section& s : secs) {
            if (s.stmt < 0) continue;
            GridPage gp;
            gp.table = qf::parse_csv(s.body, j->max_rows, gp.capped);
            if (gp.table.cols.empty()) continue;                 // statement without a result set
            pages.push_back(std::move(gp));
        }
        for (size_t i = 0; i < pages.size(); ++i)
            pages[i].title = L"Result " + std::to_wstring(i + 1);
        if (r.truncated) status += L"  \x00B7  output stopped at " + std::to_wstring(g_settings.max_output_mb) + L" MB";
        if (pages.empty()) grid_show_message(status, L"Done. The statements returned no result set.");
        else               grid_show_results(std::move(pages), status);
        delete j;
        return;
    }

    // Text tab
    std::string out;
    if (g_settings.header_line) {
        char when[32];
        time_t now = time(nullptr);
        tm lt;
        localtime_s(&lt, &now);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &lt);
        out += "-- " + narrow(status) + "  \xC2\xB7  " + when + "\n\n";
    }
    int with_output = 0;
    for (const qf::Section& s : secs) if (s.stmt >= 0 && !s.body.empty()) ++with_output;
    for (size_t i = 0; i < secs.size(); ++i) {
        const qf::Section& s = secs[i];
        if (failed && i + 1 == secs.size()) break;               // the error, below
        if (s.body.empty()) continue;
        if (s.stmt >= 0 && with_output > 1)
            out += "-- [" + std::to_string(s.stmt + 1) + "] " +
                   one_line(qf::unalias_this(j->stmts[s.stmt].text, false), 80) + "\n";
        out += qf::unalias_this(qf::clean_text_output(s.body), true) + "\n";
    }
    if (!error.empty()) out += "-- " + error + "\n";
    if (r.truncated)
        out += "\n-- Output stopped at " + std::to_string(g_settings.max_output_mb) +
               " MB (Settings \xE2\x86\x92 Results).\n";
    if (out.empty() || (g_settings.header_line && with_output == 0 && error.empty()))
        out += "-- Done. The statements returned no result set.\n";
    show_text_result(out);
    delete j;
}

static LRESULT CALLBACK msgwnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_QF_DONE) { finish_job(reinterpret_cast<Job*>(l)); return 0; }
    if (m == WM_TIMER && w == TIMER_TIMEOUT) {
        KillTimer(h, TIMER_TIMEOUT);
        g_timed_out = true;
        cancel_duckdb();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ensure_msgwnd() {
    if (g_msgwnd) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = msgwnd_proc;
    wc.hInstance     = g_module;
    wc.lpszClassName = L"QueryFilesMsgWnd";
    RegisterClassW(&wc);
    g_msgwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_module, nullptr);
}

enum class Kind { Run, Describe, Summarize };

// Where THIS comes from for the current tab.
struct ThisSource {
    UINT_PTR     buf = 0;          // an open buffer, or
    std::wstring path;             // a file on disk (-- this: header)
};

static void start(Kind kind) {
    if (g_job) {
        if (MessageBoxW(g_npp._nppHandle, L"A query is still running. Cancel it?", TITLE,
                        MB_YESNO | MB_ICONQUESTION) == IDYES)
            cancel_duckdb();
        return;
    }
    std::wstring exe = find_duckdb(g_settings.duckdb_path);
    if (exe.empty()) {
        msg(L"duckdb.exe not found.\n\nPut it on your PATH or next to the plugin "
            L"(plugins\\QueryFiles\\duckdb.exe), or set its location in Settings. "
            L"If Datamodder File Tools already uses DuckDB, that one is picked up automatically.\n\n"
            L"Download: https://duckdb.org/", MB_ICONWARNING);
        return;
    }

    HWND sci = current_editor();
    UINT_PTR cur = current_buffer();
    std::wstring cur_path = buffer_path(cur);
    std::wstring cur_ext = lower_ext(cur_path);
    std::string doc = get_text(sci);

    // ── the SQL ──
    std::string sql;
    int base_line = 0;
    if (kind == Kind::Run) {
        int ss = (int)SendMessage(sci, SCI_GETSELECTIONSTART, 0, 0);
        int se = (int)SendMessage(sci, SCI_GETSELECTIONEND, 0, 0);
        if (ss != se && se <= (int)doc.size()) {
            sql = doc.substr(ss, se - ss);
            base_line = (int)SendMessage(sci, SCI_LINEFROMPOSITION, ss, 0);
        } else sql = doc;
    } else {
        sql = (kind == Kind::Describe) ? "describe this;" : "summarize this;";
    }

    // ── where THIS comes from ──
    ThisSource src;
    auto b = g_bind.find(cur);
    std::string header = qf::this_header_path(doc);
    if (b != g_bind.end() && buffer_open(b->second)) {
        src.buf = b->second;
    } else if (!header.empty()) {
        std::wstring p = widen(header);
        if (p.size() < 2 || (p[1] != L':' && p[0] != L'\\'))        // relative to the SQL file
            p = dir_of(cur_path) + L"\\" + p;
        src.path = p;
    } else if (kind != Kind::Run) {
        src.buf = cur;                                              // Describe/Summarize on a data tab
    } else if (is_data_ext(cur_ext) && cur_ext != L".txt") {
        msg(L"This tab is a data file. Use \"Query this file\" to open a SQL tab for it, "
            L"or Describe / Summarize THIS.", MB_ICONINFORMATION);
        return;
    }

    // ── statements, THIS rewrite ──
    std::vector<qf::Stmt> stmts = qf::split_statements(sql);
    if (stmts.empty()) return;
    bool uses_this = false;
    for (qf::Stmt& s : stmts) {
        if (s.dot) continue;
        qf::ThisRewrite r = qf::rewrite_this(s.text);
        if (!r.ok) {
            int line = base_line + s.first_line + r.line + 1;
            show_notice(L"Query Files  \x00B7  not run",
                        "THIS is reserved by Query Files for the bound data file and can't be used "
                        "as a name (line " + std::to_string(line) + ").\n\nTHIS is read-only: use it in FROM, "
                        "JOIN, DESCRIBE, COPY ... TO, but not as an alias, CTE, table or view name, or as "
                        "the target of INSERT / UPDATE / DELETE. For a column called \"this\", write it "
                        "with double quotes.");
            return;
        }
        uses_this |= r.uses_this;
        s.text = r.text;
    }

    Job* j = new Job;
    j->exe = exe;
    j->db = widen(g_settings.db_file);
    j->grid = g_settings.view == ResultView::Table;
    j->max_rows = (size_t)g_settings.max_rows;
    j->cap = (size_t)g_settings.max_output_mb << 20;
    j->base_line = base_line;

    // ── materialise THIS ──
    qf::ScriptOptions o;
    std::wstring this_path;
    if (uses_this) {
        if (!src.buf && src.path.empty()) {
            delete j;
            msg(L"THIS is not bound to a file.\n\nUse \"Query this file\" from a data tab, add a line\n"
                L"-- this: <path to the file>\nat the top of this SQL, or name the file in the query: "
                L"from 'orders.csv'.", MB_ICONINFORMATION);
            return;
        }
        if (src.buf) {
            std::wstring p = buffer_path(src.buf);
            std::wstring ext = lower_ext(p);
            j->this_label = file_name(p);
            if (is_binary_ext(ext)) {
                this_path = p;
            } else {
                std::string text;
                bool modified = false;
                read_buffer(src.buf, text, modified);
                if (!modified && file_exists(p)) {
                    this_path = p;                                  // fast path: read from disk
                } else {
                    this_path = temp_path(ext.empty() || ext == L".txt" ? L".csv" : ext);
                    write_file(this_path, text);
                    j->temps.push_back(this_path);
                    if (modified) j->this_label += L" (unsaved changes)";
                }
            }
        } else {
            this_path = src.path;
            j->this_label = file_name(this_path);
        }
        if (!file_exists(this_path)) {
            delete j;
            msg(L"The file for THIS was not found:\n\n" + this_path, MB_ICONWARNING);
            return;
        }
        o.this_reader = qf::reader_sql(narrow(this_path), narrow(lower_ext(this_path)));
    }

    o.grid      = j->grid;
    o.text_mode = g_settings.text_mode;
    o.max_rows  = g_settings.max_rows;
    o.prelude   = g_settings.prelude;
    j->stmts    = stmts;

    j->script = temp_path(L".sql");
    if (!write_file(j->script, qf::build_script(o, stmts))) {
        delete j;
        msg(L"Could not write the query script to the temp folder.", MB_ICONWARNING);
        return;
    }
    j->temps.push_back(j->script);

    // Relative paths in the query resolve next to the SQL file.
    if (file_exists(cur_path))                 j->cwd = dir_of(cur_path);
    else if (!this_path.empty() && j->temps.size() == 1) j->cwd = dir_of(this_path);
    else {
        wchar_t home[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        j->cwd = home;
    }

    ensure_msgwnd();
    g_job = j;
    g_timed_out = false;
    set_status_bar(L"Query Files: running...  (Plugins \x2192 Datamodder Query Files \x2192 Cancel query)");
    if (g_settings.timeout_s > 0) SetTimer(g_msgwnd, TIMER_TIMEOUT, (UINT)g_settings.timeout_s * 1000, nullptr);
    HANDLE t = CreateThread(nullptr, 0, worker, j, 0, nullptr);
    if (t) CloseHandle(t);
    else { g_job = nullptr; delete j; msg(L"Could not start the query thread.", MB_ICONWARNING); }
}

// ─── commands ───────────────────────────────────────────────────────────────

static void cmd_run()       { start(Kind::Run); }
static void cmd_describe()  { start(Kind::Describe); }
static void cmd_summarize() { start(Kind::Summarize); }

static void cmd_cancel() {
    if (g_job) cancel_duckdb();
    else msg(L"No query is running.", MB_ICONINFORMATION);
}

static void cmd_query_this() {
    UINT_PTR data = current_buffer();
    std::wstring p = buffer_path(data);
    if (lower_ext(p) == L".sql") {
        msg(L"Open a data file first (CSV, Parquet, JSON, Excel ...), then choose Query this file.",
            MB_ICONINFORMATION);
        return;
    }
    std::string head = file_exists(p) ? "-- this: " + narrow(p) + "\n"
                                      : "-- THIS = " + narrow(file_name(p)) + " (unsaved tab)\n";
    std::string text = head + "select *\nfrom this\nlimit 100;\n";

    HWND npp = g_npp._nppHandle;
    SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
    SendMessage(npp, NPPM_MENUCOMMAND, 0, IDM_FORMAT_AS_UTF_8);
    SendMessage(npp, NPPM_SETCURRENTLANGTYPE, 0, L_SQL);
    g_bind[current_buffer()] = data;
    HWND sci = current_editor();
    SendMessage(sci, SCI_SETTEXT, 0, (LPARAM)text.c_str());
    SendMessage(sci, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessage(sci, SCI_DOCUMENTEND, 0, 0);
    SetFocus(sci);
}

static void update_view_check() {
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, (WPARAM)g_funcs[F_AS_TABLE]._cmdID,
                g_settings.view == ResultView::Table ? TRUE : FALSE);
}

static void cmd_toggle_view() {
    g_settings.view = g_settings.view == ResultView::Table ? ResultView::Text : ResultView::Table;
    save_settings();
    update_view_check();
}

static void cmd_show_panel() { grid_show(); }
static void cmd_settings()   { show_settings_dialog(g_npp._nppHandle); update_view_check(); }
static void cmd_about()      { show_about_dialog(g_npp._nppHandle); }
static void cmd_help() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring p = dir_of(path) + L"\\help.txt";
    SendMessage(g_npp._nppHandle, NPPM_DOOPEN, 0, (LPARAM)p.c_str());
}

static void add_toolbar_button() {
    toolbarIconsWithDarkMode icons = {};
    icons.hToolbarBmp = (HBITMAP)LoadImageW(g_module, MAKEINTRESOURCEW(IDB_TB_RUN), IMAGE_BITMAP, 0, 0,
                                            LR_DEFAULTSIZE | LR_LOADMAP3DCOLORS);
    icons.hToolbarIcon = (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(IDI_TB_RUN), IMAGE_ICON, 0, 0,
                                           LR_LOADTRANSPARENT | LR_DEFAULTSIZE);
    icons.hToolbarIconDarkMode = (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(IDI_TB_RUN_DARK), IMAGE_ICON, 0, 0,
                                                   LR_LOADTRANSPARENT | LR_DEFAULTSIZE);
    SendMessage(g_npp._nppHandle, NPPM_ADDTOOLBARICON_FORDARKMODE, (WPARAM)g_funcs[F_RUN]._cmdID, (LPARAM)&icons);
}

// ─── plugin exports ─────────────────────────────────────────────────────────

extern "C" {

__declspec(dllexport) const wchar_t* getName() { return TITLE; }

__declspec(dllexport) void setInfo(NppData d) {
    g_npp = d;
    load_settings();
    GridHost host;
    host.npp = d._nppHandle;
    host.func_index = F_SHOW_PANEL;
    host.open_as_text = open_csv_tab;
    grid_init(host);
}

__declspec(dllexport) FuncItem* getFuncsArray(int* n) { *n = NFUNCS; return g_funcs; }

__declspec(dllexport) void beNotified(SCNotification* n) {
    switch (n->code) {
    case NPPN_TBMODIFICATION:
        add_toolbar_button();
        break;
    case NPPN_READY: {
        update_view_check();
        GridHost host;                                    // cmdIDs are known now
        host.npp = g_npp._nppHandle;
        host.func_index = F_SHOW_PANEL;
        host.cmd_id = g_funcs[F_SHOW_PANEL]._cmdID;
        host.open_as_text = open_csv_tab;
        grid_init(host);
        break;
    }
    case NPPN_FILECLOSED: {
        UINT_PTR buf = (UINT_PTR)n->idFrom;
        for (auto it = g_bind.begin(); it != g_bind.end();)
            it = (it->first == buf || it->second == buf) ? g_bind.erase(it) : std::next(it);
        if (buf == g_result_buf) g_result_buf = 0;
        break;
    }
    case NPPN_DARKMODECHANGED:
        grid_dark_mode_changed();
        break;
    case NPPN_SHUTDOWN:
        cancel_duckdb();
        break;
    }
}

__declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

__declspec(dllexport) BOOL isUnicode() { return TRUE; }

} // extern "C"

// ─── DLL entry ──────────────────────────────────────────────────────────────

static void init_func(int i, const wchar_t* name, PFUNCPLUGINCMD fn) {
    wcscpy_s(g_funcs[i]._menuItemName, name);
    g_funcs[i]._pFunc = fn;
}

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
        init_func(F_RUN,        L"Run query",              cmd_run);
        init_func(F_QUERY_THIS, L"Query this file",        cmd_query_this);
        init_func(F_CANCEL,     L"Cancel query",           cmd_cancel);
        init_func(F_SEP1,       L"-",                      nullptr);
        init_func(F_DESCRIBE,   L"Describe THIS",          cmd_describe);
        init_func(F_SUMMARIZE,  L"Summarize THIS",         cmd_summarize);
        init_func(F_SEP2,       L"-",                      nullptr);
        init_func(F_AS_TABLE,   L"Results as table",       cmd_toggle_view);
        init_func(F_SHOW_PANEL, L"Show result panel",      cmd_show_panel);
        init_func(F_SEP3,       L"-",                      nullptr);
        init_func(F_SETTINGS,   L"Settings...",            cmd_settings);
        init_func(F_HELP,       L"Help",                   cmd_help);
        init_func(F_ABOUT,      L"About",                  cmd_about);
    }
    return TRUE;
}
