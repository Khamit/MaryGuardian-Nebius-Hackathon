#include "Verifier.hpp"
#include <regex>

namespace mary {

namespace {

std::string stripInvisible(std::string s) {
    for (const char* seq : {"\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", "\xE2\x81\xA0", "\xEF\xBB\xBF"}) {
        size_t p;
        while ((p = s.find(seq)) != std::string::npos) s.erase(p, 3);
    }
    return s;
}

const std::vector<std::pair<std::string, std::regex>>& rules() {
    static const std::vector<std::pair<std::string, std::regex>> r = {
        {"ignore_previous_en", std::regex(R"((ignore|disregard|forget)\s+(all\s+|everything\s+)?(the\s+)?(previous|prior|above|earlier)\s+(instructions|prompts|rules|messages))", std::regex::icase)},
        {"system_marker",      std::regex(R"(system\s*(notice|override|prompt|message)\s*:)", std::regex::icase)},
        {"role_switch_en",     std::regex(R"(you\s+are\s+now\s+(in\s+)?(an?\s+)?(admin|developer|root|dan|unrestricted)\b)", std::regex::icase)},
        {"new_instructions",   std::regex(R"((new|updated|real)\s+instructions?\s*:)", std::regex::icase)},
        {"reveal_prompt",      std::regex(R"(reveal\s+(the\s+)?(system|hidden|secret)\s+(prompt|instructions))", std::regex::icase)},
        {"exfiltrate_en",      std::regex(R"((upload|send|transmit|exfiltrate|forward)\s+(all|full|every|the)\s+(data|ssn|documents|files|records|keys?))", std::regex::icase)},
        {"hide_from_user_en",  std::regex(R"(do\s*not\s*(tell|reveal|mention|inform)\s+(the\s+)?(user|human|owner))", std::regex::icase)},
        // Кириллица: регистры перечисляем вручную
        {"ignore_previous_ru", std::regex(R"((игнорируй|Игнорируй|ИГНОРИРУЙ|забудь|Забудь)[^\n]{0,80}(инструкци|указани))")},
        {"hide_from_user_ru",  std::regex(R"((не\s+сообщай|не\s+говори|не\s+показывай)[^\n]{0,20}(пользовател))")},
        {"chatml_tokens",      std::regex(R"(<\|im_(start|end)\|>)")},
    };
    return r;
}

} // namespace

std::vector<std::string> Verifier::scan(const std::string& raw) {
    const std::string s = stripInvisible(raw);
    std::vector<std::string> hits;
    for (const auto& [name, re] : rules()) if (std::regex_search(s, re)) hits.push_back(name);
    return hits;
}

std::vector<Verifier::Hit> Verifier::scanSpans(const std::string& raw) {
    std::vector<Hit> out;
    for (const auto& [name, re] : rules()) {
        for (auto it = std::sregex_iterator(raw.begin(), raw.end(), re); it != std::sregex_iterator(); ++it) {
            const size_t b = (size_t)it->position();
            out.push_back({name, b, b + (size_t)it->length()});
        }
    }
    return out;
}

} // namespace mary
