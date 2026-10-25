#pragma once
// Клиент OpenAI-совместимого API (Nebius Token Factory → NVIDIA Nemotron). Только libcurl, без SDK.
//
// Особенности reasoning-моделей Nemotron, которые здесь учтены:
//   * часть бюджета max_tokens уходит на «размышления» → к бюджету ответа добавляется reasoningHeadroom;
//   * если ответ оборвался по длине (finish_reason=length) и content пуст — бюджет удваивается и запрос повторяется;
//   * content может прийти как строка, массив частей или null; <think>…</think> вырезается;
//   * 429 / 5xx / сетевые сбои — повтор с экспоненциальной паузой;
//   * если провайдер не поддерживает response_format=json_object (HTTP 400) — повтор без него.
// Ключ API никогда не попадает в логи и тексты ошибок.
#include "LlmClient.hpp"
#include <string>

namespace mary {

class RemoteLlm : public LlmClient {
public:
    struct Options {
        std::string baseUrl = "https://api.tokenfactory.nebius.com/v1/";
        std::string apiKey;
        std::string modelNano, modelSuper, modelUltra;
        int contextTokens = 32000;
        int timeoutSec = 120;
        int maxRetries = 3;
        int reasoningHeadroom = 1536;
        int backoffMs = 500;            // база экспоненциальной паузы (в тестах уменьшают)
        std::string extraBody;          // JSON-объект, подмешивается в тело (model/messages не перезаписываются)
    };

    explicit RemoteLlm(Options o);

    bool        available() const override { return ready_; }
    bool        isRemote() const override { return true; }
    std::string modelId(Tier tier) const override;
    int         contextTokens() const override { return o_.contextTokens; }
    int         countTokens(const std::string& text) const override { return (int)(text.size() / 3) + 1; }
    ChatResult  complete(const ChatRequest& req) override;

    // GET /models: какие ID видит этот ключ (для scripts/check_models.sh и вкладки Settings).
    nlohmann::json listModels();

    const std::string& initError() const { return initError_; }

private:
    Options o_;
    std::string chatUrl_, modelsUrl_, initError_;
    nlohmann::json extra_ = nlohmann::json::object();
    bool ready_ = false;
};

} // namespace mary
