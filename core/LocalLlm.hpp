#pragma once
// Локальная модель (Qwen3.5-2B GGUF через llama.cpp) за тем же интерфейсом LlmClient.
// Компилируется только при -DMG_LOCAL_LLM=ON. Данные не покидают машину, поэтому маскирование здесь не нужно.
#include "LlmClient.hpp"
#include "ModelRuntime.hpp"

namespace mary {

class LocalLlm : public LlmClient {
public:
    explicit LocalLlm(ModelRuntime& rt) : rt_(rt) {}

    bool        available() const override { return rt_.isLoaded(); }
    bool        isRemote() const override { return false; }
    std::string modelId(Tier) const override { return rt_.getModelId(); }   // путь к gguf — часть ключа кеша
    int         contextTokens() const override { return rt_.getContextSize(); }
    int         countTokens(const std::string& text) const override { return rt_.countTokens(text); }
    ChatResult  complete(const ChatRequest& req) override;

private:
    ModelRuntime& rt_;
};

} // namespace mary
