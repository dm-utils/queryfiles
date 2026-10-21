#define NOMINMAX
#include "result_grid.h"
#include <commctrl.h>
#include <cwchar>
#include <algorithm>
#include "settings.h"
#include "resource.h"
#include "duck_runner.h"

extern HINSTANCE g_module;

// ─── Notepad++ docking ABI (only what the panel uses) ─────────────────────────

namespace {

const UINT NPPMSG_                       = WM_USER + 1000;
const UINT NPPM_MODELESSDIALOG           = NPPMSG_ + 12;
const UINT NPPM_DMMSHOW                  = NPPMSG_ + 30;
const UINT NPPM_DMMREGASDCKDLG           = NPPMSG_ + 33;
const UINT NPPM_SETMENUITEMCHECK         = NPPMSG_ + 40;
const UINT NPPM_DARKMODESUBCLASSANDTHEME = NPPMSG_ + 112;
const WPARAM MODELESSDIALOGADD   = 0;
const WPARAM DMF_INIT            = 0x0000000B;
const WPARAM DMF_HANDLE_CHANGE   = 0x0000000C;
const UINT   DMN_CLOSE           = 1050 + 1;
const UINT   DWS_ICONTAB         = 0x00000001;
const UINT   DWS_DF_CONT_BOTTOM  = 3u << 28;

struct DockedWidgetData {
    HWND           hClient = nullptr;
    const wchar_t* pszName = nullptr;
    int            dlgID = 0;
    UINT           uMask = 0;
    HICON          hIconTab = nullptr;
    const wchar_t* pszAddInfo = nullptr;
    RECT           rcFloat = {};
    int            iPrevCont = 0;
    const wchar_t* pszModuleName = nullptr;
};

// ─── state ────────────────────────────────────────────────────────────────────

const int ID_STATUS = 1001, ID_TABS = 1002, ID_LIST = 1003, ID_MSG = 1004;
const wchar_t NULL_W = L'\x01';

struct Page {
    std::wstring                           title;
    std::vector<std::wstring>              cols;
    std::vector<std::vector<std::wstring>> rows;     // NULL cell = L"\x01"
    std::vector<bool>                      numeric;  // per column
    std::vector<int>                       order;    // display row -> data row
    int  sort_col  = 0;                              // 0 = "#" (original order)
    bool sort_desc = false;
    bool capped    = false;
};

GridHost          g_host;
HWND              g_dlg = nullptr, g_status = nullptr, g_tabs = nullptr, g_list = nullptr, g_msg = nullptr;
HFONT             g_mono = nullptr;
DockedWidgetData  g_dock;
std::vector<Page> g_pages;
int               g_cur = -1;
bool              g_showing_msg = false;
std::wstring      g_base_status;
std::wstring      g_null_text = L"NULL";

int scale(int px) {
    static int dpi = [] { HDC dc = GetDC(nullptr); int d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); return d; }();
    return MulDiv(px, dpi, 96);
}

bool is_null(const std::wstring& s) { return s.size() == 1 && s[0] == NULL_W; }

bool parses_as_number(const std::wstring& s) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    wcstod(s.c_str(), &end);
    return end && *end == 0 && end != s.c_str();
}

std::wstring thousands(size_t n) {
    std::wstring d = std::to_wstring(n), o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o += L',';
        o += d[i];
    }
    return o;
}

void set_menu_check(bool on) {
    if (g_host.npp && g_host.cmd_id)
        SendMessage(g_host.npp, NPPM_SETMENUITEMCHECK, (WPARAM)g_host.cmd_id, on ? TRUE : FALSE);
}

// ─── layout ───────────────────────────────────────────────────────────────────

void layout() {
    if (!g_dlg) return;
    RECT rc;
    GetClientRect(g_dlg, &rc);
    int w = rc.right, h = rc.bottom;
    int pad = scale(4);
    int sh  = scale(20);
    MoveWindow(g_status, pad, pad / 2, w - 2 * pad, sh - pad / 2, TRUE);
    int y = sh;
    bool tabs = !g_showing_msg && g_pages.size() > 1;
    ShowWindow(g_tabs, tabs ? SW_SHOW : SW_HIDE);
    if (tabs) {
        int th = scale(24);
        MoveWindow(g_tabs, 0, y, w, th, TRUE);
        y += th;
    }
    MoveWindow(g_list, 0, y, w, std::max(0, h - y), TRUE);
    MoveWindow(g_msg,  0, y, w, std::max(0, h - y), TRUE);
}

