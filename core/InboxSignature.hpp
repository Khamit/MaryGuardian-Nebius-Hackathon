#pragma once
// Дешёвая «подпись» папки inbox для Watcher: один обход каталога, без открытия файлов и без SQL.
// Нужна, чтобы фоновый тик НЕ запускал полный Indexer::run(), когда ничего не изменилось:
//   * run() обнуляет stats_/report_ и пишет "index:scan" в аудит → без этого фильтра Activity обнулялся бы
//     каждые 10 секунд, а audit.jsonl рос бы на ~8 600 строк в сутки;
//   * вкладка Activity перестаёт мигать фазой "scanning".
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mary {

struct InboxSig {
    std::uint64_t hash = 0;     // FNV-1a по (имя, размер, mtime) всех файлов; порядок обхода не важен (XOR)
    bool          recent = false; // есть файл, изменённый недавно: он ещё «оседает» → нужен повторный проход, даже если hash тот же
    bool          ok = false;   // false — каталог не прочитан (не считать это «изменением» и не «пропажей» файлов)
};

inline InboxSig inboxSignature(const std::string& dir, int settleSec = 3) {
    namespace fs = std::filesystem;
    InboxSig s;
    std::error_code lec;
    fs::directory_iterator it(dir, lec), end;
    if (lec) return s;
    const auto now = fs::file_time_type::clock::now();
    for (; it != end; it.increment(lec)) {
        if (lec) return s;
        std::error_code ec;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        const auto size = it->is_regular_file(ec) ? (std::uint64_t)it->file_size(ec) : 0ULL;
        const auto ft = it->last_write_time(ec);
        if (ec) continue;
        if (now - ft < std::chrono::seconds(settleSec)) s.recent = true;
        std::uint64_t h = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ULL; } };
        for (unsigned char c : name) { h ^= c; h *= 1099511628211ULL; }
        mix(size);
        mix((std::uint64_t)std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count());
        s.hash ^= h;
    }
    s.ok = true;
    return s;
}

} // namespace mary