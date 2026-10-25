#include "Corrections.hpp"

namespace mary {
using json = nlohmann::json;

Corrections::Corrections(Db& db) : db_(db) {
    // Db::exec выполняет по одному выражению, поэтому таблицы создаются отдельными вызовами.
    // ON DELETE CASCADE обязателен: в Db включён PRAGMA foreign_keys=ON, и без него удаление коллекции
    // или слияние компаний (Library::deleteCollection / renameCompany) упало бы на ссылках отсюда.
    db_.exec("CREATE TABLE IF NOT EXISTS type_corrections("
             "company_id INTEGER NOT NULL REFERENCES companies(id) ON DELETE CASCADE, from_type TEXT NOT NULL, to_type TEXT NOT NULL, "
             "hits INTEGER NOT NULL DEFAULT 1, updated_at TEXT, PRIMARY KEY(company_id, from_type))");
    db_.exec("CREATE TABLE IF NOT EXISTS collection_prefs("
             "company_id INTEGER NOT NULL REFERENCES companies(id) ON DELETE CASCADE, doc_type TEXT NOT NULL, "
             "collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE, "
             "hits INTEGER NOT NULL DEFAULT 1, updated_at TEXT, PRIMARY KEY(company_id, doc_type))");
}

void Corrections::learnType(long long companyId, const std::string& guessed, const std::string& correct) {
    if (!companyId || guessed.empty() || correct.empty() || guessed == correct) return;
    db_.exec("INSERT INTO type_corrections(company_id,from_type,to_type,hits,updated_at) VALUES(?,?,?,1,datetime('now')) "
             "ON CONFLICT(company_id,from_type) DO UPDATE SET to_type=excluded.to_type, hits=hits+1, updated_at=excluded.updated_at",
             {companyId, guessed, correct});
}

void Corrections::learnCollection(long long companyId, const std::string& docType, long long collectionId) {
    if (!companyId || docType.empty() || !collectionId) return;
    db_.exec("INSERT INTO collection_prefs(company_id,doc_type,collection_id,hits,updated_at) VALUES(?,?,?,1,datetime('now')) "
             "ON CONFLICT(company_id,doc_type) DO UPDATE SET collection_id=excluded.collection_id, hits=hits+1, updated_at=excluded.updated_at",
             {companyId, docType, collectionId});
}

std::string Corrections::typeFor(long long companyId, const std::string& guessed) {
    if (!companyId || guessed.empty()) return "";
    auto r = db_.query("SELECT to_type FROM type_corrections WHERE company_id=? AND from_type=?", {companyId, guessed});
    return (!r.empty() && r[0]["to_type"].is_string()) ? r[0]["to_type"].get<std::string>() : "";
}

long long Corrections::collectionFor(long long companyId, const std::string& docType) {
    if (!companyId || docType.empty()) return 0;
    // Коллекцию могли удалить после обучения — тогда правило молча не применяется.
    auto r = db_.query("SELECT p.collection_id AS id FROM collection_prefs p JOIN collections c ON c.id=p.collection_id "
                       "WHERE p.company_id=? AND p.doc_type=?", {companyId, docType});
    return (!r.empty() && r[0]["id"].is_number_integer()) ? r[0]["id"].get<long long>() : 0;
}

void Corrections::countApplied() { ++applied_; }

json Corrections::stats() {
    auto n = [&](const char* sql) -> long long {
        auto r = db_.query(sql);
        return (!r.empty() && r[0]["n"].is_number_integer()) ? r[0]["n"].get<long long>() : 0;
    };
    return json{{"type_rules", n("SELECT COUNT(*) AS n FROM type_corrections")},
                {"collection_rules", n("SELECT COUNT(*) AS n FROM collection_prefs")},
                {"applied", applied_.load()}};
}

} // namespace mary
