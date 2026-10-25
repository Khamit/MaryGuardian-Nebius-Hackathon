#include "core/PiiMasker.hpp"
#include <cassert>
#include <iostream>

using namespace mary;

static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

int main() {
    // --- структурированные идентификаторы маскируются ---
    {
        MaskSession s;
        const std::string in =
            "Card 4111 1111 1111 1111, IBAN DE89 3704 0044 0532 0130 00, compact GB82WEST12345698765432, "
            "mail john.doe@example.com, SSN 123-45-6789, EIN: 12-3456789, tel +49 30 1234567, (415) 555-0132, "
            "ИНН: 7707083893, СНИЛС 112-233-445 95, паспорт 4510 123456, р/с 40817810099910004312, "
            "дата рождения: 12.03.1985, mobile 8 701 123 45 67, VAT DE123456789";
        const std::string m = s.mask(in);
        for (const char* secret : {"4111", "3704", "WEST1234", "john.doe", "123-45-6789", "3456789", "1234567",
                                   "555-0132", "7707083893", "112-233", "123456", "40817810099910004312",
                                   "12.03.1985", "701 123", "DE123456789"})
            if (has(m, secret)) { std::cerr << "LEAK of " << secret << "\n  " << m << "\n"; return 1; }
        CHECK(!s.leaks(m));
        CHECK(has(m, "[CARD_1]") && has(m, "[IBAN_1]") && has(m, "[IBAN_2]") && has(m, "[EMAIL_1]"));
        CHECK(has(m, "ИНН: [INN_1]"));            // метка остаётся — модель понимает, что это за поле
        CHECK(s.unmask(m) == in);                  // полностью обратимо
        std::cout << m << "\n";
    }
    // --- обычные данные счёта не трогаем (иначе модель ослепнет) ---
    {
        MaskSession s;
        const std::string in = "Invoice INV-2026-0042 dated 2026-03-12, total 1,250.00 USD, due 2026-04-11. See [12] and [7].";
        CHECK(s.mask(in) == in);
        CHECK(s.total() == 0);
        // не-Luhn «номер» из 16 цифр с пробелами — не карта
        CHECK(s.mask("счёт-фактура 1234 5678 9012 3456") == "счёт-фактура 1234 5678 9012 3456");
    }
    // --- одинаковое значение → одна заглушка; разные → разные ---
    {
        MaskSession s;
        const std::string m = s.mask("a@x.io b@x.io a@x.io");
        CHECK(m == "[EMAIL_1] [EMAIL_2] [EMAIL_1]");
        CHECK(s.mask("b@x.io") == "[EMAIL_2]");   // и между вызовами в одной сессии
        CHECK(s.unmask("hello [EMAIL_2]!") == "hello b@x.io!");
        CHECK(s.unmask("[EMAIL_9] stays") == "[EMAIL_9] stays");   // неизвестные заглушки не трогаем
    }
    // --- защищённые термины (имена, адреса), в том числе кириллица без учёта регистра ---
    {
        auto terms = PiiMasker::parseTerms({"PERSON:Иван Петров", "ул. Ленина 5", "ab"});   // "ab" слишком короткий
        CHECK(terms.size() == 2 && terms[0].type == "PERSON" && terms[1].type == "TERM");
        MaskSession s(terms);
        const std::string in = "Клиент ИВАН ПЕТРОВ проживает: Ул. Ленина 5. Письмо для Иван Петров.";
        const std::string m = s.mask(in);
        CHECK(!has(m, "ПЕТРОВ") && !has(m, "Петров") && !has(m, "Ленина"));
        CHECK(has(m, "[PERSON_1]") && has(m, "[TERM_1]"));
        CHECK(!s.leaks(m));
    }
    // --- JSON-безопасная подстановка ---
    {
        auto terms = PiiMasker::parseTerms({"PERSON:O\"Brien & Sons"});
        MaskSession s(terms);
        const std::string m = s.mask("Supplier: O\"Brien & Sons");
        CHECK(m == "Supplier: [PERSON_1]");
        CHECK(s.unmask("{\"company\":\"[PERSON_1]\"}", true) == "{\"company\":\"O\\\"Brien & Sons\"}");
    }
    // --- перекрытия: 20 цифр = ACCOUNT, а не кусок карты/NUM ---
    {
        MaskSession s;
        const std::string m = s.mask("account 40817810099910004312 end");
        CHECK(m == "account [ACCOUNT_1] end");
    }
    // --- заглушки сами не считаются утечкой и не маскируются повторно ---
    {
        MaskSession s;
        const std::string m1 = s.mask("x@y.com");
        CHECK(s.mask(m1) == m1);
    }
    std::cout << "pii OK\n";
    return 0;
}
