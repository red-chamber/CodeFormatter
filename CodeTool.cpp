// CodeTool.cpp —— C/C++ 代码整理工具(图形界面)
//
// 功能:
//   1) 紧凑代码: 删除多余空格(运算符两侧、参数两侧、逗号后等),
//      保留 int a; / if(x) 里的必要空格, 不改变语法, 只删不补。
//   2) 删除注释: 删除 // 与 /* */ 注释, 字符串/字符常量里的内容不动。
//   3) 紧凑+删注释: 先删注释再紧凑。
//   4) 变量/函数改名: 列出输入代码中名字长度超过 4 的变量和函数及出现次数,
//      每一项都可以填写新名字, 单独替换或全部替换。
//
// 其他规则:
//   * 连续 2 个以上空行压缩为 1 个空行(字符串/原始字符串内部不动)
//   * 输入上限 16 MB, 仅用于防止无限输入; 正常代码远小于此值
//
// 编译(GUI):
//   g++ -std=c++17 -O2 -mwindows -static -o CodeTool.exe CodeTool.cpp -lcomdlg32
// 编译(命令行自测):
//   g++ -std=c++17 -O2 -DCODETOOL_CLI -o codetool_cli.exe CodeTool.cpp
//   用法: codetool_cli <compact|strip|both> 输入文件 [-o 输出]
//         codetool_cli vars 输入文件 [最小长度]
//         codetool_cli rename 输入文件 旧名 新名 [-o 输出]

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#ifndef CODETOOL_CLI
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501   // 编辑框为空时显示的提示文字
#endif

namespace {

// 输入上限:只用于防止无限输入,正常代码远小于此值
const size_t kMaxInputBytes = 16u * 1024u * 1024u;

// ============================ 词法表 ============================

const std::unordered_set<std::string> KEYWORDS = {
    // C
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "_Bool", "_Complex", "_Imaginary", "_Alignas",
    "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert",
    "_Thread_local",
    // C++
    "alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "bool",
    "catch", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
    "consteval", "constexpr", "constinit", "const_cast", "co_await",
    "co_return", "co_yield", "decltype", "delete", "dynamic_cast", "explicit",
    "export", "false", "friend", "mutable", "namespace", "new", "noexcept",
    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "reinterpret_cast", "requires", "static_assert",
    "static_cast", "template", "this", "thread_local", "throw", "true", "try",
    "typeid", "typename", "using", "virtual", "wchar_t", "xor", "xor_eq",
};

// 后面保留一个空格的关键字
const std::unordered_set<std::string> SPACE_AFTER_KW = {
    "return", "case", "throw", "goto", "else", "do",
    "co_return", "co_yield", "co_await",
};

// 相邻两个运算符拼接后会产生新 token 的组合
const std::unordered_set<std::string> OPS2 = {
    "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
    "->", "::", "&&", "||", "==", "!=", "<=", ">=", "##", ".*", "//", "/*",
    "*/", "<%", "%>", "<:", ":>", "%:",
};

const std::unordered_set<std::string> OPS3 = {
    "<<=", ">>=", "->*", "...", "<=>",
};

// 按长度从长到短排列,保证最长匹配
const std::vector<std::string> PUNCTS = {
    ">>=", "<<=", "...", "->*", "<=>",
    "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
    "->", "::", "&&", "||", "==", "!=", "<=", ">=", ".*", "##", "<%", "%>",
    "<:", ":>", "%:",
    "{", "}", "[", "]", "(", ")", ";", ":", ",", "?", ".", "+", "-", "*",
    "/", "%", "^", "&", "|", "~", "!", "=", "<", ">", "#", "\\",
};

enum class Kind { WS, RAW, COMMENT, STRING, CHARLIT, IDENT, NUMBER, PUNCT };

struct Token {
    Kind kind;
    std::string text;
    bool tclose = false;   // 该 > 是模板参数列表的收尾
};

bool isWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

bool isDigitByte(unsigned char c) { return c >= '0' && c <= '9'; }

bool isWsByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

bool startsWith(const std::string& s, size_t pos, const std::string& p) {
    return pos + p.size() <= s.size() && s.compare(pos, p.size(), p) == 0;
}

// 扫描引号包围的内容(body 指向开引号之后),返回结束位置的下一位
size_t scanQuoted(const std::string& t, size_t body, char quote) {
    size_t n = t.size();
    size_t i = body;
    while (i < n) {
        if (t[i] == '\\') { i += 2; continue; }
        if (t[i] == quote) return i + 1;
        ++i;
    }
    return n;
}

size_t scanString(const std::string& t, size_t start, size_t body) {
    // 原始字符串 R"delim( ... )delim"
    if (body - start >= 2 && t[body - 2] == 'R' && t[body - 1] == '"') {
        size_t k = t.find('(', body);
        if (k == std::string::npos) return t.size();
        std::string delim = t.substr(body, k - body);
        std::string term = ")" + delim + "\"";
        size_t e = t.find(term, k + 1);
        return e == std::string::npos ? t.size() : e + term.size();
    }
    // 普通字符串(允许 \ 续行)
    return scanQuoted(t, body, '"');
}

size_t scanNumber(const std::string& t, size_t i) {
    size_t n = t.size();
    size_t j = i;
    while (j < n) {
        unsigned char c = static_cast<unsigned char>(t[j]);
        if (std::isalnum(c) || c == '_' || c == '\'' || c == '.') {
            ++j;
        } else if ((c == '+' || c == '-') && j > i &&
                   (t[j - 1] == 'e' || t[j - 1] == 'E' ||
                    t[j - 1] == 'p' || t[j - 1] == 'P')) {
            ++j;   // 指数部分 1e+5 / 0x1p-3
        } else {
            break;
        }
    }
    return j;
}

bool matchStringStart(const std::string& t, size_t i, size_t& body) {
    static const std::vector<std::string> prefixes = {
        "u8R\"", "uR\"", "UR\"", "LR\"", "R\"", "u8\"", "u\"", "U\"", "L\"", "\""};
    for (const auto& p : prefixes) {
        if (startsWith(t, i, p)) { body = i + p.size(); return true; }
    }
    return false;
}

bool matchCharStart(const std::string& t, size_t i, size_t& body) {
    static const std::vector<std::string> prefixes = {"u8'", "u'", "U'", "L'", "'"};
    for (const auto& p : prefixes) {
        if (startsWith(t, i, p)) { body = i + p.size(); return true; }
    }
    return false;
}

// 字符常量的收尾引号;找不到(例如数字分隔符 1'000)时 closed=false
size_t scanCharLiteral(const std::string& t, size_t body, bool& closed) {
    size_t n = t.size(), i = body, steps = 0;
    closed = false;
    while (i < n && steps < 16) {
        if (t[i] == '\\') { i += 2; steps += 2; continue; }
        if (t[i] == '\'') { closed = true; return i + 1; }
        if (t[i] == '\n' || t[i] == '\r') return i;
        ++i;
        ++steps;
    }
    return i;
}

// ============================ 分词(紧凑用) ============================

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> toks;
    size_t i = 0, n = text.size();
    bool atLineStart = true;

    // UTF-8 BOM:原样保留,并且不影响"行首"判断(否则 #include 会被当成普通运算符)
    if (n >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        toks.push_back({Kind::RAW, text.substr(0, 3)});
        i = 3;
    }

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        // 空白:全部记录,换行信息用于判断行首
        if (isWsByte(c)) {
            size_t j = i;
            while (j < n && isWsByte(static_cast<unsigned char>(text[j]))) ++j;
            std::string seg = text.substr(i, j - i);
            if (seg.find('\n') != std::string::npos) atLineStart = true;
            toks.push_back({Kind::WS, std::move(seg)});
            i = j;
            continue;
        }

        // 预处理指令:整行(含反斜杠续行)原样保留
        if (c == '#' && atLineStart) {
            size_t j = i;
            for (;;) {
                size_t e = text.find('\n', j);
                if (e == std::string::npos) { j = n; break; }
                size_t k = e - 1;
                if (k > 0 && text[k] == '\r') --k;
                if (text[k] == '\\') { j = e + 1; continue; }
                j = e;
                break;
            }
            toks.push_back({Kind::RAW, text.substr(i, j - i)});
            i = j;
            atLineStart = false;
            continue;
        }
        atLineStart = false;

        // 注释
        if (startsWith(text, i, "//")) {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = n;
            toks.push_back({Kind::COMMENT, text.substr(i, e - i)});
            i = e;
            continue;
        }
        if (startsWith(text, i, "/*")) {
            size_t e = text.find("*/", i + 2);
            e = (e == std::string::npos) ? n : e + 2;
            toks.push_back({Kind::COMMENT, text.substr(i, e - i)});
            i = e;
            continue;
        }

