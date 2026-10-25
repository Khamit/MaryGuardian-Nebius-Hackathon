#include "ClassificationRules.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <regex>
#include <string_view>
#include <utility>
#include <vector>

namespace mary::classification {
namespace {

// Lowercase ASCII, Russian Cyrillic and common Kazakh Cyrillic while preserving
// UTF-8 byte length. Preserving byte length lets regex offsets refer to the source.
std::string lowerUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }
        if (i + 1 < s.size()) {
            const unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xD0 && c2 >= 0x90 && c2 <= 0x9F) {
                // U+0410..U+041F -> U+0430..U+043F
                out.push_back(static_cast<char>(0xD0));
                out.push_back(static_cast<char>(c2 + 0x20));
                i += 2;
                continue;
            }
            if (c == 0xD0 && c2 >= 0xA0 && c2 <= 0xAF) {
                // U+0420..U+042F -> U+0440..U+044F (D1 80..8F)
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(c2 - 0x20));
                i += 2;
                continue;
            }
            if (c == 0xD0 && c2 >= 0x80 && c2 <= 0x8F) {
                // U+0400..U+040F -> U+0450..U+045F (D1 90..9F)
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(c2 + 0x10));
                i += 2;
                continue;
            }
            // Kazakh-specific uppercase letters with dedicated lowercase codepoints.
            static const std::pair<std::string_view, std::string_view> pairs[] = {
                {"\xD2\x92", "\xD2\x93"}, // Ғ -> ғ
                {"\xD3\x98", "\xD3\x99"}, // Ә -> ә
                {"\xD2\x9A", "\xD2\x9B"}, // Қ -> қ
                {"\xD2\xA2", "\xD2\xA3"}, // Ң -> ң
                {"\xD3\xA8", "\xD3\xA9"}, // Ө -> ө
                {"\xD2\xB0", "\xD2\xB1"}, // Ұ -> ұ
                {"\xD2\xAE", "\xD2\xAF"}, // Ү -> ү
                {"\xD2\xBA", "\xD2\xBB"}  // Һ -> һ
            };
            bool mapped = false;
            for (const auto& p : pairs) {
                if (i + p.first.size() <= s.size() &&
                    std::string_view(s.data() + i, p.first.size()) == p.first) {
                    out.append(p.second.data(), p.second.size());
                    i += p.first.size();
                    mapped = true;
                    break;
                }
            }
            if (mapped) continue;
            out.push_back(s[i++]);
            out.push_back(s[i++]);
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) || c == 0xC2; };
    std::size_t first = 0;
    while (first < s.size() && (ws(static_cast<unsigned char>(s[first])) || s[first] == '\xA0')) ++first;
    std::size_t last = s.size();
    while (last > first && (ws(static_cast<unsigned char>(s[last - 1])) || s[last - 1] == '\xA0')) --last;
    s = s.substr(first, last - first);
    while (!s.empty() && (s.front() == ':' || s.front() == '#' || s.front() == '-' || s.front() == '|')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ';' || s.back() == ',' || s.back() == '|' || s.back() == '\t' || s.back() == ' ')) s.pop_back();
    return s;
}


struct PhraseRule {
    const char* type;
    const char* phrase;
    int weight;
};

