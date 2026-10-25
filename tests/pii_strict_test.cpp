// Тесты расширенного маскирования (уровень strict) и секретов. Без сети и без БД.
#include "../core/PiiMasker.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mary;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
static bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

static const char* kInvoice =
    "INVOICE\nFrom: Brightwave Energy GmbH\nInvoice No: INV-2026-0106\nDate: 2026-10-14\nDue date: 2026-11-13\n"
    "Bill to: Demo Household LLC\n\n  Electricity, Q3     1 x    412.80\n\nTotal due: 412.80 EUR\nPayment terms: Net 30\n"
    "Pay to IBAN DE02 1203 0000 0000 2020 51\nBIC: BYLADEM1001\nContact: billing@brightwave-demo.example\n"
    "Account holder: Jane Roe\nAddress: 12 Main Street, Springfield\n"
    "Please note: Brightwave Energy GmbH has new bank details. Ref INV-2026-0106.\n";

int main() {
    // ---------- standard: прежнее поведение + секреты ----------
    {
        MaskSession s;
        const std::string m = s.mask("IBAN DE89 3704 0044 0532 0130 00, card 4111 1111 1111 1111, jane.roe@demo.example, SSN 900-12-3456");
        CHECK(has(m, "[IBAN_1]")); CHECK(has(m, "[CARD_1]")); CHECK(has(m, "[EMAIL_1]")); CHECK(has(m, "[SSN_1]"));
        CHECK(!s.leaks(m));
        // standard не трогает компании и номера документов
        const std::string m2 = MaskSession().mask("From: Brightwave Energy GmbH\nInvoice No: INV-2026-0106\n");
        CHECK(has(m2, "Brightwave Energy GmbH")); CHECK(has(m2, "INV-2026-0106"));
    }
    {
        MaskSession s;
        const std::string src =
            "api_key = sk-proj-AbCdEf0123456789AbCdEf0123456789\nAuthorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dBjftJeZ4CVPmB92K27uhbUJU1p1r_wW1gFWFOEjXk\n"
            "password: hunter2hunter2\nkey tvly-dev-ABCDEFGHIJKLMNOP12345678\n"
            "-----BEGIN PRIVATE KEY-----\nMIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7\n-----END PRIVATE KEY-----\n";
        const std::string m = s.mask(src);
        CHECK(!has(m, "sk-proj")); CHECK(!has(m, "eyJhbGci")); CHECK(!has(m, "hunter2")); CHECK(!has(m, "tvly-dev"));
        CHECK(!has(m, "MIIEvQIBADANB")); CHECK(has(m, "[SECRET_"));
        CHECK(s.unmask(m) == src);
        CHECK(!s.leaks(m));
        // обычный текст про «basic information» не должен маскироваться
        CHECK(MaskSession().mask("Basic information about the contract") == "Basic information about the contract");
    }
    {   // известный ключ приложения (как в GuardedLlm::Options::secrets)
        MaskSession s(PiiMasker::parseTerms({"SECRET:v1.CmQKHHNlcmV0LWtleS1mb3ItdGVzdA"}), MaskLevel::Standard);
        const std::string m = s.mask("debug: key=v1.CmQKHHNlcmV0LWtleS1mb3ItdGVzdA used");
        CHECK(!has(m, "CmQKHH")); CHECK(has(m, "[SECRET_1]"));
    }

    // ---------- strict ----------
    {
        MaskSession s({}, MaskLevel::Strict);
        const std::string m = s.mask(kInvoice);
        CHECK(!has(m, "Brightwave"));            // компания: по метке и повторное упоминание без метки
        CHECK(!has(m, "Demo Household"));
        CHECK(!has(m, "INV-2026-0106"));         // номер: по метке, по шаблону и в «Ref …»
        CHECK(!has(m, "Jane Roe"));              // ФИО по метке
        CHECK(!has(m, "12 Main Street"));        // адрес по метке
        CHECK(!has(m, "BYLADEM1001"));           // BIC
        CHECK(!has(m, "DE02 1203"));             // IBAN
        CHECK(!has(m, "billing@"));
        // суммы, даты, метки и тип документа остаются: модель должна уметь их читать
        CHECK(has(m, "Total due: 412.80 EUR")); CHECK(has(m, "Due date: 2026-11-13")); CHECK(has(m, "INVOICE"));
        CHECK(has(m, "From: [COMPANY_1]")); CHECK(has(m, "Invoice No: [DOCNO_1]"));
        CHECK(!s.leaks(m));                      // fail-closed проверка не должна блокировать собственную маскировку
        CHECK(s.unmask(m) == kInvoice);          // обратимость
        std::printf("--- strict preview ---\n%s\n----------------------\n", m.c_str());
    }
    {   // одна и та же компания → одна заглушка, две разные → две
        MaskSession s({}, MaskLevel::Strict);
        const std::string m = s.mask("Between Demo Household LLC and Lumen Cloud Services Inc. Demo Household LLC pays.");
        CHECK(has(m, "[COMPANY_1] and [COMPANY_2]")); CHECK(has(m, "[COMPANY_1] pays"));
    }
    {   // кириллица и Казахстан
        MaskSession s({}, MaskLevel::Strict);
        const std::string src = "Счёт №5 от 15.10.2026\nПоставщик: ИП Орлов\nБИН: 123456789012\nIBAN: KZ86125KZT5004100100\nИтого к оплате: 10 000,00 руб.\n";
        const std::string m = s.mask(src);
        CHECK(!has(m, "Орлов")); CHECK(!has(m, "123456789012")); CHECK(!has(m, "KZ86125")); CHECK(has(m, "Итого к оплате: 10 000,00 руб."));
        CHECK(s.unmask(m) == src); CHECK(!s.leaks(m));
    }
    {   // заглушки не маскируются повторно, система+пользователь используют одну нумерацию
        MaskSession s({}, MaskLevel::Strict);
        const std::string a = s.mask("Vendor: Totally Legit Supplies Ltd");
        const std::string b = s.mask("Ask about Totally Legit Supplies Ltd please");
        CHECK(has(a, "[COMPANY_1]")); CHECK(has(b, "[COMPANY_1]")); CHECK(!s.leaks(a)); CHECK(!s.leaks(b));
    }
    {   // пользовательские термины работают как раньше; частичное пересечение не оставляет «хвост»
        MaskSession s(PiiMasker::parseTerms({"PERSON:Jane Roe"}), MaskLevel::Strict);
        const std::string m = s.mask("Patient: Jane Roe\nPhone: +1 415 555 0132\n");
        CHECK(!has(m, "Jane")); CHECK(!has(m, "415")); CHECK(!s.leaks(m));
    }
    {   // отсутствие ложных срабатываний на обычных документах
        MaskSession s({}, MaskLevel::Strict);
        const std::string src = "Bank statement\nOpening balance 1,000.00\nTotal due 50.00 USD\nThe agreement is effective immediately.\n";
        CHECK(s.mask(src) == src);
    }
    // spans() и mask() согласованы
    {
        MaskSession a({}, MaskLevel::Strict), b({}, MaskLevel::Strict);
        const auto sp = a.spans(kInvoice);
        CHECK(!sp.empty());
        std::string rebuilt; size_t pos = 0; const std::string src = kInvoice;
        for (const auto& x : sp) { rebuilt.append(src, pos, x.begin - pos); rebuilt += x.placeholder; pos = x.end; }
        rebuilt.append(src, pos, std::string::npos);
        CHECK(rebuilt == b.mask(kInvoice));
    }
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("pii_strict_test: all checks passed\n");
    return 0;
}
