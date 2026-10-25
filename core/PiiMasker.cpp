#include "PiiMasker.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <regex>
#include <set>

namespace mary {
namespace {

using Validator = std::function<bool(const std::string&)>;

struct Rule {
    std::string type;
    std::regex re;
    int group = 0;            // 0 = весь матч, N = только группа N (метка остаётся видна модели)
    Validator validate;       // nullptr = без проверки
    bool strictOnly = false;  // правило работает только на уровне strict
    bool trimLead = false;    // срезать ведущие служебные слова («Between», «Pay», «The»…) из найденной компании
};

// ---------------------------------------------------------------------------
// Проверки значений
// ---------------------------------------------------------------------------
std::string digitsOnly(const std::string& s) {
    std::string d;
    for (unsigned char c : s) if (std::isdigit(c)) d += (char)c;
    return d;
}

bool luhn(const std::string& digits) {
    int sum = 0;
    bool dbl = false;
    for (int i = (int)digits.size() - 1; i >= 0; --i) {
        int d = digits[(size_t)i] - '0';
        if (dbl) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
        dbl = !dbl;
    }
    return sum % 10 == 0;
}

bool validCard(const std::string& s) {
    const std::string d = digitsOnly(s);
    return d.size() >= 13 && d.size() <= 19 && luhn(d);
}

// Намеренно мягко: лучше замаскировать лишнее, чем пропустить. Контрольную сумму mod-97 не требуем
// (OCR любит ошибаться в одной цифре, и такой IBAN всё равно нельзя отправлять).
bool plausibleIban(const std::string& s) {
    std::string t;
    for (unsigned char c : s) if (!std::isspace(c)) t += (char)c;
    return t.size() >= 15 && t.size() <= 34 && digitsOnly(t).size() >= 6;
}

bool plausiblePhone(const std::string& s) {
    const size_t n = digitsOnly(s).size();
    return n >= 9 && n <= 15;
}

// Уже заглушка («[COMPANY_1]») — повторно не маскируем, иначе fail-closed проверка заблокировала бы запрос.
bool isPlaceholder(const std::string& s) { return s.find('[') != std::string::npos || s.find(']') != std::string::npos; }
bool notPlaceholder(const std::string& s) { return !s.empty() && !isPlaceholder(s); }

bool hasDigit(const std::string& s) { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
bool hasAlpha(const std::string& s) { return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x80 || std::isalpha(c); }); }

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
        else cur += (char)c;
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

bool startsUpper(const std::string& w) {
    if (w.empty()) return false;
    const unsigned char c = (unsigned char)w[0];
    if (c < 0x80) return std::isupper(c) != 0;
    if (c == 0xD0 && w.size() > 1) { const unsigned char d = (unsigned char)w[1]; return (d >= 0x90 && d <= 0xAF) || d == 0x81; }
    return false;
}

bool plausibleCompany(const std::string& s) {
    if (!notPlaceholder(s) || s.size() < 2 || s.size() > 80 || !hasAlpha(s)) return false;
    return splitWords(s).size() <= 10;
}

// «Jane Roe», «Иван Петров»: 2–4 слова с заглавной, без цифр.
bool looksLikeName(const std::string& s) {
    if (!notPlaceholder(s)) return false;
    const auto w = splitWords(s);
    if (w.size() < 2 || w.size() > 4) return false;
    for (const auto& x : w) {
        if (x.size() < 2 || !startsUpper(x) || hasDigit(x)) return false;
    }
    return true;
}

bool salutationName(const std::string& s) {
    if (!notPlaceholder(s)) return false;
    static const std::set<std::string> stop = {"Sir", "Sirs", "Madam", "Madame", "Customer", "Client", "Team", "All",
                                               "Colleagues", "Applicant", "Friend", "Partner", "User"};
    const auto w = splitWords(s);
    if (w.empty() || w.size() > 3) return false;
    for (const auto& x : w) {
        if (x.size() < 2 || !startsUpper(x) || hasDigit(x) || stop.count(x)) return false;
    }
    return true;
}

bool docNumber(const std::string& s) { return notPlaceholder(s) && hasDigit(s); }
bool tokenLike(const std::string& s) { return notPlaceholder(s) && s.size() >= 32 && hasDigit(s) && hasAlpha(s); }

// ---------------------------------------------------------------------------
// Правила
// ---------------------------------------------------------------------------
const std::vector<Rule>& rules() {
    static const std::vector<Rule> r = [] {
        std::vector<Rule> v;
        const auto icase = std::regex::ECMAScript | std::regex::icase;
        const auto plain = std::regex::ECMAScript;
        auto add = [&](const char* type, const char* re, int group = 0, Validator val = nullptr,
                       std::regex::flag_type fl = std::regex::ECMAScript, bool strict = false, bool trimLead = false) {
            v.push_back(Rule{type, std::regex(re, fl), group, std::move(val), strict, trimLead});
        };
        auto addStrict = [&](const char* type, const char* re, int group, Validator val, std::regex::flag_type fl) {
            add(type, re, group, std::move(val), fl, true);
        };

        // ===== standard: структурированные идентификаторы =====
        add("EMAIL",   R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
        add("CARD",    R"(\b(?:\d[ -]?){13,19}\b)", 0, validCard);
        add("IBAN",    R"(\b[A-Z]{2}\d{2}(?:[ ]?[A-Z0-9]{2,4}){3,8}\b)", 0, plausibleIban);
        add("SSN",     R"(\b\d{3}-\d{2}-\d{4}\b)");
        add("EIN",     R"(\bEIN\b[^\n]{0,10}?(\d{2}-\d{7})\b)", 1);
        add("SNILS",   R"(\b\d{3}-\d{3}-\d{3}[ -]\d{2}\b)");
        // ИНН (РФ), БИН/ИИН (КЗ): только с меткой. Кириллица: icase не работает, регистры вручную.
        add("INN",     R"((?:ИНН|инн|Инн|БИН|бин|Бин|ИИН|иин|Иин|BIN|IIN)\s*[:№#]?\s*(\d{10}|\d{12})\b)", 1);
        add("PASSPORT", R"((?:паспорт|Паспорт|ПАСПОРТ)[^\n]{0,40}?(\d{4}\s?\d{6})\b)", 1);
        add("PHONE",   R"(\+\d[\d\s().-]{8,17}\d)", 0, plausiblePhone);
        add("PHONE",   R"(\(\d{3}\)\s?\d{3}[\s.-]\d{4}\b)");
        add("PHONE",   R"(\b\d{3}[\s.-]\d{3}[\s.-]\d{4}\b)", 0, plausiblePhone);
        add("PHONE",   R"(\b8[\s(-]*\d{3}[\s)-]*\d{3}[\s-]*\d{2}[\s-]*\d{2}\b)");
        add("ACCOUNT", R"(\b\d{20}\b)");
        add("VAT",     R"(\b[A-Z]{2}\d{8,12}\b)");
        add("DOB",     R"((?:date of birth|dob|born)\s*[:\-]?\s*(\d{1,2}[./-]\d{1,2}[./-]\d{2,4}|\d{4}-\d{2}-\d{2}))", 1,
                       nullptr, icase);
        add("DOB",     R"((?:дата рождения|Дата рождения|ДАТА РОЖДЕНИЯ)\s*[:\-]?\s*(\d{1,2}[./-]\d{1,2}[./-]\d{2,4}|\d{4}-\d{2}-\d{2}))", 1);

        // ===== standard: секреты =====
        add("SECRET",  R"(-----BEGIN [A-Z ]{0,20}PRIVATE KEY-----[\s\S]{16,4000}?-----END [A-Z ]{0,20}PRIVATE KEY-----)");
        add("SECRET",  R"(\b(?:sk|pk|rk)-[A-Za-z0-9_-]{20,})");
        add("SECRET",  R"(\btvly-[A-Za-z0-9_-]{16,})");
        add("SECRET",  R"(\bAKIA[0-9A-Z]{16}\b)");
        add("SECRET",  R"(\bgh[pousr]_[A-Za-z0-9]{30,})");
        add("SECRET",  R"(\bxox[abprs]-[A-Za-z0-9-]{10,})");
        add("SECRET",  R"(\bAIza[0-9A-Za-z_-]{30,})");
        add("SECRET",  R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})");
        add("SECRET",  R"(\b(?:bearer|basic)\s+([A-Za-z0-9._~+/=-]{16,}))", 1,
                       [](const std::string& s) { return notPlaceholder(s) && hasDigit(s) && hasAlpha(s); }, icase);
        add("SECRET",  R"((?:api[_ -]?key|apikey|secret|client[_ -]?secret|access[_ -]?token|auth[_ -]?token|token|password|passwd|pwd|passphrase|пароль|Пароль|ПАРОЛЬ)\s*[:=]\s*["']?([^\s"'<>]{6,}))",
                       1, notPlaceholder, icase);

        // ===== strict: сущности по контексту =====
        // Компания по метке. Группа заканчивается на двойном пробеле, табе, «|» или конце строки.
        addStrict("COMPANY", R"(\b(?:from|vendor|supplier|seller|billed\s+by|issued\s+by|merchant|bill\s+to|customer|client|buyer|payee|beneficiary|employer|landlord|tenant|insurer|taxpayer|bank)\s*:\s*([^\n\t|]{2,60}?)(?=\s{2,}|\t|\||\r|\n|$))",
                  1, plausibleCompany, icase);
        addStrict("COMPANY", R"((?:Поставщик|поставщик|Продавец|продавец|Исполнитель|исполнитель|Заказчик|заказчик|Покупатель|покупатель|Плательщик|плательщик|Получатель|получатель|Организация|организация|Эмитент|эмитент|Контрагент|контрагент)\s*:\s*([^\n\t|]{2,60}?)(?=\s{2,}|\t|\||\r|\n|$))",
                  1, plausibleCompany, plain);
        // Компания по юридической форме: «Demo Household LLC», «Lumen Cloud Services Inc», «Brightwave Energy GmbH».
        addStrict("COMPANY", R"(\b((?:[A-Z][A-Za-z0-9&'.-]*[ \t]+){0,4}[A-Z][A-Za-z0-9&'.-]*,?[ \t]+(?:LLC|L\.L\.C\.|Inc\.?|Ltd\.?|Limited|GmbH|Corp\.?|Corporation|PLC|LLP|AG|S\.A\.|B\.V\.|Co\.))(?![A-Za-z]))",
                  1, plausibleCompany, plain);
        v.back().trimLead = true;
        addStrict("COMPANY", R"((?:^|[\s(])((?:ТОО|ООО|АО|ИП|ПАО|ЗАО|ТОВ|ЖШС|АҚ)[ \t]+(?:«|")?[^\n,;]{2,40}))", 1, plausibleCompany, plain);

        // ФИО по метке и по обращению.
        addStrict("PERSON", R"(\b(?:employee|patient|account\s+holder|card\s*holder|full\s+name|name|contact|attn|attention|payee|beneficiary|owner|signed\s+by|prepared\s+by)\s*:\s*([^\n\t|,;:]{3,50}))",
                  1, looksLikeName, icase);
        addStrict("PERSON", R"(\bDear\s+(?:(?:Mr|Mrs|Ms|Dr|Prof)\.?[ \t]+)?([A-Z][a-z]+(?:[ \t]+[A-Z][a-z]+){0,2}))", 1, salutationName, plain);
        addStrict("PERSON", R"((?:ФИО|Ф\.И\.О\.|Пациент|пациент|Сотрудник|сотрудник|Клиент|клиент|Владелец счета|Владелец счёта)\s*:\s*([^\n\t|,;:]{3,50}))",
                  1, notPlaceholder, plain);

        // Адрес по метке.
        addStrict("ADDRESS", R"(\b(?:billing\s+address|shipping\s+address|address|addr|ship\s+to|deliver\s+to)\s*:\s*([^\n]{6,100}))", 1, notPlaceholder, icase);
        addStrict("ADDRESS", R"((?:Адрес|адрес|Юридический адрес|Место нахождения)\s*:\s*([^\n]{6,100}))", 1, notPlaceholder, plain);

        // Номера документов и счетов.
        addStrict("DOCNO", R"(\b(?:invoice|inv|receipt|order|reference|ref|contract|policy|account|acct|customer|client|member|case|statement|po|patient|meter|subscription|tax)\s*(?:no\.?|number|num\.?|nr\.?|#|id)\s*[:#]?\s*([A-Za-z0-9][A-Za-z0-9\-/._]{2,30}))",
                  1, docNumber, icase);
        addStrict("DOCNO", R"(\b(?:INV|RCPT|REC|PO|SO|CN|DN|QT|ORD)[-_]\d{3,}[A-Za-z0-9_-]*)", 0, nullptr, plain);
        addStrict("DOCNO", R"((?:Счёт|Счет|счёт|счет|Договор|договор|Накладная|накладная|Акт|акт|Заказ|заказ)[ \t]*(?:№|N|No\.?)[ \t]*([A-Za-z0-9/_.-]{1,30}))", 1, docNumber, plain);
        addStrict("BIC", R"(\b(?:bic|swift)(?:\s*/\s*(?:bic|swift))?(?:\s*code)?\s*[:#]?\s*([A-Z]{6}[A-Z0-9]{2}(?:[A-Z0-9]{3})?)\b)", 1, notPlaceholder, icase);

        // Длинные токеноподобные строки (ключи неизвестного формата).
        addStrict("SECRET", R"(\b[A-Za-z0-9_-]{32,}\b)", 0, tokenLike, plain);

        // Последняя сетка: любая «голая» последовательность из 9+ цифр — почти всегда идентификатор.
        add("NUM",     R"(\b\d{9,}\b)");
        return v;
    }();
    return r;
}

