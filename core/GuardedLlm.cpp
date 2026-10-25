#include "GuardedLlm.hpp"
#include "PiiMasker.hpp"
#include "Platform.hpp"
#include "Sha256.hpp"
#include <ctime>

namespace mary {
using json = nlohmann::json;

namespace {

std::string nowStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm = mary::plat::localTm(t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

// Обрезка по границе UTF-8, чтобы превью не заканчивалось «половиной» кириллической буквы.
std::string cutUtf8(std::string s, size_t n) {
    if (s.size() <= n) return s;
    s.resize(n);
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0xC0) s.pop_back();
    return s + "…";
}

json countsJson(const std::map<std::string, int>& c) {
    json j = json::object();
    for (const auto& kv : c) j[kv.first] = kv.second;
    return j;
}

} // namespace

GuardedLlm::GuardedLlm(LlmClient& inner, OutboundLog& log, Options opt, AllowFn allowed, AuditFn audit, TermsFn terms)
    : inner_(inner), log_(log), opt_(opt), allowed_(std::move(allowed)), audit_(std::move(audit)), terms_(std::move(terms)) {}

bool GuardedLlm::takeBudget() {
    std::lock_guard<std::mutex> lk(budgetMu_);
    const auto now = std::chrono::steady_clock::now();
    while (!window_.empty() && now - window_.front() > std::chrono::hours(1)) window_.pop_front();
    if ((int)window_.size() >= opt_.maxRequestsPerHour) return false;
    window_.push_back(now);
    return true;
}

ChatResult GuardedLlm::block(const ChatRequest& rq, const std::string& reason, const json& masked) {
    ChatResult r;
    r.model = inner_.modelId(rq.tier);
    r.error = "blocked by Guardian: " + reason;
    log_.add(json{{"ts", nowStamp()}, {"purpose", rq.purpose}, {"tier", tierName(rq.tier)}, {"model", r.model},
                  {"status", "blocked"}, {"reason", reason}, {"masked", masked.is_object() ? masked : json::object()}});
    if (audit_) audit_("llm_remote:" + rq.purpose, false, reason);
    return r;
}

ChatResult GuardedLlm::complete(const ChatRequest& rq) {
    if (!allowed_ || !allowed_()) return block(rq, "action 'llm_remote' is not in the allow-list");
    if (!takeBudget()) return block(rq, "hourly request budget exhausted (LLM_MAX_REQUESTS_PER_HOUR)");

    std::vector<std::string> rawTerms = terms_ ? terms_() : std::vector<std::string>{};
    for (const auto& s : opt_.secrets) if (s.size() >= 8) rawTerms.push_back("SECRET:" + s);
    MaskSession session(PiiMasker::parseTerms(rawTerms), opt_.level);
    
    ChatRequest masked = rq;
    masked.system = session.mask(rq.system);
    masked.user = session.mask(rq.user);
    const json counts = countsJson(session.counts());

    const size_t bytes = masked.system.size() + masked.user.size();
    if ((int)bytes > opt_.maxPromptChars) return block(rq, "prompt too large after masking", counts);
    if (session.leaks(masked.system) || session.leaks(masked.user))
        return block(rq, "residual PII after masking; request was not sent", counts);

    const std::string digest = Sha256::of(masked.system + "\n" + masked.user);
    const auto t0 = std::chrono::steady_clock::now();
    ChatResult res = inner_.complete(masked);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    log_.add(json{{"ts", nowStamp()}, {"purpose", rq.purpose}, {"tier", tierName(rq.tier)}, {"model", res.model},
                  {"status", res.ok ? "sent" : "error"}, {"reason", res.ok ? "" : res.error},
                  {"bytes", bytes}, {"masked", counts}, {"masked_total", session.total()},
                  {"prompt_tokens", res.prompt_tokens}, {"completion_tokens", res.completion_tokens},
                  {"latency_ms", ms}, {"sha256", digest},
                  {"preview", cutUtf8(masked.user, (size_t)opt_.previewChars)}});

    if (res.ok) res.text = session.unmask(res.text, rq.json);   // оригиналы возвращаются только здесь и только в память
    return res;
}

} // namespace mary
