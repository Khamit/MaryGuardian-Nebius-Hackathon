#pragma once
// Скачивание необязательных локальных моделей (GGUF с HuggingFace) по кнопке в интерфейсе.
//   * по умолчанию разрешён только https://huggingface.co/ (редиректы на CDN — только по https);
//   * пишет в <dest>.part, проверяет SHA-256 (если задан) и только потом переименовывает в dest;
//   * прогресс и отмена; одна загрузка на id.
// Это единственное место, где приложение ходит в сеть не за моделью Nemotron, и оно НЕ отправляет никаких данных пользователя.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

namespace mary {

class Downloader {
public:
    struct Job {
        std::string id;        // "llm" | "ocr" | "mmproj"
        std::string url;
        std::string dest;      // итоговый путь
        std::string sha256;    // необязательно; если задан — несовпадение = ошибка
    };
    using UrlPolicy = std::function<bool(const std::string& url)>;

    explicit Downloader(UrlPolicy policy = defaultPolicy) : policy_(std::move(policy)) {}
    ~Downloader();
    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    static bool defaultPolicy(const std::string& url);   // https://huggingface.co/...

    bool start(const Job& job, std::string* error = nullptr);
    void cancel(const std::string& id);
    nlohmann::json status() const;                       // {id: {state, done, total, path, error}}

private:
    struct State {
        std::atomic<unsigned long long> done{0}, total{0};
        std::atomic<bool> cancel{false};
        std::string state = "downloading", error, path;   // защищены mu_
        std::thread th;
    };
    void run(Job job, std::shared_ptr<State> st);

    UrlPolicy policy_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<State>> jobs_;
};

} // namespace mary
