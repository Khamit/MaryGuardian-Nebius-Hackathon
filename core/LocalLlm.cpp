#include "LocalLlm.hpp"
#include <algorithm>

namespace mary {
namespace {

// Ни документ, ни промпт не должны уметь создать special-токены или закрыть блок данных.
std::string breakSpecial(std::string s) {
    for (size_t p = 0; (p = s.find("<|", p)) != std::string::npos; p += 3) s.insert(p + 1, " ");
    return s;
}

std::string stripThink(std::string s) {
    size_t a;
    while ((a = s.find("<think>")) != std::string::npos) {
        const size_t b = s.find("</think>", a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b + 8 - a);
    }
    const size_t c = s.find("</think>");
    if (c != std::string::npos) s.erase(0, c + 8);
    return s;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

} // namespace

ChatResult LocalLlm::complete(const ChatRequest& rq) {
    ChatResult out;
    out.model = rt_.getModelId();
    if (!rt_.isLoaded()) { out.error = "local model is not loaded"; return out; }

    // Тот же формат, что раньше собирался вручную в Indexer/Library/BookkeeperTask (Qwen, thinking выключен).
    const std::string prompt =
        "<|im_start|>system\n" + breakSpecial(rq.system) + "\n<|im_end|>\n"
        "<|im_start|>user\n" + breakSpecial(rq.user) + "\n<|im_end|>\n"
        "<|im_start|>assistant\n<think>\n\n</think>\n\n";

    GenerationConfig cfg;
    cfg.max_tokens = rq.max_tokens;
    cfg.temperature = rq.json ? std::min(rq.temperature, 0.1f) : rq.temperature;
    out.prompt_tokens = rt_.countTokens(prompt);
    out.text = trim(stripThink(rt_.generateText(prompt, cfg)));
    out.ok = !out.text.empty();
    if (!out.ok) out.error = "the model returned no answer";
    return out;
}

} // namespace mary
