// Сценарий BEC: договор (IBAN A) + прошлый счёт (IBAN A) + новый счёт (IBAN B) + письмо «реквизиты обновлены».
#include "../core/Consistency.hpp"
#include <cstdio>
#include <string>

using namespace mary;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static const char* kIbanA = "DE89 3704 0044 0532 0130 00";
static const char* kIbanB = "DE02 1203 0000 0000 2020 51";

static long long addDoc(Db& db, const std::string& name, const std::string& type, const std::string& date, long long company,
                        long long amount = -1, const std::string& number = "", const std::string& cur = "EUR",
                        const std::string& expires = "", const std::string& basis = "") {
    long long id = 0;
    db.exec("INSERT INTO documents(sha256,filename,status,doc_type,doc_date,doc_number,amount_minor,currency,company_id,expires,expires_basis) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            {name + "#sha", name, "classified", type, date.empty() ? json(nullptr) : json(date), number.empty() ? json(nullptr) : json(number),
             amount < 0 ? json(nullptr) : json(amount), cur, company ? json(company) : json(nullptr),
             expires.empty() ? json(nullptr) : json(expires), basis.empty() ? json(nullptr) : json(basis)}, &id);
    return id;
}
static bool hasFlag(Consistency& c, long long id, const char* code, int sev = -1) {
    for (const auto& f : c.flags(id)) if (f["code"] == code && (sev < 0 || f["severity"] == sev)) return true;
    return false;
}

