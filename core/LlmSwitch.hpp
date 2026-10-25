#pragma once
// LlmSwitch — то, что видят Indexer/Library/BookkeeperTask. Позволяет переключать бэкенд на лету
// (Nemotron в облаке ↔ локальная Qwen ↔ выключено) без пересоздания остальных объектов.
// NullLlm — «модели нет»: вызывающий код откатывается на правила, как и раньше при незагруженной модели.
#include "LlmClient.hpp"
#include <atomic>
#include <mutex>
#include <string>

namespace mary {

class NullLlm : public LlmClient {
public:
    bool        available() const override { return false; }
    bool        isRemote() const override { return false; }
    std::string modelId(Tier) const override { return "none"; }
    int         contextTokens() const override { return 4096; }
    int         countTokens(const std::string& t) const override { return (int)(t.size() / 3) + 1; }
    ChatResult  complete(const ChatRequest&) override {
        ChatResult r;
        r.error = "no model backend is active";
        return r;
    }
};

class LlmSwitch : public LlmClient {
public:
    explicit LlmSwitch(LlmClient* initial, std::string mode) : cur_(initial), mode_(std::move(mode)) {}

    void set(LlmClient* next, const std::string& mode) {
        std::lock_guard<std::mutex> lk(mu_);
        mode_ = mode;
        cur_.store(next);
    }
    std::string mode() const {
        std::lock_guard<std::mutex> lk(mu_);
        return mode_;
    }

    bool        available() const override { return cur_.load()->available(); }
    bool        isRemote() const override { return cur_.load()->isRemote(); }
    std::string modelId(Tier t) const override { return cur_.load()->modelId(t); }
    int         contextTokens() const override { return cur_.load()->contextTokens(); }
    int         countTokens(const std::string& s) const override { return cur_.load()->countTokens(s); }
    ChatResult  complete(const ChatRequest& r) override { return cur_.load()->complete(r); }

private:
    std::atomic<LlmClient*> cur_;
    mutable std::mutex mu_;
    std::string mode_;
};

} // namespace mary
