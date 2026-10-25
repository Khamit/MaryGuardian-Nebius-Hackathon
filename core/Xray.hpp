#pragma once
// «Рентген» документа: страница PDF + прямоугольники поверх неё.
//   field   — то, что нашёл конвейер (компания, номер, дата, сумма, срок, банковский счёт)
//   risk    — банковский счёт, не совпадающий с эталоном (см. Consistency)
//   mask    — что Guardian скрыл бы от облака (в метке — заглушка вида [IBAN_1])
//   inject  — фраза, похожая на инструкцию для ИИ
//   hidden  — строка, набранная «невидимым» (почти белым) цветом
// Координаты — доли страницы (0..1), поэтому не зависят от масштаба рендера. Всё считается локально.
#include "PiiMasker.hpp"
#include <functional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mary { namespace xray {

struct Needle {
    std::string label, text;
    std::string kind = "field";
    std::string tip;
};

struct Options {
    int   page = 0;
    float zoom = 1.6f;
    std::vector<Needle>  needles;            // что искать на странице (без учёта регистра и пробелов)
    std::vector<PiiTerm> terms;              // защищённые термины пользователя
    MaskLevel level = MaskLevel::Strict;
    std::function<std::string(const std::string&)> ibanFp;   // нормализованный IBAN → отпечаток
    std::set<std::string> riskFps;           // отпечатки «неправильных» счетов
    bool markInjection = true;
};

// {ok, page, pages, width, height, png_b64, boxes:[{x,y,w,h,kind,label,tip}], hidden_lines}  или  {error}
nlohmann::json buildPageView(const std::string& path, const Options& opt);

// Как одна и та же дата / сумма могут быть напечатаны в документе.
std::vector<std::string> dateForms(const std::string& iso);        // "2026-10-14" → ["2026-10-14","14.10.2026",…]
std::vector<std::string> amountForms(long long minor);             // 125000 → ["1,250.00","1250.00","1250,00","1.250,00"]

}} // namespace mary::xray
