#include "Consistency.hpp"
#include "PiiMasker.hpp"
#include "Platform.hpp"
#include "Sha256.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <regex>

namespace mary {
using json = nlohmann::json;

namespace {

std::string sOf(const json& r, const char* k) { return (r.contains(k) && r[k].is_string()) ? r[k].get<std::string>() : std::string(); }
long long iOf(const json& r, const char* k, long long def = 0) { return (r.contains(k) && r[k].is_number_integer()) ? r[k].get<long long>() : def; }

std::string money(long long minor, const std::string& cur) {
    std::string whole = std::to_string(minor / 100), out;
    int cnt = 0;
    for (int i = (int)whole.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), whole[(size_t)i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    char b[8];
    std::snprintf(b, sizeof b, ".%02lld", minor % 100);
    return out + b + (cur.empty() ? "" : " " + cur);
}

std::string last4(const std::string& norm) { return norm.size() >= 4 ? norm.substr(norm.size() - 4) : norm; }

std::string joinStr(const std::vector<std::string>& v, const char* sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += sep; o += v[i]; }
    return o;
}

} // namespace

// ===========================================================================
// consistency:: — чистые функции
// ===========================================================================
namespace consistency {

std::string normalizeIban(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::toupper(c);
    return o;
}

bool ibanChecksumOk(const std::string& n) {
    if (n.size() < 15 || n.size() > 34) return false;
    if (!std::isupper((unsigned char)n[0]) || !std::isupper((unsigned char)n[1]) ||
        !std::isdigit((unsigned char)n[2]) || !std::isdigit((unsigned char)n[3])) return false;
    int rem = 0;
    auto feed = [&](int digit) { rem = (rem * 10 + digit) % 97; };
    for (size_t k = 0; k < n.size(); ++k) {
        const char c = n[(k + 4) % n.size()];                 // первые 4 символа переносятся в конец
        if (std::isdigit((unsigned char)c)) feed(c - '0');
        else if (std::isupper((unsigned char)c)) { const int v = c - 'A' + 10; feed(v / 10); feed(v % 10); }
        else return false;
    }
    return rem == 1;
}

std::vector<IbanFound> findIbans(const std::string& text) {
    static const std::regex re(R"(\b[A-Z]{2}\d{2}(?:[ ]?[A-Z0-9]{2,4}){3,8}\b)");
    static const std::regex ctx(R"(iban|account|acct|pay\s+to|beneficiary|bank|рас|сч|р/с)", std::regex::icase);
    std::vector<IbanFound> out;
    std::set<std::string> seen;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        std::string c = normalizeIban(it->str());
        if (c.size() < 15) continue;
        bool valid = ibanChecksumOk(c);
        // Жадный шаблон мог «съесть» соседнее слово заглавными буквами («… 0130 00 IBAN»): отрезаем хвост, пока сумма не сойдётся.
        if (!valid) {
            for (size_t cut = 1; cut <= 8 && c.size() - cut >= 15; ++cut) {
                const std::string t = c.substr(0, c.size() - cut);
                if (ibanChecksumOk(t)) { c = t; valid = true; break; }
            }
        }
        if (!valid) {
            // Неверная сумма: интересно только если рядом стоит слово «IBAN/account/…», иначе это просто код из документа.
            const size_t pos = (size_t)it->position();
            const std::string before = foldUtf8(text.substr(pos > 40 ? pos - 40 : 0, std::min<size_t>(40, pos)));
            if (!std::regex_search(before, ctx) || c.size() > 34) continue;
        }
        if (seen.insert(c).second) out.push_back({c, valid});
    }
    return out;
}

bool announcesBankChange(const std::string& text, std::string* matched) {
    static const std::vector<std::regex> res = {
        std::regex(R"((bank|banking|payment|account)\s+(details|information|data)[^.\n]{0,50}(changed|updated|have\s+been\s+updated|has\s+been\s+updated))"),
        std::regex(R"((changed|changing|updated|updating)\s+(our\s+)?(bank|banking|payment)\s+(details|account|information))"),
        std::regex(R"(new\s+(bank\s+)?(account|iban)\b)"),
        std::regex(R"((change|update)\s+(of|to)\s+(our\s+)?(bank|banking|payment|account))"),
        std::regex(R"(please\s+(use|update|remit)[^.\n]{0,40}(new|following|updated)\s+(bank|account|iban|payment))"),
        std::regex(R"((we|our\s+bank)\s+(have\s+)?(changed|moved|switched)[^.\n]{0,20}(bank|account))"),
        // русский (текст приводится к нижнему регистру foldUtf8)
        std::regex(R"(реквизит[^.\n]{0,60}(изменил|обновил|сменил|новые))"),
        std::regex(R"((изменени|смен|обновлени)[^.\n]{0,25}реквизит)"),
        std::regex(R"(новые\s+реквизиты)"),
        std::regex(R"(новый\s+(расч(?:е|ё)тный\s+)?сч(?:е|ё)т)"),
    };
    const std::string t = foldUtf8(text);
    for (const auto& re : res) {
        std::smatch m;
        if (std::regex_search(t, m, re)) {
            if (matched) {
                const size_t p = (size_t)m.position();
                // только сама фраза-триггер (длина foldUtf8 == длине исходника в байтах), а не «хвост» с номером счёта
                *matched = text.substr(p, std::min<size_t>(100, (size_t)m.length()));
            }
            return true;
        }
    }
    return false;
}

const char* roleOf(const std::string& t) {
    if (t == "invoice" || t == "quote" || t == "purchase_order" || t == "contract" || t == "utility" || t == "insurance") return "payee";
    if (t == "statement" || t == "payslip" || t == "tax" || t == "identity" || t == "medical") return "own";   // это ваши счета, не получателя
    return "claimed";   // письма и прочее: «заявлено», но эталоном не служит
}

} // namespace consistency