std::string trimWs(const std::string& s, size_t& lead) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    lead = b;
    return s.substr(b, e - b);
}

// «Between Demo Household LLC» → «Demo Household LLC»: в начале предложения заглавными бывают и служебные слова.
size_t leadingStopChars(const std::string& v) {
    static const std::set<std::string> stop = {"between", "and", "the", "to", "from", "by", "with", "of", "for", "bill", "billed",
        "pay", "paid", "payment", "invoice", "contract", "agreement", "mutual", "lease", "dear", "thank", "thanks", "regards",
        "sincerely", "total", "date", "due", "please", "note", "attn", "re", "ref", "our", "your", "this", "that", "at", "in",
        "on", "as", "per", "all", "any", "new", "a", "an", "we", "you", "is", "are", "service"};
    size_t off = 0;
    for (;;) {
        const size_t sp = v.find_first_of(" \t", off);
        if (sp == std::string::npos) break;
        std::string w = v.substr(off, sp - off);
        std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (!stop.count(w)) break;
        size_t next = v.find_first_not_of(" \t", sp);
        if (next == std::string::npos) break;
        // после срезания должно остаться минимум два слова (иначе «Service Corp» потеряет имя)
        if (v.find_first_of(" \t", next) == std::string::npos) break;
        off = next;
    }
    return off;
}

