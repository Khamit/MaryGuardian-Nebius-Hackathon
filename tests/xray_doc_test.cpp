// Карточка документа → X-ray: счёт с «чужим» IBAN получает красную рамку, прежний счёт — зелёную.
#include "../core/XrayDoc.hpp"
#include "pdf_fixture.hpp"
#include <cstdio>
#include <fstream>
#include <filesystem>

using namespace mary;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
static int count(const json& boxes, const char* kind) { int n = 0; for (const auto& b : boxes) if (b["kind"] == kind) ++n; return n; }

static std::string invoiceText(const std::string& number, const std::string& date, const std::string& iban) {
    return "INVOICE\nFrom: Brightwave Energy GmbH\nInvoice No: " + number + "\nDate: " + date + "\nTotal due: 412.80 EUR\nPay to IBAN " + iban + "\n";
}

int main() {
    namespace fs = std::filesystem;
    const std::string inbox = "/tmp/xray_doc_inbox";
    fs::remove_all(inbox); fs::create_directories(inbox);
    auto write = [&](const std::string& name, const std::vector<std::string>& lines) { std::ofstream(inbox + "/" + name, std::ios::binary) << fixture::makePdf(lines); };
    const std::string A = "DE89 3704 0044 0532 0130 00", B = "DE02 1203 0000 0000 2020 51";
    write("inv1.pdf", {"INVOICE", "From: Brightwave Energy GmbH", "Invoice No: INV-2026-0101", "Date: 2026-07-02", "Total due: 412.80 EUR", "Pay to IBAN " + A});
    write("inv2.pdf", {"INVOICE", "From: Brightwave Energy GmbH", "Invoice No: INV-2026-0106", "Date: 2026-10-12", "Total due: 412.80 EUR", "Pay to IBAN " + B});
    std::ofstream(inbox + "/notes.txt") << "plain text";

    Db db(":memory:");
    Consistency c(db);
    const long long bw = db.companyId("Brightwave Energy GmbH", false);
    auto add = [&](const std::string& name, const std::string& number, const std::string& date) {
        long long id = 0;
        db.exec("INSERT INTO documents(sha256,filename,status,doc_type,doc_date,doc_number,amount_minor,currency,company_id) VALUES(?,?,?,?,?,?,?,?,?)",
                {name + "#", name, "classified", "invoice", date, number, 41280, "EUR", bw}, &id);
        return id;
    };
    const long long d1 = add("inv1.pdf", "INV-2026-0101", "2026-07-02"), d2 = add("inv2.pdf", "INV-2026-0106", "2026-10-12");
    c.recordDocument(d1, invoiceText("INV-2026-0101", "2026-07-02", A));
    c.recordDocument(d2, invoiceText("INV-2026-0106", "2026-10-12", B));
    c.recompute();

    auto row = [&](long long id) {
        return db.query("SELECT d.id, d.filename, d.doc_type, d.doc_date, d.doc_number, d.amount_minor, d.currency, d.expires, d.summary, c.display AS company "
                        "FROM documents d LEFT JOIN companies c ON c.id=d.company_id WHERE d.id=?", {id})[0];
    };
    const json r2 = xray::viewForDocument(row(d2), inbox, 0, &c, {}, MaskLevel::Strict);
    CHECK(r2.value("ok", false));
    CHECK(count(r2["boxes"], "risk") == 1);
    CHECK(r2["flags"].size() >= 1 && r2["flags"][0]["code"] == "PAYEE_CHANGED");
    CHECK(r2["doc"]["filename"] == "inv2.pdf");
    CHECK(count(r2["boxes"], "field") >= 5);                   // компания, номер, дата, сумма, счёт

    const json r1 = xray::viewForDocument(row(d1), inbox, 0, &c, {}, MaskLevel::Strict);
    CHECK(r1.value("ok", false) && count(r1["boxes"], "risk") == 0 && r1["flags"].empty());

    // без Consistency просмотр тоже работает
    CHECK(xray::viewForDocument(row(d1), inbox, 0, nullptr, {}, MaskLevel::Standard).value("ok", false));
    // не-PDF, отсутствующий файл, попытка выйти из папки
    json bad = row(d1); bad["filename"] = "notes.txt";
    CHECK(xray::viewForDocument(bad, inbox, 0, &c, {}, MaskLevel::Strict).contains("error"));
    bad["filename"] = "../../etc/passwd.pdf";
    CHECK(xray::viewForDocument(bad, inbox, 0, &c, {}, MaskLevel::Strict).contains("error"));
    bad["filename"] = "missing.pdf";
    CHECK(xray::viewForDocument(bad, inbox, 0, &c, {}, MaskLevel::Strict).contains("error"));

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("xray_doc_test: all checks passed\n");
    return 0;
}
