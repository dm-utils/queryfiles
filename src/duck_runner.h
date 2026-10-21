#pragma once
// Starting duckdb.exe: Windows-only, but no Notepad++ dependency, so the
// console harness uses it too for the integration tests.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

struct DuckRun {
    int         exit_code = -1;     // -1: could not be started
    std::string out;                // stdout + stderr, in order
    bool        truncated = false;  // output cap reached, process stopped
    bool        cancelled = false;  // stopped by cancel_duckdb()
    DWORD       ms = 0;             // wall-clock run time
};

// Run `exe [db]` with the script file on stdin and `cwd` as working folder.
// Blocks until DuckDB exits, the output passes cap_bytes, or cancel_duckdb()
// is called from another thread. Only one run at a time.
DuckRun run_duckdb(const std::wstring& exe, const std::wstring& db,
                   const std::wstring& script, const std::wstring& cwd,
                   size_t cap_bytes);

// Stop the running DuckDB process, if any. Safe to call from any thread.
void cancel_duckdb();

// `duckdb --version`, first line, or "".
std::string duckdb_version(const std::wstring& exe);

// ─── small Win32 helpers shared by the plugin and the harness ─────────────────

std::wstring widen(const std::string& s);
std::string  narrow(const std::wstring& w);
bool         write_file(const std::wstring& path, const std::string& bytes);
std::string  read_file(const std::wstring& path);
bool         file_exists(const std::wstring& path);
// Fresh, unique, not-yet-existing path in %TEMP%, e.g. ...\qf_1A2B.csv
std::wstring temp_path(const std::wstring& ext);
// Lower-case extension incl. the dot (".csv"), or "".
std::wstring lower_ext(const std::wstring& path);
// Folder part of a path, without the trailing backslash, or "".
std::wstring dir_of(const std::wstring& path);
