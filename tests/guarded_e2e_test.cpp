// E2E: GuardedLlm → RemoteLlm → mock OpenAI API (scripts/mock_openai.py).
// Запуск: MOCK_URL=http://127.0.0.1:8099/v1/ MOCK_LOG=/tmp/mock.jsonl ./guarded_e2e_test
#include "core/GuardedLlm.hpp"
#include "core/RemoteLlm.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace mary;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static size_t lines(const std::string& s) { size_t n = 0; for (char c : s) n += c == '\n'; return n; }

int main() {
    const char* url = std::getenv("MOCK_URL");
    const char* mlog = std::getenv("MOCK_LOG");
    if (!url || !mlog) { std::cerr << "set MOCK_URL and MOCK_LOG\n"; return 2; }
    const std::string outPath = "/tmp/outbound_test.jsonl";
    std::remove(outPath.c_str());

    // Небезопасный URL для ключа должен быть отвергнут.
    {
        RemoteLlm::Options bad;
        bad.baseUrl = "http://example.com/v1/";
        bad.apiKey = "k";
        CHECK(!RemoteLlm(bad).available());
        RemoteLlm::Options nokey;
        CHECK(!RemoteLlm(nokey).available());
    }

    RemoteLlm::Options o;
    o.baseUrl = url;
    o.apiKey = "test-key-123";
    o.modelNano = "nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B";
    o.modelSuper = "nvidia/nemotron-3-super-120b-a12b";
    o.modelUltra = "nvidia/Nemotron-3-Ultra-550b-a55b";
    o.backoffMs = 5;
    o.reasoningHeadroom = 1000;
    RemoteLlm remote(o);
    CHECK(remote.available());
    CHECK(remote.listModels()["data"].size() == 3);

    OutboundLog olog(outPath);
    bool allowed = true;
    GuardedLlm::Options go;
    GuardedLlm llm(remote, olog, go, [&] { return allowed; },
                   [](const std::string&, bool, const std::string&) {},
                   [] { return std::vector<std::string>{"PERSON:Иван Петров"}; });

    const std::vector<std::string> secrets = {"DE89", "3704 0044", "a@b.com", "7707083893", "Иван", "Петров", "4111 1111"};

    // 1) Классификация: ответ приходит с заглушками, вызывающий получает оригиналы.
    {
        ChatRequest rq;
        rq.tier = Tier::Nano; rq.purpose = "classify"; rq.json = true;
        rq.system = "Reply JSON";
        rq.user = "Supplier Иван Петров, IBAN DE89 3704 0044 0532 0130 00, mail a@b.com, ИНН: 7707083893, "
                  "card 4111 1111 1111 1111, total 1,250.00 USD";
        ChatResult r = llm.complete(rq);
        CHECK(r.ok);
        auto j = parseJsonObject(r.text);
        CHECK(j.value("company", "") == "Иван Петров");
        CHECK(j.value("summary", "").find("DE89 3704 0044 0532 0130 00") != std::string::npos);
        CHECK(r.prompt_tokens == 120 && r.completion_tokens == 40);
    }
    // 2) 429 → повтор.   3) reasoning: пустой content + length → бюджет растёт.   4) json_object не поддержан → без него.
    for (const char* mode : {"MODE:429 ok", "MODE:REASONING ok", "MODE:JSONFMT ok"}) {
        ChatRequest rq;
        rq.tier = Tier::Super; rq.purpose = "extract"; rq.json = std::string(mode).find("JSONFMT") != std::string::npos;
        rq.system = "s"; rq.user = std::string(mode) + " mail a@b.com";
        ChatResult r = llm.complete(rq);
        if (!r.ok) std::cerr << mode << " → " << r.error << "\n";
        CHECK(r.ok);
    }
    // 5) Эхо: заглушки возвращаются в оригинал, в том числе в обычном тексте.
    {
        ChatRequest rq;
        rq.tier = Tier::Ultra; rq.purpose = "ask"; rq.system = "s"; rq.user = "ECHO: write to a@b.com about Иван Петров";
        ChatResult r = llm.complete(rq);
        CHECK(r.ok && r.text == "ECHO: write to a@b.com about Иван Петров");
        CHECK(r.model == "nvidia/Nemotron-3-Ultra-550b-a55b");
    }

    // --- что реально увидел «провайдер» ---
    const std::string seen = slurp(mlog);
    for (const auto& s : secrets)
        if (seen.find(s) != std::string::npos) { std::cerr << "LEAK to provider: " << s << "\n"; return 1; }
    CHECK(seen.find("Bearer test-key-123") != std::string::npos);
    CHECK(seen.find("[IBAN_1]") != std::string::npos && seen.find("[PERSON_1]") != std::string::npos);
    CHECK(seen.find("\"nvidia/nemotron-3-super-120b-a12b\"") != std::string::npos);   // tier → модель

    // --- журнал исходящих: превью замаскировано, ключа и оригиналов нет ---
    const std::string ob = slurp(outPath);
    for (const auto& s : secrets)
        if (ob.find(s) != std::string::npos) { std::cerr << "LEAK in outbound log: " << s << "\n"; return 1; }
    CHECK(ob.find("test-key-123") == std::string::npos);
    CHECK(ob.find("\"status\":\"sent\"") != std::string::npos);
    auto t = olog.totals();
    CHECK(t["requests"].get<int>() == 5);
    CHECK(t["masked_values"].get<int>() >= 5);
    CHECK(t["masked_by_type"].contains("IBAN") && t["masked_by_type"].contains("EMAIL"));

    // 6) Белый список: если действие запрещено, запрос не уходит вообще.
    {
        const size_t before = lines(slurp(mlog));
        allowed = false;
        ChatRequest rq; rq.purpose = "classify"; rq.system = "s"; rq.user = "hello";
        ChatResult r = llm.complete(rq);
        CHECK(!r.ok && r.error.find("blocked by Guardian") != std::string::npos);
        CHECK(lines(slurp(mlog)) == before);
        CHECK(olog.totals()["blocked"].get<int>() == 1);
        allowed = true;
    }
    // 7) Часовой лимит.
    {
        GuardedLlm::Options tiny; tiny.maxRequestsPerHour = 1;
        GuardedLlm strict(remote, olog, tiny, [] { return true; }, nullptr, nullptr);
        ChatRequest rq; rq.purpose = "classify"; rq.system = "s"; rq.user = "ECHO 1";
        CHECK(strict.complete(rq).ok);
        CHECK(!strict.complete(rq).ok);
    }
    std::cout << "guarded e2e OK  (provider saw " << lines(seen) << " requests, 0 leaks)\n";
    return 0;
}
