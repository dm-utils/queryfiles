# Datamodder Query Files — design

Run SQL against CSV, Parquet, JSON and Excel files from inside Notepad++,
powered by DuckDB. Third Datamodder plugin, next to FormatSQL (write the SQL)
and File Tools (tidy / convert the files): Query Files *runs* the SQL.

Same build style as the other two: Win32 C++, 64-bit, self-declared
Notepad++ ABI (no SDK), `g_funcs[]` menu regrouped at `NPPN_READY`, tabbed
Settings with INI + JSON profiles, console regression harness, `build.cmd`
deploy, version in `settings.rc`.

Proposed: repo `dm-utils/queryfiles`, plugin folder `QueryFiles`, DLL
`QueryFiles.dll`, display name "Datamodder Query Files", MIT.

**Version** in `src/resource.h` (3 places). Started at `1.3.1.0`, in line
with the other Datamodder plugins; **every push raises the last number by
one**. Once the plugin is in the Notepad++ plugin list, the number there must
equal the DLL's file version and may only go up.

## Why this plugin

The existing option is **CSV Query** (Plugin Admin; jokedst, later chcg).
It loads the active CSV into an in-memory SQLite database and lets you query
it as table `THIS`. Its limits are the opening for this plugin:

| | CSV Query | Datamodder Query Files |
|---|---|---|
| Engine | SQLite 3.7.7.1 (2011): no window functions, no CTE niceties | Current DuckDB: window functions, `QUALIFY`, `PIVOT`, `GROUP BY ALL`, structs/lists |
| Formats | CSV only | CSV, TSV, Parquet, JSON / NDJSON, Excel (`.xlsx`) |
| Sources | The active document only | The active document **and** any file on disk, globs (`'data/*.parquet'`), joins across files |
| Large files | Everything is first loaded into SQLite | DuckDB scans the file directly, parallel, out-of-core |
| Result | Grid in its own window | Choice in Settings: text in a Notepad++ tab (searchable, savable) or a grid panel docked in Notepad++ |

Keep what CSV Query users know: **`THIS` is the active data document.**

Audience: data engineers and analysts who already live in Notepad++ and want
a quick look into a file without opening a database client, Python or Excel.

Plugin Admin description (draft): *"Run SQL on CSV, Parquet, JSON and Excel
files with DuckDB. Query the open file as THIS, join files on disk, results as
text or in a table."*

## Engine: shell out to `duckdb.exe`

Same reasoning as File Tools' Parquet support: linking DuckDB (`duckdb.dll`,
~40 MB, C API) is possible but bloats the plugin and pins a DuckDB version
into every release. The CLI is one self-contained exe (MIT) the user can
update independently.

**Finding `duckdb.exe` — the File Tools approach, plus reuse of File Tools'
DuckDB.** Based on `duckdb_or_warn` in File Tools' `dllmain.cpp`. Not bundled,
no download helper. A user who already set DuckDB up for File Tools never has
to do it again:

1. The path in Query Files' own Settings (`duckdb_path`, Browse… button).
2. File Tools' setting: `duckdb_path` in
   `%APPDATA%\Notepad++\plugins\config\FileTools.ini`.