        // 字符串 / 字符常量
        size_t body = 0;
        if (matchStringStart(text, i, body)) {
            size_t e = scanString(text, i, body);
            toks.push_back({Kind::STRING, text.substr(i, e - i)});
            i = e;
            continue;
        }
        if (matchCharStart(text, i, body)) {
            size_t e = scanQuoted(text, body, '\'');
            toks.push_back({Kind::CHARLIT, text.substr(i, e - i)});
            i = e;
            continue;
        }

        // 标识符/关键字
        if (isWordByte(c) && !isDigitByte(c)) {
            size_t j = i + 1;
            while (j < n && isWordByte(static_cast<unsigned char>(text[j]))) ++j;
            toks.push_back({Kind::IDENT, text.substr(i, j - i)});
            i = j;
            continue;
        }

        // 数字
        if (isDigitByte(c) ||
            (c == '.' && i + 1 < n && isDigitByte(static_cast<unsigned char>(text[i + 1])))) {
            size_t j = scanNumber(text, i);
            toks.push_back({Kind::NUMBER, text.substr(i, j - i)});
            i = j;
            continue;
        }

        // 运算符 / 标点
        bool matched = false;
        for (const auto& p : PUNCTS) {
            if (startsWith(text, i, p)) {
                toks.push_back({Kind::PUNCT, p});
                i += p.size();
                matched = true;
                break;
            }
        }
        if (!matched) {
            toks.push_back({Kind::PUNCT, std::string(1, text[i])});
            i += 1;
        }
    }
    return toks;
}

// ============================ 紧凑排版 ============================

bool mergeRisk(const std::string& l, const std::string& r) {
    if (l.empty() || r.empty()) return false;
    std::string two(1, l.back());
    two += r[0];
    if (OPS2.count(two)) return true;
    if (l.size() >= 2 && OPS3.count(l.substr(l.size() - 2) + r[0])) return true;
    if (r.size() >= 2 && OPS3.count(std::string(1, l.back()) + r.substr(0, 2))) return true;
    return false;
}

// 标记"模板参数列表收尾的 >",仅用于排版时在该 > 后留一个空格。
// 判错了也只是多/少一个空格,不影响语法。
void markTemplateClosers(std::vector<Token>& toks) {
    struct Angle { bool valid; };
    std::vector<Angle> stack;
    const Token* prev = nullptr;

    for (auto& t : toks) {
        if (t.kind == Kind::WS) continue;

        if (t.kind == Kind::PUNCT) {
            const std::string& s = t.text;
            if (s == "<") {
                bool typeish = prev && (prev->kind == Kind::IDENT ||
                                        prev->text == ">" || prev->text == "::");
                stack.push_back({typeish});
            } else if (s == ">") {
                if (!stack.empty()) {
                    if (stack.back().valid) t.tclose = true;
                    stack.pop_back();
                }
            } else if (s == ">>") {
                // C++11 中 vector<vector<int>> 用 >> 收尾,可能是两层模板
                if (!stack.empty()) {
                    bool ok = stack.back().valid;
                    stack.pop_back();
                    if (!stack.empty()) {
                        ok = ok && stack.back().valid;
                        stack.pop_back();
                    }
                    t.tclose = ok;
                }
            } else if (s == ";" || s == "{" || s == "}" || s == ")" ||
                       s == "&&" || s == "||" || s == "?" || s == "=" ||
                       s == "==" || s == "!=" || s == "+=" || s == "-=" ||
                       s == "*=" || s == "/=") {
                stack.clear();      // 出现在这里说明前面是小于号比较,不是模板
            }
        }
        prev = &t;
    }
}

// 判断 seq[i-1] 与 seq[i] 之间是否需要保留一个空格
// hadSpace: 源码里这两个 token 之间本来是否有空白(本工具只删不补)
bool needSpace(const std::vector<const Token*>& seq, size_t i,
               bool inBlockBrace, bool hadSpace) {
    const Token& a = *seq[i - 1];
    const Token& b = *seq[i];
    const std::string& lt = a.text;
    const std::string& rt = b.text;
    if (lt.empty() || rt.empty()) return false;

    bool lw = (a.kind == Kind::IDENT || a.kind == Kind::NUMBER);
    bool rw = (b.kind == Kind::IDENT || b.kind == Kind::NUMBER);

    // 1) 标识符 / 数字相邻,必须留空格(int a 之类)
    if (lw && rw) return true;
    if (a.kind == Kind::NUMBER && rt == ".") return true;
    if (lt == "." && b.kind == Kind::NUMBER) return true;
    if (a.kind == Kind::NUMBER && rt == "...") return true;
    if (lt == "..." && b.kind == Kind::NUMBER) return true;
    if (a.kind == Kind::IDENT && (b.kind == Kind::STRING || b.kind == Kind::CHARLIT)) return true;

    // 2) 运算符拼接会变成别的 token,必须留空格
    if (mergeRisk(lt, rt)) return true;

    // 以下是"美观"规则:只有源码本来就有空白才保留,不凭空添加
    if (!hadSpace) return false;

    // 3) 模板收尾的 > / >> 后面跟标识符时留空格(vector<int> v)
    if ((lt == ">" || lt == ">>") && a.tclose &&
        (b.kind == Kind::IDENT || b.kind == Kind::NUMBER)) {
        return true;
    }

    // 4) 关键字前留空格:  ) return / ) const / ) noexcept
    if (b.kind == Kind::IDENT && KEYWORDS.count(rt) &&
        (lt == ")" || lt == "]" || a.kind == Kind::IDENT ||
         a.kind == Kind::NUMBER)) {
        return true;
    }

    // 5) return / case / throw 等关键字后保留空格(else / do 紧跟 { 的情况除外)
    if (a.kind == Kind::IDENT && SPACE_AFTER_KW.count(lt) &&
        rt != ";" && rt != "}" && rt != ")" && rt != "]" && rt != "," &&
        rt != "{") {
        return true;
    }

    // 6) 语句块内部留空隙 { return ...; };初始化列表 {1, 2, 3} 不加
    if (lt == "{" && inBlockBrace && b.kind != Kind::PUNCT) return true;
    if (rt == "}" && inBlockBrace && lt != "{" && lt != ",") return true;

    // 7) ; 之后紧跟语句关键字时留一个空格
    if (lt == ";" && b.kind == Kind::IDENT && KEYWORDS.count(rt)) return true;

    // 8) 注释前后留一个空格
    if (b.kind == Kind::COMMENT && lt != "(" && lt != "[" && lt != "{") return true;
    if (a.kind == Kind::COMMENT && rw) return true;

    return false;
}

// 删除空白串中"行尾多余的空格/制表符",保留换行与行首缩进
std::string stripTrail(const std::string& ws) {
    std::string r;
    r.reserve(ws.size());
    size_t i = 0;
    while (i < ws.size()) {
        if (ws[i] == ' ' || ws[i] == '\t') {
            size_t j = i;
            while (j < ws.size() && (ws[j] == ' ' || ws[j] == '\t')) ++j;
            bool beforeNewline = (j < ws.size() && ws[j] == '\n');
            if (!beforeNewline && j + 1 < ws.size() && ws[j] == '\r' && ws[j + 1] == '\n') {
                beforeNewline = true;
            }
            if (!beforeNewline) r.append(ws, i, j - i);
            i = j;
        } else {
            r.push_back(ws[i]);
            ++i;
        }
    }
    return r;
}

// 行首缩进:每 4 个空格换成 1 个制表符(源码本来用 Tab 缩进的原样保留)
std::string normalizeIndent(const std::string& ws) {
    std::string r;
    r.reserve(ws.size());
    size_t i = 0;
    bool atLineStart = true;
    while (i < ws.size()) {
        char c = ws[i];
        if (c == '\n') {
            r.push_back(c);
            ++i;
            atLineStart = true;
            continue;
        }
        if (c == '\r') {
            r.push_back(c);
            ++i;
            continue;
        }
        if (atLineStart && (c == ' ' || c == '\t')) {
            size_t j = i, tabs = 0, spaces = 0;
            while (j < ws.size() && (ws[j] == ' ' || ws[j] == '\t')) {
                if (ws[j] == '\t') ++tabs; else ++spaces;
                ++j;
            }
            if (tabs > 0) {                     // 已经是 Tab 缩进,原样保留
                r.append(tabs, '\t');
                r.append(spaces, ' ');
            } else {
                r.append(spaces / 4, '\t');     // 4 空格 -> 1 Tab
                r.append(spaces % 4, ' ');
            }
            i = j;
            atLineStart = false;
            continue;
        }
        r.push_back(c);
        ++i;
        atLineStart = false;
    }
    return r;
}