void update_status() {
    std::wstring s = g_base_status;
    if (!g_showing_msg && g_cur >= 0 && g_cur < (int)g_pages.size()) {
        const Page& p = g_pages[g_cur];
        if (g_pages.size() > 1) s += L"  \x00B7  " + p.title;
        s += L"  \x00B7  " + thousands(p.rows.size()) + (p.rows.size() == 1 ? L" row" : L" rows");
        if (p.capped) s += L" (first " + thousands(p.rows.size()) + L" shown, see Settings \x2192 Max. rows)";
    }
    SetWindowTextW(g_status, s.c_str());
}

// ─── list view ────────────────────────────────────────────────────────────────

void set_sort_arrow() {
    HWND hdr = ListView_GetHeader(g_list);
    int n = Header_GetItemCount(hdr);
    const Page& p = g_pages[g_cur];
    for (int i = 0; i < n; ++i) {
        HDITEMW it = {};
        it.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == p.sort_col && i != 0) it.fmt |= p.sort_desc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hdr, i, &it);
    }
}

void show_page(int idx) {
    g_cur = idx;
    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCount(g_list, 0);
    while (ListView_DeleteColumn(g_list, 0)) {}

    const Page& p = g_pages[idx];
    HDC dc = GetDC(g_list);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessage(g_list, WM_GETFONT, 0, 0));
    auto text_w = [&](const std::wstring& s) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, s.c_str(), (int)std::min<size_t>(s.size(), 200), &sz);
        return (int)sz.cx;
    };

    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt  = LVCFMT_LEFT;
    c.pszText = const_cast<wchar_t*>(L"#");
    c.cx = text_w(thousands(p.rows.size())) + scale(16);
    ListView_InsertColumn(g_list, 0, &c);

    const size_t sample = std::min<size_t>(p.rows.size(), 200);
    for (size_t k = 0; k < p.cols.size(); ++k) {
        int w = text_w(p.cols[k]) + scale(24);
        for (size_t r = 0; r < sample; ++r) {
            const std::wstring& v = p.rows[r][k];
            w = std::max(w, text_w(is_null(v) ? g_null_text : v) + scale(14));
        }
        c.fmt     = (g_settings.right_align && p.numeric[k]) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = const_cast<wchar_t*>(p.cols[k].c_str());
        c.cx      = std::min(std::max(w, scale(40)), scale(400));
        ListView_InsertColumn(g_list, (int)k + 1, &c);
    }
    SelectObject(dc, old);
    ReleaseDC(g_list, dc);

    ListView_SetItemCountEx(g_list, (int)p.rows.size(), 0);
    set_sort_arrow();
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    update_status();
}

void sort_page(int col) {
    Page& p = g_pages[g_cur];
    if (col == p.sort_col) p.sort_desc = !p.sort_desc;
    else { p.sort_col = col; p.sort_desc = false; }

    if (col == 0) {
        for (size_t i = 0; i < p.order.size(); ++i) p.order[i] = (int)i;
        if (p.sort_desc) std::reverse(p.order.begin(), p.order.end());
    } else {
        const size_t c = (size_t)col - 1;
        const bool num = p.numeric[c], desc = p.sort_desc;
        std::stable_sort(p.order.begin(), p.order.end(), [&](int a, int b) {
            const std::wstring& x = p.rows[a][c];
            const std::wstring& y = p.rows[b][c];
            bool xn = is_null(x), yn = is_null(y);
            if (xn || yn) return !xn && yn;                // NULLs last, either direction
            int r;
            if (num) {
                double dx = wcstod(x.c_str(), nullptr), dy = wcstod(y.c_str(), nullptr);
                r = dx < dy ? -1 : (dx > dy ? 1 : 0);
            } else {
                r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                                    x.c_str(), (int)x.size(), y.c_str(), (int)y.size(),
                                    nullptr, nullptr, 0) - CSTR_EQUAL;
            }
            return desc ? r > 0 : r < 0;
        });
    }
    set_sort_arrow();
    InvalidateRect(g_list, nullptr, FALSE);
}