// Keep phrases specific. Generic words such as "statement", "tax", "order" or
// "final" alone are intentionally low-weight because they create false positives.
static const PhraseRule kPhrases[] = {
    // Invoices / bills
    {"invoice", "invoice", 3}, {"invoice", "tax invoice", 5}, {"invoice", "commercial invoice", 5},
    {"invoice", "invoice number", 4}, {"invoice", "invoice no", 4},
    {"invoice", "invoice #", 4}, {"invoice", "amount due", 2},
    {"invoice", "total due", 3}, {"invoice", "bill to:", 2},
    {"invoice", "счет-фактура", 5}, {"invoice", "счёт-фактура", 5},
    {"invoice", "счет на оплату", 5}, {"invoice", "счёт на оплату", 5},
    {"invoice", "итого к оплате", 3}, {"invoice", "төлем шоты", 4},
    {"invoice", "шот-фактура", 5},

    // Receipts and payment confirmations
    {"receipt", "receipt", 3}, {"receipt", "payment receipt", 5}, {"receipt", "sales receipt", 5},
    {"receipt", "cash receipt", 5}, {"receipt", "receipt no", 4},
    {"receipt", "payment received", 4}, {"receipt", "transaction approved", 3},
    {"receipt", "кассовый чек", 5}, {"receipt", "товарный чек", 5},
    {"receipt", "квитанция об оплате", 5}, {"receipt", "чек №", 4},
    {"receipt", "төлем түбіртегі", 5},
    {"payment_confirmation", "payment confirmation", 5},
    {"payment_confirmation", "proof of payment", 5},
    {"payment_confirmation", "подтверждение платежа", 5},
    {"payment_confirmation", "подтверждение оплаты", 5},

    // Contracts and legal agreements
    {"contract", "contract", 3}, {"contract", "agreement", 2},
    {"contract", "employment agreement", 5}, {"contract", "service agreement", 5},
    {"contract", "lease agreement", 5}, {"contract", "rental agreement", 5},
    {"contract", "this agreement is made", 6}, {"contract", "terms and conditions", 2},
    {"contract", "contract no", 5}, {"contract", "contract number", 5},
    {"contract", "parties agree", 4}, {"contract", "hereby agree", 4},
    {"contract", "договор №", 5}, {"contract", "договор о", 4},
    {"contract", "настоящий договор", 6}, {"contract", "соглашение №", 5},
    {"contract", "тараптар келіседі", 5},

    // Bank statements, kept separate from financial statements
    {"bank_statement", "bank statement", 6}, {"bank_statement", "account statement", 5},
    {"bank_statement", "statement of account", 5}, {"bank_statement", "account activity", 4},
    {"bank_statement", "transaction history", 3}, {"bank_statement", "opening balance", 2},
    {"bank_statement", "closing balance", 2}, {"bank_statement", "банковская выписка", 6},
    {"bank_statement", "выписка по счету", 5}, {"bank_statement", "выписка по счёту", 5},
    {"bank_statement", "выписка по карте", 5}, {"bank_statement", "шот бойынша үзінді", 5},

    // Financial statements
    {"financial_statement", "statement of financial position", 6},
    {"financial_statement", "statement of cash flows", 6},
    {"financial_statement", "income statement", 5}, {"financial_statement", "balance sheet", 5},
    {"financial_statement", "financial statements", 5}, {"financial_statement", "profit and loss", 5},
    {"financial_statement", "бухгалтерский баланс", 5},
    {"financial_statement", "финансовая отчетность", 5},
    {"financial_statement", "финансовая отчётность", 5},
    {"financial_statement", "отчет о прибылях и убытках", 5},
    {"financial_statement", "отчёт о прибылях и убытках", 5},

    // Tax
    {"tax_return", "tax return", 6}, {"tax_return", "tax declaration", 6},
    {"tax_return", "vat return", 6}, {"tax_return", "corporate income tax", 4},
    {"tax_return", "tax assessment", 5}, {"tax_return", "налоговая декларация", 6},
    {"tax_return", "декларация по ндс", 6}, {"tax_return", "декларация по налогу", 5},
    {"tax_return", "расчет налогов", 4}, {"tax_return", "расчёт налогов", 4},
    {"tax_return", "салық декларациясы", 6},

    // Payroll
    {"payslip", "payslip", 6}, {"payslip", "pay slip", 6}, {"payslip", "pay stub", 6},
    {"payslip", "payroll statement", 6}, {"payslip", "gross pay", 3}, {"payslip", "net pay", 3},
    {"payslip", "расчетный листок", 6}, {"payslip", "расчётный листок", 6},
    {"payslip", "расчет заработной платы", 5}, {"payslip", "расчёт заработной платы", 5},
    {"payslip", "ведомость начисления зарплаты", 5},

    // Purchase orders and shipping
    {"purchase_order", "purchase order", 6}, {"purchase_order", "p.o. number", 4},
    {"purchase_order", "purchase order no", 5}, {"purchase_order", "заказ на поставку", 6},
    {"purchase_order", "заказ поставщику", 5},
    {"delivery_note", "delivery note", 5}, {"delivery_note", "proof of delivery", 5},
    {"delivery_note", "товарная накладная", 5}, {"delivery_note", "накладная на отпуск", 5},
    {"shipping_document", "bill of lading", 6}, {"shipping_document", "packing list", 5},
    {"shipping_document", "air waybill", 6}, {"shipping_document", "cmr consignment note", 6},
    {"shipping_document", "транспортная накладная", 5}, {"shipping_document", "упаковочный лист", 5},

    // Credit/debit notes and quotes
    {"credit_note", "credit note", 6}, {"credit_note", "credit memo", 6},
    {"credit_note", "кредит-нота", 6}, {"credit_note", "кредитовое авизо", 5},
    {"debit_note", "debit note", 6}, {"debit_note", "debit memo", 6},
    {"quote", "quotation", 5}, {"quote", "sales quote", 5}, {"quote", "estimate", 2},
    {"quote", "коммерческое предложение", 5}, {"quote", "ценовое предложение", 5},

    // Utility bills / policies / certificates / identity
    {"utility_bill", "electricity bill", 5}, {"utility_bill", "water bill", 5},
    {"utility_bill", "utility bill", 6}, {"utility_bill", "gas bill", 5},
    {"utility_bill", "internet service invoice", 4}, {"utility_bill", "счет за электроэнергию", 5},
    {"utility_bill", "счёт за электроэнергию", 5}, {"utility_bill", "квитанция за коммунальные услуги", 5},
    {"insurance_policy", "insurance policy", 6}, {"insurance_policy", "policy number", 3},
    {"insurance_policy", "страховой полис", 6}, {"insurance_policy", "страховая премия", 4},
    {"certificate", "certificate of completion", 6}, {"certificate", "certificate of origin", 6},
    {"certificate", "certificate no", 4}, {"certificate", "сертификат соответствия", 6},
    {"certificate", "акт выполненных работ", 4},
    {"identity_document", "passport number", 6}, {"identity_document", "driver's license", 6},
    {"identity_document", "identity card", 5}, {"identity_document", "удостоверение личности", 6},
    {"identity_document", "паспорт гражданина", 6},

    // General documents. Intentionally less powerful than specialist categories.
    {"report", "annual report", 5}, {"report", "meeting minutes", 5},
    {"report", "audit report", 5}, {"report", "technical report", 4},
    {"report", "отчет о", 2}, {"report", "отчёт о", 2},
    {"letter", "dear sir", 3}, {"letter", "dear madam", 3},
    {"letter", "to whom it may concern", 5}, {"letter", "sincerely", 2},
    {"letter", "уважаемый", 3}, {"letter", "уважаемая", 3},
    {"resume", "curriculum vitae", 6}, {"resume", "work experience", 3},
    {"resume", "professional experience", 3}, {"resume", "резюме", 5},
    {"medical_record", "laboratory results", 5}, {"medical_record", "lab results", 5},
    {"medical_record", "medical report", 5}, {"medical_record", "результаты анализов", 5},
    {"medical_record", "медицинское заключение", 5},

    // Product catalogs, feeds, datasets, price lists
    {"product_catalog", "product catalog", 6}, {"product_catalog", "product feed", 6},
    {"product_catalog", "product data", 4}, {"product_catalog", "price list", 5},
    {"product_catalog", "price sheet", 5}, {"product_catalog", "item catalog", 5},
    {"product_catalog", "product list", 4}, {"product_catalog", "sku list", 5},
    {"product_catalog", "каталог товаров", 6}, {"product_catalog", "прайс-лист", 6},
    {"product_catalog", "прайс лист", 6}, {"product_catalog", "товарный каталог", 6},
    {"product_catalog", "ассортимент", 3}, {"product_catalog", "перечень товаров", 5},
    {"product_catalog", "тауарлар каталогы", 6}, {"product_catalog", "баға парағы", 5},
    {"product_catalog", "тауарлар тізімі", 5},

    {"product_dataset", "product dataset", 6}, {"product_dataset", "data export", 4},
    {"product_dataset", "product export", 5}, {"product_dataset", "inventory export", 5},
    {"product_dataset", "набор данных", 5}, {"product_dataset", "выгрузка товаров", 6},
    {"product_dataset", "экспорт товаров", 5}, {"product_dataset", "выгрузка каталога", 6},
    {"product_dataset", "деректер жиынтығы", 5}, {"product_dataset", "тауарлар выгрузкасы", 5},
};

