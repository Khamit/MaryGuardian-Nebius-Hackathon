#pragma once
// Новые операции API (приватность, настройки бэкенда, локальные модели) в отдельном классе:
// в HttpServer добавляется одна строка делегирования, а не сотня строк логики.
//
//   POST /api/lib/<op>:  backend, backend_set, models_check, mask_preview, outbound, learning,
//                        model_download, model_cancel, model_load, downloads
//   GET  /api/backend, GET /api/outbound   (вызывают backend() / outbound() напрямую)
#include "../core/Services.hpp"
#include <string>
#include <nlohmann/json.hpp>

namespace mary {

class ApiExtras {
public:
    explicit ApiExtras(Services& s) : s_(s) {}

    // true — операция обработана; result содержит тело ответа или {"error": "..."}.
    bool handle(const std::string& op, const nlohmann::json& body, nlohmann::json& result);

    nlohmann::json backend() const;                 // состояние бэкендов; ключ API наружу не отдаётся
    nlohmann::json outbound(size_t n = 100) const;  // {recent: [...], totals: {...}}

private:
    struct Slot { std::string id, url, path, sha, defName; };
    Slot slot(const std::string& what) const;
    static nlohmann::json err(const std::string& m) { return nlohmann::json{{"error", m}}; }

    Services& s_;
};

} // namespace mary
