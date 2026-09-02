#include "stl_text.h"
#include <windows.h>

namespace stl {

namespace {

struct Xlat { unsigned cp; const char* out; };

const Xlat XLAT[] = {
    { 0x00A0, " " }, { 0x00A1, "!" }, { 0x00A2, " cents " }, { 0x00A3, " pounds " }, { 0x00A5, " yen " },
    { 0x00A9, " copyright " }, { 0x00AB, "\"" }, { 0x00AE, " registered " }, { 0x00B0, " degrees " },
    { 0x00B1, " plus or minus " }, { 0x00B2, "2" }, { 0x00B3, "3" }, { 0x00B4, "'" }, { 0x00B5, " micro " },
    { 0x00B7, " " }, { 0x00B9, "1" }, { 0x00BB, "\"" }, { 0x00BC, " one quarter " }, { 0x00BD, " one half " },
    { 0x00BE, " three quarters " }, { 0x00BF, "?" }, { 0x00C0, "A" }, { 0x00C1, "A" }, { 0x00C2, "A" },
    { 0x00C3, "A" }, { 0x00C4, "A" }, { 0x00C5, "A" }, { 0x00C6, "AE" }, { 0x00C7, "C" }, { 0x00C8, "E" },
    { 0x00C9, "E" }, { 0x00CA, "E" }, { 0x00CB, "E" }, { 0x00CC, "I" }, { 0x00CD, "I" }, { 0x00CE, "I" },
    { 0x00CF, "I" }, { 0x00D0, "D" }, { 0x00D1, "N" }, { 0x00D2, "O" }, { 0x00D3, "O" }, { 0x00D4, "O" },
    { 0x00D5, "O" }, { 0x00D6, "O" }, { 0x00D7, " times " }, { 0x00D8, "O" }, { 0x00D9, "U" }, { 0x00DA, "U" },
    { 0x00DB, "U" }, { 0x00DC, "U" }, { 0x00DD, "Y" }, { 0x00DF, "ss" }, { 0x00E0, "a" }, { 0x00E1, "a" },
    { 0x00E2, "a" }, { 0x00E3, "a" }, { 0x00E4, "a" }, { 0x00E5, "a" }, { 0x00E6, "ae" }, { 0x00E7, "c" },
    { 0x00E8, "e" }, { 0x00E9, "e" }, { 0x00EA, "e" }, { 0x00EB, "e" }, { 0x00EC, "i" }, { 0x00ED, "i" },
    { 0x00EE, "i" }, { 0x00EF, "i" }, { 0x00F0, "d" }, { 0x00F1, "n" }, { 0x00F2, "o" }, { 0x00F3, "o" },
    { 0x00F4, "o" }, { 0x00F5, "o" }, { 0x00F6, "o" }, { 0x00F7, " divided by " }, { 0x00F8, "o" },
    { 0x00F9, "u" }, { 0x00FA, "u" }, { 0x00FB, "u" }, { 0x00FC, "u" }, { 0x00FD, "y" }, { 0x00FF, "y" },
    { 0x0152, "OE" }, { 0x0153, "oe" }, { 0x0160, "S" }, { 0x0161, "s" }, { 0x0178, "Y" }, { 0x017D, "Z" },
    { 0x017E, "z" }, { 0x0192, "f" }, { 0x02C6, "^" }, { 0x02DC, "~" },
    { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" }, { 0x2013, "-" }, { 0x2014, " - " }, { 0x2015, " - " },
    { 0x2018, "'" }, { 0x2019, "'" }, { 0x201A, "'" }, { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" },
    { 0x2020, " " }, { 0x2021, " " }, { 0x2022, " " }, { 0x2026, "..." }, { 0x2030, " per mille " },
    { 0x2039, "'" }, { 0x203A, "'" }, { 0x20AC, " euros " }, { 0x2122, " trademark " }, { 0x2190, " left arrow " },
    { 0x2192, " right arrow " }, { 0x2191, " up arrow " }, { 0x2193, " down arrow " },
};

void append_codepoint(std::string& out, unsigned cp, bool keep_backslash) {
    if (cp < 0x80) {
        char c = static_cast<char>(cp);
        if (c == '\\') { out += keep_backslash ? '\\' : ' '; return; }
        if (c == '\r') { out += ' '; return; }
        if (c == '\t' || c == '\n' || (c >= 0x20 && c < 0x7F)) { out += c; return; }
        out += ' ';
        return;
    }
    for (const Xlat& x : XLAT) {
        if (x.cp == cp) { out += x.out; return; }
    }
    out += ' ';
}

}  // namespace

std::string wide_to_engine_text(const wchar_t* text, size_t len, bool keep_backslash, std::vector<unsigned>* offsets) {
    std::string out;
    out.reserve(len + 8);
    if (offsets) { offsets->clear(); offsets->reserve(len + 8); }
    for (size_t i = 0; i < len; i++) {
        unsigned cp = text[i];
        size_t src = i;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            i++;
        }
        size_t before = out.size();
        append_codepoint(out, cp, keep_backslash);
        if (offsets) for (size_t k = before; k < out.size(); k++) offsets->push_back(static_cast<unsigned>(src));
    }
    return out;
}

std::string utf8_to_engine_text(const std::string& utf8, bool keep_backslash) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &w[0], n);
    return wide_to_engine_text(w.c_str(), w.size(), keep_backslash, nullptr);
}

}  // namespace stl