int scoreType(const std::string& low, const std::string& type) {
    int score = 0;
    for (const auto& r : kPhrases) {
        if (type == r.type && low.find(r.phrase) != std::string::npos) score += r.weight;
    }
    return score;
}

bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
bool validDate(int y, int m, int d) {
    if (y < 1900 || y > 2100 || m < 1 || m > 12 || d < 1) return false;
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const int maxd = (m == 2 && leap(y)) ? 29 : days[m - 1];
    return d <= maxd;
}
std::string isoDate(int y, int m, int d) {
    char b[11];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}

std::string findDate(const std::string& original, const std::string& low) {
    // ISO and year-first: 2026-10-15 / 2026.10.15
    static const std::regex isoRe(R"(\b((?:19|20)\d{2})[-/.](\d{1,2})[-/.](\d{1,2})\b)");
    for (auto i = std::sregex_iterator(low.begin(), low.end(), isoRe); i != std::sregex_iterator(); ++i) {
        const int y = std::stoi((*i)[1].str()), m = std::stoi((*i)[2].str()), d = std::stoi((*i)[3].str());
        if (!validDate(y,m,d)) continue;
        const std::size_t pos = static_cast<std::size_t>(i->position());
        const std::string before = low.substr(pos > 80 ? pos - 80 : 0, std::min<std::size_t>(80, pos));
        if (before.find("due date") != std::string::npos || before.find("payment due") != std::string::npos ||
            before.find("valid until") != std::string::npos || before.find("expiry") != std::string::npos ||
            before.find("expires") != std::string::npos || before.find("действителен до") != std::string::npos ||
            before.find("срок до") != std::string::npos || before.find("дата рождения") != std::string::npos ||
            before.find("period from") != std::string::npos || before.find("за период") != std::string::npos) continue;
        return isoDate(y,m,d);
    }
    // Numeric dates are interpreted day/month/year, appropriate for common RU/KZ documents.
    static const std::regex dmyRe(R"(\b(\d{1,2})[./-](\d{1,2})[./-]((?:19|20)\d{2})\b)");
    for (auto i = std::sregex_iterator(low.begin(), low.end(), dmyRe); i != std::sregex_iterator(); ++i) {
        const int d = std::stoi((*i)[1].str()), m = std::stoi((*i)[2].str()), y = std::stoi((*i)[3].str());
        if (!validDate(y,m,d)) continue;
        const std::size_t pos = static_cast<std::size_t>(i->position());
        const std::string before = low.substr(pos > 80 ? pos - 80 : 0, std::min<std::size_t>(80, pos));
        if (before.find("due date") != std::string::npos || before.find("valid until") != std::string::npos ||
            before.find("expiry") != std::string::npos || before.find("expires") != std::string::npos ||
            before.find("действителен до") != std::string::npos || before.find("срок до") != std::string::npos ||
            before.find("дата рождения") != std::string::npos) continue;
        return isoDate(y,m,d);
    }
    // Month-name dates are parsed manually instead of with Unicode regex word boundaries:
    // std::regex on std::string is byte-oriented and \b does not understand Cyrillic.
    static const std::pair<const char*, int> months[] = {
        {"jan",1},{"january",1},{"feb",2},{"february",2},{"mar",3},{"march",3},
        {"apr",4},{"april",4},{"may",5},{"jun",6},{"june",6},{"jul",7},
        {"july",7},{"aug",8},{"august",8},{"sep",9},{"sept",9},{"september",9},
        {"oct",10},{"october",10},{"nov",11},{"november",11},{"dec",12},{"december",12},
        {"января",1},{"февраля",2},{"марта",3},{"апреля",4},{"мая",5},{"июня",6},
        {"июля",7},{"августа",8},{"сентября",9},{"октября",10},{"ноября",11},{"декабря",12}
    };
    auto skipSpacePunct = [&](std::size_t& p, std::size_t limit) {
        while (p < limit && (low[p] == ' ' || low[p] == '\t' || low[p] == '.' || low[p] == ',')) ++p;
    };
    auto readDigits = [&](std::size_t& p, std::size_t limit, int maxDigits, int& value, int& count) {
        value = 0; count = 0;
        while (p < limit && count < maxDigits && low[p] >= '0' && low[p] <= '9') {
            value = value * 10 + (low[p] - '0'); ++p; ++count;
        }
    };
    for (const auto& month : months) {
        const std::string name = month.first;
        std::size_t pos = low.find(name);
        while (pos != std::string::npos) {
            const std::size_t afterMonth = pos + name.size();
            // Month first: Oct 15, 2026.
            {
                std::size_t p = afterMonth;
                skipSpacePunct(p, low.size());
                int d = 0, dc = 0; readDigits(p, low.size(), 2, d, dc);
                if (dc >= 1 && dc <= 2) {
                    skipSpacePunct(p, low.size());
                    int y = 0, yc = 0; readDigits(p, low.size(), 4, y, yc);
                    if (yc == 4 && validDate(y, month.second, d)) return isoDate(y, month.second, d);
                }
            }
            // Day first: 15 октября 2026.
            {
                std::size_t p = pos;
                while (p > 0 && (low[p - 1] == ' ' || low[p - 1] == '\t')) --p;
                const std::size_t digitEnd = p;
                while (p > 0 && low[p - 1] >= '0' && low[p - 1] <= '9' && digitEnd - p < 2) --p;
                if (digitEnd > p && digitEnd - p <= 2 && (p == 0 || !(low[p - 1] >= '0' && low[p - 1] <= '9'))) {
                    const int d = std::stoi(low.substr(p, digitEnd - p));
                    std::size_t yPos = afterMonth;
                    skipSpacePunct(yPos, low.size());
                    int y = 0, yc = 0; readDigits(yPos, low.size(), 4, y, yc);
                    if (yc == 4 && validDate(y, month.second, d)) return isoDate(y, month.second, d);
                }
            }
            pos = low.find(name, pos + 1);
        }
    }
    (void)original;
    return {};
}

