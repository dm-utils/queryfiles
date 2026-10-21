#include "duck_runner.h"
#include <cwctype>
#include <mutex>

// ─── process state (one run at a time) ────────────────────────────────────────

static std::mutex g_mtx;
static HANDLE     g_proc      = nullptr;   // owned by run_duckdb while running
static bool       g_cancelled = false;

// All DuckDB children go into one job that dies with Notepad++, so a long
// query never outlives the editor.
static HANDLE job() {
    static HANDLE j = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
        return h;
    }();
    return j;
}

void cancel_duckdb() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_proc) { g_cancelled = true; TerminateProcess(g_proc, 1); }
}

static int spawn_capture(const std::wstring& cmdline, HANDLE in, const std::wstring& cwd,
                         size_t cap, DuckRun& r) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof si };
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = in;

    std::wstring cl = cmdline;                       // CreateProcessW may modify it
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    if (HANDLE j = job()) AssignProcessToJobObject(j, pi.hProcess);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_proc = pi.hProcess;
        g_cancelled = false;
    }
    ResumeThread(pi.hThread);

    char buf[16384];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
        r.out.append(buf, n);
        if (cap && r.out.size() > cap) {
            r.truncated = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        r.cancelled = g_cancelled;
        g_proc = nullptr;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

DuckRun run_duckdb(const std::wstring& exe, const std::wstring& db,
                   const std::wstring& script, const std::wstring& cwd, size_t cap) {
    DuckRun r;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE in = CreateFileW(script.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) { r.out = "Could not open the query script."; return r; }

    // DuckDB 2.x renders "agent" output when it detects an AI coding agent
    // (AI_AGENT etc. in the environment). Never wanted here; older versions
    // ignore the variable.
    SetEnvironmentVariableW(L"DUCKDB_AGENT_MODE", L"0");

    std::wstring cmd = L"\"" + exe + L"\" -batch";
    if (!db.empty()) cmd += L" \"" + db + L"\"";

    DWORD t0 = GetTickCount();
    r.exit_code = spawn_capture(cmd, in, cwd, cap, r);
    r.ms = GetTickCount() - t0;
    CloseHandle(in);
    if (r.exit_code == -1 && r.out.empty()) r.out = "duckdb.exe could not be started.";
    return r;
}

std::string duckdb_version(const std::wstring& exe) {
    DuckRun r;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    SetEnvironmentVariableW(L"DUCKDB_AGENT_MODE", L"0");
    int code = spawn_capture(L"\"" + exe + L"\" --version", nul, L"", 4096, r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (code != 0) return "";
    size_t e = r.out.find_first_of("\r\n");
    return r.out.substr(0, e);
}

// ─── helpers ──────────────────────────────────────────────────────────────────

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool write_file(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == bytes.size();
}

std::string read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    std::string buf((size_t)sz.QuadPart, '\0');
    DWORD n = 0;
    if (!buf.empty()) ReadFile(h, &buf[0], (DWORD)buf.size(), &n, nullptr);
    CloseHandle(h);
    buf.resize(n);
    return buf;
}

bool file_exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring temp_path(const std::wstring& ext) {
    wchar_t dir[MAX_PATH] = {}, f[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"qf_", 0, f);          // creates <name>.tmp
    DeleteFileW(f);
    std::wstring p = f;
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p.erase(dot);
    return p + ext;
}

std::wstring lower_ext(const std::wstring& p) {
    size_t dot = p.find_last_of(L".\\/");
    if (dot == std::wstring::npos || p[dot] != L'.') return L"";
    std::wstring e = p.substr(dot);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

std::wstring dir_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}
