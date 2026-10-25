#include "Xray.hpp"
#include "Verifier.hpp"
#include <mupdf/fitz.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>

namespace mary { namespace xray {
using json = nlohmann::json;

namespace {

// В MuPDF ≤1.23 цвет символа лежит в поле color, в новых версиях — в argb.
template <class T> auto charColor(const T& c, int) -> decltype((uint32_t)c.argb) { return (uint32_t)c.argb & 0xFFFFFFu; }
template <class T> auto charColor(const T& c, long) -> decltype((uint32_t)c.color) { return (uint32_t)c.color & 0xFFFFFFu; }

bool nearWhite(uint32_t rgb) { return ((rgb >> 16) & 0xFF) >= 0xF0 && ((rgb >> 8) & 0xFF) >= 0xF0 && (rgb & 0xFF) >= 0xF0; }

struct Ch { size_t off = 0, len = 0; fz_rect r{}; int line = 0; bool space = false, white = false; };

struct Page {
    std::string text;
    std::vector<Ch> ch;
    std::vector<int> byteToChar;   // смещение в text → индекс в ch (или -1 для служебных переводов строки)
    float x0 = 0, y0 = 0, w = 1, h = 1;
    int pages = 0;
    std::string png;
};

std::string b64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (unsigned char)in[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

bool isWs(const std::string& s, size_t i, size_t& adv) {
    const unsigned char c = (unsigned char)s[i];
    adv = 1;
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') return true;
    if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA0) { adv = 2; return true; }   // NBSP
    return false;
}

// Текст без пробелов (+ карта «позиция в сжатом тексте → смещение в оригинале»).
std::string compact(const std::string& s, std::vector<size_t>* map) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        size_t adv = 1;
        if (isWs(s, i, adv)) { i += adv; continue; }
        o += s[i];
        if (map) map->push_back(i);
        ++i;
    }
    return o;
}

// Все вхождения needle в text без учёта регистра и пробелов. Возвращает диапазоны байт [begin, end) в text.
std::vector<std::pair<size_t, size_t>> findAll(const std::string& text, const std::string& needleRaw, size_t cap = 4) {
    std::vector<std::pair<size_t, size_t>> out;
    const std::string needle = compact(foldUtf8(needleRaw), nullptr);
    if (needle.size() < 2) return out;
    std::vector<size_t> map;
    const std::string hay = compact(foldUtf8(text), &map);
    for (size_t p = hay.find(needle); p != std::string::npos && out.size() < cap; p = hay.find(needle, p + needle.size()))
        out.push_back({map[p], map[p + needle.size() - 1] + 1});
    return out;
}

std::vector<fz_rect> rangeRects(const Page& pg, size_t b, size_t e, bool onlyWhiteLines = false) {
    std::map<int, fz_rect> byLine;
    std::map<int, bool> seen;
    for (size_t i = b; i < e && i < pg.byteToChar.size(); ++i) {
        const int ci = pg.byteToChar[i];
        if (ci < 0) continue;
        const Ch& c = pg.ch[(size_t)ci];
        if (c.space) continue;
        (void)onlyWhiteLines;
        auto it = byLine.find(c.line);
        if (it == byLine.end()) byLine[c.line] = c.r;
        else {
            it->second.x0 = std::min(it->second.x0, c.r.x0); it->second.y0 = std::min(it->second.y0, c.r.y0);
            it->second.x1 = std::max(it->second.x1, c.r.x1); it->second.y1 = std::max(it->second.y1, c.r.y1);
        }
    }
    std::vector<fz_rect> out;
    for (auto& kv : byLine) out.push_back(kv.second);
    return out;
}

json boxJson(const Page& pg, const fz_rect& r, const std::string& kind, const std::string& label, const std::string& tip) {
    return json{{"x", (r.x0 - pg.x0) / pg.w}, {"y", (r.y0 - pg.y0) / pg.h}, {"w", (r.x1 - r.x0) / pg.w}, {"h", (r.y1 - r.y0) / pg.h},
                {"kind", kind}, {"label", label}, {"tip", tip}};
}

std::string alnumUpper(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::toupper(c);
    return o;
}

// Страница → текст с геометрией символов + PNG.
bool loadPage(const std::string& path, int pageNo, float zoom, Page& pg, std::string& err) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) { err = "cannot create MuPDF context"; return false; }
    fz_document* doc = nullptr;
    fz_page* page = nullptr;
    fz_stext_page* st = nullptr;
    fz_pixmap* pix = nullptr;
    fz_buffer* buf = nullptr;
    bool ok = false;
    fz_var(doc); fz_var(page); fz_var(st); fz_var(pix); fz_var(buf);

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, path.c_str());
        pg.pages = fz_count_pages(ctx, doc);
        if (pageNo < 0 || pageNo >= pg.pages) fz_throw(ctx, FZ_ERROR_GENERIC, "page out of range");
        page = fz_load_page(ctx, doc, pageNo);
        const fz_rect b = fz_bound_page(ctx, page);
        pg.x0 = b.x0; pg.y0 = b.y0; pg.w = std::max(1.0f, b.x1 - b.x0); pg.h = std::max(1.0f, b.y1 - b.y0);

        fz_stext_options opts{};
        st = fz_new_stext_page_from_page(ctx, page, &opts);
        int lineNo = 0;
        for (fz_stext_block* block = st->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next, ++lineNo) {
                for (fz_stext_char* c = line->first_char; c; c = c->next) {
                    char u[10];
                    const int n = fz_runetochar(u, c->c);
                    if (n <= 0) continue;
                    Ch x;
                    x.off = pg.text.size(); x.len = (size_t)n; x.r = fz_rect_from_quad(c->quad); x.line = lineNo;
                    x.space = (c->c == ' ' || c->c == 0xA0 || c->c == '\t');
                    x.white = nearWhite(charColor(*c, 0));
                    pg.text.append(u, (size_t)n);
                    for (int k = 0; k < n; ++k) pg.byteToChar.push_back((int)pg.ch.size());
                    pg.ch.push_back(x);
                }
                pg.text += '\n';
                pg.byteToChar.push_back(-1);
            }
            pg.text += '\n';
            pg.byteToChar.push_back(-1);
        }
        pix = fz_new_pixmap_from_page_number(ctx, doc, pageNo, fz_scale(zoom, zoom), fz_device_rgb(ctx), 0);
        buf = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
        unsigned char* data = nullptr;
        const size_t len = fz_buffer_storage(ctx, buf, &data);
        pg.png.assign((const char*)data, len);
        ok = true;
    }
    fz_always(ctx) {
        if (buf) fz_drop_buffer(ctx, buf);
        if (pix) fz_drop_pixmap(ctx, pix);
        if (st) fz_drop_stext_page(ctx, st);
        if (page) fz_drop_page(ctx, page);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) { err = fz_caught_message(ctx); ok = false; }
    fz_drop_context(ctx);
    return ok;
}

} // namespace