std::string findDocumentNumber(const std::string& low, const std::string& original) {
    static const std::regex labeledRe(
        R"((?:invoice|receipt|contract|agreement|purchase\s+order|document|reference|ref\.?|catalog|feed|dataset|sku|product|номер|счет-фактура|счёт-фактура|счет|счёт|договор|накладная|акт|заказ|артикул|каталог)\s*(?:number|no\.?|id|№|#|num(?:ber)?|номер)?\s*[:№#-]?\s*([a-zа-яё0-9][a-zа-яё0-9./_-]{1,39}))");
    for (auto i = std::sregex_iterator(low.begin(), low.end(), labeledRe); i != std::sregex_iterator(); ++i) {
        std::string v = original.substr(static_cast<std::size_t>((*i)[1].first - low.begin()), (*i)[1].length());
        // A document number should contain a digit to avoid swallowing a label word.
        if (std::any_of(v.begin(), v.end(), [](unsigned char c){ return std::isdigit(c); })) return trim(v);
    }
    static const std::regex genericNoRe(R"((?:\bno\.?\s*|№\s*|#\s*)([a-z0-9][a-z0-9./_-]{2,39}))");
    for (auto i = std::sregex_iterator(low.begin(), low.end(), genericNoRe); i != std::sregex_iterator(); ++i) {
        std::string v = original.substr(static_cast<std::size_t>((*i)[1].first - low.begin()), (*i)[1].length());
        if (std::any_of(v.begin(), v.end(), [](unsigned char c){ return std::isdigit(c); })) return trim(v);
    }
    return {};
}

