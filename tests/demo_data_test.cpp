// Демо-набор: PDF читаются, у «злого» счёта инструкции белым текстом, у Brightwave — два разных IBAN.
#include "../core/DemoData.hpp"
#include "../core/Xray.hpp"
#include <cstdio>
#include <filesystem>

using namespace mary;
using json = nlohmann::json;
static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
static int count(const json& b, const char* k) { int n = 0; for (const auto& x : b) if (x["kind"] == k) ++n; return n; }
static bool label(const json& b, const char* k, const std::string& p) { for (const auto& x : b) if (x["kind"] == k && x["label"].get<std::string>().rfind(p, 0) == 0) return true; return false; }

int main() {
    const std::string dir = "/tmp/demo_inbox_test";
    std::filesystem::remove_all(dir);
    const int n = demo::writeDemoInbox(dir);
    CHECK(n == 24);                                                   // 21 прежних + договор, письмо и новый счёт Brightwave
    CHECK(demo::writeDemoInbox(dir) == 0);                                       // повторный вызов ничего не дублирует
    xray::Options o; o.level = MaskLevel::Strict;

    json evil = xray::buildPageView(dir + "/INV-2026-0999_evil_vendor.pdf", o);
    CHECK(evil.value("ok", false) && evil["hidden_lines"] == 2 && count(evil["boxes"], "inject") >= 2);

    json inv = xray::buildPageView(dir + "/INV-2026-0106_brightwave.pdf", o);
    CHECK(inv.value("ok", false) && label(inv["boxes"], "mask", "[IBAN_") && label(inv["boxes"], "mask", "[COMPANY_") && inv["hidden_lines"] == 0);
    json letter = xray::buildPageView(dir + "/letter_brightwave_bank_update.pdf", o);
    CHECK(letter.value("ok", false) && label(letter["boxes"], "mask", "[IBAN_") && count(letter["boxes"], "inject") == 0);
    json contract = xray::buildPageView(dir + "/service_agreement_brightwave.pdf", o);
    CHECK(contract.value("ok", false) && label(contract["boxes"], "mask", "[IBAN_"));
    // обычный счёт без скрытого текста не получает ложных срабатываний
    json plain = xray::buildPageView(dir + "/INV-2026-0102_lumen_cloud.pdf", o);
    CHECK(plain.value("ok", false) && plain["hidden_lines"] == 0 && count(plain["boxes"], "inject") == 0);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("demo_data_test: all checks passed\n");
    return 0;
}