std::vector<int> selected_rows() {
    std::vector<int> sel;
    for (int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); i != -1;
         i = ListView_GetNextItem(g_list, i, LVNI_SELECTED))
        sel.push_back(i);
    return sel;
}

void to_clipboard(const std::wstring& text) {
    if (!OpenClipboard(g_dlg)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

std::wstring tsv_cell(const std::wstring& v) {
    if (is_null(v)) return L"";
    std::wstring o = v;
    for (auto& ch : o) if (ch == L'\t' || ch == L'\n' || ch == L'\r') ch = L' ';
    return o;
}

// Tab-separated, pastes straight into Excel. No selection = everything.
void copy_rows(bool header) {
    if (g_cur < 0) return;
    const Page& p = g_pages[g_cur];
    std::vector<int> sel = selected_rows();
    if (sel.empty()) for (size_t i = 0; i < p.order.size(); ++i) sel.push_back((int)i);
    std::wstring t;
    if (header) {
        for (size_t k = 0; k < p.cols.size(); ++k) t += (k ? L"\t" : L"") + tsv_cell(p.cols[k]);
        t += L"\r\n";
    }
    for (int d : sel) {
        const auto& row = p.rows[p.order[d]];
        for (size_t k = 0; k < row.size(); ++k) t += (k ? L"\t" : L"") + tsv_cell(row[k]);
        t += L"\r\n";
    }
    to_clipboard(t);
}

std::string csv_cell(const std::wstring& v) {
    if (is_null(v)) return "";
    std::string s = narrow(v);
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char ch : s) { if (ch == '"') o += '"'; o += ch; }
    return o + "\"";
}

void open_as_text() {
    if (g_cur < 0 || !g_host.open_as_text) return;
    const Page& p = g_pages[g_cur];
    std::string t;
    for (size_t k = 0; k < p.cols.size(); ++k) t += (k ? "," : "") + csv_cell(p.cols[k]);
    t += "\n";
    for (int r : p.order) {
        const auto& row = p.rows[r];
        for (size_t k = 0; k < row.size(); ++k) t += (k ? "," : "") + csv_cell(row[k]);
        t += "\n";
    }
    g_host.open_as_text(t);
}

void select_all() {
    ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED);
}

void context_menu(int item, int sub) {
    enum { M_COPY = 1, M_COPY_NOHDR, M_COPY_CELL, M_SELALL, M_TEXT };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, M_COPY,        L"Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING, M_COPY_NOHDR,  L"Copy without header");
    AppendMenuW(m, MF_STRING | (item >= 0 && sub > 0 ? 0 : MF_GRAYED), M_COPY_CELL, L"Copy cell");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_SELALL,      L"Select all\tCtrl+A");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_TEXT,        L"Open as CSV in a new tab");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_dlg, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case M_COPY:       copy_rows(true);  break;
    case M_COPY_NOHDR: copy_rows(false); break;
    case M_COPY_CELL: {
        const Page& p = g_pages[g_cur];
        to_clipboard(tsv_cell(p.rows[p.order[item]][sub - 1]));
        break;
    }
    case M_SELALL:     select_all();     break;
    case M_TEXT:       open_as_text();   break;
    }
}

COLORREF blend(COLORREF a, COLORREF b) {
    return RGB((GetRValue(a) + GetRValue(b)) / 2, (GetGValue(a) + GetGValue(b)) / 2, (GetBValue(a) + GetBValue(b)) / 2);
}