// 连续 2 个以上空行压缩为 1 个空行:换行数 >= 3 时只保留 2 个
std::string collapseBlankLines(const std::string& ws) {
    size_t nl = static_cast<size_t>(std::count(ws.begin(), ws.end(), '\n'));
    if (nl <= 2) return ws;
    size_t lastNl = ws.rfind('\n');
    std::string indent = ws.substr(lastNl + 1);
    std::string eol = (ws.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    return eol + eol + indent;
}

// 只压缩代码中的空行,字符串/原始字符串/注释内容保持原样
std::string collapseBlankLinesInCode(const std::string& code) {
    std::vector<Token> toks = tokenize(code);
    std::string out;
    out.reserve(code.size());
    for (const auto& t : toks) {
        if (t.kind == Kind::WS) out += collapseBlankLines(t.text);
        else out += t.text;
    }
    return out;
}

std::string render(const std::vector<Token>& toks) {
    // 非空白 token 序列,排版时用来前后查找
    std::vector<const Token*> seq;
    seq.reserve(toks.size());
    for (const auto& t : toks) {
        if (t.kind != Kind::WS) seq.push_back(&t);
    }

    std::string out;
    std::string pending;
    std::vector<bool> braceStack;   // 每层 { 是否为语句块(而不是初始化列表)

    size_t si = 0;                  // 当前非空白 token 在 seq 中的下标
    for (const auto& t : toks) {
        if (t.kind == Kind::WS) {
            pending += t.text;
            continue;
        }
        bool inBlock = !braceStack.empty() && braceStack.back();
        bool hasNewline = pending.find('\n') != std::string::npos;
        bool hadSpace = !pending.empty();

        if (hasNewline) {
            // 保留换行,连续空行压成一个,行首缩进转 Tab
            out += normalizeIndent(collapseBlankLines(stripTrail(pending)));
        } else if (si > 0) {
            if (needSpace(seq, si, inBlock, hadSpace)) out += ' ';
        }
        out += t.text;

        if (t.kind == Kind::PUNCT) {
            if (t.text == "{") {
                bool isBlock = false;
                if (si > 0) {
                    const Token& p = *seq[si - 1];
                    isBlock = !(p.text == "=" || p.text == "(" || p.text == "[" ||
                                p.text == "," || p.text == "{" || p.text == ":" ||
                                p.text == "return" || (p.text == ">" && p.tclose));
                }
                braceStack.push_back(isBlock);
            } else if (t.text == "}") {
                if (!braceStack.empty()) braceStack.pop_back();
            }
        }

        pending.clear();
        ++si;
    }
    if (!pending.empty()) out += normalizeIndent(collapseBlankLines(stripTrail(pending)));

    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

// 只保留非空白 token,用于压缩前后的等价性自检
std::vector<std::pair<int, std::string>> signature(const std::vector<Token>& toks) {
    std::vector<std::pair<int, std::string>> sig;
    sig.reserve(toks.size());
    for (const auto& t : toks) {
        if (t.kind != Kind::WS) sig.emplace_back(static_cast<int>(t.kind), t.text);
    }
    return sig;
}

std::string compact(const std::string& text) {
    std::vector<Token> before = tokenize(text);
    markTemplateClosers(before);
    std::string out = render(before);
    std::vector<Token> after = tokenize(out);
    if (signature(before) != signature(after)) {
        throw std::runtime_error("压缩后 token 序列发生变化,已放弃(请反馈该源文件)");
    }
    return out;
}

// ============================ 删除注释 ============================
// 删注释标记:某行删过注释,稍后去掉该行行尾空白
const char CMT_MARK = '\x01';

// 处理删过注释的行:去掉标记与行尾空格/制表符(保留 CRLF 的 \r)
std::string tidyMarkedLines(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        if (line.find(CMT_MARK) != std::string::npos) {
            line.erase(std::remove(line.begin(), line.end(), CMT_MARK), line.end());
            bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.pop_back();
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (cr) line.push_back('\r');
        }
        r += line;
        if (e >= s.size()) break;
        r.push_back('\n');
        i = e + 1;
    }
    return r;
}

std::string removeComments(const std::string& t) {
    std::string out;
    out.reserve(t.size());
    size_t i = 0, n = t.size();

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(t[i]);
        size_t body = 0;

        // 字符串:整体保留
        if (matchStringStart(t, i, body)) {
            size_t e = scanString(t, i, body);
            out.append(t, i, e - i);
            i = e;
            continue;
        }
        // 字符常量:确认是真正的字面量(避免把 1'000'000 的 ' 当引号)
        if (matchCharStart(t, i, body)) {
            bool closed = false;
            size_t e = scanCharLiteral(t, body, closed);
            if (closed) {
                out.append(t, i, e - i);
                i = e;
                continue;
            }
        }

        // 行注释:支持反斜杠续行,保留换行结构
        if (c == '/' && i + 1 < n && t[i + 1] == '/') {
            out.push_back(CMT_MARK);
            size_t j = i + 2;
            while (j < n) {
                if (t[j] == '\\') {
                    size_t k = j + 1;
                    if (k + 1 < n && t[k] == '\r' && t[k + 1] == '\n') {
                        out += "\r\n";               // 续行:保留 CRLF
                        j = k + 2;
                        continue;
                    }
                    if (k < n && t[k] == '\n') {     // 续行:注释继续到下一行
                        out.push_back('\n');
                        j = k + 1;
                        continue;
                    }
                    ++j;
                    continue;
                }
                if (t[j] == '\r' && j + 1 < n && t[j + 1] == '\n') break;
                if (t[j] == '\n') break;
                ++j;
            }
            i = j;
            continue;
        }

        // 块注释:内容删除,内部换行保留,以免后面代码挤到一行
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            out.push_back(CMT_MARK);
            size_t j = i + 2;
            bool closed = false;
            while (j + 1 < n) {
                if (t[j] == '*' && t[j + 1] == '/') { j += 2; closed = true; break; }
                if (t[j] == '\r' || t[j] == '\n') out.push_back(t[j]);
                ++j;
            }
            if (!closed) j = n;
            // 注释相当于一个空格:两边拼在一起会变成另一个 token 时补一个空格
            if (j < n) {
                char prevCh = 0;
                for (size_t k = out.size(); k > 0; --k) {
                    if (out[k - 1] != CMT_MARK) { prevCh = out[k - 1]; break; }
                }
                char nextCh = t[j];
                bool merge = prevCh != 0 &&
                             isWordByte(static_cast<unsigned char>(prevCh)) &&
                             isWordByte(static_cast<unsigned char>(nextCh));
                if (!merge && prevCh != 0) {
                    std::string two(1, prevCh);
                    two += nextCh;
                    merge = OPS2.count(two) != 0;
                }
                if (merge) out.push_back(' ');
            }
            i = j;
            continue;
        }

        out.push_back(t[i]);
        ++i;
    }
    return collapseBlankLinesInCode(tidyMarkedLines(out));
}

// ============================ 变量识别与重命名 ============================

struct VarCount {
    std::string name;
    int count;
    bool isFunc = false;
};

bool isSpecKw(const std::string& s) {
    static const std::unordered_set<std::string> S = {
        "const", "volatile", "static", "extern", "register", "mutable",
        "constexpr", "consteval", "constinit", "inline", "thread_local",
        "virtual", "friend", "explicit", "signed", "unsigned", "short",
        "long", "struct", "class", "union", "enum", "typename",
    };
    return S.count(s) != 0;
}

bool isBuiltinTypeKw(const std::string& s) {
    static const std::unordered_set<std::string> S = {
        "void", "bool", "_Bool", "char", "wchar_t", "char8_t", "char16_t",
        "char32_t", "int", "float", "double", "auto",
    };
    return S.count(s) != 0;
}

// 语句开头出现这些关键字,一定不是变量声明
bool isStmtBlockKw(const std::string& s) {
    static const std::unordered_set<std::string> S = {
        "return", "goto", "using", "namespace", "typedef", "template", "case",
        "default", "if", "else", "while", "for", "switch", "do", "break",
        "continue", "delete", "new", "throw", "sizeof", "this", "operator",
        "public", "private", "protected", "static_assert", "try", "catch",
        "co_return", "co_yield", "co_await", "asm", "concept", "requires",
    };
    return S.count(s) != 0;
}

bool skipAngles(const std::vector<const Token*>& t, size_t& p) {
    int depth = 0;
    size_t i = p;
    while (i < t.size()) {
        const std::string& s = t[i]->text;
        if (s == "<") ++depth;
        else if (s == ">") {
            if (--depth == 0) { p = i + 1; return true; }
        } else if (s == ">>") {
            depth -= 2;
            if (depth <= 0) { p = i + 1; return true; }
        } else if (s == ";" || s == "{" || s == "}" || s == "&&" || s == "||" ||
                   s == "?" || s == "=" || s == "==" || s == "!=" || s == "<=" ||
                   s == ">=") {
            return false;
        }
        ++i;
    }
    return false;
}