// Побеждает тот, кто начинается раньше; при равенстве — длиннее; затем порядок правил.
// Частичное пересечение не теряет «хвост»: спан расширяется.
std::vector<PiiHit> mergeHits(const std::string& text, std::vector<PiiHit> hits) {
    std::stable_sort(hits.begin(), hits.end(), [](const PiiHit& a, const PiiHit& b) {
        if (a.begin != b.begin) return a.begin < b.begin;
        return (a.end - a.begin) > (b.end - b.begin);
    });
    std::vector<PiiHit> out;
    for (auto& h : hits) {
        if (!out.empty() && h.begin < out.back().end) {
            if (h.end > out.back().end) {
                out.back().end = h.end;
                out.back().value = text.substr(out.back().begin, out.back().end - out.back().begin);
            }
            continue;
        }
        out.push_back(std::move(h));
    }
    return out;
}

std::string normalizeValue(const std::string& type, const std::string& v) {
    if (type == "SECRET") return v;
    if (type == "CARD" || type == "IBAN" || type == "PHONE" || type == "ACCOUNT" || type == "NUM" ||
        type == "SSN" || type == "SNILS" || type == "INN" || type == "EIN" || type == "PASSPORT" || type == "VAT" ||
        type == "DOCNO" || type == "BIC") {
        std::string o;
        for (unsigned char c : v) if (std::isalnum(c)) o += (char)std::toupper(c);
        return o;
    }
    return foldUtf8(v);   // EMAIL, COMPANY, PERSON, ADDRESS, DOB, термины пользователя
}