LRESULT on_list_notify(NMHDR* h) {
    switch (h->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(h);
        if (!(di->item.mask & LVIF_TEXT) || g_cur < 0) return 0;
        const Page& p = g_pages[g_cur];
        if (di->item.iItem < 0 || di->item.iItem >= (int)p.order.size()) return 0;
        int r = p.order[di->item.iItem];
        if (di->item.iSubItem == 0) {
            swprintf(di->item.pszText, di->item.cchTextMax, L"%d", r + 1);
        } else {
            const std::wstring& v = p.rows[r][di->item.iSubItem - 1];
            lstrcpynW(di->item.pszText, is_null(v) ? g_null_text.c_str() : v.c_str(), di->item.cchTextMax);
        }
        return 0;
    }
    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(h);
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            COLORREF fg = ListView_GetTextColor(g_list);
            bool dim = cd->iSubItem == 0;
            if (!dim && g_cur >= 0) {
                const Page& p = g_pages[g_cur];
                int d = (int)cd->nmcd.dwItemSpec;
                if (d >= 0 && d < (int)p.order.size())
                    dim = is_null(p.rows[p.order[d]][cd->iSubItem - 1]);
            }
            cd->clrText = dim ? blend(fg, ListView_GetBkColor(g_list)) : fg;
            return CDRF_NEWFONT;
        }
        }
        return CDRF_DODEFAULT;
    }
    case LVN_COLUMNCLICK:
        sort_page(reinterpret_cast<NMLISTVIEW*>(h)->iSubItem);
        return 0;
    case LVN_KEYDOWN: {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(h);
        if (GetKeyState(VK_CONTROL) < 0) {
            if (kd->wVKey == 'C') copy_rows(true);
            if (kd->wVKey == 'A') select_all();
        }
        return 0;
    }
    case NM_RCLICK: {
        auto* ia = reinterpret_cast<NMITEMACTIVATE*>(h);
        context_menu(ia->iItem, ia->iSubItem);
        return 0;
    }
    }
    return 0;
}

// ─── the panel window ─────────────────────────────────────────────────────────

INT_PTR CALLBACK panel_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_dlg = dlg;
        HFONT ui = (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0);
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX,
                                   0, 0, 0, 0, dlg, (HMENU)(INT_PTR)ID_STATUS, g_module, nullptr);
        g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                                 0, 0, 0, 0, dlg, (HMENU)(INT_PTR)ID_TABS, g_module, nullptr);
        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                                 0, 0, 0, 0, dlg, (HMENU)(INT_PTR)ID_LIST, g_module, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                          LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
        g_msg = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                0, 0, 0, 0, dlg, (HMENU)(INT_PTR)ID_MSG, g_module, nullptr);
        for (HWND c : { g_status, g_tabs, g_list }) SendMessage(c, WM_SETFONT, (WPARAM)ui, FALSE);
        g_mono = CreateFontW(-scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessage(g_msg, WM_SETFONT, (WPARAM)g_mono, FALSE);
        return TRUE;
    }
    case WM_SIZE:
        layout();
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* h = reinterpret_cast<NMHDR*>(lp);
        if (h->hwndFrom == g_list) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, on_list_notify(h));
            return TRUE;
        }
        if (h->hwndFrom == g_tabs && h->code == TCN_SELCHANGE) {
            int i = TabCtrl_GetCurSel(g_tabs);
            if (i >= 0 && i < (int)g_pages.size()) show_page(i);
            return TRUE;
        }
        if (LOWORD(h->code) == DMN_CLOSE) {
            set_menu_check(false);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        // Keep Enter / Esc from closing the dialog when it floats.
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) return TRUE;
        break;
    }
    return FALSE;
}

void ensure_created() {
    if (g_dlg) return;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    CreateDialogParamW(g_module, MAKEINTRESOURCEW(IDD_RESULT_PANEL), g_host.npp, panel_proc, 0);
    if (!g_dlg) return;

    g_dock.hClient       = g_dlg;
    g_dock.pszName       = L"Query Files";
    g_dock.dlgID         = g_host.func_index;
    g_dock.uMask         = DWS_DF_CONT_BOTTOM | DWS_ICONTAB;
    g_dock.hIconTab      = (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(IDI_TB_RUN), IMAGE_ICON, 16, 16, 0);
    g_dock.pszModuleName = L"QueryFiles.dll";
    SendMessage(g_host.npp, NPPM_DMMREGASDCKDLG, 0, (LPARAM)&g_dock);
    SendMessage(g_host.npp, NPPM_MODELESSDIALOG, MODELESSDIALOGADD, (LPARAM)g_dlg);
    SendMessage(g_host.npp, NPPM_DARKMODESUBCLASSANDTHEME, DMF_INIT, (LPARAM)g_dlg);
}

