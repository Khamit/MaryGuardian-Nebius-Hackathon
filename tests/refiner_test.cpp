// FieldRefiner поверх GuardedLlm → RemoteLlm → mock. MOCK_URL обязателен.
#include "core/FieldRefiner.hpp"
#include "core/GuardedLlm.hpp"
#include "core/RemoteLlm.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace mary;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

// Минимальная замена extract::parseDate: строго YYYY-MM-DD и существующий день.
static std::string parseIso(const std::string& s) {
    int y, m, d;
    if (s.size() < 10 || std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return "";
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1990 || y > 2100 || m < 1 || m > 12 || d < 1 || d > dm[m - 1]) return "";
    return s.substr(0, 10);
}

int main() {
    const char* url = std::getenv("MOCK_URL");
    if (!url) { std::cerr << "set MOCK_URL\n"; return 2; }
    std::remove("/tmp/outbound_refiner.jsonl");

    RemoteLlm::Options o;
    o.baseUrl = url; o.apiKey = "k"; o.modelNano = "nano"; o.modelSuper = "super"; o.modelUltra = "ultra"; o.backoffMs = 5;
    RemoteLlm remote(o);
    OutboundLog olog("/tmp/outbound_refiner.jsonl");
    GuardedLlm llm(remote, olog, GuardedLlm::Options{}, [] { return true; }, nullptr, nullptr);

    FieldRefiner refiner(llm, {"invoice", "receipt", "contract", "other"}, parseIso);
    const std::string text =
        "Acme Supplies Ltd\nInvoice No: INV-2026-0042\nDate: 2026-03-12\nDue date: 2026-04-11\nTotal due: $1,250.00\n";

    // 1) Корректные значения принимаются (и проходят через маскирование без потерь).
    {
        RefinedFields r = refiner.refine(text + "MODE:FIELDS");
        CHECK(r.ok && r.rejected == 0);
        CHECK(r.doc_type == "invoice" && r.doc_date == "2026-03-12" && r.expires == "2026-04-11");
        CHECK(r.doc_number == "INV-2026-0042" && r.company == "Acme Supplies Ltd");
        CHECK(r.amount_minor == 125000 && r.currency == "USD");
        CHECK(r.prompt_tokens == 200);
    }
    // 2) Выдуманные: несуществующая дата, номер/компания которых нет в тексте, сумма не из текста, не-ISO дата.
    {
        RefinedFields r = refiner.refine(text + "MODE:FIELDS_BAD");
        CHECK(r.doc_type == "invoice");                              // тип из белого списка — допустим
        CHECK(r.doc_date.empty() && r.expires.empty());
        CHECK(r.doc_number.empty() && r.company.empty());
        CHECK(r.amount_minor == -1 && r.currency.empty());           // EUR в тексте нет (там $)
        CHECK(r.rejected == 6 && r.accepted == 1);
    }
    // 3) Недоступная модель → пустой результат, без исключений.
    {
        RemoteLlm::Options dead = o;
        dead.baseUrl = "http://127.0.0.1:1/v1/"; dead.maxRetries = 0;
        RemoteLlm bad(dead);
        FieldRefiner r2(bad, {"invoice"}, parseIso);
        CHECK(!r2.refine(text).ok);
    }
    std::cout << "refiner OK\n";
    return 0;
}
