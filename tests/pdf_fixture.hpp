#pragma once
// Одностраничный PDF для тестов: видимые строки чёрным, скрытые — белым (как в «злом» счёте).
#include <cstdio>
#include <string>
#include <vector>
namespace fixture {
inline std::string esc(const std::string& s) { std::string o; for (char c : s) { if (c == '(' || c == ')' || c == '\\') o += '\\'; o += c; } return o; }
inline std::string makePdf(const std::vector<std::string>& visible, const std::vector<std::string>& hidden = {}) {
    std::string cs = "BT\n/F1 11 Tf\n56 780 Td\n";
    for (size_t i = 0; i < visible.size(); ++i) { if (i) cs += "0 -16 Td\n"; cs += "(" + esc(visible[i]) + ") Tj\n"; }
    cs += "ET\n";
    if (!hidden.empty()) {
        cs += "BT\n/F1 8 Tf\n1 1 1 rg\n56 300 Td\n";
        for (size_t i = 0; i < hidden.size(); ++i) { if (i) cs += "0 -12 Td\n"; cs += "(" + esc(hidden[i]) + ") Tj\n"; }
        cs += "ET\n0 g\n";
    }
    std::vector<std::string> obj(6);
    obj[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    obj[2] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    obj[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>";
    obj[4] = "<< /Length " + std::to_string(cs.size()) + " >>\nstream\n" + cs + "endstream";
    obj[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> off(6, 0);
    for (int i = 1; i <= 5; ++i) { off[(size_t)i] = out.size(); out += std::to_string(i) + " 0 obj\n" + obj[(size_t)i] + "\nendobj\n"; }
    const size_t xref = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) { char b[32]; std::snprintf(b, sizeof b, "%010zu 00000 n \n", off[(size_t)i]); out += b; }
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}
} // namespace fixture
