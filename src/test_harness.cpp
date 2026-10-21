// Console harness for Query Files. Pure modes print the result of one
// sql_text function for a fixture; `duck` runs a query end-to-end through
// duckdb.exe, the way the plugin does.
//
//   test_harness split  <file.sql>
//   test_harness this   <file.sql>
//   test_harness header <file.sql>
//   test_harness script <file.sql> [grid]
//   test_harness csv    <file.csv>
//   test_harness duck   <duckdb.exe> <file.sql> <this-file|-> [grid]
#include <cstdio>
#include <string>
#include "sql_text.h"
#include "duck_runner.h"

using namespace qf;

static void put(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

// Make control characters visible in golden files.
static std::string vis(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\x01') o += "<NUL>";
        else if (c == '\r') o += "<CR>";
        else o += c;
    }
    return o;
}

static void print_table(const Table& t, bool capped) {
    put("columns:");
    for (auto& c : t.cols) put(" [" + vis(c) + "]");
    put("\n");
    for (size_t r = 0; r < t.rows.size(); ++r) {
        put("  row " + std::to_string(r) + ":");
        for (auto& c : t.rows[r]) put(is_null_cell(c) ? " NULL" : " [" + vis(c) + "]");
        put("\n");
    }
    if (capped) put("  (capped)\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: see test_harness.cpp\n"); return 2; }
    std::string mode = argv[1];

    if (mode == "duck") {
        if (argc < 5) return 2;
        std::wstring exe = widen(argv[2]);
        std::string  sql = read_file(widen(argv[3]));
        std::string  thisf = argv[4];
        bool grid = argc > 5 && std::string(argv[5]) == "grid";

        std::vector<Stmt> stmts = split_statements(sql);
        for (auto& s : stmts) {
            if (s.dot) continue;
            ThisRewrite r = rewrite_this(s.text);
            if (!r.ok) { put("REFUSED line " + std::to_string(s.first_line + r.line + 1) + "\n"); return 0; }
            s.text = r.text;
        }
        ScriptOptions o;
        o.grid = grid;
        o.max_rows = 5;
        if (thisf != "-") o.this_reader = reader_sql(thisf, narrow(lower_ext(widen(thisf))));
        std::wstring script = temp_path(L".sql");
        write_file(script, build_script(o, stmts));
        DuckRun run = run_duckdb(exe, L"", script, dir_of(widen(argv[3])), 50u << 20);
        DeleteFileW(script.c_str());

        put("exit " + std::to_string(run.exit_code) + "\n");
        std::vector<Section> secs = split_output(run.out);
        for (size_t i = 0; i < secs.size(); ++i) {
            const Section& sec = secs[i];
            bool is_error = run.exit_code != 0 && i + 1 == secs.size();
            put("--- section " + std::to_string(sec.stmt) + (is_error ? " (error)" : "") + "\n");
            if (grid && sec.stmt >= 0 && !is_error) {
                bool capped = false;
                Table t = parse_csv(sec.body, 3, capped);
                if (t.cols.empty()) put("(no result)\n");
                else print_table(t, capped);
            } else {
                std::string b = is_error ? unalias_error(sec.body)
                                         : unalias_this(clean_text_output(sec.body), true);
                put(vis(b));
                int l = error_line(b);
                if (l > 0 && sec.stmt >= 0)
                    put("=> error at source line " + std::to_string(stmts[sec.stmt].first_line + l) + "\n");
            }
        }
        return 0;
    }

    std::string src = read_file(widen(argv[2]));

    if (mode == "split") {
        auto st = split_statements(src);
        for (size_t i = 0; i < st.size(); ++i)
            put("[" + std::to_string(i) + "] line " + std::to_string(st[i].first_line + 1) +
                (st[i].dot ? " dot" : "") + "\n" + st[i].text + "\n");
    } else if (mode == "this") {
        for (const Stmt& s : split_statements(src)) {
            ThisRewrite r = rewrite_this(s.text);
            int line = s.first_line + 1;
            if (!r.ok)  put("REFUSED  (line " + std::to_string(line + r.line) + ")  " + s.text + "\n");
            else        put(std::string(r.uses_this ? "REWRITE  " : "PLAIN    ") + r.text + "\n");
        }
    } else if (mode == "header") {
        put("[" + this_header_path(src) + "]\n");
    } else if (mode == "script") {
        ScriptOptions o;
        o.grid = argc > 3 && std::string(argv[3]) == "grid";
        o.prelude = "SET threads = 2";
        o.this_reader = reader_sql("C:\\data\\it's.tsv", ".tsv");
        std::vector<Stmt> st = split_statements(src);
        for (auto& s : st) if (!s.dot) { ThisRewrite r = rewrite_this(s.text); if (r.ok) s.text = r.text; }
        put(vis(build_script(o, st)));
    } else if (mode == "csv") {
        bool capped = false;
        print_table(parse_csv(src, 100, capped), capped);
    } else {
        return 2;
    }
    return 0;
}
