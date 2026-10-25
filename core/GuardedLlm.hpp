#pragma once
// GuardedLlm — единственные ворота к облачной модели. Любой вызов проходит так:
//
//   1. действие "llm_remote" есть в белом списке Guardian (default-deny)
//   2. часовой лимит запросов не исчерпан (предохранитель кредитов)
//   3. system и user маскируются (карты, IBAN, SSN/ИНН/СНИЛС, e-mail, телефоны, длинные числа
//      + «защищённые термины» пользователя: имена, адреса)
//   4. fail-closed: если после маскирования правила всё ещё что-то видят — запрос НЕ отправляется
//   5. запрос уходит к модели уже без PII
//   6. в ответе заглушки заменяются оригиналами (только здесь, в памяти этого процесса)
//   7. в журнал исходящих пишется замаскированное превью, SHA-256 и счётчики
//
// Вызывающий код ничего не знает про маскирование: он просто зовёт complete().
#include "LlmClient.hpp"
#include "OutboundLog.hpp"
#include "PiiMasker.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mary {

class GuardedLlm : public LlmClient {
public:
    struct Options {
        int maxRequestsPerHour = 2000;
        int maxPromptChars = 60000;
        int previewChars = 1500;
        MaskLevel level = MaskLevel::Standard;      // main.cpp ставит из PII_MASK_LEVEL
        std::vector<std::string> secrets;           // значения, которые не должны уйти в промпт НИКОГДА (ключ API приложения)
    };
    using AllowFn = std::function<bool()>;                                              // safety::isAllowedAction("llm_remote")
    using AuditFn = std::function<void(const std::string& action, bool allowed, const std::string& reason)>;  // Guardian::record
    using TermsFn = std::function<std::vector<std::string>()>;                          // защищённые термины (env + профиль)

    GuardedLlm(LlmClient& inner, OutboundLog& log, Options opt, AllowFn allowed, AuditFn audit, TermsFn terms);

    bool        available() const override { return inner_.available(); }
    bool        isRemote() const override { return true; }
    std::string modelId(Tier t) const override { return inner_.modelId(t); }
    // Бюджет промпта не больше лимита шлюза (иначе вызывающий соберёт промпт, который шлюз заблокирует).
    // 3 байта на токен — та же оценка, что в RemoteLlm::countTokens.
    int         contextTokens() const override { return std::min(inner_.contextTokens(), opt_.maxPromptChars / 3); }
    int         countTokens(const std::string& s) const override { return inner_.countTokens(s); }
    ChatResult  complete(const ChatRequest& req) override;

private:
    ChatResult block(const ChatRequest& rq, const std::string& reason, const nlohmann::json& masked = {});
    bool takeBudget();

    LlmClient& inner_;
    OutboundLog& log_;
    Options opt_;
    AllowFn allowed_;
    AuditFn audit_;
    TermsFn terms_;

    std::mutex budgetMu_;
    std::deque<std::chrono::steady_clock::time_point> window_;
};

} // namespace mary