// 跳过一个成对的分组,i 指向开括号;成功时 i 指向闭括号之后
bool skipGroup(const std::vector<const Token*>& t, size_t& i,
               const std::string& open, const std::string& close) {
    int depth = 0;
    while (i < t.size()) {
        const std::string& s = t[i]->text;
        if (s == open) ++depth;
        else if (s == close) {
            if (--depth == 0) { ++i; return true; }
        }
        ++i;
    }
    return false;
}

// 跳到顶层的终结符(; 或 ,),i 停在终结符上
void skipToTerminator(const std::vector<const Token*>& t, size_t& i) {
    int par = 0, brk = 0, brc = 0;
    while (i < t.size()) {
        const std::string& s = t[i]->text;
        if (s == "(") ++par;
        else if (s == ")") { if (par > 0) --par; else return; }
        else if (s == "[") ++brk;
        else if (s == "]") { if (brk > 0) --brk; }
        else if (s == "{") ++brc;
        else if (s == "}") { if (brc > 0) --brc; else return; }
        else if (par == 0 && brk == 0 && brc == 0 && (s == ";" || s == ",")) return;
        ++i;
    }
}

// 数组维度:只接受 [] 或 [常量]
bool skipArrayDims(const std::vector<const Token*>& t, size_t& i) {
    while (i < t.size() && t[i]->text == "[") {
        ++i;
        if (i < t.size() && t[i]->text == "]") { ++i; continue; }
        if (i < t.size() && t[i]->kind == Kind::NUMBER) {
            ++i;
            if (i < t.size() && t[i]->text == "]") { ++i; continue; }
        }
        return false;
    }
    return true;
}

// 名字后面跟着 ( 时,判断括号里更像参数表(函数)还是更像表达式(变量构造实参)
bool looksLikeParamList(const std::vector<const Token*>& t, size_t open) {
    size_t j = open + 1;
    if (j >= t.size()) return false;
    if (t[j]->text == ")") return true;                        // f()
    if (t[j]->kind != Kind::IDENT) return false;               // f(3) / f("x")
    if (isBuiltinTypeKw(t[j]->text) || isSpecKw(t[j]->text)) return true;
    size_t k = j + 1;
    // 跳过 Type::sub 与模板实参 vector<int>
    for (;;) {
        if (k + 1 < t.size() && t[k]->text == "::" && t[k + 1]->kind == Kind::IDENT) {
            k += 2;
            continue;
        }
        if (k < t.size() && t[k]->text == "<") {
            size_t save = k;
            if (skipAngles(t, k)) continue;
            k = save;
        }
        break;
    }
    while (k < t.size() && (t[k]->text == "*" || t[k]->text == "&" ||
                            (t[k]->kind == Kind::IDENT && isSpecKw(t[k]->text)))) {
        ++k;
    }
    if (k < t.size() && t[k]->kind == Kind::IDENT) return true;  // f(MyType v)
    return false;                                               // f(x) / f(a, b)
}

// 解析类型前缀(const T / unsigned long / std::vector<int> 等),i 移到类型之后
bool parseTypePrefix(const std::vector<const Token*>& t, size_t& i, bool& confident) {
    size_t end = t.size();
    bool got = false;
    confident = false;
    while (i < end && t[i]->kind == Kind::IDENT && isSpecKw(t[i]->text)) {
        got = true;
        ++i;
    }
    while (i < end && t[i]->kind == Kind::IDENT && isBuiltinTypeKw(t[i]->text)) {
        got = true;
        confident = true;
        ++i;
    }
    if (!confident && i < end && t[i]->kind == Kind::IDENT &&
        !isStmtBlockKw(t[i]->text) && !isSpecKw(t[i]->text) &&
        !isBuiltinTypeKw(t[i]->text)) {
        ++i;
        while (i + 1 < end && t[i]->text == "::" && t[i + 1]->kind == Kind::IDENT) i += 2;
        if (i < end && t[i]->text == "<") {
            size_t save = i;
            if (skipAngles(t, i)) confident = true;
            else i = save;
        }
        got = true;
    }
    return got;
}

// 逗号后面是否又是一组"类型 + 名字"(函数参数表里每个参数都有自己的类型)
bool startsNewDecl(const std::vector<const Token*>& t, size_t pos) {
    size_t i = pos, end = t.size();
    if (i >= end || t[i]->kind != Kind::IDENT) return false;
    bool confident = false;
    if (!parseTypePrefix(t, i, confident)) return false;
    while (i < end && (t[i]->text == "*" || t[i]->text == "&" || t[i]->text == "&&" ||
                       (t[i]->kind == Kind::IDENT && isSpecKw(t[i]->text)))) {
        ++i;
    }
    if (i >= end || t[i]->kind != Kind::IDENT) return false;
    return true;
}

// 从 start 尝试解析一条声明,把变量名追加到 vars、函数名追加到 funcs
void tryParseDeclaration(const std::vector<const Token*>& t, size_t start,
                         std::vector<std::string>& vars,
                         std::vector<std::string>& funcs) {
    size_t i = start, end = t.size();
    if (i >= end || t[i]->kind != Kind::IDENT) return;
    if (isStmtBlockKw(t[i]->text)) return;

    auto text = [&](size_t k) -> const std::string& { return t[k]->text; };
    bool confident = false;
    if (!parseTypePrefix(t, i, confident)) return;

    // 声明符(逗号分隔的多个变量)
    size_t guard = 0;
    while (i < end && guard++ < 4096) {
        // 指针 / 引用 / 限定符
        while (i < end && (text(i) == "*" || text(i) == "&" || text(i) == "&&" ||
                           (t[i]->kind == Kind::IDENT && isSpecKw(text(i))))) {
            ++i;
        }
        if (i >= end) return;

        std::string name;
        if (text(i) == "(" && i + 2 < end &&
            (text(i + 1) == "*" || text(i + 1) == "&") &&
            t[i + 2]->kind == Kind::IDENT) {
            // 函数指针: int (*fp)(int)
            name = text(i + 2);
            i += 3;
            if (i < end && text(i) == ")") ++i;
            if (i < end && text(i) == "(") {
                if (!skipGroup(t, i, "(", ")")) return;
            }
        } else if (t[i]->kind == Kind::IDENT) {
            name = text(i);
            ++i;
            // 名字后面紧跟 ( → 函数名;形如 T v(3) 的构造实参仍算变量
            if (i < end && text(i) == "(") {
                if (!isStmtBlockKw(name) && !KEYWORDS.count(name) && !isSpecKw(name)) {
                    if (looksLikeParamList(t, i)) funcs.push_back(name);
                    else vars.push_back(name);
                }
                return;
            }
            // 名字后面紧跟 < → 模板函数特化(name<...>(...))
            if (i < end && text(i) == "<") {
                if (!isStmtBlockKw(name) && !KEYWORDS.count(name) && !isSpecKw(name)) {
                    funcs.push_back(name);
                }
                return;
            }
            if (i < end && text(i) == "[") {
                if (!confident) return;          // 形如 a[i] 的表达式
                if (!skipArrayDims(t, i)) return;
            }
        } else {
            return;
        }

        if (KEYWORDS.count(name) || isStmtBlockKw(name) || isSpecKw(name)) return;
        vars.push_back(name);

        if (i >= end) return;
        const std::string& nxt = text(i);
        if (nxt == ";" || nxt == ":" || nxt == ")" || nxt == "}") return;
        if (nxt == ",") {
            if (startsNewDecl(t, i + 1)) return;   // 下一个参数自带类型
            ++i;
            continue;
        }
        if (nxt == "=") {
            ++i;
            skipToTerminator(t, i);
            if (i >= end) return;
            if (text(i) == ";") return;
            if (text(i) == ",") { ++i; continue; }
            return;
        }
        if (nxt == "{") {
            if (!skipGroup(t, i, "{", "}")) return;
            if (i < end && text(i) == ";") return;
            if (i < end && text(i) == ",") { ++i; continue; }
            return;
        }
        return;   // 构造函数实参等按非变量处理
    }
}

std::unordered_map<std::string, int> countIdentifiers(const std::string& code) {
    std::unordered_map<std::string, int> counts;
    size_t i = 0, n = code.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(code[i]);
        size_t body = 0;
        if (matchStringStart(code, i, body)) {
            i = scanString(code, i, body);
            continue;
        }
        if (matchCharStart(code, i, body)) {
            bool closed = false;
            size_t e = scanCharLiteral(code, body, closed);
            if (closed) { i = e; continue; }
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '/') {
            size_t e = code.find('\n', i);
            i = (e == std::string::npos) ? n : e;
            continue;
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '*') {
            size_t e = code.find("*/", i + 2);
            i = (e == std::string::npos) ? n : e + 2;
            continue;
        }
        if (isWordByte(c) && !isDigitByte(c)) {
            size_t j = i + 1;
            while (j < n && isWordByte(static_cast<unsigned char>(code[j]))) ++j;
            ++counts[code.substr(i, j - i)];
            i = j;
            continue;
        }
        ++i;
    }
    return counts;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