// ===========================================================================
// Consistency
// ===========================================================================
Consistency::Consistency(Db& db) : db_(db) {
    // Db::exec выполняет по одному выражению, поэтому каждая таблица создаётся отдельным вызовом.
    db_.exec("CREATE TABLE IF NOT EXISTS consistency_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    db_.exec("CREATE TABLE IF NOT EXISTS doc_accounts("
             "doc_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE, fp TEXT NOT NULL, last4 TEXT NOT NULL, "
             "valid INTEGER NOT NULL DEFAULT 1, PRIMARY KEY(doc_id, fp))");
    db_.exec("CREATE TABLE IF NOT EXISTS doc_facts("
             "doc_id INTEGER PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE, change_notice INTEGER NOT NULL DEFAULT 0, notice_text TEXT)");
    db_.exec("CREATE TABLE IF NOT EXISTS payee_accounts("
             "company_id INTEGER NOT NULL REFERENCES companies(id) ON DELETE CASCADE, fp TEXT NOT NULL, last4 TEXT NOT NULL, "
             "confirmed_at TEXT, via_doc INTEGER, PRIMARY KEY(company_id, fp))");
    db_.exec("CREATE TABLE IF NOT EXISTS doc_flags("
             "doc_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE, code TEXT NOT NULL, severity INTEGER NOT NULL, "
             "message TEXT NOT NULL, evidence TEXT, created_at TEXT, source TEXT NOT NULL DEFAULT 'consistency', PRIMARY KEY(doc_id, code))");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_flags_sev ON doc_flags(severity, doc_id)");
    // Соль создаётся здесь, пока поток один: пул классификации вызывает fingerprint() параллельно, и два потока
    // иначе сгенерировали бы РАЗНЫЕ соли, а отпечатки одного и того же счёта перестали бы совпадать.
    salt();
}

std::string Consistency::salt() {
    if (!salt_.empty()) return salt_;
    auto r = db_.query("SELECT value FROM consistency_meta WHERE key='fp_salt'");
    if (!r.empty() && r[0]["value"].is_string()) { salt_ = r[0]["value"].get<std::string>(); return salt_; }
    salt_ = plat::randomHex(16);
    db_.exec("INSERT OR REPLACE INTO consistency_meta(key,value) VALUES('fp_salt',?)", {salt_});
    return salt_;
}

std::string Consistency::fingerprint(const std::string& normalizedIban) {
    return Sha256::of(salt() + ":" + normalizedIban);
}

void Consistency::addFlag(long long docId, const std::string& code, int severity, const std::string& message, const json& evidence) {
    db_.exec("INSERT OR REPLACE INTO doc_flags(doc_id,code,severity,message,evidence,created_at,source) "
             "VALUES(?,?,?,?,?,datetime('now'),'classify')",
             {docId, code, severity, message, evidence.is_null() ? json(nullptr) : json(evidence.dump())});
}

void Consistency::recordDocument(long long docId, const std::string& text) {
    const auto ibans = consistency::findIbans(text);
    std::string noticeText;
    const bool notice = consistency::announcesBankChange(text, &noticeText);
    if (notice) noticeText = MaskSession({}, MaskLevel::Strict).mask(noticeText);   // в БД не должно оказаться ни номера счёта, ни имён
    db_.batch([&]() -> bool {
        db_.exec("DELETE FROM doc_accounts WHERE doc_id=?", {docId});
        db_.exec("DELETE FROM doc_facts WHERE doc_id=?", {docId});
        db_.exec("DELETE FROM doc_flags WHERE doc_id=? AND source='classify'", {docId});
        for (const auto& f : ibans)
            db_.exec("INSERT OR IGNORE INTO doc_accounts(doc_id,fp,last4,valid) VALUES(?,?,?,?)",
                     {docId, fingerprint(f.normalized), last4(f.normalized), f.valid ? 1 : 0});
        if (notice) db_.exec("INSERT OR REPLACE INTO doc_facts(doc_id,change_notice,notice_text) VALUES(?,1,?)", {docId, noticeText});
        return true;
    });
}

// ---------------------------------------------------------------------------
// Пересчёт по всей библиотеке
// ---------------------------------------------------------------------------
namespace {

struct Acc { std::string fp, last4; bool valid = true; };
struct Doc {
    long long id = 0, companyId = 0, amount = -1;
    std::string filename, type, date, number, currency, expires, basis, company, role, key;
    std::vector<Acc> acc;
    bool notice = false;
    std::set<std::string> fps() const { std::set<std::string> s; for (const auto& a : acc) s.insert(a.fp); return s; }
};
struct Trust { std::string kind, last4, when, source; };   // kind: confirmed | first

} // namespace

int Consistency::recompute() {
    int made = 0;
    db_.batch([&]() -> bool {
        db_.exec("DELETE FROM doc_flags WHERE source='consistency'");

        std::vector<Doc> docs;
        std::map<long long, size_t> byId;
        for (const auto& r : db_.query(
                 "SELECT d.id, d.filename, d.doc_type, d.doc_date, d.doc_number, d.amount_minor, d.currency, d.expires, d.expires_basis, "
                 "d.company_id, c.display AS company FROM documents d LEFT JOIN companies c ON c.id=d.company_id WHERE d.present=1")) {
            Doc d;
            d.id = iOf(r, "id"); d.filename = sOf(r, "filename"); d.type = sOf(r, "doc_type"); d.date = sOf(r, "doc_date");
            d.number = sOf(r, "doc_number"); d.amount = iOf(r, "amount_minor", -1); d.currency = sOf(r, "currency");
            d.expires = sOf(r, "expires"); d.basis = sOf(r, "expires_basis"); d.companyId = iOf(r, "company_id"); d.company = sOf(r, "company");
            d.role = consistency::roleOf(d.type);
            d.key = (d.date.empty() ? "9999-99-99" : d.date) + "#" + std::string(10 - std::min<size_t>(10, std::to_string(d.id).size()), '0') + std::to_string(d.id);
            byId[d.id] = docs.size();
            docs.push_back(std::move(d));
        }
        for (const auto& r : db_.query("SELECT a.doc_id, a.fp, a.last4, a.valid FROM doc_accounts a JOIN documents d ON d.id=a.doc_id WHERE d.present=1")) {
            auto it = byId.find(iOf(r, "doc_id"));
            if (it != byId.end()) docs[it->second].acc.push_back({sOf(r, "fp"), sOf(r, "last4"), iOf(r, "valid", 1) != 0});
        }
        for (const auto& r : db_.query("SELECT doc_id FROM doc_facts WHERE change_notice=1")) {
            auto it = byId.find(iOf(r, "doc_id"));
            if (it != byId.end()) docs[it->second].notice = true;
        }
        std::map<long long, std::map<std::string, Trust>> trusted;   // компания → fp → источник доверия
        for (const auto& r : db_.query("SELECT company_id, fp, last4, confirmed_at, via_doc FROM payee_accounts")) {
            std::string src;
            auto it = byId.find(iOf(r, "via_doc"));
            if (it != byId.end()) src = docs[it->second].filename;
            trusted[iOf(r, "company_id")][sOf(r, "fp")] = {"confirmed", sOf(r, "last4"), sOf(r, "confirmed_at").substr(0, 10), src};
        }
        const auto tq = db_.query("SELECT date('now') AS today, date('now','+7 days') AS soon");
        const std::string today = tq.empty() ? "" : sOf(tq[0], "today"), soon = tq.empty() ? "" : sOf(tq[0], "soon");

        auto put = [&](const Doc& d, const char* code, int sev, const std::string& msg, const json& ev) {
            if (db_.exec("INSERT OR REPLACE INTO doc_flags(doc_id,code,severity,message,evidence,created_at,source) "
                         "VALUES(?,?,?,?,?,datetime('now'),'consistency')",
                         {d.id, code, sev, msg, ev.is_null() ? json(nullptr) : json(ev.dump())}) >= 0) ++made;
        };

        std::vector<size_t> order(docs.size());
        for (size_t i = 0; i < docs.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return docs[a].key < docs[b].key; });

        // ---------- реквизиты по компаниям ----------
        std::vector<size_t> flaggedChanged;                 // документы с PAYEE_CHANGED
        std::map<long long, std::vector<size_t>> perCompany;
        for (size_t i : order) if (docs[i].companyId) perCompany[docs[i].companyId].push_back(i);

        for (auto& kv : perCompany) {
            const long long cid = kv.first;
            auto& tr = trusted[cid];
            size_t base = (size_t)-1;                       // первый «платёжный» документ с реквизитами: доверие при первом использовании
            for (size_t i : kv.second) if (docs[i].role == "payee" && !docs[i].acc.empty()) { base = i; break; }
            if (base != (size_t)-1)
                for (const auto& a : docs[base].acc) if (!tr.count(a.fp)) tr[a.fp] = {"first", a.last4, docs[base].date, docs[base].filename};

            for (size_t i : kv.second) {
                const Doc& d = docs[i];
                if (d.role != "payee" || d.acc.empty() || i == base) continue;
                std::vector<std::string> newLast4, hit;
                std::set<std::string> newFps;
                bool any = false, matchedConfirmed = false;
                std::string confirmedWhen;
                for (const auto& a : d.acc) {
                    auto t = tr.find(a.fp);
                    if (t != tr.end()) {
                        any = true;
                        if (t->second.kind == "confirmed") { matchedConfirmed = true; confirmedWhen = t->second.when; }
                    } else { newFps.insert(a.fp); newLast4.push_back(a.last4); }
                }
                if (newFps.empty()) {
                    if (matchedConfirmed)
                        put(d, "PAYEE_CONFIRMED", 1, "Bank account matches the one you confirmed on " + confirmedWhen + ".", json());
                    continue;
                }
                json baseline = json::array();
                for (const auto& t : tr) baseline.push_back({{"kind", t.second.kind}, {"last4", t.second.last4}, {"when", t.second.when}, {"doc", t.second.source}});
                std::string known;
                for (const auto& t : tr) {
                    if (!known.empty()) known += "; ";
                    known += "ending " + t.second.last4 + (t.second.kind == "confirmed"
                        ? " (confirmed by you on " + t.second.when + ")"
                        : " (first seen in " + t.second.source + (t.second.when.empty() ? "" : ", " + t.second.when) + "; not independently confirmed)");
                }
                // письма «мы сменили реквизиты» из того же канала
                json notices = json::array();
                std::vector<std::string> noticeFiles;
                const auto mine = d.fps();
                for (size_t j = 0; j < docs.size(); ++j) {
                    if (j == i || !docs[j].notice) continue;
                    bool related = docs[j].companyId == d.companyId;
                    for (const auto& a : docs[j].acc) if (mine.count(a.fp)) related = true;
                    if (related) { notices.push_back({{"id", docs[j].id}, {"filename", docs[j].filename}}); noticeFiles.push_back(docs[j].filename); }
                }
                std::string name = d.company.empty() ? "this vendor" : d.company;
                std::string msg = any
                    ? "This document lists an additional bank account ending " + joinStr(newLast4, ", ") + " that is not on file for " + name + " (" + known + "). "
                    : "Bank account ending " + joinStr(newLast4, ", ") + " does not match the account(s) on file for " + name + ": " + known + ". ";
                if (!noticeFiles.empty())
                    msg += "A notice in " + joinStr(noticeFiles, ", ") + " says the details changed, but it came through the same channel as the invoice, so it is not independent confirmation. ";
                msg += "No independent confirmation is recorded: verify by phone using a number from your own records.";
                json ev = {{"fps", json(std::vector<std::string>(newFps.begin(), newFps.end()))}, {"last4", newLast4}, {"baseline", baseline}, {"notice_docs", notices}};
                put(d, any ? "PAYEE_EXTRA" : "PAYEE_CHANGED", any ? 2 : 3, msg, ev);
                if (!any) flaggedChanged.push_back(i);
            }
        }

        // ---------- уведомления «реквизиты изменились» ----------
        for (size_t i = 0; i < docs.size(); ++i) {
            const Doc& d = docs[i];
            if (!d.notice) continue;
            const auto mine = d.fps();
            bool related = false;
            for (size_t f : flaggedChanged) {
                if (docs[f].companyId == d.companyId && d.companyId) related = true;
                for (const auto& a : docs[f].acc) if (mine.count(a.fp)) related = true;
            }
            bool untrustedClaim = false, confirmedClaim = false;
            const auto& tr = trusted[d.companyId];
            if (d.companyId && !tr.empty())
                for (const auto& a : d.acc) {
                    auto t = tr.find(a.fp);
                    if (t == tr.end()) untrustedClaim = true;
                    else if (t->second.kind == "confirmed") confirmedClaim = true;
                }
            json ev = {{"matched", ""}};
            if (related || untrustedClaim)
                put(d, "CHANGE_NOTICE", 3, "This document says the bank details have changed, and the new account does not match what is on file. "
                    "A notice from the same sender is not independent confirmation: call the vendor on a number from your own records.", ev);
            else if (confirmedClaim && !untrustedClaim)
                put(d, "CHANGE_NOTICE", 1, "This document announces new bank details; the new account was confirmed by you.", ev);
            else
                put(d, "CHANGE_NOTICE", 2, "This document says the bank details have changed. There is no earlier account on file to compare with and no independent confirmation recorded.", ev);
        }

        // ---------- контрольная сумма IBAN ----------
        for (const auto& d : docs) {
            std::vector<std::string> bad;
            for (const auto& a : d.acc) if (!a.valid) bad.push_back(a.last4);
            if (!bad.empty())
                put(d, "IBAN_INVALID", 2, "Account number ending " + joinStr(bad, ", ") + " fails the IBAN checksum: a typo or a forged number.", json());
        }

        // ---------- повторно использованный номер счёта ----------
        std::map<std::string, std::vector<size_t>> byNumber;
        for (size_t i : order)
            if (docs[i].type == "invoice" && docs[i].companyId && !docs[i].number.empty())
                byNumber[std::to_string(docs[i].companyId) + "|" + docs[i].number].push_back(i);
        for (const auto& kv : byNumber) {
            if (kv.second.size() < 2) continue;
            const Doc& first = docs[kv.second.front()];
            for (size_t k = 1; k < kv.second.size(); ++k) {
                const Doc& d = docs[kv.second[k]];
                const bool amt = d.amount >= 0 && first.amount >= 0 && d.amount != first.amount;
                const bool acc = !d.acc.empty() && !first.acc.empty() && d.fps() != first.fps();
                if (!amt && !acc) continue;   // точный дубликат: это уже видно во вкладке Cleanup
                put(d, "NUMBER_REUSED", 3, "Invoice number " + d.number + " was already used by " + first.filename +
                    (amt ? " with a different amount (" + money(first.amount, first.currency) + " vs " + money(d.amount, d.currency) + ")" : "") +
                    (acc ? std::string(amt ? " and" : "") + " with different bank details" : "") + ".",
                    json{{"first_doc", first.id}});
            }
        }

        // ---------- необычная сумма ----------
        for (auto& kv : perCompany) {
            std::map<std::string, std::vector<long long>> prev;   // валюта → прошлые суммы
            for (size_t i : kv.second) {
                const Doc& d = docs[i];
                if (d.type != "invoice" || d.amount < 0) continue;
                auto& p = prev[d.currency];
                if (p.size() >= 3) {
                    std::vector<long long> s = p;
                    std::sort(s.begin(), s.end());
                    const long long med = s[s.size() / 2];
                    if (med > 0 && d.amount > 3 * med) {
                        char ratio[16];
                        std::snprintf(ratio, sizeof ratio, "%.1f", (double)d.amount / (double)med);
                        put(d, "AMOUNT_OUTLIER", 2, "Amount " + money(d.amount, d.currency) + " is " + ratio + "x the median of the " +
                            std::to_string(p.size()) + " earlier invoices from this vendor (" + money(med, d.currency) + ").", json());
                    }
                }
                p.push_back(d.amount);
            }
        }

        // ---------- даты ----------
        for (const auto& d : docs) {
            if (!d.date.empty() && !d.expires.empty() && d.expires < d.date && (d.basis == "due" || d.basis == "net" || d.basis == "term" || d.basis == "expires"))
                put(d, "DATES", 2, "The due/valid-until date (" + d.expires + ") is earlier than the document date (" + d.date + ").", json());
            else if (!soon.empty() && !d.date.empty() && d.date > soon)
                put(d, "DATES", 2, "The document is dated in the future (" + d.date + ", today is " + today + ").", json());
        }
        return true;
    });
    return made;
}