3. `PATH` (`SearchPathW`).
4. Next to `QueryFiles.dll`.
5. Next to File Tools: `plugins\FileTools\duckdb.exe` (sibling folder of
   Query Files' own plugin folder).
6. Not found → message box with the duckdb.org download link (same wording
   as File Tools).

The Settings page shows which one was found (*"Using duckdb.exe from File
Tools' settings: C:\…"*), so it is clear where it comes from. Query Files
never writes to `FileTools.ini`.

**Invocation.** Unlike File Tools (which passes SQL via `-c "..."`), Query
Files runs arbitrary user SQL, full of quotes, newlines and semicolons.
So: never put the query on the command line. Write a **script file** to
`%TEMP%` and feed it to DuckDB on **stdin**:

```
CreateProcessW("duckdb.exe" [db], stdin = script file, stdout+stderr = pipe,
               cwd = folder of the SQL document, CREATE_NO_WINDOW)
```

The script is a generated prelude followed by the user's SQL:

```sql
.bail on
.mode box               -- from Settings: box | table | markdown | csv | json | line
.maxrows 1000           -- from Settings (only affects table-like modes)
.timer on               -- optional, from Settings
CREATE OR REPLACE TEMP VIEW __qf_this AS SELECT * FROM read_csv('C:/…/qf_1234.csv');
-- user SQL starts here, `this` already rewritten by the plugin
select klant, sum(bedrag) from __qf_this group by all;
```

(What the user typed: `select klant, sum(bedrag) from this group by all;`
— see *`THIS` is a reserved word* below.)

`cwd` = the SQL document's folder, so relative paths in the query
(`from 'orders.parquet'`) resolve next to the `.sql` file — what a user
expects. Unsaved SQL document → cwd = the folder of the data document, else
`%USERPROFILE%`.

## `THIS` — the active data document

The hard part, because when you run a query the *active* tab is the SQL, not
the data. Two entry points:

### 1. `Query this file` (the CSV Query workflow)

With a data file active (`.csv .tsv .txt .parquet .json .ndjson .jsonl
.xlsx`), `Query this file`:

- remembers that document's **buffer id** as the *data source*,
- opens a new tab, language SQL, with a starter query:

  ```sql
  -- Query Files: THIS = orders.csv
  select *
  from this
  limit 100;
  ```

- and binds that new tab's buffer id to the data source (map
  `sql buffer id → data buffer id`, in memory for the session).

Running a query in that tab defines `THIS` as the bound data document.

### 2. Running any SQL document

If the SQL tab has no binding, `THIS` is **not** defined, and the user
refers to files by path (`from 'C:/data/orders.parquet'`). Only if the query
text mentions `this` as a word (simple token scan, outside strings and
comments) and there is no binding: message box *"THIS is not bound to a
file. Use Query this file from a data tab, or name the file in the query."*

A `-- this: <path>` header comment in a saved `.sql` file binds `THIS` to that
file persistently. Cheap to implement and makes saved query files portable.

### `THIS` is a reserved word

`THIS` belongs to Query Files. It always means the bound data document and
can't be used as a name for anything else. It is resolved **by the plugin**,
not by DuckDB, so it doesn't depend on DuckDB's keyword list or version.

Before every run the plugin tokenizes the SQL (pure function in
`sql_text`, golden-tested; strings, `--` / `/* */` comments and `$$` bodies
are skipped) and handles each bare `this` token, case-insensitive:

- **As a reference** (`from this`, `join this t`, `this.klant`,
  `describe this`): rewritten to `__qf_this`, the internal view the prelude
  creates. `this.klant` → `__qf_this.klant`.
- **As a definition** — the token after `AS` (column or table alias), after
  `CREATE [OR REPLACE] [TEMP] TABLE|VIEW|MACRO|SEQUENCE`, after
  `INSERT INTO`, `UPDATE`, `DELETE FROM`, `DROP … `, `ALTER TABLE`, or as
  a CTE name (`with this as (…)`): the run is refused before DuckDB starts,
  with the line and the message *"THIS is reserved by Query Files for the
  bound data file and can't be used as a name."* THIS is read-only.
- **Quoted** `"this"` is left alone: that's an ordinary DuckDB identifier
  (for a column that happens to be called *this*). Mentioned in help.txt.

Errors from DuckDB that mention `__qf_this` are shown with `THIS` substituted
back, so the internal name never surfaces.

### How `THIS` is materialised

| Data document state | What `THIS` reads |
|---|---|
| Saved, not modified, binary (`.parquet`, `.xlsx`) | The file on disk, directly |
| Saved, not modified, text | The file on disk, directly (fast path for big files) |
| Modified or never saved, text | Buffer spilled to `%TEMP%\qf_<n>.<ext>` (UTF-8 bytes from `SCI_GETTEXT`) — unsaved edits included, like File Tools |
| Modified binary | Impossible in Notepad++; read from disk |

Reader per extension: `read_csv` (auto-detect; `.tsv` → `delim='\t'`),
`read_parquet`, `read_json` (`.ndjson`/`.jsonl` → `format='newline_delimited'`),
`read_xlsx` (needs DuckDB ≥ 1.2 with the `excel` extension — fallback
message if it fails). Unknown extension → `read_csv`, which sniffs.

Temp files are deleted after the run.

Phase 2: other open tabs addressable by name, e.g. `from tab('orders.csv')`
— implemented as one extra view per referenced tab, found by scanning the SQL
for `tab('…')` before the run.

## What gets run

- **Selection** if there is one, else **the whole document**.
- Phase 2: *Run statement at cursor* — split on `;` outside strings and
  comments (a pure function, golden-tested) and run only the statement
  containing the caret. FormatSQL already has a tokenizer that knows
  strings, comments and dollar quoting; copy the relevant part rather than
  depend on the FormatSQL DLL.

## Results

**Two ways to show a result, chosen in Settings → Results → *Show results
as*: `Text tab` (default) or `Table`.** Both get the focus after every run,
errors included. A menu item *Switch result view* flips the setting without
opening the dialog.

### Text tab

- Written to a **result tab** named like `Query result` (new buffer;
  Notepad++ can't rename unsaved buffers through the ABI, so the first line is
  a header instead):

  ```
  -- Query Files · THIS = orders.csv · 1.234 rows · 0.18 s · 2026-10-21 14:03
  ┌─────────┬──────────────┐
  │  klant  │ sum(bedrag)  │
  …
  ```

- **Reuse the result tab** (Settings, default on): remember its buffer id;
  if it is still open, replace its text, otherwise open a new one. Off = a
  new tab per run (handy for comparing). The tab keeps Notepad++'s default
  `new N` name; no effort spent on naming it.
- **Focus moves to the result tab after every run**, errors included:
  `NPPM_GETPOSFROMBUFFERID` → `NPPM_ACTIVATEDOC(view, index)`, then
  `SetFocus` on that view's Scintilla handle so the keyboard is in the
  result (Ctrl+F, Ctrl+A work straight away), caret at the top
  (`SCI_DOCUMENTSTART`). Because the run is asynchronous this also happens
  when the user switched tabs in the meantime — the result is what they
  asked for.
- Output mode per run from Settings: `box` (default, Unicode box drawing, UTF-8),
  `table` (ASCII), `markdown`, `csv`, `json`, `line`. The `csv`/`json` modes
  hand off neatly to File Tools for further conversion.
- **Row cap**: `.maxrows` for the table-like modes. For `csv`/`json` there is
  no CLI cap, so the plugin caps the captured output at N MB (Settings,
  default 50) and appends `-- output truncated at 50 MB`.
- Multiple statements → DuckDB prints each result in sequence; that's fine.

### Table (grid panel)

A **dockable panel** inside Notepad++ (`NPPM_DMMREGASDCKDLG`, docked at the
bottom by default, like the Find results), with a Win32 **ListView** in
virtual mode (`LVS_OWNERDATA`), so 100.000 rows don't create 100.000 items.

- **Getting the data.** The prelude uses `.mode csv`, `.headers on` and
  `.nullvalue` set to a sentinel (an unprintable control character), so the
  grid can tell `NULL` from an empty string. Parsed with the CSV reader from
  File Tools (`csv_tools.cpp`, copied). CSV rather than JSON because JSON
  objects collapse duplicate column names (`select a.id, b.id`).
- **Multiple statements.** The splitter from `sql_text` separates the
  statements and the plugin puts `.print` with a marker line between them;
  each result set becomes its own tab strip page in the panel (*Result 1*,
  *Result 2*, …), the last one selected.
- **Row cap**: the plugin stops reading after *Max rows* (same setting as the
  text tab) and shows *"first 1.000 rows of more"*.
- **Display.** Header row = column names; `NULL` shown greyed; columns where
  every value parses as a number are right-aligned; column widths fit the
  first 200 rows, capped at ~400 px. Status line above the grid: *THIS =
  orders.csv · 1.234 rows · 0.18 s*.
- **Interaction.** Click a header to sort (client-side, numeric where the
  column is numeric); Ctrl+C copies the selected rows as tab-separated text
  with header (pastes straight into Excel); Ctrl+A selects all; right-click
  menu *Copy · Copy with header · Copy cell · Open as text tab*.
- **Focus.** After a run the panel is shown (`NPPM_DMMSHOW`), the last
  result page selected, and `SetFocus` goes to the ListView with the first
  row selected, so arrow keys and Ctrl+C work immediately.
- **Errors** are shown in the panel in place of the grid, in a read-only,
  selectable multi-line edit control (copyable), with the *Go to error*
  button when there's a line number.
- **Dark mode**: `NPPM_DARKMODESUBCLASSANDTHEME` on the panel, so the
  ListView and tab strip follow Notepad++'s theme.
- Closing the panel just hides it; the next run shows it again.

### Errors

With `.bail on` DuckDB stops at the first error and prints
`Error: … LINE n: …`. Show the error **where the result would go** — the
text tab or the grid panel — never in a message box: errors in data work
are long and you want to copy them. When a `LINE n` is present, offer *Go to
error*, which moves the caret in the SQL tab to line
`n − prelude_lines + selection_start_line`.

## Long queries don't freeze Notepad++

File Tools runs DuckDB synchronously; fine for a preview, not for a query
over a few GB. Query Files runs DuckDB on a **worker thread**:

- Plugin creates a hidden message-only window (`HWND_MESSAGE`) at load.
- Worker: `CreateProcessW`, read the pipe until EOF, `PostMessage` the result
  (heap-allocated string) back to that window. The UI thread writes it into
  the result tab or the grid panel.
- While running: Notepad++ status bar shows `Query Files: running… (Esc to
  cancel)` via `NPPM_SETSTATUSBAR`, and the menu item **Cancel query** is
  enabled. Cancel = `TerminateProcess` on the DuckDB process.
- One query at a time; starting a new one while one runs asks to cancel it.
- Timeout (Settings, default off).

## Database: in-memory or a file

Default: every run is a fresh `:memory:` DuckDB. Consequence, to put in
help.txt: a `create table` in one run is gone in the next.

Settings option **Database file**: a `.duckdb` file that is opened for every
run (`duckdb.exe "<file>"`). Tables, macros and views then persist between
runs and the plugin turns into a lightweight local analytics workbench.
`THIS` remains a `TEMP VIEW`, so it never lands in that file.

Phase 2: a per-document `-- database: <path>` header, like `-- this:`.

## Menu

No default keyboard shortcut (users can still assign one in Notepad++'s
Shortcut Mapper). Instead a **toolbar button**, see below.

```
Datamodder Query Files
├─ Run query                     (also the toolbar button)
├─ Query this file
├─ Cancel query
├─ ─────────
├─ Run statement at cursor       (phase 2)
├─ Describe THIS                 → DESCRIBE this  (columns + types)
├─ Summarize THIS                → SUMMARIZE this (min/max/nulls/distinct per column)
├─ Export result ▸ CSV / Parquet / JSON   (phase 2: re-run as COPY (…) TO)
├─ ─────────
├─ Switch result view            (text tab ⇄ table; checkmark shows the current one)
├─ Show result panel             (re-open the grid panel after closing it)
├─ ─────────
├─ Insert ▸ read_csv(…) / read_parquet(…) / read_json(…) for a file…  (file picker → snippet at caret)
├─ ─────────
├─ Settings…  ·  Help  ·  About
```

`Describe THIS` and `Summarize THIS` work from a data tab too (implicit
`Query this file` + run), which makes them the quickest way to see what's in
an unknown file — a strong first-run experience.

### Toolbar button

One button on Notepad++'s toolbar: **Run query** (a ▶ play icon). New for
the Datamodder plugins — neither FormatSQL nor File Tools has one yet.

- On `NPPN_TBMODIFICATION` (`NPPN_FIRST + 2`) send
  `NPPM_ADDTOOLBARICON_FORDARKMODE` (`NPPMSG + 101`) with
  `g_funcs[RUN]._cmdID` and a `toolbarIconsWithDarkMode`
  `{ hToolbarBmp, hToolbarIcon, hToolbarIconDarkMode }`.
- Three resources in `settings.rc`: a 16×16 `.bmp` (standard icon set), a
  `.ico` with 16/24/32 px for Fluent light, and a light-coloured `.ico` for
  Fluent dark. Loaded with `LoadImage` from the DLL's `HINSTANCE`.
- Tooltip = the menu item's name, which Notepad++ takes from `g_funcs`.
- While a query runs, the button stays enabled; clicking it again asks
  whether to cancel the running query (same as the menu item).

`Export result` re-runs the last query wrapped as
`COPY (<query>) TO '<picked file>' (FORMAT …)`; only valid for a single
`SELECT`. Writes the full result, not the capped preview.

## Settings (tabbed, same dialog pattern as File Tools)

INI at `%APPDATA%\Notepad++\plugins\config\QueryFiles.ini`, profiles under
`...\config\QueryFiles_profiles\`.

- **DuckDB** — path to `duckdb.exe` (Browse…, same control as File Tools'
  Parquet tab; empty = auto, incl. File Tools' DuckDB) with a line showing
  which `duckdb.exe` is in use and where it came from, database file
  (empty = in-memory), extra prelude SQL (e.g. `SET threads = 4;` or
  `INSTALL excel; LOAD excel;`).
- **Results** — **show results as: Text tab / Table**, max rows (both views),
  timer on/off. *Text tab* group: output mode (box / table / markdown / csv /
  json / line), max output MB, reuse result tab, header line. *Table* group:
  NULL display text (default `NULL`), right-align numeric columns.
- **THIS** — default CSV options when sniffing fails: delimiter, header yes/no,
  decimal separator (`,` matters for Dutch CSVs), date format.
- **Behavior** — timeout, profiles.

## Code layout (target)

```
src/dllmain.cpp         NPP glue, menu, toolbar button, buffer-id bindings, result tab
src/duck_runner.{h,cpp} find duckdb.exe (incl. File Tools'), build script,
                        worker thread, cancel
src/result_grid.{h,cpp} dockable panel: tab strip, virtual ListView, sort,
                        copy, error view, dark mode
src/csv_read.{h,cpp}    CSV parser for the grid (from File Tools' csv_tools)
src/sql_text.{h,cpp}    pure: tokenizer, statement splitter, THIS rewrite +
                        reserved-word check, header comments (-- this: /
                        -- database:), prelude builder, error-line remap,
                        __qf_this → THIS in messages — no Windows deps,
                        harness-tested
src/settings*.{h,cpp,rc} copied from File Tools, trimmed
src/test_harness.cpp    golden tests for sql_text
tests/                  goldens + small fixtures (orders.csv, orders.parquet, nl.csv)
```

Copy, don't share, code with File Tools/FormatSQL (`run_capture`,
`duckdb_or_warn`, settings dialog, `build_all_submenus`): three plugins that
each build standalone is simpler than a common library for now.

## Tests

- **Golden, no DuckDB needed** (`test.cmd`): statement splitting (strings,
  `--`/`/* */` comments, `$$` quotes, `;` in strings), THIS rewrite (`from
  this`, `this.col`, `THIS` in any case, untouched inside strings/comments and
  as `"this"`), reserved-word refusals (`as this`, `create table this`,
  `with this as`, `insert into this`), header parsing, prelude generation
  per extension / modified state, error-line remapping, CSV parsing with the
  NULL sentinel.
- **Integration, needs `duckdb.exe`** (`test_duck.cmd`, skipped if absent):
  run fixtures through `duck_runner` and diff the CSV-mode output. Covers
  `THIS` over a spilled buffer vs. file on disk, Dutch CSV with `;` and decimal
  comma, a syntax error (line remap), cancel of a slow query
  (`select count(*) from range(10_000_000_000)`).

## MVP (v1.0)

Run query (selection / whole document) · Query this file + `THIS` binding ·
`THIS` as reserved word (rewrite + refusals) · `-- this:` header · text tab
(box / csv / markdown) **and** table panel, switchable in Settings · reuse
result tab · errors where the result goes · focus to the result after each
run · toolbar button · worker thread + Cancel · Describe / Summarize THIS ·
duckdb.exe lookup incl. File Tools' DuckDB · Settings (DuckDB / Results /
Behavior) · Help / About.

The statement splitter is needed in v1.0 anyway (multiple results in the
grid), which makes *Run statement at cursor* cheap to add early.

Phase 2: statement at cursor, Go to error, Export result, `tab('…')`, Insert
snippets, database file, `-- database:` header, THIS CSV options tab.

## Release / distribution

Same as File Tools / FormatSQL: tag `vX.Y.Z.W`, GitHub Release with
`QueryFiles.zip` (DLL + `help.txt` at zip root; **not** `duckdb.exe` — too
big and has its own release cycle), version in `settings.rc` matches the tag,
then a PR to `notepad-plus-plus/nppPluginList` (`src/pl.x64.json`).

## Open questions

Decided (2026-10-21/22):

- No default shortcut but a toolbar button.
- DuckDB handled as in File Tools (no bundling, no download helper), and a
  DuckDB already set up for File Tools is reused.
- The result tab keeps its `new N` name, but the result always gets the focus.
- `THIS` is a Query Files reserved word: the plugin rewrites it to an internal
  view and refuses it as a name, so DuckDB's own keyword rules don't matter.
- Both a text tab and a table view, chosen in Settings.

Still open:

1. **Verify `.print` and `.nullvalue`** behave as assumed in the DuckDB CLI
   version current at build time (marker lines between results, NULL
   sentinel in CSV mode). Test first; the grid design leans on them.

## Implementation notes (first build, 2026-10-22)

What the first build does differently from, or more precisely than, the
design above — found while testing against DuckDB 2.0.0:

- **Script on stdin, `-batch`.** No `-f`/`-no-agent` flags (missing in older
  CLIs). DuckDB 2.x switches to an "agent mode" output when it detects an AI
  coding agent in the environment; the runner sets `DUCKDB_AGENT_MODE=0`
  before starting the process, which older versions ignore.
- **Default text format is `duckbox`**, not `box`: it shows column types and
  the row count, and is the only box mode that honours `.maxrows`. No
  `.timer`; the plugin measures the run time itself. The interactive hint
  "use .last to show entire result" is stripped.
- **A `.print` marker precedes every statement** (`\x01QF:<n>\x01`), in both
  views. DuckDB's `LINE n:` in errors is relative to the *statement*, so the
  marker is what tells which statement failed; document line =
  selection start + statement start + n − 1.
- **NULL sentinel** in the table view: `.nullvalue \x01`; DuckDB quotes it in
  CSV output, the parser treats a cell equal to `\x01` as NULL.
- **THIS in output**: `__qf_this` is shown as `THIS` padded to the same width,
  so box tables (e.g. `DESCRIBE`) and the `^^^` under `LINE n:` stay aligned.
- **Menu is flat** (13 items), no submenus: Run query · Query this file ·
  Cancel query · Describe THIS · Summarize THIS · Results as table (checkmark)
  · Show result panel · Settings · Help · About.
- **Table view** row cap is applied while parsing (DuckDB's CSV mode has no
  `.maxrows`); the output-MB cap still protects against huge results.
- **THIS from a bound tab that isn't on screen**: the plugin activates it with
  redraw off to read its text and modified flag, then switches back.
- **UI text is English** throughout (menus, dialogs, messages, help).
- **Toolbar icons** (`src/tb_run.*`) are generated; the light/dark ICOs hold
  16–64 px.
