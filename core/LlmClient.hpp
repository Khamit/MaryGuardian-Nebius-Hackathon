#pragma once
// Единый интерфейс «языковой модели». Раньше Indexer/Library/BookkeeperTask зависели от ModelRuntime
// (llama.cpp) и собирали ChatML-строки руками. Теперь они формируют ChatRequest (system + user + tier),
// а за интерфейсом может стоять: Nemotron на Nebius Token Factory (через GuardedLlm), локальная Qwen или ничего.
#include <string>
#include <nlohmann/json.hpp>

namespace mary {

// Уровень модели: что дёшево — на маленькую, что трудно — на большую.
enum class Tier { Nano, Super, Ultra };

inline const char* tierName(Tier t) {
    switch (t) { case Tier::Nano: return "nano"; case Tier::Super: return "super"; default: return "ultra"; }
}

struct ChatRequest {
    Tier        tier = Tier::Nano;
    std::string purpose;            // classify | extract | draft | ask — попадает в аудит исходящих запросов
    std::string system;
    std::string user;
    int         max_tokens = 512;   // бюджет ОТВЕТА; запас под reasoning добавляет сам бэкенд
    float       temperature = 0.2f;
    bool        json = false;       // ждём один JSON-объект
};

struct ChatResult {
    bool        ok = false;
    std::string text;               // уже без <think>…</think>
    std::string error;
    std::string model;
    int         prompt_tokens = 0;
    int         completion_tokens = 0;
};

class LlmClient {
public:
    virtual ~LlmClient() = default;
    virtual bool        available() const = 0;
    virtual bool        isRemote() const = 0;
    virtual std::string modelId(Tier tier) const = 0;     // входит в ключ кеша ответов
    virtual int         contextTokens() const = 0;
    virtual int         countTokens(const std::string& text) const = 0;
    virtual ChatResult  complete(const ChatRequest& req) = 0;
};

// Достаёт первый JSON-объект из ответа модели (она любит добавлять пояснения/``` вокруг).
inline nlohmann::json parseJsonObject(const std::string& text) {
    const size_t a = text.find('{');
    const size_t b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return nlohmann::json{{"error", "no_json"}};
    auto j = nlohmann::json::parse(text.substr(a, b - a + 1), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return nlohmann::json{{"error", "json_parse_failed"}};
    return j;
}

} // namespace mary
