#include "RemoteLlm.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace mary {
using json = nlohmann::json;

namespace {

struct Http {
    long status = 0;
    std::string body, error;
};

size_t onWrite(char* p, size_t sz, size_t n, void* user) {
    static_cast<std::string*>(user)->append(p, sz * n);
    return sz * n;
}

void curlInitOnce() {
    static const bool once = [] { curl_global_init(CURL_GLOBAL_DEFAULT); return true; }();
    (void)once;
}

Http httpCall(const std::string& url, const std::string& apiKey, const std::string* body, int timeoutSec) {
    curlInitOnce();
    Http out;
    CURL* c = curl_easy_init();
    if (!c) { out.error = "curl_easy_init failed"; return out; }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, ("Authorization: Bearer " + apiKey).c_str());
    hdr = curl_slist_append(hdr, "Accept: application/json");
    if (body) hdr = curl_slist_append(hdr, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)std::max(5, timeoutSec));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MaryGuardian/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);          // ключ не должен уехать на чужой хост по редиректу
    if (body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->size());
    }
    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) out.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return out;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string stripThinkTags(std::string s) {
    size_t a;
    while ((a = s.find("<think>")) != std::string::npos) {
        const size_t b = s.find("</think>", a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    const size_t c = s.find("</think>");          // шаблон мог открыть <think> до начала ответа
    if (c != std::string::npos) s.erase(0, c + 8);
    return s;
}

std::string strField(const json& j, const char* key) {
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    }
    return "";
}

int intField(const json& j, const char* key) {
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && it->is_number_integer()) return it->get<int>();
    }
    return 0;
}

std::string contentText(const json& message) {
    if (!message.is_object()) return "";
    auto it = message.find("content");
    if (it == message.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_array()) {                              // [{type:"text", text:"..."}]
        std::string o;
        for (const auto& part : *it)
            if (part.is_object() && part.contains("text") && part["text"].is_string()) o += part["text"].get<std::string>();
        return o;
    }
    return "";                                         // null: reasoning-модель не успела дойти до ответа
}

std::string apiErrorMessage(const std::string& body) {
    auto j = json::parse(body, nullptr, false);
    std::string m;
    if (!j.is_discarded() && j.is_object() && j.contains("error")) {
        const json& e = j["error"];
        m = e.is_string() ? e.get<std::string>() : strField(e, "message");
    }
    if (m.empty()) m = body;
    if (m.size() > 300) m.resize(300);
    return m;
}

std::string normalizeBase(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string v1 = "/v1";
    if (base.size() < v1.size() || base.compare(base.size() - v1.size(), v1.size(), v1) != 0) base += v1;
    return base;
}

bool isLoopbackHttp(const std::string& url) {
    return url.rfind("http://localhost", 0) == 0 || url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://[::1]", 0) == 0;
}

void sleepBackoff(int baseMs, int failures) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    const int ms = std::min(8000, baseMs * (1 << std::min(failures, 5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(ms + (int)(rng() % (unsigned)(baseMs + 1))));
}

} // namespace

RemoteLlm::RemoteLlm(Options o) : o_(std::move(o)) {
    if (o_.apiKey.empty()) { initError_ = "NEBIUS_API_KEY is not set"; return; }
    const std::string base = normalizeBase(o_.baseUrl);
    // Ключ уходит в заголовке: по открытому HTTP — только на loopback (тесты/локальный прокси).
    if (base.rfind("https://", 0) != 0 && !isLoopbackHttp(base)) {
        initError_ = "refusing to send the API key over a non-HTTPS URL";
        return;
    }
    chatUrl_ = base + "/chat/completions";
    modelsUrl_ = base + "/models";
    if (!o_.extraBody.empty()) {
        auto j = json::parse(o_.extraBody, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { initError_ = "LLM_EXTRA_BODY is not a JSON object"; return; }
        extra_ = std::move(j);
    }
    ready_ = true;
}

std::string RemoteLlm::modelId(Tier tier) const {
    switch (tier) {
        case Tier::Nano:  return o_.modelNano;
        case Tier::Super: return o_.modelSuper.empty() ? o_.modelNano : o_.modelSuper;
        default:          return o_.modelUltra.empty() ? (o_.modelSuper.empty() ? o_.modelNano : o_.modelSuper) : o_.modelUltra;
    }
}

json RemoteLlm::listModels() {
    if (!ready_) return json{{"error", initError_}};
    Http h = httpCall(modelsUrl_, o_.apiKey, nullptr, 30);
    if (h.status != 200) return json{{"error", h.status ? "HTTP " + std::to_string(h.status) : h.error}};
    auto j = json::parse(h.body, nullptr, false);
    return j.is_discarded() ? json{{"error", "bad_json"}} : j;
}

ChatResult RemoteLlm::complete(const ChatRequest& rq) {
    ChatResult out;
    out.model = modelId(rq.tier);
    if (!ready_) { out.error = initError_.empty() ? "remote backend not ready" : initError_; return out; }

    int maxTok = rq.max_tokens + o_.reasoningHeadroom;
    bool jsonMode = rq.json;
    int failures = 0, bumps = 0;
    std::string lastErr;
    const int maxAttempts = o_.maxRetries + 4;

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        json body = {
            {"model", out.model},
            {"messages", json::array({json{{"role", "system"}, {"content", rq.system}},
                                      json{{"role", "user"},   {"content", rq.user}}})},
            {"max_tokens", maxTok},
            {"temperature", rq.temperature},
            {"stream", false}};
        if (jsonMode) body["response_format"] = {{"type", "json_object"}};
        for (auto it = extra_.begin(); it != extra_.end(); ++it)
            if (it.key() != "model" && it.key() != "messages" && it.key() != "stream") body[it.key()] = it.value();

        const std::string payload = body.dump();
        Http h = httpCall(chatUrl_, o_.apiKey, &payload, o_.timeoutSec);

        if (h.status == 0 || h.status == 429 || h.status >= 500) {
            lastErr = h.status ? "HTTP " + std::to_string(h.status) : "network: " + h.error;
            if (++failures > o_.maxRetries) break;
            sleepBackoff(o_.backoffMs, failures);
            continue;
        }
        if (h.status == 400 && jsonMode && h.body.find("response_format") != std::string::npos) {
            jsonMode = false;                          // провайдер не знает json_object — просим «просто JSON» промптом
            continue;
        }
        if (h.status >= 400) { out.error = "HTTP " + std::to_string(h.status) + ": " + apiErrorMessage(h.body); return out; }

        json j = json::parse(h.body, nullptr, false);
        if (j.is_discarded() || !j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
            lastErr = "unexpected response shape";
            if (++failures > o_.maxRetries) break;
            sleepBackoff(o_.backoffMs, failures);
            continue;
        }
        const json& choice = j["choices"][0];
        const std::string finish = strField(choice, "finish_reason");
        if (j.contains("usage")) {
            out.prompt_tokens = intField(j["usage"], "prompt_tokens");
            out.completion_tokens = intField(j["usage"], "completion_tokens");
        }
        const std::string content = trim(stripThinkTags(contentText(choice.value("message", json::object()))));
        if (content.empty()) {
            if (finish == "length" && bumps < 2) { maxTok = std::min(maxTok * 2, 16384); ++bumps; continue; }
            lastErr = "empty content (finish_reason=" + (finish.empty() ? "?" : finish) + "): the model used its whole token budget on reasoning";
            break;
        }
        out.ok = true;
        out.text = content;
        return out;
    }
    out.error = lastErr.empty() ? "request failed" : lastErr;
    return out;
}

} // namespace mary
