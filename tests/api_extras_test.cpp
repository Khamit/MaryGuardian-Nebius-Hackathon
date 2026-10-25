// ApiExtras поверх настоящих RemoteLlm/GuardedLlm и mock-сервера. MOCK_URL, MOCK_LOG обязательны.
#include "server/ApiExtras.hpp"
#include "core/GuardedLlm.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace mary;
using json = nlohmann::json;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

static size_t mockLines(const char* p) {
    std::ifstream f(p); std::string l; size_t n = 0;
    while (std::getline(f, l)) ++n;
    return n;
}

int main() {
    const char* url = std::getenv("MOCK_URL");
    const char* mlog = std::getenv("MOCK_LOG");
    if (!url || !mlog) { std::cerr << "set MOCK_URL and MOCK_LOG\n"; return 2; }
    std::remove("/tmp/outbound_api.jsonl");

    Services svc;
    svc.settings.baseUrl = url;
    svc.settings.apiKey = "secret-key-XYZ";
    svc.workspace = "/tmp/ws_api";

    RemoteLlm::Options ro;
    ro.baseUrl = url; ro.apiKey = svc.settings.apiKey; ro.modelNano = svc.settings.modelNano;
    ro.modelSuper = svc.settings.modelSuper; ro.modelUltra = svc.settings.modelUltra; ro.backoffMs = 5;
    RemoteLlm raw(ro);
    OutboundLog olog("/tmp/outbound_api.jsonl");
    svc.protectedTerms = [] { return std::vector<std::string>{"PERSON:Иван Петров"}; };
    GuardedLlm guarded(raw, olog, GuardedLlm::Options{}, [] { return true; }, nullptr, svc.protectedTerms);
    LlmSwitch sw(&guarded, "remote");
    Downloader dl;
    svc.llm = &sw; svc.remote = &guarded; svc.rawRemote = &raw; svc.outbound = &olog; svc.downloader = &dl;

    ApiExtras api(svc);
    json r;

    // backend: ключ наружу не уходит никогда
    CHECK(api.handle("backend", {}, r));
    CHECK(r["backend"] == "remote" && r["remote"]["configured"] == true && r["local"]["compiled"] == false);
    CHECK(r["remote"]["host"].get<std::string>().find("127.0.0.1") == 0);
    CHECK(r.dump().find("secret-key-XYZ") == std::string::npos);

    // mask_preview работает офлайн: ни одного нового запроса к «провайдеру»
    const size_t before = mockLines(mlog);
    CHECK(api.handle("mask_preview", {{"text", "Иван Петров, IBAN DE89 3704 0044 0532 0130 00, a@b.com"}}, r));
    CHECK(r["masked"] == "[PERSON_1], IBAN [IBAN_1], [EMAIL_1]" && r["total"] == 3);
    CHECK(mockLines(mlog) == before);

    // проверка идентификаторов моделей по каталогу провайдера
    CHECK(api.handle("models_check", {}, r));
    CHECK(r["all_available"] == true && r["models"]["ultra"]["available"] == true);
    svc.settings.modelSuper = "nvidia/no-such-model";
    CHECK(api.handle("models_check", {}, r) && r["all_available"] == false && r["models"]["super"]["available"] == false);
    svc.settings.modelSuper = "nvidia/nemotron-3-super-120b-a12b";

    // переключение бэкенда
    CHECK(api.handle("backend_set", {{"backend", "local"}}, r) && r.contains("error"));       // локальной модели нет
    CHECK(api.handle("backend_set", {{"backend", "off"}}, r) && r["backend"] == "off");
    ChatRequest rq; rq.purpose = "ask"; rq.system = "s"; rq.user = "ECHO hi";
    CHECK(!sw.complete(rq).ok);                                                               // «off»: вызовы не уходят никуда
    CHECK(api.handle("backend_set", {{"backend", "remote"}}, r) && r["backend"] == "remote");
    CHECK(sw.complete(rq).ok);
    CHECK(api.handle("backend_set", {{"backend", "bogus"}}, r) && r.contains("error"));

    // журнал исходящих виден через API
    CHECK(api.handle("outbound", {}, r) && r["totals"]["requests"] == 1 && r["recent"].size() == 1);

    // загрузка моделей: нет URL / запрещённый хост / неизвестный слот / нет локальной сборки
    CHECK(api.handle("model_download", {{"what", "llm"}}, r) && r["error"].get<std::string>().find("LOCAL_MODEL_URL") != std::string::npos);
    svc.settings.localModelUrl = "https://evil.example/m.gguf";
    CHECK(api.handle("model_download", {{"what", "llm"}}, r) && r["error"].get<std::string>().find("not allowed") != std::string::npos);
    CHECK(api.handle("model_download", {{"what", "nope"}}, r) && r.contains("error"));
    CHECK(api.handle("model_load", {}, r) && r["error"].get<std::string>().find("MG_LOCAL_LLM") != std::string::npos);

    // демо-режим блокирует всё, что меняет состояние
    svc.settings.demoMode = true;
    for (const char* op : {"backend_set", "model_download", "model_load", "model_cancel"}) {
        CHECK(api.handle(op, {{"backend", "off"}, {"what", "llm"}}, r) && r["error"] == "disabled in demo mode");
    }
    CHECK(api.handle("backend", {}, r) && r["demo_mode"] == true && r["backend"] == "remote");

    CHECK(!api.handle("no_such_op", {}, r));
    std::cout << "api extras OK\n";
    return 0;
}