std::string findCompany(const std::string& original, const std::string& low) {
    struct Marker { const char* text; };
    static const Marker markers[] = {
        {"supplier:"},{"vendor:"},{"issuer:"},{"seller:"},{"from:"},
        {"issued by:"},{"provided by:"},{"store:"},{"shop:"},{"marketplace:"},
        {"поставщик:"},{"продавец:"},{"магазин:"},{"маркетплейс:"},
        {"исполнитель:"},{"эмитент:"},{"организация:"},{"наименование поставщика:"},
        {"заңды тұлға:"},{"жеткізуші:"},{"дүкен:"}
    };
    for (const auto& m : markers) {
        const std::size_t p = low.find(m.text);
        if (p == std::string::npos) continue;
        std::size_t start = p + std::strlen(m.text);
        while (start < original.size() && (original[start] == ' ' || original[start] == '\t' || original[start] == ':' || original[start] == '-')) ++start;
        std::size_t end = original.find_first_of("\r\n", start);
        if (end == std::string::npos) end = original.size();
        std::string candidate = trim(original.substr(start, end - start));
        if (candidate.size() > 100) candidate.resize(100);
        if (candidate.size() >= 2) {
            // Strip common trailing fields accidentally placed on the same line.
            const std::string lc = lowerUtf8(candidate);
            for (const char* stop : {" invoice ", " date ", " phone ", " email ", " bin ", " iin ", " address ", " дата ", " телефон ", " бин ", " иин "}) {
                std::size_t q = lc.find(stop);
                if (q != std::string::npos && q >= 2) candidate = trim(candidate.substr(0,q));
            }
            return candidate;
        }
    }
    // Fallback: legal suffix on a line, e.g. "Acme Services LLC" or "ТОО Example".
    static const char* suffixes[] = {" llc", " inc.", " inc ", " ltd", " limited", " gmbh", " corp", " corporation", "ооо", "тоо", "ип ", "ао ", "пао", "зао", "жшс"};
    for (const char* suffix : suffixes) {
        std::size_t p = low.find(suffix);
        if (p == std::string::npos) continue;
        std::size_t lineStart = low.rfind('\n', p);
        lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        std::size_t lineEnd = low.find('\n', p);
        if (lineEnd == std::string::npos) lineEnd = original.size();
        std::size_t start = lineStart;
        while (start < p && (original[start] == ' ' || original[start] == '\t' || original[start] == '|' || original[start] == ':')) ++start;
        if (p > start && p - start < 90) {
            std::string candidate = trim(original.substr(start, std::min<std::size_t>(lineEnd, p + std::char_traits<char>::length(suffix) + 30) - start));
            if (candidate.size() >= 3 && candidate.size() <= 100) return candidate;
        }
    }
    return {};
}