bool isTypeName(const std::string& s) {
    if (s.empty() || s.size() > 24 || !std::isupper((unsigned char)s[0])) return false;
    for (unsigned char c : s) if (!(std::isupper(c) || std::isdigit(c) || c == '_')) return false;
    return true;
}

// Какие найденные значения маскируются и при повторных упоминаниях без метки.
bool propagates(const std::string& type, const std::string& value) {
    static const std::set<std::string> kinds = {"COMPANY", "PERSON", "ADDRESS", "DOCNO", "BIC", "SECRET"};
    if (!kinds.count(type) || value.size() < 6) return false;
    if (type == "DOCNO") return hasAlpha(value) || value.find('-') != std::string::npos || value.size() >= 8;
    return true;
}

} // namespace

// Приведение к нижнему регистру с сохранением длины в байтах: ASCII и кириллица (U+0410..U+044F, Ё).
std::string foldUtf8(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { s[i] = (char)std::tolower(c); continue; }
        if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F)      { s[i + 1] = (char)(d + 0x20); }                       // А–П → а–п
            else if (d >= 0xA0 && d <= 0xAF) { s[i] = (char)0xD1; s[i + 1] = (char)(d - 0x20); }    // Р–Я → р–я
            else if (d == 0x81)              { s[i] = (char)0xD1; s[i + 1] = (char)0x91; }          // Ё → ё
            ++i;
        } else if (c >= 0xC0) {
            ++i;   // прочие двубайтовые символы не трогаем
        }
    }
    return s;
}

