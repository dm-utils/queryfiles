#include "sql_text.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace qf {

const char* const THIS_VIEW     = "__qf_this";
const char        NULL_SENTINEL = '\x01';

// ─── small string helpers ─────────────────────────────────────────────────────

static std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
static bool ieq(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
static void rtrim(std::string& s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
}
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ─── lexer ────────────────────────────────────────────────────────────────────

static bool word_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
static bool word_char (unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> toks;
    const size_t n = s.size();
    size_t i = 0;
    int line = 0;
    auto count_nl = [&](size_t b, size_t e) { for (size_t k = b; k < e; ++k) if (s[k] == '\n') ++line; };

    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        const size_t b = i;
        const int    l = line;
        Tok kind;

        if (std::isspace(c)) {
            while (i < n && std::isspace((unsigned char)s[i])) ++i;
            kind = Tok::Space;
        } else if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            while (i < n && s[i] != '\n') ++i;
            kind = Tok::Comment;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            i = (e == std::string::npos) ? n : e + 2;
            kind = Tok::Comment;
        } else if (c == '\'' || c == '"') {
            ++i;
            while (i < n) {
                if (s[i] == (char)c) {
                    if (i + 1 < n && s[i + 1] == (char)c) { i += 2; continue; }   // '' or ""
                    ++i;
                    break;
                }
                ++i;
            }
            kind = (c == '\'') ? Tok::String : Tok::QIdent;
        } else if (c == '$' && !(i + 1 < n && std::isdigit((unsigned char)s[i + 1])) &&
                   [&] { size_t j = i + 1;
                         while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
                         return j < n && s[j] == '$'; }()) {
            size_t j = i + 1;
            while (s[j] != '$') ++j;
            std::string tag = s.substr(i, j - i + 1);              // "$$" or "$tag$"
            size_t e = s.find(tag, j + 1);
            i = (e == std::string::npos) ? n : e + tag.size();
            kind = Tok::Dollar;
        } else if (word_start(c)) {
            while (i < n && word_char((unsigned char)s[i])) ++i;
            kind = Tok::Word;
        } else if (std::isdigit(c)) {
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) ++i;
            kind = Tok::Number;
        } else {
            ++i;
            kind = Tok::Punct;
        }
        count_nl(b, i);
        toks.push_back({ kind, b, i, l });
    }
    return toks;
}

// ─── statements ───────────────────────────────────────────────────────────────

static bool at_line_start(const std::string& s, size_t pos) {
    while (pos > 0) {
        char c = s[pos - 1];
        if (c == '\n') return true;
        if (c != ' ' && c != '\t') return false;
        --pos;
    }
    return true;
}

std::vector<Stmt> split_statements(const std::string& s) {
    std::vector<Token> toks = lex(s);
    std::vector<Stmt>  out;
    bool   open  = false;     // inside a statement (seen a significant token)
    size_t begin = 0;
    int    line  = 0;

    auto flush = [&](size_t end) {
        if (!open) return;
        std::string t = s.substr(begin, end - begin);
        rtrim(t);
        if (!t.empty()) out.push_back({ t, line, false });
        open = false;
    };

    for (size_t k = 0; k < toks.size(); ++k) {
        const Token& t = toks[k];
        if (t.kind == Tok::Punct && s[t.begin] == ';') { flush(t.begin); continue; }
        if (t.kind == Tok::Space || t.kind == Tok::Comment) continue;
        if (open) continue;

        if (t.kind == Tok::Punct && s[t.begin] == '.' && at_line_start(s, t.begin)) {
            size_t e = s.find('\n', t.begin);
            if (e == std::string::npos) e = s.size();
            std::string d = s.substr(t.begin, e - t.begin);
            rtrim(d);
            out.push_back({ d, t.line, true });
            while (k + 1 < toks.size() && toks[k + 1].begin < e) ++k;
            continue;
        }
        open  = true;
        begin = t.begin;
        line  = t.line;
    }
    flush(s.size());
    return out;
}

// ─── THIS ─────────────────────────────────────────────────────────────────────

// THIS right after one of these words is being *defined* (named), not read.
static bool def_keyword(const std::string& w) {
    static const char* const K[] = {
        "AS", "TABLE", "VIEW", "MACRO", "FUNCTION", "SEQUENCE", "TYPE", "SCHEMA",
        "INDEX", "INTO", "UPDATE", "EXISTS", "TRUNCATE", "DATABASE",
    };
    for (const char* k : K) if (w == k) return true;
    return false;
}

