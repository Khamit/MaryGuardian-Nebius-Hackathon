#pragma once
#include <string>
#include <vector>

namespace mary {

// Verifier — проверяет документы перед подачей в LLM.
// Защита от prompt injection (эвристика, не гарантия).
// PII-детекция живёт в Guardian через Safety::pii::contains() и в PiiMasker.

class Verifier {
public:
    struct Hit {
        std::string rule;
        size_t begin = 0, end = 0;   // позиции в исходном тексте (для подсветки на странице)
    };

    // Имена сработавших правил (пусто = ничего не найдено). Невидимые символы (zero-width) вырезаются перед проверкой.
    std::vector<std::string> scan(const std::string& content);

    // Те же правила с позициями. Работает по сырому тексту: если инъекция спрятана за zero-width символами,
    // scan() её увидит, а scanSpans() вернёт пустой список (подсветить нечего).
    std::vector<Hit> scanSpans(const std::string& content);

    // Совместимость: true, если сработало хотя бы одно правило
    bool checkPromptInjection(const std::string& content) { return !scan(content).empty(); }
};

} // namespace mary