int main() {
    using namespace consistency;
    // ---------- чистые функции ----------
    CHECK(ibanChecksumOk("DE89370400440532013000"));
    CHECK(ibanChecksumOk(normalizeIban(kIbanB)));
    CHECK(ibanChecksumOk("KZ86125KZT5004100100"));
    CHECK(!ibanChecksumOk("DE89370400440532013001"));
    {   // жадный шаблон «съел» слово IBAN после номера: хвост отрезается
        const auto f = findIbans("Pay to DE89 3704 0044 0532 0130 00 IBAN thanks");
        CHECK(f.size() == 1 && f[0].valid && f[0].normalized == "DE89370400440532013000");
    }
    CHECK(findIbans("Reference ABCD 1234 5678 9012 3456").empty());                        // обычный код: не IBAN
    { const auto f = findIbans("Please pay to IBAN DE89 3704 0044 0532 0130 01"); CHECK(f.size() == 1 && !f[0].valid); }   // опечатка в контексте IBAN
    CHECK(announcesBankChange("Please note: our bank details have changed with effect from 1 October."));
    CHECK(announcesBankChange("We have updated our bank account. New IBAN: DE02 1203 0000 0000 2020 51"));
    CHECK(announcesBankChange("Уважаемые партнёры, наши банковские реквизиты изменились."));
    CHECK(announcesBankChange("Новые реквизиты для оплаты указаны ниже"));
    CHECK(!announcesBankChange("Payment details: bank transfer within 30 days. New customers get 10% off."));
    CHECK(std::string(roleOf("invoice")) == "payee" && std::string(roleOf("statement")) == "own" && std::string(roleOf("letter")) == "claimed");

    // ---------- сценарий из четырёх файлов ----------
    Db db(":memory:");
    Consistency c(db);
    const long long bw = db.companyId("Brightwave Energy GmbH", false);
    const long long contract = addDoc(db, "service_agreement_brightwave.pdf", "contract", "2026-04-02", bw);
    const long long inv1 = addDoc(db, "INV-2026-0101_brightwave.pdf", "invoice", "2026-07-02", bw, 41280, "INV-2026-0101");
    const long long inv2 = addDoc(db, "INV-2026-0106_brightwave.pdf", "invoice", "2026-10-12", bw, 41280, "INV-2026-0106");
    const long long letter = addDoc(db, "letter_brightwave_bank_update.pdf", "letter", "2026-10-11", bw);
    c.recordDocument(contract, std::string("SERVICE AGREEMENT\nPay to IBAN ") + kIbanA + "\n");
    c.recordDocument(inv1, std::string("INVOICE\nTotal due: 412.80 EUR\nPay to IBAN ") + kIbanA + "\n");
    c.recordDocument(inv2, std::string("INVOICE\nTotal due: 412.80 EUR\nPay to IBAN ") + kIbanB + "\n");
    c.recordDocument(letter, std::string("Dear customer,\nPlease note: our bank details have changed. New IBAN: ") + kIbanB + "\n");
    c.recompute();

    CHECK(hasFlag(c, inv2, "PAYEE_CHANGED", 3));
    CHECK(!hasFlag(c, inv1, "PAYEE_CHANGED"));
    CHECK(!hasFlag(c, contract, "PAYEE_CHANGED"));
    CHECK(hasFlag(c, letter, "CHANGE_NOTICE", 3));
    {
        const auto fl = c.flags(inv2);
        std::string msg; for (const auto& f : fl) if (f["code"] == "PAYEE_CHANGED") msg = f["message"];
        std::printf("--- PAYEE_CHANGED message ---\n%s\n-----------------------------\n", msg.c_str());
        CHECK(msg.find("2051") != std::string::npos);                       // новый счёт назван по последним 4 символам
        CHECK(msg.find("3000") != std::string::npos);                       // эталон: последние 4 символа DE89…3000
        CHECK(msg.find("service_agreement_brightwave.pdf") != std::string::npos);
        CHECK(msg.find("letter_brightwave_bank_update.pdf") != std::string::npos);
        CHECK(msg.find("not independent confirmation") != std::string::npos);
        CHECK(msg.find("DE02") == std::string::npos && msg.find("1203") == std::string::npos);
    }
    CHECK(!c.riskFingerprints(inv2).empty());
    CHECK(c.riskFingerprints(inv1).empty());
    CHECK(c.overview()["high"].get<int>() >= 2);

    // приватность: полный номер счёта нигде в таблицах не хранится
    {
        std::string dump;
        for (const char* t : {"doc_accounts", "doc_flags", "doc_facts", "payee_accounts", "consistency_meta"})
            dump += db.query(std::string("SELECT * FROM ") + t).dump();
        CHECK(dump.find("DE89") == std::string::npos && dump.find("37040044") == std::string::npos);
        CHECK(dump.find("DE02") == std::string::npos && dump.find("12030000") == std::string::npos);
    }

    // соль живёт в БД: второй экземпляр на той же базе даёт тот же отпечаток (иначе после перезапуска эталон «потеряется»)
    { Consistency again(db); CHECK(again.fingerprint("DE89370400440532013000") == c.fingerprint("DE89370400440532013000")); }

    // идемпотентность: повторный пересчёт даёт тот же результат
    const int n1 = c.recompute(), n2 = c.recompute();
    CHECK(n1 == n2);

    // подтверждение по независимому каналу: флаг превращается в info, письмо перестаёт быть «high»
    {
        const auto r = c.confirmPayee(inv2);
        CHECK(r.contains("confirmed"));
        CHECK(!hasFlag(c, inv2, "PAYEE_CHANGED"));
        CHECK(hasFlag(c, inv2, "PAYEE_CONFIRMED", 1));
        CHECK(hasFlag(c, letter, "CHANGE_NOTICE", 1));
        CHECK(!hasFlag(c, inv1, "PAYEE_CHANGED"));                          // старые счета с прежним реквизитом не пострадали
    }
    CHECK(c.confirmPayee(99999).contains("error"));

    // ---------- случай «поддельный IBAN с первого письма»: без эталона сравнивать не с чем ----------
    {
        Db db2(":memory:"); Consistency c2(db2);
        const long long v = db2.companyId("Solo Vendor Ltd", false);
        const long long a = addDoc(db2, "inv_a.pdf", "invoice", "2026-09-01", v, 10000, "A-1");
        c2.recordDocument(a, std::string("Invoice\nIBAN ") + kIbanB + "\n");
        c2.recompute();
        CHECK(!hasFlag(c2, a, "PAYEE_CHANGED"));                            // честно: первый документ становится эталоном
    }

    // ---------- прочие проверки согласованности ----------
    {
        Db db3(":memory:"); Consistency c3(db3);
        const long long v = db3.companyId("Orchard Supplies Ltd", false);
        const long long i1 = addDoc(db3, "o1.pdf", "invoice", "2026-01-10", v, 100000, "O-1");
        addDoc(db3, "o2.pdf", "invoice", "2026-02-10", v, 110000, "O-2");
        addDoc(db3, "o3.pdf", "invoice", "2026-03-10", v, 105000, "O-3");
        const long long big = addDoc(db3, "o4.pdf", "invoice", "2026-04-10", v, 999900, "O-4");
        const long long dup = addDoc(db3, "o5.pdf", "invoice", "2026-05-10", v, 123400, "O-1");          // номер O-1 уже был, сумма другая
        const long long dates = addDoc(db3, "o6.pdf", "invoice", "2026-06-10", v, 100000, "O-6", "EUR", "2026-05-01", "due");
        const long long bad = addDoc(db3, "o7.pdf", "invoice", "2026-07-10", v, 100000, "O-7");
        c3.recordDocument(bad, "Pay to IBAN DE89 3704 0044 0532 0130 01");
        c3.recompute();
        CHECK(hasFlag(c3, big, "AMOUNT_OUTLIER", 2));
        CHECK(!hasFlag(c3, i1, "AMOUNT_OUTLIER"));
        CHECK(hasFlag(c3, dup, "NUMBER_REUSED", 3));
        CHECK(hasFlag(c3, dates, "DATES", 2));
        CHECK(hasFlag(c3, bad, "IBAN_INVALID", 2));
        std::printf("--- AMOUNT_OUTLIER ---\n%s\n", c3.flags(big)[0]["message"].get<std::string>().c_str());
    }

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("consistency_test: all checks passed\n");
    return 0;
}
