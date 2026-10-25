#pragma once
// Журнал исходящих запросов к облачной модели: что именно ушло и сколько значений было скрыто.
// Хранится только то, что УЖЕ замаскировано (превью), плюс SHA-256 полезной нагрузки.
// Оригиналы PII в этот журнал не попадают никогда.
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace mary {

class OutboundLog {
public:
    explicit OutboundLog(std::string path, size_t keep = 300) : path_(std::move(path)), keep_(keep) { loadExisting(); }

    void add(const nlohmann::json& e) {
        std::lock_guard<std::mutex> lk(mu_);
        account(e);
        ring_.push_back(e);
        if (ring_.size() > keep_) ring_.pop_front();
        std::ofstream f(path_, std::ios::app);
        if (f) f << e.dump() << "\n";
    }

    // Последние n записей, новые первыми.
    nlohmann::json recent(size_t n = 100) const {
        std::lock_guard<std::mutex> lk(mu_);
        nlohmann::json out = nlohmann::json::array();
        for (auto it = ring_.rbegin(); it != ring_.rend() && out.size() < n; ++it) out.push_back(*it);
        return out;
    }

    // Итоги для экрана Privacy: сколько запросов, сколько значений скрыто, сколько токенов потрачено.
    nlohmann::json totals() const {
        std::lock_guard<std::mutex> lk(mu_);
        return nlohmann::json{{"requests", sent_}, {"blocked", blocked_}, {"failed", failed_},
                              {"masked_values", masked_}, {"masked_by_type", byType_},
                              {"prompt_tokens", tokIn_}, {"completion_tokens", tokOut_}};
    }

private:
    void account(const nlohmann::json& e) {
        const std::string st = e.value("status", "");
        if (st == "sent") ++sent_; else if (st == "blocked") ++blocked_; else ++failed_;
        masked_ += e.value("masked_total", 0ULL);
        tokIn_ += e.value("prompt_tokens", 0ULL);
        tokOut_ += e.value("completion_tokens", 0ULL);
        if (e.contains("masked") && e["masked"].is_object())
            for (auto it = e["masked"].begin(); it != e["masked"].end(); ++it)
                if (it.value().is_number_integer()) byType_[it.key()] += it.value().get<unsigned long long>();
    }

    void loadExisting() {
        std::ifstream f(path_);
        std::string line;
        while (std::getline(f, line)) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object()) continue;
            account(j);
            ring_.push_back(std::move(j));
            if (ring_.size() > keep_) ring_.pop_front();
        }
    }

    mutable std::mutex mu_;
    std::string path_;
    size_t keep_;
    std::deque<nlohmann::json> ring_;
    unsigned long long sent_ = 0, blocked_ = 0, failed_ = 0, masked_ = 0, tokIn_ = 0, tokOut_ = 0;
    std::map<std::string, unsigned long long> byType_;
};

} // namespace mary