std::vector<std::string> dateForms(const std::string& iso) {
    std::vector<std::string> out;
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return out;
    const std::string y = iso.substr(0, 4), m = iso.substr(5, 2), d = iso.substr(8, 2);
    const int mi = std::atoi(m.c_str()), di = std::atoi(d.c_str());
    static const char* full[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    if (mi < 1 || mi > 12) return out;
    const std::string dn = std::to_string(di), mon = full[mi - 1], abbr = mon.substr(0, 3);
    out = {iso, d + "." + m + "." + y, m + "/" + d + "/" + y, d + "/" + m + "/" + y,
           dn + " " + mon + " " + y, mon + " " + dn + ", " + y, abbr + " " + dn + ", " + y, dn + " " + abbr + " " + y};
    return out;
}

std::vector<std::string> amountForms(long long minor) {
    if (minor < 0) return {};
    const std::string whole = std::to_string(minor / 100);
    char fr[8];
    std::snprintf(fr, sizeof fr, "%02lld", minor % 100);
    auto group = [&](char sep) {
        std::string o;
        int cnt = 0;
        for (int i = (int)whole.size() - 1; i >= 0; --i) {
            o.insert(o.begin(), whole[(size_t)i]);
            if (++cnt % 3 == 0 && i > 0) o.insert(o.begin(), sep);
        }
        return o;
    };
    return {group(',') + "." + fr, whole + "." + fr, whole + "," + fr, group('.') + "," + fr};
}

json buildPageView(const std::string& path, const Options& opt) {
    Page pg;
    std::string err;
    if (!loadPage(path, opt.page, opt.zoom, pg, err)) return json{{"error", err.empty() ? "cannot read the page" : err}};

    json boxes = json::array();

    // 1) что нашёл конвейер
    for (const auto& n : opt.needles) {
        if (n.text.empty()) continue;
        for (const auto& rg : findAll(pg.text, n.text))
            for (const auto& r : rangeRects(pg, rg.first, rg.second)) boxes.push_back(boxJson(pg, r, n.kind, n.label, n.tip));
    }

    // 2) что скрыл бы Guardian; счета, не совпавшие с эталоном, получают kind=risk
    MaskSession ms(opt.terms, opt.level);
    for (const auto& s : ms.spans(pg.text)) {
        const std::string value = pg.text.substr(s.begin, s.end - s.begin);
        for (const auto& r : rangeRects(pg, s.begin, s.end)) boxes.push_back(boxJson(pg, r, "mask", s.placeholder, "Replaced before the text is sent to the cloud"));
        if (s.type == "IBAN") {
            const std::string norm = alnumUpper(value);
            const std::string last4 = norm.size() >= 4 ? norm.substr(norm.size() - 4) : norm;
            const bool risky = opt.ibanFp && opt.riskFps.count(opt.ibanFp(norm)) > 0;
            for (const auto& r : rangeRects(pg, s.begin, s.end))
                boxes.push_back(boxJson(pg, r, risky ? "risk" : "field", risky ? "Bank account …" + last4 + " differs from the one on file" : "Bank account …" + last4,
                                        risky ? "Does not match the account confirmed or first seen for this vendor" : "Account number (kept on this computer)"));
        }
    }

    // 3) инструкции для ИИ и невидимый текст
    if (opt.markInjection) {
        Verifier v;
        for (const auto& h : v.scanSpans(pg.text))
            for (const auto& r : rangeRects(pg, h.begin, h.end)) boxes.push_back(boxJson(pg, r, "inject", "Instruction aimed at an AI model", h.rule));
    }
    int hiddenLines = 0;
    {
        std::map<int, std::pair<int, int>> lines;   // строка → {всего непробельных, белых}
        for (const auto& c : pg.ch) { if (c.space) continue; auto& p = lines[c.line]; ++p.first; if (c.white) ++p.second; }
        for (const auto& kv : lines) {
            if (kv.second.first < 3 || kv.second.second * 10 < kv.second.first * 9) continue;   // ≥90% почти белых символов
            fz_rect u{1e9f, 1e9f, -1e9f, -1e9f};
            for (const auto& c : pg.ch) if (c.line == kv.first && !c.space) {
                u.x0 = std::min(u.x0, c.r.x0); u.y0 = std::min(u.y0, c.r.y0); u.x1 = std::max(u.x1, c.r.x1); u.y1 = std::max(u.y1, c.r.y1);
            }
            boxes.push_back(boxJson(pg, u, "hidden", "Invisible text (white on white)", "Text that a person cannot see but a model would read"));
            ++hiddenLines;
        }
    }

    return json{{"ok", true}, {"page", opt.page}, {"pages", pg.pages}, {"width", pg.w}, {"height", pg.h},
                {"png_b64", b64(pg.png)}, {"boxes", boxes}, {"hidden_lines", hiddenLines}};
}

}} // namespace mary::xray
