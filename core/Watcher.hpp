#pragma once
// Фоновое наблюдение за папкой inbox. Не inotify/FSEvents (они разные на macOS и Linux), а дешёвый опрос:
// повторный скан читает только записи каталога и сверяет их с RAM-индексом, поэтому тик на тысячи файлов
// занимает миллисекунды. Файлы, которые ещё копируются, пропускает сам Indexer (окно «стабильности» mtime).
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace mary {

class Watcher {
public:
    Watcher(int intervalSec, std::function<void()> tick) : interval_(intervalSec), tick_(std::move(tick)) {}
    ~Watcher() { stop(); }
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    void start() {
        if (interval_ <= 0 || th_.joinable()) return;
        th_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(mu_);
            while (!stop_) {
                lk.unlock();
                tick_();
                lk.lock();
                cv_.wait_for(lk, std::chrono::seconds(interval_), [this] { return stop_; });
            }
        });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    int interval_;
    std::function<void()> tick_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread th_;
};

} // namespace mary
