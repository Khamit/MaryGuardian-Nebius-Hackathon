// Использует только публичный API Db (query/exec), поэтому работает и с вашим реальным core/Db.cpp:
//   g++ -std=c++17 -I. -Ithird_party tests/corrections_test.cpp core/Corrections.cpp core/Db.cpp core/Extract.cpp -lsqlite3
#include "core/Corrections.hpp"
#include <cstdio>
#include <iostream>

using namespace mary;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

int main() {
    const char* path = ":memory:";
    Db db(path);
    CHECK(db.ok());
    long long acme = 0, other = 0, legal = 0, misc = 0;
    db.exec("INSERT INTO companies(canonical,display,confirmed) VALUES('acme','Acme',1)", {}, &acme);
    db.exec("INSERT INTO companies(canonical,display,confirmed) VALUES('globex','Globex',1)", {}, &other);
    db.exec("INSERT INTO collections(name,auto_type,keywords,sort) VALUES('Legal',NULL,'',1)", {}, &legal);
    db.exec("INSERT INTO collections(name,auto_type,keywords,sort) VALUES('Misc',NULL,'',2)", {}, &misc);
    CHECK(acme && other && legal && misc);

    Corrections c(db);
    CHECK(c.typeFor(acme, "invoice").empty());                       // пока ничего не выучено

    c.learnType(acme, "invoice", "contract");
    c.learnType(acme, "invoice", "contract");                        // повтор увеличивает счётчик, не плодит строки
    c.learnType(acme, "invoice", "invoice");                         // «исправление» на то же самое игнорируется
    CHECK(c.typeFor(acme, "invoice") == "contract");
    CHECK(c.typeFor(acme, "receipt").empty());                       // другие промахи этой компании не затронуты
    CHECK(c.typeFor(other, "invoice").empty());                      // и другие компании тоже
    CHECK(c.typeFor(0, "invoice").empty());                          // компания не определена — правила нет

    c.learnCollection(acme, "contract", legal);
    c.learnCollection(acme, "contract", misc);                       // последнее решение пользователя побеждает
    CHECK(c.collectionFor(acme, "contract") == misc);
    CHECK(c.collectionFor(other, "contract") == 0);

    // Удаление коллекции (Library::deleteCollection) не должно ломаться из-за внешних ключей.
    CHECK(db.exec("DELETE FROM collections WHERE id=?", {misc}) >= 0);
    CHECK(c.collectionFor(acme, "contract") == 0);
    // Слияние/удаление компании (Library::renameCompany) — тоже.
    CHECK(db.exec("DELETE FROM companies WHERE id=?", {acme}) >= 0);
    CHECK(c.typeFor(acme, "invoice").empty());

    c.countApplied();
    auto st = c.stats();
    CHECK(st["type_rules"].get<int>() == 0 && st["applied"].get<int>() == 1);
    std::cout << "corrections OK\n";
    return 0;
}
