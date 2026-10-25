#pragma once
// Память о правках пользователя («учится на ваших исправлениях»).
//
//   Тип:       «для компании X система решила invoice, а вы поправили на contract» → в следующий раз для X такой же
//              промах исправляется сам. Правило привязано к паре (компания, ошибочный тип): остальные документы
//              этой компании не страдают (поставщик может присылать и счета, и договоры).
//   Коллекция: «договоры компании X вы кладёте в Legal» → новые договоры X попадают туда же.
//
// Всё хранится локально в library.db и не покидает машину; ни в какие промпты не подмешивается.
#include "Db.hpp"
#include <atomic>
#include <string>
#include <nlohmann/json.hpp>

namespace mary {

class Corrections {
public:
    explicit Corrections(Db& db);   // создаёт таблицы, если их ещё нет

    void learnType(long long companyId, const std::string& guessedType, const std::string& correctType);
    void learnCollection(long long companyId, const std::string& docType, long long collectionId);

    // Применение при классификации. "" / 0 — правила нет.
    std::string typeFor(long long companyId, const std::string& guessedType);
    long long   collectionFor(long long companyId, const std::string& docType);

    nlohmann::json stats();   // {"type_rules": N, "collection_rules": M, "applied": K}
    void countApplied();      // вызывается Indexer'ом, когда правило сработало

private:
    Db& db_;
    std::atomic<long long> applied_{0};   // классификация идёт в несколько потоков
};

} // namespace mary