// THIS right after one of these words is a reference to the data. Any other
// word in front of it makes THIS an implicit alias (`from t this`).
static bool ref_keyword(const std::string& w) {
    static const char* const K[] = {
        "SELECT", "FROM", "JOIN", "ON", "USING", "WHERE", "AND", "OR", "NOT", "BY",
        "HAVING", "QUALIFY", "WHEN", "THEN", "ELSE", "CASE", "IN", "IS", "DISTINCT",
        "ALL", "ANY", "SOME", "LATERAL", "DESCRIBE", "SUMMARIZE", "SHOW", "COPY",
        "PIVOT", "UNPIVOT", "UNION", "EXCEPT", "INTERSECT", "RETURNING", "LIKE",
        "ILIKE", "BETWEEN", "WITH", "RECURSIVE", "LIMIT", "OFFSET", "EXPLAIN",
        "ANALYZE", "OVER", "PARTITION", "FILTER", "ORDER", "GROUP", "WINDOW",
    };
    for (const char* k : K) if (w == k) return true;
    return false;
}

ThisRewrite rewrite_this(const std::string& st) {
    ThisRewrite r;
    std::vector<Token> toks = lex(st);
    std::vector<size_t> sig;                       // significant token indices
    for (size_t k = 0; k < toks.size(); ++k)
        if (toks[k].kind != Tok::Space && toks[k].kind != Tok::Comment) sig.push_back(k);

    auto at    = [&](long i) -> const Token* { return (i >= 0 && i < (long)sig.size()) ? &toks[sig[i]] : nullptr; };
    auto word  = [&](const Token* t) { return (t && t->kind == Tok::Word) ? upper(st.substr(t->begin, t->end - t->begin)) : std::string(); };
    auto punct = [&](const Token* t, char c) { return t && t->kind == Tok::Punct && st[t->begin] == c; };

    std::string out;
    size_t last = 0;
    for (long i = 0; i < (long)sig.size(); ++i) {
        const Token& t = *at(i);
        if (t.kind != Tok::Word || !ieq(st.substr(t.begin, t.end - t.begin), "this")) continue;
        r.uses_this = true;

        const Token* prev  = at(i - 1);
        const Token* prev2 = at(i - 2);
        const Token* next  = at(i + 1);
        const Token* next2 = at(i + 2);
        bool refused = false;

        if (punct(next, '.')) {
            // this.column — always a reference
        } else if (word(next) == "AS" && punct(next2, '(')) {
            refused = true;                                   // WITH this AS (...)
        } else if (prev) {
            std::string pw = word(prev);
            if (!pw.empty()) {
                if (def_keyword(pw))                               refused = true;
                else if (pw == "FROM" && word(prev2) == "DELETE")  refused = true;
                else if (!ref_keyword(pw))                         refused = true;   // implicit alias
            } else if (prev->kind == Tok::QIdent || prev->kind == Tok::String ||
                       prev->kind == Tok::Number || prev->kind == Tok::Dollar || punct(prev, ')')) {
                refused = true;                                                       // implicit alias
            }
        }
        if (refused) {
            r.ok   = false;
            r.line = t.line;
            r.text.clear();
            return r;
        }
        out.append(st, last, t.begin - last);
        out += THIS_VIEW;
        last = t.end;
    }
    out.append(st, last, std::string::npos);
    r.text = out;
    return r;
}

std::string this_header_path(const std::string& src) {
    size_t i = 0;
    while (i < src.size()) {
        size_t e = src.find('\n', i);
        if (e == std::string::npos) e = src.size();
        std::string line = trim(src.substr(i, e - i));
        i = e + 1;
        if (line.empty()) continue;
        if (line.compare(0, 2, "--") != 0) break;          // end of the leading comment block
        std::string body = trim(line.substr(2));
        if (body.size() >= 4 && ieq(body.substr(0, 4), "this")) {
            std::string rest = trim(body.substr(4));
            if (!rest.empty() && rest[0] == ':') return trim(rest.substr(1));
        }
    }
    return "";
}

std::string reader_sql(const std::string& path, const std::string& ext) {
    std::string lit = "'";
    for (char c : path) {
        if (c == '\\')      lit += '/';
        else if (c == '\'') lit += "''";
        else                lit += c;
    }
    lit += "'";
    if (ext == ".parquet")                                   return "read_parquet(" + lit + ")";
    if (ext == ".json" || ext == ".ndjson" || ext == ".jsonl") return "read_json(" + lit + ")";
    if (ext == ".xlsx")                                      return "read_xlsx(" + lit + ")";
    if (ext == ".tsv" || ext == ".tab")                      return "read_csv(" + lit + ", delim = '\\t')";
    return "read_csv(" + lit + ")";
}

