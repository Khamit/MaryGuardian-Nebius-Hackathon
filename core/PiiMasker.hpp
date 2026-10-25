#pragma once
// Обратимое маскирование PII перед отправкой в облако.
//
//   "IBAN DE89 3704 0044 0532 0130 00, ИНН: 7707083893"
//        → "IBAN [IBAN_1], ИНН: [INN_1]"
//
// Соответствие «заглушка → оригинал» живёт только в MaskSession, то есть в памяти процесса на время
// одного запроса. Ни в логи, ни в базу, ни в сеть оно не попадает.
//
// Два уровня (PII_MASK_LEVEL):
//   standard — структурированные идентификаторы (карты, IBAN, SSN/ИНН/БИН/СНИЛС, e-mail, телефоны, длинные числа)
//              + секреты (API-ключи, токены, JWT, приватные ключи, «password: …»).
//   strict   — всё из standard + сущности по контексту: названия компаний (по метке «From:/Поставщик:» и по
//              юридической форме LLC/GmbH/ТОО/ИП…), номера документов и счетов, ФИО по метке, адреса, BIC/SWIFT,
//              длинные «токеноподобные» строки. Суммы и даты остаются: они нужны модели для извлечения полей.
//
// Честные границы: имена и адреса вне меток регулярками не поймать — для них «защищённые термины».
#include <map>
#include <string>
#include <vector>

namespace mary {

enum class MaskLevel { Standard, Strict };

struct PiiHit {
    size_t begin = 0, end = 0;
    std::string type;    // CARD, IBAN, EMAIL, COMPANY, DOCNO, SECRET, ...
    std::string value;   // найденный фрагмент
};

// Пользовательский термин: "PERSON:Иван Петров" или просто "Иван Петров" (тип TERM).
struct PiiTerm {
    std::string type;
    std::string text;
};

// Нижний регистр с сохранением длины в байтах (ASCII + кириллица). Нужен и для поиска по тексту страницы (X-ray).
std::string foldUtf8(std::string s);

class PiiMasker {
public:
    explicit PiiMasker(MaskLevel level = MaskLevel::Standard) : level_(level) {}
    static std::vector<PiiTerm> parseTerms(const std::vector<std::string>& raw);

    // Все непересекающиеся находки, по возрастанию позиции. Частично пересекающиеся спаны СЛИВАЮТСЯ
    // (а не отбрасываются), чтобы «хвост» второй находки не остался открытым текстом.
    std::vector<PiiHit> scan(const std::string& text, const std::vector<PiiTerm>& terms = {}) const;
    MaskLevel level() const { return level_; }

private:
    MaskLevel level_;
};

struct MaskedSpan {
    size_t begin = 0, end = 0;
    std::string type, placeholder;     // placeholder: "[COMPANY_1]"
};

class MaskSession {
public:
    explicit MaskSession(std::vector<PiiTerm> terms = {}, MaskLevel level = MaskLevel::Standard)
        : masker_(level), terms_(std::move(terms)) {}

    // Одно и то же значение внутри сессии всегда получает одну и ту же заглушку.
    std::string mask(const std::string& text);

    // То же самое, но возвращает спаны (для X-ray: что именно было бы скрыто и где). Нумерация заглушек общая с mask().
    std::vector<MaskedSpan> spans(const std::string& text);

    // Подставляет оригиналы обратно. jsonEscape=true — если результат пойдёт в JSON-строку.
    std::string unmask(const std::string& text, bool jsonEscape = false) const;

    // Проверка на утечку: после mask() в тексте не должно остаться ничего, что видят правила.
    bool leaks(const std::string& masked) const;

    const std::map<std::string, int>& counts() const { return counters_; }   // тип → число разных значений
    size_t total() const;
    MaskLevel level() const { return masker_.level(); }

private:
    std::vector<PiiHit> propagate(const std::string& text, std::vector<PiiHit> hits);

    PiiMasker masker_;
    std::vector<PiiTerm> terms_;
    std::map<std::string, std::string> forward_;   // тип|нормализованное значение → заглушка
    std::map<std::string, std::string> reverse_;   // заглушка → оригинал
    std::map<std::string, int> counters_;
    std::map<std::string, std::string> known_;     // значение → тип: повторные упоминания маскируются и без метки
};

std::string jsonEscapeBody(const std::string& s);   // экранирование для вставки внутрь JSON-строки

} // namespace mary
