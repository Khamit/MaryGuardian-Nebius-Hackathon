#pragma once
// Consistency & Fraud Detection: проверки связей между документами. Всё считается ЛОКАЛЬНО, детерминированно и без модели:
// номер банковского счёта в облако не уходит (модель видит только [IBAN_1]), а в базе хранится лишь отпечаток
// (SHA-256 с солью установки) и последние 4 символа.
//
// Что проверяется (код флага → серьёзность 1 info / 2 warn / 3 high):
//   PAYEE_CHANGED     3  реквизиты в счёте/договоре не совпадают ни с подтверждённым вами счётом, ни с первым наблюдавшимся
//   PAYEE_EXTRA       2  среди реквизитов есть дополнительный, неизвестный счёт
//   CHANGE_NOTICE     2/3 документ сообщает «реквизиты изменились»; 3, если смена не подтверждена независимо
//   PAYEE_CONFIRMED   1  счёт совпал с тем, что вы подтвердили по независимому каналу
//   IBAN_INVALID      2  контрольная сумма IBAN (mod 97) не сходится: опечатка или подделка
//   NUMBER_REUSED     3  тот же номер счёта у того же поставщика, но другая сумма или другие реквизиты
//   AMOUNT_OUTLIER    2  сумма в 3+ раза выше медианы прошлых счетов этого поставщика
//   DATES             2  срок раньше даты документа или документ из будущего
//   INJECTION         3  (ставится при классификации) в тексте инструкции для ИИ; модели документ не показывался
//
// Эталон реквизитов: (1) счета, которые вы подтвердили кнопкой «Confirmed by phone», (2) иначе первый документ
// поставщика («доверие при первом использовании»). Если с самого первого документа реквизиты поддельные, сравнением это
// не поймать: нужен ваш подтверждённый реестр. Письмо с «мы сменили реквизиты» из того же канала подтверждением НЕ считается.
#include "Db.hpp"
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mary {

namespace consistency {

struct IbanFound { std::string normalized; bool valid = false; };

std::string normalizeIban(const std::string& s);               // без пробелов, верхний регистр
bool ibanChecksumOk(const std::string& normalized);            // ISO 13616, mod 97
std::vector<IbanFound> findIbans(const std::string& text);     // валидные всегда; с неверной суммой — только в контексте «IBAN/account/…»
bool announcesBankChange(const std::string& text, std::string* matched = nullptr);
const char* roleOf(const std::string& docType);                // "payee" | "own" | "claimed"

} // namespace consistency

class Consistency {
public:
    explicit Consistency(Db& db);   // создаёт таблицы, если их ещё нет

    // Вызывается из Indexer::classify для документа, который прошёл проверку на инъекции. Только локальная обработка.
    void recordDocument(long long docId, const std::string& text);
    void addFlag(long long docId, const std::string& code, int severity, const std::string& message,
                 const nlohmann::json& evidence = nlohmann::json());

    // Пересчитывает проверки по всей библиотеке (после скана, после правки компании/типа, после подтверждения).
    int recompute();   // возвращает число флагов

    nlohmann::json flags(long long docId);                    // [{code, severity, message, evidence, source}]
    nlohmann::json overview();                                // {high, warn, info, docs}
    nlohmann::json confirmPayee(long long docId);             // «подтвердил по независимому каналу»
    std::set<std::string> riskFingerprints(long long docId);  // для подсветки на странице (X-ray)
    std::string fingerprint(const std::string& normalizedIban);

private:
    std::string salt();
    Db& db_;
    std::string salt_;
};

} // namespace mary