// 找出名字长度超过 minLen 的变量和函数,按出现次数降序
std::vector<VarCount> findLongNames(const std::string& code, size_t minLen) {
    std::vector<Token> toks = tokenize(code);
    markTemplateClosers(toks);          // 标记模板收尾的 >,用于识别模板函数
    std::vector<const Token*> seq;
    seq.reserve(toks.size());
    for (const auto& t : toks) {
        if (t.kind == Kind::WS || t.kind == Kind::COMMENT || t.kind == Kind::RAW) continue;
        seq.push_back(&t);
    }

    std::vector<std::string> vars, funcs;
    for (size_t i = 0; i < seq.size(); ++i) {
        bool start = false;
        if (i == 0) {
            start = true;
        } else {
            const Token* prev = seq[i - 1];
            if (prev->text == ";" || prev->text == "{" || prev->text == "}" ||
                prev->text == ":" || prev->text == ",") {
                start = true;
            } else if (prev->text == "(") {
                // 只在"参数表"位置解析: f(...) / for(...) / [](...)
                if (i >= 2) {
                    const Token* p2 = seq[i - 2];
                    start = (p2->kind == Kind::IDENT || p2->text == "]" ||
                             p2->text == ")" || p2->text == ">");
                }
            } else if (prev->text == ">") {
                start = prev->tclose;   // 模板头结束后: template <...> T f(...)
            }
        }
        if (start) tryParseDeclaration(seq, i, vars, funcs);
    }

    std::unordered_map<std::string, int> counts = countIdentifiers(code);
    std::unordered_set<std::string> funcSet(funcs.begin(), funcs.end());
    std::vector<VarCount> res;
    std::unordered_set<std::string> seen;

    auto push = [&](const std::string& nm, bool isFunc) {
        if (utf8Length(nm) <= minLen) return;
        if (!seen.insert(nm).second) return;
        auto it = counts.find(nm);
        res.push_back({nm, it == counts.end() ? 1 : it->second, isFunc});
    };
    for (const auto& nm : vars) push(nm, funcSet.count(nm) != 0);
    for (const auto& nm : funcs) push(nm, true);

    std::sort(res.begin(), res.end(), [](const VarCount& a, const VarCount& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.name < b.name;
    });
    return res;
}

// 一次性替换多个标识符(字符串、字符常量、注释里的内容不动)
std::string replaceIdentifiers(const std::string& code,
                               const std::unordered_map<std::string, std::string>& mapping) {
    std::string out;
    out.reserve(code.size());
    size_t i = 0, n = code.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(code[i]);
        size_t body = 0;
        if (matchStringStart(code, i, body)) {
            size_t e = scanString(code, i, body);
            out.append(code, i, e - i);
            i = e;
            continue;
        }
        if (matchCharStart(code, i, body)) {
            bool closed = false;
            size_t e = scanCharLiteral(code, body, closed);
            if (closed) {
                out.append(code, i, e - i);
                i = e;
                continue;
            }
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '/') {
            size_t e = code.find('\n', i);
            if (e == std::string::npos) e = n;
            out.append(code, i, e - i);
            i = e;
            continue;
        }
        if (c == '/' && i + 1 < n && code[i + 1] == '*') {
            size_t e = code.find("*/", i + 2);
            e = (e == std::string::npos) ? n : e + 2;
            out.append(code, i, e - i);
            i = e;
            continue;
        }
        if (isWordByte(c) && !isDigitByte(c)) {
            size_t j = i + 1;
            while (j < n && isWordByte(static_cast<unsigned char>(code[j]))) ++j;
            auto it = mapping.find(code.substr(i, j - i));
            if (it != mapping.end()) out += it->second;
            else out.append(code, i, j - i);
            i = j;
            continue;
        }
        out.push_back(code[i]);
        ++i;
    }
    return out;
}

bool isValidIdentifier(const std::string& s) {
    if (s.empty()) return false;
    if (KEYWORDS.count(s)) return false;
    size_t i = 0;
    bool first = true;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            bool ok = first ? (std::isalpha(c) || c == '_')
                            : (std::isalnum(c) || c == '_');
            if (!ok) return false;
            ++i;
        } else {
            int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
            if (i + static_cast<size_t>(len) > s.size()) return false;
            i += len;
        }
        first = false;
    }
    return true;
}

}  // namespace

// ============================ 命令行自测入口 ============================
#ifdef CODETOOL_CLI

static bool ReadAll(const char* path, std::string& data, bool& tooBig) {
    tooBig = false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    data.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
        if (data.size() > kMaxInputBytes) {
            tooBig = true;
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static bool WriteAll(const char* path, const std::string& data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "用法: %s <compact|strip|both> 输入文件 [-o 输出文件]\n"
                "      %s vars 输入文件 [最小长度=4]\n"
                "      %s rename 输入文件 旧名 新名 [-o 输出文件]\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }
    std::string op = argv[1];
    std::string in, out;
    bool tooBig = false;
    if (!ReadAll(argv[2], in, tooBig)) {
        if (tooBig) {
            fprintf(stderr, "输入超过 %zu 字节上限(仅用于防止无限输入),已放弃处理\n",
                    kMaxInputBytes);
        } else {
            fprintf(stderr, "读取失败: %s\n", argv[2]);
        }
        return 1;
    }

    if (op == "vars") {
        size_t minLen = (argc >= 4) ? static_cast<size_t>(atoi(argv[3])) : 4;
        std::vector<VarCount> vars = findLongNames(in, minLen);
        printf("共 %zu 项(变量/函数,名字长度 > %zu):\n", vars.size(), minLen);
        for (const auto& v : vars) {
            printf("  %-24s %-6s %d 次\n", v.name.c_str(), v.isFunc ? "函数" : "变量", v.count);
        }
        return 0;
    }

    try {
        if (op == "compact") {
            out = compact(in);
        } else if (op == "strip") {
            out = removeComments(in);
        } else if (op == "both") {
            out = compact(removeComments(in));
        } else if (op == "rename") {
            if (argc < 5) { fprintf(stderr, "rename 需要旧名与新名\n"); return 1; }
            std::unordered_map<std::string, std::string> m;
            m[argv[3]] = argv[4];
            out = replaceIdentifiers(in, m);
        } else {
            fprintf(stderr, "未知操作: %s\n", op.c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "处理失败: %s\n", e.what());
        return 2;
    }

    const char* dst = "out.cpp";
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "-o") { dst = argv[i + 1]; break; }
    }
    if (!WriteAll(dst, out)) {
        fprintf(stderr, "写入失败: %s\n", dst);
        return 1;
    }
    printf("%s: %zu -> %zu 字节\n", dst, in.size(), out.size());
    return 0;
}

#else  // ============================ 图形界面 ============================

namespace ui {

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HWND g_in = nullptr;
HWND g_out = nullptr;
HWND g_status = nullptr;
HWND g_labIn = nullptr;
HWND g_labOut = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_monoFont = nullptr;
HBRUSH g_bgBrush = nullptr;
WNDPROC g_oldIn = nullptr;
WNDPROC g_oldOut = nullptr;
double g_k = 1.0;
std::wstring g_path;
bool g_bom = false;
bool g_eolLf = false;      // 原文件是否只用 LF 换行(保存时还原)
bool g_hasFile = false;
int g_minW = 760;
int g_minH = 460;

// 长变量重命名窗口
HWND g_varWnd = nullptr;
HWND g_rowHost = nullptr;
HWND g_varInfo = nullptr;
HWND g_varStatus = nullptr;
HWND g_btnAll = nullptr;
HWND g_btnRescan = nullptr;
HWND g_btnCloseVar = nullptr;

struct VarRow {
    std::wstring name;
    int count = 0;
    bool isFunc = false;
    HWND lab = nullptr;
    HWND typ = nullptr;
    HWND cnt = nullptr;
    HWND edit = nullptr;
    HWND btn = nullptr;
};
std::vector<VarRow> g_rows;
int g_scroll = 0;

enum {
    ID_OPEN = 1001, ID_SAVE, ID_COMPACT, ID_STRIP, ID_BOTH, ID_VARS, ID_COPY, ID_CLEAR
};

enum { ID_VARS_ALL = 1101, ID_VARS_RESCAN, ID_VARS_CLOSE };
const int ROW_BTN_BASE = 6000;
const size_t LONG_NAME = 4;        // 名字长度超过 4 才列出

struct BtnDef { int id; const wchar_t* text; int w; };

const BtnDef BUTTONS[] = {
    {ID_OPEN, L"打开文件…", 92},
    {ID_SAVE, L"保存结果…", 92},
    {ID_COMPACT, L"紧凑代码", 84},
    {ID_STRIP, L"删除注释", 84},
    {ID_BOTH, L"紧凑+删注释", 104},
    {ID_VARS, L"变量/函数改名…", 124},
    {ID_COPY, L"复制结果", 84},
    {ID_CLEAR, L"清空", 56},
};
const int NBUTTONS = static_cast<int>(sizeof(BUTTONS) / sizeof(BUTTONS[0]));
HWND g_btn[NBUTTONS] = {};

int S(int v) { return static_cast<int>(v * g_k + 0.5); }

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring ToWideAnsi(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool IsValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + static_cast<size_t>(len) > n) return false;
        for (int k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

std::wstring GetText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    int got = GetWindowTextW(h, &s[0], len + 1);
    if (got > 0) s.resize(static_cast<size_t>(got));
    else s.clear();
    return s;
}

// 编辑框按 CRLF 显示换行,这里把 LF / CR 统一成 CRLF
std::wstring NormalizeToCrlf(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\r') {
            r += L"\r\n";
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
        } else if (c == L'\n') {
            r += L"\r\n";
        } else {
            r += c;
        }
    }
    return r;
}