std::string currencyNear(const std::string& low, std::size_t begin, std::size_t len) {
    const std::size_t b = begin > 40 ? begin - 40 : 0;
    const std::size_t e = std::min(low.size(), begin + len + 24);
    const std::string w = low.substr(b, e - b);
    struct Cur { const char* marker; const char* code; };
    static const Cur curs[] = {
        {"usd","USD"},{"eur","EUR"},{"gbp","GBP"},{"kzt","KZT"},{"rub","RUB"},
        {"uzs","UZS"},{"cad","CAD"},{"aud","AUD"},{"cny","CNY"},{"jpy","JPY"},
        {"chf","CHF"},{"aed","AED"},{"pln","PLN"},{"uah","UAH"},{"₸","KZT"},
        {"тг","KZT"},{"тенге","KZT"},{"€","EUR"},{"£","GBP"},{"₽","RUB"},
        {"руб.","RUB"},{"руб","RUB"},{"доллар","USD"},{"евро","EUR"}
    };
    for (const auto& c : curs) {
        if (w.find(c.marker) != std::string::npos) return c.code;
    }
    return {};
}

bool hasAmountLabel(const std::string& low, std::size_t p, std::size_t /*len*/) {
    // Amount labels normally precede the value. Looking both ways can make a nearby
    // invoice date look like a total merely because "TOTAL" occurs later on the page.
    const std::size_t b = p > 100 ? p - 100 : 0;
    const std::string w = low.substr(b, p - b);
    static const char* labels[] = {
        "grand total", "invoice total", "total amount", "total due", "amount due", "balance due",
        "total payable", "amount payable", "total:", "subtotal", "итого к оплате", "итого",
        "сумма к оплате", "всего к оплате", "общая сумма", "сумма платежа", "сумма:",
        "төлеуге жататын сома", "төлем сомасы", "жалпы сома", "amount"
    };
    for (const char* l : labels) if (w.find(l) != std::string::npos) return true;
    return false;
}