// ---------------------------------------------------------------------------
// Чтение и действия пользователя
// ---------------------------------------------------------------------------
json Consistency::flags(long long docId) {
    json out = json::array();
    for (auto r : db_.query("SELECT code, severity, message, evidence, source FROM doc_flags WHERE doc_id=? ORDER BY severity DESC, code", {docId})) {
        if (r["evidence"].is_string()) r["evidence"] = json::parse(r["evidence"].get<std::string>(), nullptr, false);
        if (r["evidence"].is_discarded()) r["evidence"] = nullptr;
        out.push_back(std::move(r));
    }
    return out;
}

json Consistency::overview() {
    auto r = db_.query("SELECT COALESCE(SUM(f.severity=3),0) AS high, COALESCE(SUM(f.severity=2),0) AS warn, COALESCE(SUM(f.severity=1),0) AS info, "
                       "COUNT(DISTINCT f.doc_id) AS docs FROM doc_flags f JOIN documents d ON d.id=f.doc_id WHERE d.present=1");
    return r.empty() ? json::object() : r[0];
}

std::set<std::string> Consistency::riskFingerprints(long long docId) {
    std::set<std::string> out;
    for (const auto& f : flags(docId)) {
        const std::string code = f.value("code", "");
        if ((code == "PAYEE_CHANGED" || code == "PAYEE_EXTRA") && f.contains("evidence") && f["evidence"].is_object() && f["evidence"].contains("fps"))
            for (const auto& x : f["evidence"]["fps"]) if (x.is_string()) out.insert(x.get<std::string>());
    }
    return out;
}

json Consistency::confirmPayee(long long docId) {
    auto d = db_.query("SELECT company_id FROM documents WHERE id=?", {docId});
    if (d.empty()) return json{{"error", "document not found"}};
    const long long cid = iOf(d[0], "company_id");
    if (!cid) return json{{"error", "set the company on this document first"}};
    auto acc = db_.query("SELECT fp, last4 FROM doc_accounts WHERE doc_id=?", {docId});
    if (acc.empty()) return json{{"error", "no bank account found in this document"}};
    db_.batch([&]() -> bool {
        for (const auto& a : acc)
            db_.exec("INSERT OR REPLACE INTO payee_accounts(company_id,fp,last4,confirmed_at,via_doc) VALUES(?,?,?,datetime('now'),?)",
                     {cid, sOf(a, "fp"), sOf(a, "last4"), docId});
        return true;
    });
    recompute();
    return json{{"confirmed", acc.size()}, {"flags", flags(docId)}};
}

} // namespace mary