void SetText(HWND h, const std::wstring& s) {
    std::wstring disp = NormalizeToCrlf(s);
    SetWindowTextW(h, disp.c_str());
    SendMessageW(h, EM_SETSEL, 0, 0);
}

void SetStatus(const std::wstring& s) {
    SetWindowTextW(g_status, s.c_str());
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring DefaultOutName(const std::wstring& src) {
    size_t slash = src.find_last_of(L"\\/");
    size_t start = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t dot = src.find_last_of(L'.');
    std::wstring dir = (slash == std::wstring::npos) ? L"" : src.substr(0, slash + 1);
    std::wstring stem, ext;
    if (dot != std::wstring::npos && dot > start) {
        stem = src.substr(start, dot - start);
        ext = src.substr(dot);
    } else {
        stem = src.substr(start);
    }
    return dir + stem + L"_out" + ext;
}

void UpdateTitle() {
    std::wstring t = L"C/C++ 代码整理工具 — 紧凑代码 / 删除注释";
    if (g_hasFile) t += L"  [" + FileNameOf(g_path) + L"]";
    SetWindowTextW(g_hwnd, t.c_str());
}

bool LoadFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        MessageBoxW(g_hwnd, L"无法打开该文件。", L"出错了", MB_ICONERROR);
        return false;
    }
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        data.append(buf, n);
        if (data.size() > kMaxInputBytes) {
            fclose(f);
            wchar_t msg[256];
            swprintf(msg, 256, L"文件超过 %llu MB 上限（该上限只用于防止无限输入），已放弃打开。",
                     static_cast<unsigned long long>(kMaxInputBytes / (1024ull * 1024ull)));
            MessageBoxW(g_hwnd, msg, L"文件过大", MB_ICONWARNING);
            SetStatus(L"文件过大，未打开。");
            return false;
        }
    }
    fclose(f);

    g_bom = false;
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        g_bom = true;
        data.erase(0, 3);
    }
    std::wstring text = IsValidUtf8(data) ? ToWide(data) : ToWideAnsi(data);
    g_eolLf = (data.find("\r\n") == std::string::npos);
    SetText(g_in, text);
    SetText(g_out, L"");
    g_path = path;
    g_hasFile = true;
    UpdateTitle();
    SetStatus(L"已打开 " + FileNameOf(path) + L"（" + std::to_wstring(data.size()) +
              L" 字节）");
    return true;
}