std::string unalias_this(std::string s, bool keep_width) {
    const std::string from = THIS_VIEW;
    const std::string to   = keep_width ? std::string("THIS") + std::string(from.size() - 4, ' ') : "THIS";
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

std::string unalias_error(const std::string& msg) {
    // Keep the width on DuckDB's "LINE n: <sql>" echo, so the ^^^ marker
    // underneath still points at the right spot.
    std::string out;
    size_t i = 0;
    while (i < msg.size()) {
        size_t e = msg.find('\n', i);
        size_t next = (e == std::string::npos) ? msg.size() : e + 1;
        std::string line = msg.substr(i, next - i);
        out += unalias_this(line, line.compare(0, 5, "LINE ") == 0);
        i = next;
    }
    return out;
}

// ─── script ───────────────────────────────────────────────────────────────────

std::string build_script(const ScriptOptions& o, const std::vector<Stmt>& stmts) {
    std::string s = ".bail on\n";
    if (o.grid) {
        s += ".mode csv\n.headers on\n.nullvalue ";
        s += NULL_SENTINEL;
        s += "\n";
    } else {
        s += ".mode " + o.text_mode + "\n";
        s += ".maxrows " + std::to_string(o.max_rows > 0 ? o.max_rows : 1000) + "\n";
    }
    std::string pre = trim(o.prelude);
    if (!pre.empty()) s += pre + "\n;\n";
    if (!o.this_reader.empty())
        s += std::string("CREATE OR REPLACE TEMP VIEW ") + THIS_VIEW + " AS SELECT * FROM " + o.this_reader + ";\n";
    for (size_t i = 0; i < stmts.size(); ++i) {
        s += ".print \x01QF:" + std::to_string(i) + "\x01\n";
        s += stmts[i].text;
        s += stmts[i].dot ? "\n" : "\n;\n";
    }
    return s;
}

std::vector<Section> split_output(const std::string& raw) {
    std::vector<Section> res(1);                   // [0] = prelude, stmt -1
    size_t i = 0;
    while (i < raw.size()) {
        size_t e    = raw.find('\n', i);
        size_t next = (e == std::string::npos) ? raw.size() : e + 1;
        size_t le   = (e == std::string::npos) ? raw.size() : e;
        if (le > i && raw[le - 1] == '\r') --le;
        if (le - i > 5 && raw[i] == '\x01' && raw.compare(i + 1, 3, "QF:") == 0 && raw[le - 1] == '\x01') {
            Section sec;
            sec.stmt = std::atoi(raw.c_str() + i + 4);
            res.push_back(sec);
        } else {
            res.back().body.append(raw, i, le - i);
            if (e != std::string::npos) res.back().body += '\n';
        }
        i = next;
    }
    return res;
}

int error_line(const std::string& msg) {
    size_t p = 0;
    while ((p = msg.find("LINE ", p)) != std::string::npos) {
        p += 5;
        size_t q = p;
        while (q < msg.size() && std::isdigit((unsigned char)msg[q])) ++q;
        if (q > p && q < msg.size() && msg[q] == ':') return std::atoi(msg.c_str() + p);
    }
    return 0;
}

std::string clean_text_output(const std::string& body) {
    std::string s = body;
    const std::string hint = "use .last to show entire result";
    size_t p;
    while ((p = s.find(hint)) != std::string::npos) s.erase(p, hint.size());
    return s;
}

// ─── CSV ──────────────────────────────────────────────────────────────────────

Table parse_csv(const std::string& b, size_t max_rows, bool& capped) {
    Table t;
    capped = false;
    std::vector<std::string> row;
    std::string cell;
    bool inq = false, header = true, pending = false;

    auto end_row = [&]() -> bool {
        row.push_back(cell);
        cell.clear();
        pending = false;
        if (header) { t.cols.swap(row); header = false; }
        else {
            if (t.rows.size() >= max_rows) { capped = true; return false; }
            t.rows.push_back(std::move(row));
        }
        row.clear();
        return true;
    };

    for (size_t i = 0; i < b.size(); ++i) {
        char c = b[i];
        if (inq) {
            if (c == '"') {
                if (i + 1 < b.size() && b[i + 1] == '"') { cell += '"'; ++i; }
                else inq = false;
            } else cell += c;
            continue;
        }
        if (c == '"')                                   { inq = true; pending = true; }
        else if (c == ',')                              { row.push_back(cell); cell.clear(); pending = true; }
        else if (c == '\r' && i + 1 < b.size() && b[i + 1] == '\n') { }
        else if (c == '\n')                             { if (!end_row()) return t; }
        else                                            { cell += c; pending = true; }
    }
    if (pending || !row.empty()) end_row();
    return t;
}

bool is_null_cell(const std::string& c) { return c.size() == 1 && c[0] == NULL_SENTINEL; }

} // namespace qf