void show_panel() {
    ensure_created();
    if (!g_dlg) return;
    SendMessage(g_host.npp, NPPM_DMMSHOW, 0, (LPARAM)g_dlg);
    set_menu_check(true);
}

Page make_page(GridPage&& gp) {
    Page p;
    p.title  = gp.title;
    p.capped = gp.capped;
    for (const std::string& c : gp.table.cols) p.cols.push_back(widen(c));
    p.rows.reserve(gp.table.rows.size());
    for (auto& r : gp.table.rows) {
        std::vector<std::wstring> wr;
        wr.reserve(p.cols.size());
        for (size_t k = 0; k < p.cols.size(); ++k) {
            if (k >= r.size())             wr.push_back(L"");
            else if (qf::is_null_cell(r[k])) wr.push_back(std::wstring(1, NULL_W));
            else                            wr.push_back(widen(r[k]));
        }
        p.rows.push_back(std::move(wr));
        r.clear();
    }
    p.numeric.assign(p.cols.size(), false);
    for (size_t k = 0; k < p.cols.size(); ++k) {
        bool any = false, all = true;
        for (const auto& r : p.rows) {
            const std::wstring& v = r[k];
            if (is_null(v) || v.empty()) continue;
            any = true;
            if (!parses_as_number(v)) { all = false; break; }
        }
        p.numeric[k] = any && all;
    }
    p.order.resize(p.rows.size());
    for (size_t i = 0; i < p.order.size(); ++i) p.order[i] = (int)i;
    return p;
}

} // namespace

// ─── public ───────────────────────────────────────────────────────────────────

void grid_init(const GridHost& host) { g_host = host; }

void grid_show_results(std::vector<GridPage>&& pages, const std::wstring& status) {
    show_panel();
    if (!g_dlg) return;
    g_null_text   = widen(g_settings.null_text);
    g_base_status = status;
    g_pages.clear();
    for (auto& gp : pages) g_pages.push_back(make_page(std::move(gp)));
    pages.clear();

    g_showing_msg = false;
    ShowWindow(g_msg, SW_HIDE);
    ShowWindow(g_list, SW_SHOW);
    TabCtrl_DeleteAllItems(g_tabs);
    for (size_t i = 0; i < g_pages.size(); ++i) {
        TCITEMW ti = {};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(g_pages[i].title.c_str());
        TabCtrl_InsertItem(g_tabs, (int)i, &ti);
    }
    layout();
    if (g_pages.empty()) return;
    int last = (int)g_pages.size() - 1;
    TabCtrl_SetCurSel(g_tabs, last);
    show_page(last);

    SetFocus(g_list);
    if (!g_pages[last].rows.empty()) {
        ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(g_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, 0, FALSE);
    }
}

void grid_show_message(const std::wstring& status, const std::wstring& text) {
    show_panel();
    if (!g_dlg) return;
    g_base_status = status;
    g_showing_msg = true;
    std::wstring t;                                  // EDIT controls want CRLF
    for (wchar_t c : text) { if (c == L'\n' && (t.empty() || t.back() != L'\r')) t += L'\r'; t += c; }
    SetWindowTextW(g_msg, t.c_str());
    ShowWindow(g_list, SW_HIDE);
    ShowWindow(g_msg, SW_SHOW);
    layout();
    update_status();
    SetFocus(g_msg);
    SendMessage(g_msg, EM_SETSEL, 0, 0);
}

void grid_show() {
    show_panel();
    if (g_dlg && g_pages.empty() && !g_showing_msg)
        grid_show_message(L"Query Files", L"No results yet. Run a query with the \x25B6 toolbar button or Plugins \x2192 Datamodder Query Files \x2192 Run query.");
}

void grid_dark_mode_changed() {
    if (!g_dlg) return;
    SendMessage(g_host.npp, NPPM_DARKMODESUBCLASSANDTHEME, DMF_HANDLE_CHANGE, (LPARAM)g_dlg);
    RedrawWindow(g_dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}