void DoOpen() {
    wchar_t path[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = L"C/C++ 源文件\0*.c;*.h;*.cpp;*.cc;*.cxx;*.hpp;*.hh;*.hxx\0所有文件\0*.*\0";
    ofn.lpstrTitle = L"打开 C/C++ 源文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) LoadFile(path);
}

void DoSave() {
    std::wstring out = GetText(g_out);
    if (out.empty()) {
        SetStatus(L"结果为空，没有可保存的内容。");
        return;
    }
    std::wstring preset = g_hasFile ? DefaultOutName(g_path) : L"output.cpp";
    std::vector<wchar_t> buf(preset.begin(), preset.end());
    buf.resize(1024, L'\0');

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = L"C/C++ 源文件\0*.c;*.h;*.cpp;*.cc;*.cxx;*.hpp;*.hh;*.hxx\0所有文件\0*.*\0";
    ofn.lpstrTitle = L"保存结果";
    ofn.lpstrDefExt = L"cpp";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring text(buf.data());
    std::string bytes = ToUtf8(out);
    if (g_eolLf) {                       // 原文件是 LF,保存时还原
        std::string lf;
        lf.reserve(bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') continue;
            lf.push_back(bytes[i]);
        }
        bytes.swap(lf);
    }
    if (g_bom) bytes.insert(0, "\xEF\xBB\xBF");

    FILE* f = _wfopen(text.c_str(), L"wb");
    if (!f) {
        MessageBoxW(g_hwnd, L"写入文件失败。", L"出错了", MB_ICONERROR);
        return;
    }
    const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    if (!ok) {
        MessageBoxW(g_hwnd, L"写入文件失败。", L"出错了", MB_ICONERROR);
        return;
    }
    SetStatus(L"已保存到 " + FileNameOf(text));
}

void DoCopy() {
    std::wstring out = GetText(g_out);
    if (out.empty()) {
        SetStatus(L"结果为空，没有可复制的内容。");
        return;
    }
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (out.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            std::memcpy(p, out.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    SetStatus(L"结果已复制到剪贴板。");
}

enum class Op { Compact, Strip, Both };

// 统计字节数时按 LF 计算,避免 CRLF 让数字虚高
size_t CountBytes(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        ++n;
    }
    return n;
}

void Transform(Op op) {
    std::wstring in = GetText(g_in);
    if (in.empty()) {
        SetStatus(L"请先打开文件或在左侧粘贴 C/C++ 代码。");
        return;
    }
    std::string src = ToUtf8(in);
    if (src.size() > kMaxInputBytes) {
        wchar_t msg[256];
        swprintf(msg, 256, L"输入超过 %llu MB 上限（该上限只用于防止无限输入），已放弃处理。",
                 static_cast<unsigned long long>(kMaxInputBytes / (1024ull * 1024ull)));
        SetStatus(msg);
        return;
    }
    std::string result;
    try {
        if (op == Op::Compact) result = compact(src);
        else if (op == Op::Strip) result = removeComments(src);
        else result = compact(removeComments(src));
    } catch (const std::exception& e) {
        MessageBoxW(g_hwnd, ToWide(std::string("处理失败：") + e.what()).c_str(),
                    L"出错了", MB_ICONERROR);
        SetStatus(L"处理失败。");
        return;
    }

    SetText(g_out, ToWide(result));

    size_t srcBytes = CountBytes(src);
    size_t outBytes = CountBytes(result);
    double before = static_cast<double>(srcBytes);
    double after = static_cast<double>(outBytes);
    double pct = before > 0 ? 100.0 * (before - after) / before : 0.0;
    const wchar_t* name = (op == Op::Compact) ? L"紧凑代码"
                        : (op == Op::Strip) ? L"删除注释"
                                            : L"紧凑+删注释";
    wchar_t buf[256];
    if (pct >= 0) {
        swprintf(buf, 256, L"%s 完成：%llu 字节 → %llu 字节（减少 %.1f%%）", name,
                 static_cast<unsigned long long>(srcBytes),
                 static_cast<unsigned long long>(outBytes), pct);
    } else {
        swprintf(buf, 256, L"%s 完成：%llu 字节 → %llu 字节（增加 %.1f%%）", name,
                 static_cast<unsigned long long>(srcBytes),
                 static_cast<unsigned long long>(outBytes), -pct);
    }
    SetStatus(buf);
}

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // 编辑框默认不支持 Ctrl+A,这里补上
    if (msg == WM_CHAR && wp == 1 && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    WNDPROC old = (h == g_in) ? g_oldIn : g_oldOut;
    return CallWindowProcW(old, h, msg, wp, lp);
}

// ============================ 长变量替换窗口 ============================

int RowHeight() { return S(34); }

void UpdateScrollInfo(HWND host) {
    RECT rc;
    GetClientRect(host, &rc);
    int content = static_cast<int>(g_rows.size()) * RowHeight();
    int view = rc.bottom;
    int maxScroll = content > view ? content - view : 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content > 0 ? content - 1 : 0;
    si.nPage = static_cast<UINT>(view > 0 ? view : 1);
    si.nPos = g_scroll;
    SetScrollInfo(host, SB_VERT, &si, TRUE);
}

void ScrollRows() {
    if (!g_rowHost) return;
    RECT rc;
    GetClientRect(g_rowHost, &rc);
    int w = rc.right;
    int rh = RowHeight();
    int editW = w - S(282) - S(92);
    if (editW < S(80)) editW = S(80);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        VarRow& r = g_rows[i];
        int y = static_cast<int>(i) * rh - g_scroll;
        MoveWindow(r.lab, S(8), y + S(8), S(150), S(20), TRUE);
        MoveWindow(r.typ, S(162), y + S(8), S(52), S(20), TRUE);
        MoveWindow(r.cnt, S(218), y + S(8), S(60), S(20), TRUE);
        MoveWindow(r.edit, S(282), y + S(4), editW, S(26), TRUE);
        MoveWindow(r.btn, w - S(86), y + S(4), S(78), S(26), TRUE);
    }
}

void BuildRows(const std::vector<VarCount>& vars,
               const std::map<std::wstring, std::wstring>& pending) {
    for (auto& r : g_rows) {
        if (r.lab) DestroyWindow(r.lab);
        if (r.typ) DestroyWindow(r.typ);
        if (r.cnt) DestroyWindow(r.cnt);
        if (r.edit) DestroyWindow(r.edit);
        if (r.btn) DestroyWindow(r.btn);
    }
    g_rows.clear();
    g_scroll = 0;
    if (!g_rowHost) return;

    for (size_t i = 0; i < vars.size(); ++i) {
        VarRow r;
        r.name = ToWide(vars[i].name);
        r.count = vars[i].count;
        r.isFunc = vars[i].isFunc;
        r.lab = CreateWindowExW(0, L"STATIC", r.name.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                0, 0, 0, 0, g_rowHost, nullptr, g_inst, nullptr);
        r.typ = CreateWindowExW(0, L"STATIC", r.isFunc ? L"函数" : L"变量",
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                0, 0, 0, 0, g_rowHost, nullptr, g_inst, nullptr);
        wchar_t cbuf[64];
        swprintf(cbuf, 64, L"%d 次", r.count);
        r.cnt = CreateWindowExW(0, L"STATIC", cbuf,
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                0, 0, 0, 0, g_rowHost, nullptr, g_inst, nullptr);
        r.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, g_rowHost, nullptr, g_inst, nullptr);
        r.btn = CreateWindowExW(0, L"BUTTON", L"替换",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, g_rowHost,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ROW_BTN_BASE + i)),
                                g_inst, nullptr);
        SendMessageW(r.lab, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
        SendMessageW(r.typ, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(r.cnt, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageW(r.edit, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
        SendMessageW(r.btn, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

        auto it = pending.find(r.name);
        if (it != pending.end()) SetWindowTextW(r.edit, it->second.c_str());

        g_rows.push_back(r);
    }

    if (g_varInfo) {
        wchar_t ibuf[320];
        swprintf(ibuf, 320, L"共找到 %zu 项（变量和函数，名字长度 > %zu）。每行依次为：名称、类型、次数、新名字；填好后点「替换」或「全部替换」。",
                 vars.size(), LONG_NAME);
        SetWindowTextW(g_varInfo, ibuf);
    }
    if (g_varStatus) {
        SetWindowTextW(g_varStatus, vars.empty()
            ? L"没有找到名字长度超过 4 的变量或函数。"
            : L"提示：替换只作用于左侧输入区的代码，注释和字符串内容不会被改动。");
    }
    UpdateScrollInfo(g_rowHost);
    ScrollRows();
}

std::map<std::wstring, std::wstring> CollectPending() {
    std::map<std::wstring, std::wstring> pending;
    for (auto& r : g_rows) {
        std::wstring v = GetText(r.edit);
        if (!v.empty() && v != r.name) pending[r.name] = v;
    }
    return pending;
}

void RefreshVarList() {
    if (!g_rowHost) return;
    std::map<std::wstring, std::wstring> pending = CollectPending();
    std::string code = ToUtf8(GetText(g_in));
    if (code.size() > kMaxInputBytes) {
        BuildRows({}, pending);
        if (g_varStatus) SetWindowTextW(g_varStatus, L"输入超过 16 MB 上限（仅用于防止无限输入），已停止扫描。");
        return;
    }
    BuildRows(findLongNames(code, LONG_NAME), pending);
}

bool ValidateNewName(const std::wstring& name, std::wstring& err) {
    if (name.empty()) {
        err = L"新名字不能为空。";
        return false;
    }
    std::string u = ToUtf8(name);
    if (!isValidIdentifier(u)) {
        err = L"“" + name + L"”不是合法的 C/C++ 标识符（只能用字母、数字、下划线，且不能是关键字）。";
        return false;
    }
    return true;
}

void ApplyRenames(const std::unordered_map<std::string, std::string>& mapping) {
    std::string code = ToUtf8(GetText(g_in));
    std::string neu = replaceIdentifiers(code, mapping);
    SetText(g_in, ToWide(neu));
}

void ReplaceOneRow(size_t index) {
    if (index >= g_rows.size()) return;
    VarRow& r = g_rows[index];
    std::wstring newName = GetText(r.edit);
    std::wstring err;
    if (!ValidateNewName(newName, err)) {
        if (g_varStatus) SetWindowTextW(g_varStatus, err.c_str());
        SetFocus(r.edit);
        return;
    }
    if (newName == r.name) {
        if (g_varStatus) SetWindowTextW(g_varStatus, L"新名字与原名相同，不需要替换。");
        return;
    }
    std::unordered_map<std::string, std::string> m;
    m[ToUtf8(r.name)] = ToUtf8(newName);
    std::wstring oldName = r.name;
    ApplyRenames(m);
    // 保留其它行已填写的名字,再重新扫描
    std::map<std::wstring, std::wstring> pending = CollectPending();
    pending.erase(oldName);
    std::string code = ToUtf8(GetText(g_in));
    BuildRows(findLongNames(code, LONG_NAME), pending);
    if (g_varStatus) {
        SetWindowTextW(g_varStatus, (L"已把 " + oldName + L" 替换为 " + newName + L"。").c_str());
    }
}

void ReplaceAllRows() {
    std::unordered_map<std::string, std::string> m;
    std::wstring badName;
    for (auto& r : g_rows) {
        std::wstring v = GetText(r.edit);
        if (v.empty() || v == r.name) continue;
        std::wstring err;
        if (!ValidateNewName(v, err)) {
            badName = v;
            if (g_varStatus) SetWindowTextW(g_varStatus, err.c_str());
            SetFocus(r.edit);
            return;
        }
        m[ToUtf8(r.name)] = ToUtf8(v);
    }
    if (m.empty()) {
        if (g_varStatus) SetWindowTextW(g_varStatus, L"还没有填写任何新名字。");
        return;
    }
    ApplyRenames(m);
    std::string code = ToUtf8(GetText(g_in));
    BuildRows(findLongNames(code, LONG_NAME), {});
    if (g_varStatus) {
        wchar_t buf[128];
        swprintf(buf, 128, L"已替换 %zu 个变量。", m.size());
        SetWindowTextW(g_varStatus, buf);
    }
}

void LayoutVarWindow(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(12), bh = S(30), gap = S(8), lh = S(20), sh = S(22);

    MoveWindow(g_varInfo, m, m, W - 2 * m, lh, TRUE);

    int y = m + lh + gap;
    int btnY = H - m - bh;
    int statusY = btnY - gap - sh;
    int hostH = statusY - gap - y;
    if (hostH < S(80)) hostH = S(80);
    MoveWindow(g_rowHost, m, y, W - 2 * m, hostH, TRUE);
    MoveWindow(g_varStatus, m, statusY, W - 2 * m, sh, TRUE);

    int bw = S(100);
    int x = W - m - bw;
    MoveWindow(g_btnCloseVar, x, btnY, bw, bh, TRUE);
    x -= bw + gap;
    MoveWindow(g_btnRescan, x, btnY, bw, bh, TRUE);
    x -= bw + gap;
    MoveWindow(g_btnAll, x, btnY, bw, bh, TRUE);
}

void CreateVarChildren(HWND hwnd) {
    g_varInfo = CreateWindowExW(0, L"STATIC", L"",
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    g_rowHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"CodeToolRowHost", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    g_varStatus = CreateWindowExW(0, L"STATIC", L"",
                                  WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                                  0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    g_btnAll = CreateWindowExW(0, L"BUTTON", L"全部替换",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               0, 0, 0, 0, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VARS_ALL)),
                               g_inst, nullptr);
    g_btnRescan = CreateWindowExW(0, L"BUTTON", L"重新扫描",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VARS_RESCAN)),
                                  g_inst, nullptr);
    g_btnCloseVar = CreateWindowExW(0, L"BUTTON", L"关闭",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VARS_CLOSE)),
                                    g_inst, nullptr);

    HWND all[] = {g_varInfo, g_varStatus, g_btnAll, g_btnRescan, g_btnCloseVar};
    for (HWND h : all) SendMessageW(h, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

    RefreshVarList();
}

