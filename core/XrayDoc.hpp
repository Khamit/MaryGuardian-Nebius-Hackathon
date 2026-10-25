#pragma once
// Склейка «карточка документа → X-ray»: какие значения искать на странице, какие счета считать подозрительными.
#include "Consistency.hpp"
#include "PiiMasker.hpp"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mary { namespace xray {

// doc — строка из Library::doc(id)["doc"]. inboxDir — папка inbox (файлы берутся только по имени, без путей).
// consistency может быть nullptr (тогда без проверки реквизитов). Возвращает то же, что buildPageView, + "doc" и "flags".
nlohmann::json viewForDocument(const nlohmann::json& doc, const std::string& inboxDir, int page, Consistency* consistency,
                               const std::vector<std::string>& protectedTerms, MaskLevel level);

}} // namespace mary::xray