bool parseAmount(std::string token, long long& minor) {
    // Keep digits and separators only. Spaces, apostrophes and NBSP are grouping separators.
    std::string s;
    s.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        if (std::isdigit(c) || token[i] == ',' || token[i] == '.') s.push_back(token[i]);
    }
    if (s.empty()) return false;
    const std::size_t lastSep = s.find_last_of(",.");
    bool decimalSep = false;
    if (lastSep != std::string::npos) {
        const std::size_t trailing = s.size() - lastSep - 1;
        decimalSep = (trailing == 1 || trailing == 2);
    }
    std::string whole, frac;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == ',' || s[i] == '.') && decimalSep && i == lastSep) {
            frac = s.substr(i + 1);
            break;
        }
        if (s[i] >= '0' && s[i] <= '9') whole.push_back(s[i]);
    }
    if (whole.empty() || whole.size() > 12 || frac.size() > 2) return false;
    if (!std::all_of(whole.begin(), whole.end(), [](unsigned char c){ return std::isdigit(c); })) return false;
    if (!std::all_of(frac.begin(), frac.end(), [](unsigned char c){ return std::isdigit(c); })) return false;
    while (frac.size() < 2) frac.push_back('0');
    try {
        const long long w = std::stoll(whole);
        const long long f = frac.empty() ? 0 : std::stoll(frac);
        if (w > (std::numeric_limits<long long>::max() - f) / 100) return false;
        minor = w * 100 + f;
        return true;
    } catch (...) { return false; }
}