LRESULT CALLBACK RowHostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= ROW_BTN_BASE && id < ROW_BTN_BASE + static_cast<int>(g_rows.size())) {
                ReplaceOneRow(static_cast<size_t>(id - ROW_BTN_BASE));
            }
            return 0;
        }
        case WM_VSCROLL: {
            RECT rc;
            GetClientRect(h, &rc);
            int content = static_cast<int>(g_rows.size()) * RowHeight();
            int maxScroll = content > rc.bottom ? content - rc.bottom : 0;
            switch (LOWORD(wp)) {
                case SB_LINEUP: g_scroll -= RowHeight(); break;
                case SB_LINEDOWN: g_scroll += RowHeight(); break;
                case SB_PAGEUP: g_scroll -= rc.bottom; break;
                case SB_PAGEDOWN: g_scroll += rc.bottom; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {};
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    GetScrollInfo(h, SB_VERT, &si);
                    g_scroll = si.nTrackPos;
                    break;
                }
                case SB_TOP: g_scroll = 0; break;
                case SB_BOTTOM: g_scroll = maxScroll; break;
                default: break;
            }
            if (g_scroll > maxScroll) g_scroll = maxScroll;
            if (g_scroll < 0) g_scroll = 0;
            ScrollRows();
            UpdateScrollInfo(h);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scroll -= (delta / WHEEL_DELTA) * RowHeight() * 2;
            ScrollRows();
            UpdateScrollInfo(h);
            return 0;
        }
        case WM_SIZE:
            UpdateScrollInfo(h);
            ScrollRows();
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(lp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK VarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            CreateVarChildren(hwnd);
            return 0;
        case WM_SIZE:
            LayoutVarWindow(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(lp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_VARS_ALL: ReplaceAllRows(); break;
                case ID_VARS_RESCAN: RefreshVarList(); break;
                case ID_VARS_CLOSE: DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_varWnd = nullptr;
            g_rowHost = nullptr;
            g_varInfo = nullptr;
            g_varStatus = nullptr;
            g_btnAll = nullptr;
            g_btnRescan = nullptr;
            g_btnCloseVar = nullptr;
            g_rows.clear();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenVarWindow() {
    if (g_varWnd && IsWindow(g_varWnd)) {
        SetForegroundWindow(g_varWnd);
        return;
    }
    int w = S(780), h = S(580);
    RECT mr;
    GetWindowRect(g_hwnd, &mr);
    int x = mr.left + ((mr.right - mr.left) - w) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - h) / 2;
    g_varWnd = CreateWindowExW(0, L"CodeToolVarWindow",
                               L"变量 / 函数改名（名字长度 > 4）",
                               WS_OVERLAPPEDWINDOW,
                               x, y, w, h, g_hwnd, nullptr, g_inst, nullptr);
    if (g_varWnd) {
        ShowWindow(g_varWnd, SW_SHOW);
        SetForegroundWindow(g_varWnd);
    }
}

void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(10), bh = S(30), gap = S(8), sh = S(24);

    int x = m;
    for (int i = 0; i < NBUTTONS; ++i) {
        int w = S(BUTTONS[i].w);
        MoveWindow(g_btn[i], x, m, w, bh, TRUE);
        x += w + gap;
    }

    int half = (W - 3 * m) / 2;
    int y = m + bh + gap;
    int lh = S(18);
    MoveWindow(g_labIn, m, y, half, lh, TRUE);
    MoveWindow(g_labOut, m + half + m, y, W - (m + half + m) - m, lh, TRUE);
    y += lh + S(4);
    int eh = H - y - m - sh;
    if (eh < S(80)) eh = S(80);
    MoveWindow(g_in, m, y, half, eh, TRUE);
    MoveWindow(g_out, m + half + m, y, W - (m + half + m) - m, eh, TRUE);
    MoveWindow(g_status, m, H - m - sh, W - 2 * m, sh, TRUE);
}

void CreateChildren(HWND hwnd) {
    for (int i = 0; i < NBUTTONS; ++i) {
        g_btn[i] = CreateWindowExW(0, L"BUTTON", BUTTONS[i].text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)BUTTONS[i].id,
                                   g_inst, nullptr);
        SendMessageW(g_btn[i], WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    }

    DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN |
                      ES_NOHIDESEL | ES_LEFT;
    g_in = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
                           0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    g_out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", editStyle,
                            0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    SendMessageW(g_in, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    SendMessageW(g_out, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    SendMessageW(g_in, EM_SETCUEBANNER, TRUE, (LPARAM)L"在此粘贴或输入 C/C++ 代码");
    SendMessageW(g_out, EM_SETCUEBANNER, TRUE, (LPARAM)L"处理结果显示在这里");
    SendMessageW(g_in, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
    SendMessageW(g_out, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));

    g_oldIn = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_in, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    g_oldOut = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_out, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));

    g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                               0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

    g_labIn = CreateWindowExW(0, L"STATIC", L"输入：C/C++ 源码（可粘贴或打开文件）",
                              WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                              0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    g_labOut = CreateWindowExW(0, L"STATIC", L"输出：处理结果（可复制或保存）",
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                               0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    SendMessageW(g_labIn, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_labOut, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

    SetStatus(L"提示：左侧粘贴代码或点「打开文件…」，再用「紧凑代码」/「删除注释」处理；「变量/函数改名…」可批量给变量和函数改名。");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            CreateChildren(hwnd);
            return 0;

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = g_minW;
            mm->ptMinTrackSize.y = g_minH;
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(lp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }

        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case ID_OPEN: DoOpen(); break;
                case ID_SAVE: DoSave(); break;
                case ID_COMPACT: Transform(Op::Compact); break;
                case ID_STRIP: Transform(Op::Strip); break;
                case ID_BOTH: Transform(Op::Both); break;
                case ID_VARS: OpenVarWindow(); break;
                case ID_COPY: DoCopy(); break;
                case ID_CLEAR:
                    SetText(g_in, L"");
                    SetText(g_out, L"");
                    g_hasFile = false;
                    g_path.clear();
                    g_eolLf = false;
                    UpdateTitle();
                    SetStatus(L"已清空。");
                    SetFocus(g_in);
                    break;
            }
            return 0;
        }

        case WM_DESTROY:
            if (g_varWnd && IsWindow(g_varWnd)) DestroyWindow(g_varWnd);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ui

// 开启 DPI 感知,避免高分屏下界面被系统拉伸变模糊
static void EnableDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        FARPROC raw = GetProcAddress(u, "SetProcessDpiAwarenessContext");
        SetCtxFn f = reinterpret_cast<SetCtxFn>(reinterpret_cast<void*>(raw));
        if (f && f(reinterpret_cast<HANDLE>(-4))) return;   // PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    EnableDpiAwareness();
    ui::g_inst = inst;

    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    if (dpi <= 0) dpi = 96;
    ui::g_k = dpi / 96.0;

    int ptUi = 9, ptMono = 10;
    ui::g_uiFont = CreateFontW(-MulDiv(ptUi, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    ui::g_monoFont = CreateFontW(-MulDiv(ptMono, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    ui::g_bgBrush = GetSysColorBrush(COLOR_BTNFACE);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ui::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ui::g_bgBrush;
    wc.lpszClassName = L"CodeToolWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW varWc = {};
    varWc.cbSize = sizeof(varWc);
    varWc.lpfnWndProc = ui::VarWndProc;
    varWc.hInstance = inst;
    varWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    varWc.hbrBackground = ui::g_bgBrush;
    varWc.lpszClassName = L"CodeToolVarWindow";
    varWc.hIcon = wc.hIcon;
    varWc.hIconSm = wc.hIconSm;
    if (!RegisterClassExW(&varWc)) return 1;

    WNDCLASSEXW rowWc = {};
    rowWc.cbSize = sizeof(rowWc);
    rowWc.lpfnWndProc = ui::RowHostProc;
    rowWc.hInstance = inst;
    rowWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rowWc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    rowWc.lpszClassName = L"CodeToolRowHost";
    if (!RegisterClassExW(&rowWc)) return 1;

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    int pad = ui::S(40);

    int w = std::min(ui::S(1180), workW - pad);
    int h = std::min(ui::S(720), workH - pad);
    if (w < ui::S(420)) w = ui::S(420);
    if (h < ui::S(300)) h = ui::S(300);

    ui::g_minW = std::min(ui::S(880), workW - pad);
    ui::g_minH = std::min(ui::S(460), workH - pad);
    if (ui::g_minW > w) ui::g_minW = w;
    if (ui::g_minH > h) ui::g_minH = h;

    int x = work.left + (workW - w) / 2;
    int y = work.top + (workH - h) / 2;

    HWND hwnd = CreateWindowExW(0, L"CodeToolWindow",
                                L"C/C++ 代码整理工具 — 紧凑代码 / 删除注释",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    SetFocus(ui::g_in);

    // 带参数启动时打开该文件(资源管理器"打开方式")
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2) ui::LoadFile(argv[1]);
        LocalFree(argv);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

#endif
