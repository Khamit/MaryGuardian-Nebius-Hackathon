// Config + Downloader + Watcher.  DL_URL / DL_SHA — файл, который раздаёт `python3 -m http.server` (см. CI).
#include "core/Config.hpp"
#include "core/Downloader.hpp"
#include "core/Watcher.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mary;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

static bool waitFor(Downloader& d, const std::string& id, const std::string& want) {
    for (int i = 0; i < 100; ++i) {
        if (d.status().value(id, nlohmann::json::object()).value("state", "") == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main() {
    // --- Config ---
    {
        std::ofstream f("/tmp/test.env");
        f << "# comment\nNEBIUS_API_KEY=\"file-key\"\nexport LLM_BACKEND=remote # inline\nMG_PORT=9001\n"
             "PII_PROTECTED_TERMS=PERSON:Иван Петров; ул. Ленина 5 ;\nNEMOTRON_MODEL_NANO=\nMG_DEMO_MODE=yes\n";
        f.close();
        setenv("MG_PORT", "9100", 1);                 // реальное окружение сильнее .env
        unsetenv("NEBIUS_API_KEY");
        Settings s = Settings::load("/tmp/test.env");
        CHECK(s.apiKey == "file-key" && s.backend == "remote" && s.port == 9100 && s.demoMode);
        CHECK(s.protectedTerms.size() == 2 && s.protectedTerms[1] == "ул. Ленина 5");
        CHECK(s.modelNano == "nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B");   // пустое значение → дефолт
        CHECK(s.resolvedBackend() == "remote");
        Settings none = Settings::load("/tmp/does-not-exist.env");
        unsetenv("MG_PORT");
        CHECK(none.apiKey.empty() && none.resolvedBackend() == "local");  // auto без ключа → local
        none.backend = "remote";
        CHECK(none.resolvedBackend() == "off");                           // remote без ключа не включается молча
    }
    // --- Downloader ---
    {
        CHECK(Downloader::defaultPolicy("https://huggingface.co/org/repo/resolve/main/m.gguf"));
        CHECK(!Downloader::defaultPolicy("https://evil.example/huggingface.co/m.gguf"));
        CHECK(!Downloader::defaultPolicy("http://huggingface.co/m.gguf"));

        Downloader strict;
        std::string err;
        CHECK(!strict.start({"llm", "https://example.com/m.gguf", "/tmp/dl/m.gguf", ""}, &err));

        const char* url = std::getenv("DL_URL");
        const char* sha = std::getenv("DL_SHA");
        if (url && sha) {
            std::filesystem::remove_all("/tmp/dl");
            Downloader d([](const std::string&) { return true; });          // тестовая политика: локальный сервер
            CHECK(d.start({"ok", url, "/tmp/dl/good.bin", sha}, &err));
            CHECK(waitFor(d, "ok", "done"));
            CHECK(std::filesystem::exists("/tmp/dl/good.bin") && !std::filesystem::exists("/tmp/dl/good.bin.part"));
            CHECK(d.start({"bad", url, "/tmp/dl/bad.bin", std::string(64, '0')}, &err));
            CHECK(waitFor(d, "bad", "error"));
            CHECK(!std::filesystem::exists("/tmp/dl/bad.bin"));              // файл с неверной суммой удаляется
            CHECK(d.status()["bad"]["error"].get<std::string>().find("SHA-256") != std::string::npos);
        } else {
            std::cout << "(download test skipped: DL_URL/DL_SHA not set)\n";
        }
    }
    // --- Watcher ---
    {
        std::atomic<int> ticks{0};
        Watcher w(1, [&] { ++ticks; });
        w.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        const auto t0 = std::chrono::steady_clock::now();
        w.stop();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300));   // остановка не ждёт интервал
        CHECK(ticks >= 2);
    }
    std::cout << "support OK\n";
    return 0;
}
