# Datamodder Query Files

A Notepad++ plugin that runs SQL on CSV, Parquet, JSON and Excel files,
powered by [DuckDB](https://duckdb.org/). Open a data file, choose
**Query this file**, and query it as `THIS`; or name files in the query and
join them. Results appear as text in a tab or as a sortable table in a docked
panel.

**Website:** [datamodder.com/en/utils/query-files](https://datamodder.com/en/utils/query-files)

For 64-bit, 32-bit and ARM64 Notepad++. Needs `duckdb.exe` (PATH, next to the plugin, set
in Settings, or the one Datamodder File Tools already uses).

See [src/help.txt](src/help.txt) for usage.

## Build

Visual Studio C++ build tools required. From an **elevated** terminal:

```
build.cmd            compile, close Notepad++, deploy to plugins\QueryFiles, restart
build.cmd nodeploy   compile only (build\QueryFiles.dll)
```

## Release

```
release.cmd
```

Builds `QueryFiles.dll` and zips it with `help.txt` into
`release_staging\QueryFiles.zip` (x64) or `QueryFiles_<arch>.zip`, printing the
SHA-256 the plugin lists need. By default only x64; `release.cmd x64 x86 arm64`
(or the commented line in the script) also builds the Win32 and ARM64
versions. ARM64 needs the "MSVC C++ ARM64/ARM64EC build tools" component of
Visual Studio. The plugin runs `duckdb.exe` as a separate program, so any
duckdb build works with any of them.

## Test

```
test.cmd             (test.cmd x86 runs them as a 32-bit program)
```

Golden tests for the SQL-text layer (statement splitting, THIS rewrite and
refusals, `-- this:` header, script generation, CSV parsing), plus end-to-end
runs through `duckdb.exe` when `QF_DUCKDB` is set or `duckdb` is on PATH.
The DuckDB goldens follow DuckDB's own messages, so a new DuckDB version can
change them.

## Layout

```
src/sql_text.{h,cpp}     pure: lexer, splitter, THIS rewrite, script, output parsing
src/duck_runner.{h,cpp}  start duckdb.exe, capture output, cancel
src/result_grid.{h,cpp}  docked table panel (virtual ListView)
src/dllmain.cpp          Notepad++ glue: menu, toolbar, THIS binding, worker thread
src/settings*.{h,cpp,rc} Settings dialog, INI + JSON profiles, About
src/test_harness.cpp     console runner for the tests
tests/                   fixtures + golden_*.txt
```

## License

MIT
