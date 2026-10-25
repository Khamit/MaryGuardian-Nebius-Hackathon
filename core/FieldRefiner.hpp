#pragma once
// Эскалация на модель уровня Super: «извлечение полей», когда правила и Nano не справились
// (тип неизвестен, правила и модель спорят, уверенность низкая).
//
// Модель только предлагает значения. Принимает или отклоняет их КОД:
//   * doc_type — только из белого списка типов;
//   * даты — строго YYYY-MM-DD и реально существующая дата;
//   * номер документа и компания — должны дословно встречаться в тексте;
//   * сумма — должна совпасть с одним из чисел, которые реально есть в тексте;
//   * валюта — трёхбуквенный код, который есть в тексте (или символ $ € £).
// Всё, что не прошло проверку, молча отбрасывается — выдуманные поля в базу не попадают.
#include "LlmClient.hpp"
#include <functional>
#include <set>
#include <string>

namespace mary {

struct RefinedFields {
    bool ok = false;                 // true, если принято хотя бы одно поле
    std::string doc_type, doc_date, doc_number, currency, company, expires;
    long long amount_minor = -1;
    int accepted = 0, rejected = 0;
    int prompt_tokens = 0, completion_tokens = 0;
};

class FieldRefiner {
public:
    using DateFn = std::function<std::string(const std::string&)>;   // extract::parseDate: строка → ISO или ""

    FieldRefiner(LlmClient& llm, std::set<std::string> types, DateFn parseDate)
        : llm_(llm), types_(std::move(types)), parseDate_(std::move(parseDate)) {}

    // snippet — исходный (немаскированный) текст; маскирование делает GuardedLlm.
    RefinedFields refine(const std::string& snippet);

private:
    LlmClient& llm_;
    std::set<std::string> types_;
    DateFn parseDate_;
};

} // namespace mary
