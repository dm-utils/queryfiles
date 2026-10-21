#pragma once
// Pure SQL-text helpers for Query Files: no Windows dependencies, so the
// console harness can golden-test everything in here.
#include <string>
#include <vector>

namespace qf {

// ─── lexer ────────────────────────────────────────────────────────────────────

enum class Tok { Space, Comment, String, QIdent, Dollar, Word, Number, Punct };

struct Token {
    Tok    kind;
    size_t begin, end;     // byte offsets into the source
    int    line;           // 0-based line of `begin`
};

std::vector<Token> lex(const std::string& src);

// ─── statements ───────────────────────────────────────────────────────────────

struct Stmt {
    std::string text;      // starts at the first significant character, no trailing ';'
    int  first_line = 0;   // 0-based line in the source where `text` starts
    bool dot = false;      // a DuckDB CLI dot command (".tables"), passed through as is
};

// Split on ';' outside strings, comments and $$ bodies. Statements that are
// only whitespace/comments are dropped. A line starting with '.' at a
// statement boundary is a dot command of its own.
std::vector<Stmt> split_statements(const std::string& src);

// ─── THIS ─────────────────────────────────────────────────────────────────────

extern const char* const THIS_VIEW;    // "__qf_this"

struct ThisRewrite {
    bool        ok = true;         // false: THIS used as a name (refused)
    bool        uses_this = false;
    int         line = 0;          // 0-based line in the statement of the refused THIS
    std::string text;              // statement with every reference rewritten
};

// THIS is reserved by Query Files: references become THIS_VIEW, use as a
// name (alias, CTE, table/view name, INSERT/UPDATE/DELETE target) is refused.
// Quoted "this" is an ordinary identifier and is left alone.
ThisRewrite rewrite_this(const std::string& stmt);

// Path from a "-- this: <path>" line in the leading comment block, or "".
std::string this_header_path(const std::string& src);

// DuckDB table function call reading `path` (forward slashes), picked by the
// lower-case extension incl. the dot: read_csv / read_parquet / read_json / read_xlsx.
std::string reader_sql(const std::string& path, const std::string& ext);

// Replace THIS_VIEW with THIS in output coming back from DuckDB. keep_width
// pads with spaces so box-drawn tables stay aligned.
std::string unalias_this(std::string s, bool keep_width);
// Same for an error message: plain, except on DuckDB's "LINE n:" echo.
std::string unalias_error(const std::string& msg);

// ─── script ───────────────────────────────────────────────────────────────────

struct ScriptOptions {
    bool        grid = false;          // CSV + NULL sentinel for the table view
    std::string text_mode = "duckbox"; // .mode for the text view
    int         max_rows = 1000;       // .maxrows (text view, table-like modes)
    std::string prelude;               // extra SQL from Settings, run first
    std::string this_reader;           // reader_sql(...) for THIS, "" = not bound
};

extern const char NULL_SENTINEL;       // '\x01' — what NULL prints as in grid mode

// Prelude + one marker line before every statement, so output can be split
// per statement and an error attributed to the statement that raised it.
std::string build_script(const ScriptOptions& opt, const std::vector<Stmt>& stmts);

struct Section {
    int         stmt = -1;             // index into the statement list, -1 = prelude
    std::string body;
};

// Cut DuckDB's output at the marker lines. Section -1 holds anything printed
// before the first marker (prelude output / errors).
std::vector<Section> split_output(const std::string& out);

// "LINE n:" in a DuckDB error message → n (1-based), or 0.
int error_line(const std::string& msg);

// Text view: drop interactive-only hints from duckbox footers.
std::string clean_text_output(const std::string& body);

// ─── CSV (grid view) ──────────────────────────────────────────────────────────

struct Table {
    std::vector<std::string>              cols;
    std::vector<std::vector<std::string>> rows;   // NULL = a cell equal to the sentinel
};

// Parse DuckDB's CSV output (',' + '"', CRLF or LF). Stops after max_rows
// data rows and sets `capped`.
Table parse_csv(const std::string& body, size_t max_rows, bool& capped);

bool is_null_cell(const std::string& cell);

} // namespace qf