std::vector<PiiTerm> PiiMasker::parseTerms(const std::vector<std::string>& raw) {
    std::vector<PiiTerm> out;
    for (const auto& r : raw) {
        PiiTerm t{"TERM", r};
        const size_t colon = r.find(':');
        if (colon != std::string::npos && isTypeName(r.substr(0, colon))) {
            t.type = r.substr(0, colon);
            t.text = r.substr(colon + 1);
        }
        const size_t b = t.text.find_first_not_of(" \t");
        const size_t e = t.text.find_last_not_of(" \t");
        t.text = b == std::string::npos ? "" : t.text.substr(b, e - b + 1);
        if (t.text.size() >= 3) out.push_back(std::move(t));   // слишком короткие термины съели бы половину текста
    }
    return out;
}

std::vector<PiiHit> PiiMasker::scan(const std::string& text, const std::vector<PiiTerm>& terms) const {
    std::vector<PiiHit> hits;

    // Термины пользователя идут первыми: при равных спанах они выигрывают у общих правил.
    if (!terms.empty()) {
        const std::string folded = foldUtf8(text);
        for (const auto& t : terms) {
            const std::string needle = foldUtf8(t.text);
            for (size_t p = folded.find(needle); p != std::string::npos; p = folded.find(needle, p + needle.size()))
                hits.push_back({p, p + needle.size(), t.type, text.substr(p, needle.size())});
        }
    }
    const bool strict = level_ == MaskLevel::Strict;
    for (const auto& rule : rules()) {
        if (rule.strictOnly && !strict) continue;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), rule.re); it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            if (rule.group >= (int)m.size() || !m[(size_t)rule.group].matched) continue;
            size_t lead = 0;
            std::string value = trimWs(m[(size_t)rule.group].str(), lead);
            if (value.empty()) continue;
            if (rule.trimLead) {
                const size_t skip = leadingStopChars(value);
                value.erase(0, skip);
                lead += skip;
                if (value.empty()) continue;
            }
            if (rule.validate && !rule.validate(value)) continue;
            const size_t b = (size_t)m.position((size_t)rule.group) + lead;
            hits.push_back({b, b + value.size(), rule.type, value});
        }
    }
    return mergeHits(text, std::move(hits));
}

// Повторные упоминания уже найденных сущностей (в этом тексте и в предыдущих вызовах сессии) маскируются
// даже без метки: «Brightwave Energy GmbH» в шапке → то же значение в теле письма.
std::vector<PiiHit> MaskSession::propagate(const std::string& text, std::vector<PiiHit> hits) {
    if (masker_.level() != MaskLevel::Strict) return hits;
    for (const auto& h : hits)
        if (propagates(h.type, h.value)) known_[h.value] = h.type;
    for (const auto& kv : known_) {
        const std::string& v = kv.first;
        for (size_t p = text.find(v); p != std::string::npos; p = text.find(v, p + v.size()))
            hits.push_back({p, p + v.size(), kv.second, v});
    }
    return mergeHits(text, std::move(hits));
}

std::vector<MaskedSpan> MaskSession::spans(const std::string& text) {
    const auto hits = propagate(text, masker_.scan(text, terms_));
    std::vector<MaskedSpan> out;
    out.reserve(hits.size());
    for (const auto& h : hits) {
        const std::string key = h.type + "\x1f" + normalizeValue(h.type, h.value);
        auto it = forward_.find(key);
        if (it == forward_.end()) {
            const int n = ++counters_[h.type];
            const std::string ph = "[" + h.type + "_" + std::to_string(n) + "]";
            it = forward_.emplace(key, ph).first;
            reverse_[ph] = h.value;
        }
        out.push_back({h.begin, h.end, h.type, it->second});
    }
    return out;
}

std::string MaskSession::mask(const std::string& text) {
    const auto sp = spans(text);
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    for (const auto& s : sp) {
        out.append(text, pos, s.begin - pos);
        out += s.placeholder;
        pos = s.end;
    }
    out.append(text, pos, std::string::npos);
    return out;
}

std::string jsonEscapeBody(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

std::string MaskSession::unmask(const std::string& text, bool jsonEscape) const {
    if (reverse_.empty()) return text;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[') {
            const size_t close = text.find(']', i + 1);
            if (close != std::string::npos && close - i <= 40) {
                auto it = reverse_.find(text.substr(i, close - i + 1));
                if (it != reverse_.end()) {
                    out += jsonEscape ? jsonEscapeBody(it->second) : it->second;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

bool MaskSession::leaks(const std::string& masked) const {
    return !masker_.scan(masked, terms_).empty();
}

size_t MaskSession::total() const {
    size_t n = 0;
    for (const auto& kv : counters_) n += (size_t)kv.second;
    return n;
}

} // namespace mary