long long findAmount(const std::string& original, const std::string& low, std::string& currency) {
    // Numeric tokens only become amounts if a monetary label or an explicit currency
    // occurs nearby. This avoids interpreting dates, invoice IDs and account numbers as money.
    static const std::regex numRe(R"(\d[\d ,.'’\xC2\xA0]*(?:\d))");
    long long best = -1;
    int bestScore = 0;
    std::string bestCur;
    for (auto i = std::sregex_iterator(low.begin(), low.end(), numRe); i != std::sregex_iterator(); ++i) {
        const std::size_t p = static_cast<std::size_t>(i->position());
        const std::size_t len = static_cast<std::size_t>(i->length());
        const std::string token = original.substr(p, len);
        // Short isolated values are very often IDs, page numbers or dates.
        const std::size_t digits = static_cast<std::size_t>(std::count_if(token.begin(), token.end(), [](unsigned char c){ return std::isdigit(c); }));
        if (digits < 3) continue;
        const bool labeled = hasAmountLabel(low, p, len);
        const std::string cur = currencyNear(low, p, len);
        if (!labeled && cur.empty()) continue;
        long long candidate = -1;
        if (!parseAmount(token, candidate)) continue;
        int score = (labeled ? 4 : 0) + (!cur.empty() ? 3 : 0);
        const std::size_t b = p > 100 ? p - 100 : 0;
        const std::string before = low.substr(b, p - b);
        if (before.find("grand total") != std::string::npos || before.find("total due") != std::string::npos ||
            before.find("amount due") != std::string::npos || before.find("итого к оплате") != std::string::npos ||
            before.find("сумма к оплате") != std::string::npos || before.find("төлеуге жататын сома") != std::string::npos) score += 2;
        if (score > bestScore) { best = candidate; bestScore = score; bestCur = cur; }
    }
    if (best >= 0) currency = bestCur;
    return best;
}

std::string toAllowedType(const std::string& t) {
    static const std::pair<const char*, const char*> m[] = {
        {"bank_statement", "statement"}, {"financial_statement", "statement"}, {"tax_return", "tax"},
        {"insurance_policy", "insurance"}, {"utility_bill", "utility"}, {"identity_document", "identity"},
        {"medical_record", "medical"}, {"payment_confirmation", "receipt"}, {"debit_note", "credit_note"},
        {"delivery_note", "other"}, {"shipping_document", "other"}, {"resume", "other"}};
    for (const auto& kv : m) if (t == kv.first) return kv.second;
    return t;
}

} // namespace

RuleResult classify(const std::string& text, const std::string& filename,
                    const std::set<std::string>& allowedTypes) {
    RuleResult out;
    // Do not let a filename alone override document content. The existing
    // extract::fromName() path remains the lower-priority filename fallback.
    (void)filename;
    const std::string low = lowerUtf8(text);

    std::vector<std::pair<std::string, int>> scores;
    for (const char* t : {"invoice", "receipt", "payment_confirmation", "contract", "bank_statement",
                          "financial_statement", "tax_return", "payslip", "purchase_order", "delivery_note",
                          "shipping_document", "credit_note", "debit_note", "quote", "utility_bill",
                          "insurance_policy", "certificate", "identity_document", "report", "letter",
                          "resume", "medical_record", "product_catalog", "product_dataset"}) {
        const int score = scoreType(low, t);
        if (score > 0) scores.emplace_back(t, score);
    }
    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if (!scores.empty()) {
        out.candidate_type = scores.front().first;
        out.type_score = scores.front().second;
        out.type_margin = out.type_score - (scores.size() > 1 ? scores[1].second : 0);
        // Require several points of evidence; do not classify from "final", "tax" or "statement" alone.
        out.type_confident = out.type_score >= 3 && (out.type_margin >= 2 || out.type_score >= 7);
        const std::string mapped = toAllowedType(out.candidate_type);
        if (out.type_confident && mapped != "other" && allowedTypes.count(mapped)) {
            out.type = mapped;
            out.type_supported = true;
        }
    }

    out.company = findCompany(text, lowerUtf8(text));
    out.date = findDate(text, lowerUtf8(text));
    out.number = findDocumentNumber(lowerUtf8(text), text);
    out.amount_minor = findAmount(text, lowerUtf8(text), out.currency);
    if (out.currency.empty()) {
        // Detect currency at document level only when an ISO code or unambiguous symbol exists.
        out.currency = currencyNear(lowerUtf8(text), 0, text.size());
    }
    return out;
}

} // namespace mary::classification
