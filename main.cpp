// MaryGuardian — точка входа. Вся конфигурация из окружения/.env (см. .env.example); флаги CLI её переопределяют.
//
//   Бэкенд LLM:   remote — NVIDIA Nemotron на Nebius Token Factory (всегда через GuardedLlm: маскирование PII + аудит)
//                 local  — Qwen GGUF через llama.cpp (только если собрано с -DMG_LOCAL_LLM=ON)
//                 off    — только правила, без моделей
#include "core/Config.hpp"
#include "core/Corrections.hpp"
#include "core/Db.hpp"
#include "core/Downloader.hpp"
#include "core/Guardian.hpp"
#include "core/GuardedLlm.hpp"
#include "core/Indexer.hpp"
#include "core/Library.hpp"
#include "core/LlmSwitch.hpp"
#include "core/OutboundLog.hpp"
#include "core/RemoteLlm.hpp"
#include "core/Services.hpp"
#include "core/Verifier.hpp"
#include "core/Watcher.hpp"
#include "server/HttpServer.hpp"
#include "task/BookkeeperTask.hpp"
#include "core/Platform.hpp"
#include "core/InboxSignature.hpp"
#include "core/DemoData.hpp"
#include "core/Consistency.hpp"

#ifdef MG_LOCAL_LLM
#include "core/LocalLlm.hpp"
#include "core/ModelRuntime.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

std::atomic<bool> g_running(true);

static std::vector<std::string> splitTerms(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t p = s.find(';', start);
        std::string item = s.substr(start, p == std::string::npos ? std::string::npos : p - start);
        const size_t b = item.find_first_not_of(" \t\r\n");
        if (b != std::string::npos) out.push_back(item.substr(b, item.find_last_not_of(" \t\r\n") - b + 1));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

int main(int argc, char* argv[]) {
    mary::plat::installStopHandler(g_running);
    mary::plat::initConsoleUtf8();

    // ---- конфигурация: окружение/.env, затем флаги ----
    const char* envFile = std::getenv("MG_ENV_FILE");
    mary::Settings st = mary::Settings::load(envFile ? envFile : ".env");
    
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        try {
            if (a == "--workspace" && i + 1 < argc)        st.workspace = argv[++i];
            else if (a == "--port" && i + 1 < argc)        st.port = std::stoi(argv[++i]);
            else if (a == "--bind" && i + 1 < argc)        st.bindAddr = argv[++i];
            else if (a == "--backend" && i + 1 < argc)     st.backend = argv[++i];
            else if (a == "--model" && i + 1 < argc)       st.localModelPath = argv[++i];
            else if (a == "--ocr-model" && i + 1 < argc)   st.ocrModelPath = argv[++i];
            else if (a == "--ocr-mmproj" && i + 1 < argc)  st.ocrMmprojPath = argv[++i];
            else if (a == "--seed-demo")                   st.demoMode = true; // пример
        } catch (const std::exception&) {
            std::cerr << "[Main] Invalid value for " << a << "\n";
            return 2;
        }
    }

    const std::string ws = st.workspace;
    const std::string inbox = ws + "/inbox";
    std::error_code iec;
    
    std::filesystem::create_directories(inbox);

    std::filesystem::create_directories(ws + "/logs");
    std::filesystem::create_directories(ws + "/output");
    std::filesystem::create_directories(ws + "/web");
    std::filesystem::create_directories(ws + "/models");

    if (st.demoMode && std::filesystem::is_empty(inbox, iec)) {
        mary::demo::writeDemoInbox(inbox);
    }

    Guardian guardian(ws + "/logs/audit.jsonl", ws + "/inbox", ws + "/output");
    mary::Verifier verifier;
    mary::Db db(ws + "/library.db");
    if (!db.ok()) { std::cerr << "[Main] Cannot open library.db\n"; return 1; }
    mary::Corrections corrections(db);
    mary::Consistency consistency(db);
    mary::OutboundLog outbound(ws + "/logs/outbound.jsonl");
    mary::Downloader downloader;

    // ---- защищённые термины: .env + поле профиля (меняется из интерфейса без перезапуска) ----
    auto protectedTerms = [&db, &st]() {
        std::vector<std::string> t = st.protectedTerms;
        auto r = db.query("SELECT value FROM profile WHERE key='protected_terms'");
        if (!r.empty() && r[0]["value"].is_string())
            for (auto& x : splitTerms(r[0]["value"].get<std::string>())) t.push_back(std::move(x));
        return t;
    };

    // ---- бэкенды ----
    mary::NullLlm nullLlm;
    std::unique_ptr<mary::RemoteLlm> remote;
    std::unique_ptr<mary::GuardedLlm> guarded;
    [[maybe_unused]] mary::Indexer* indexerPtr = nullptr;   
    // нужен колбэку setOcr, который создаётся раньше индексатора

    const std::string want = st.resolvedBackend();
    if (want == "remote" || st.remoteConfigured()) {
        mary::RemoteLlm::Options ro;
        ro.baseUrl = st.baseUrl; ro.apiKey = st.apiKey;
        ro.modelNano = st.modelNano; ro.modelSuper = st.modelSuper; ro.modelUltra = st.modelUltra;
        ro.contextTokens = st.contextTokens; ro.timeoutSec = st.timeoutSec; ro.maxRetries = st.maxRetries;
        ro.reasoningHeadroom = st.reasoningHeadroom; ro.extraBody = st.extraBody;
        remote = std::make_unique<mary::RemoteLlm>(ro);
        if (remote->available()) {
            mary::GuardedLlm::Options go;
            go.maxRequestsPerHour = st.maxRequestsPerHour;
            go.maxPromptChars = st.maxPromptChars;
            go.level = st.piiLevel == "standard" ? mary::MaskLevel::Standard : mary::MaskLevel::Strict;   // ←
            go.secrets = {st.apiKey}; // ← ключ приложения не уйдёт в промпт ни при каких условиях
            guarded = std::make_unique<mary::GuardedLlm>(
                *remote, outbound, go,
                [] { return mary::safety::isAllowedAction("llm_remote"); },
                [&guardian](const std::string& action, bool allowed, const std::string& reason) { guardian.record(action, allowed, reason); },
                protectedTerms);
        } else {
            std::cerr << "[Main] Remote backend unavailable: " << remote->initError() << "\n";
        }
    }

    mary::Services svc;
    svc.settings = st;
    svc.workspace = ws;
    svc.remote = guarded.get();
    svc.rawRemote = (remote && remote->available()) ? remote.get() : nullptr;
    svc.outbound = &outbound;
    svc.downloader = &downloader;
    svc.corrections = &corrections;
    svc.consistency = &consistency;
    svc.protectedTerms = protectedTerms;

#ifdef MG_LOCAL_LLM
    mary::ModelRuntime runtime;
    mary::LocalLlm localLlm(runtime);
    svc.local = &localLlm;
    svc.loadLocal = [&runtime](const std::string& path) { return runtime.loadModel(path, 8192, 99); };
    svc.localLoaded = [&runtime] { return runtime.isLoaded(); };
    svc.setOcr = [&indexerPtr](const std::string& m, const std::string& mm) { if (indexerPtr) indexerPtr->setOcrPaths(m, mm); };
    if (want == "local" && !st.localModelPath.empty()) {
        if (!std::filesystem::exists(st.localModelPath)) std::cerr << "[Main] WARNING: model file not found: " << st.localModelPath << "\n";
        else if (runtime.loadModel(st.localModelPath, 8192, 99)) std::cout << "[Main] Local model loaded: " << st.localModelPath << "\n";
    }
#endif

    mary::LlmClient* initial = &nullLlm;
    std::string mode = "off";
    if (guarded && guarded->available() && want != "local" && want != "off") { initial = guarded.get(); mode = "remote"; }
#ifdef MG_LOCAL_LLM
    else if (want == "local" && localLlm.available()) { initial = &localLlm; mode = "local"; }
#endif
    mary::LlmSwitch llm(initial, mode);
    svc.llm = &llm;

    // ---- остальное ----
    std::string ocrModel = st.ocrModelPath, ocrMmproj = st.ocrMmprojPath;
    if (ocrModel.empty() && std::filesystem::exists(ws + "/models/OvisOCR2-Q6_K.gguf")) ocrModel = ws + "/models/OvisOCR2-Q6_K.gguf";
    if (ocrMmproj.empty() && std::filesystem::exists(ws + "/models/mmproj-F16.gguf")) ocrMmproj = ws + "/models/mmproj-F16.gguf";

    mary::BookkeeperTask task(llm, guardian, verifier);
    mary::Indexer indexer(db, guardian, llm, verifier, corrections, ws + "/inbox", ocrModel, ocrMmproj);
    indexer.configure(st.concurrency, st.snippetChars);
    indexerPtr = &indexer;
    indexer.setConsistency(&consistency);                                  // ←
    mary::Library library(db, guardian, llm, corrections);
    library.setConsistency(&consistency); 

    HttpServer server(st, svc, task, guardian, indexer, library, ws);
    server.setRunningFlag(&g_running);
    if (!server.start()) { std::cerr << "[Main] Failed to start HTTP server\n"; return 1; }
    indexer.start();   
    
    mary::InboxSig lastSig = mary::inboxSignature(inbox);
    bool lastRecent = lastSig.recent;
    mary::Watcher watcher(st.watchIntervalSec, [&] {
        const mary::InboxSig s = mary::inboxSignature(inbox);
        if (!s.ok || indexer.isRunning()) return;
        if (s.hash != lastSig.hash || lastRecent)
            if (indexer.start()) { lastSig = s; lastRecent = s.recent; }
    });
    watcher.start();

    const std::string url = "http://127.0.0.1:" + std::to_string(st.port);
    std::cout << "\n=========================================\n"
              << "  MaryGuardian\n"
              << "  UI:       " << url << "\n"
              << "  Backend:  " << llm.mode() << "\n"
              << "  Watching: " << inbox << "\n=========================================\n";

    if (!st.demoMode && st.bindAddr == "127.0.0.1") {
        mary::plat::openInSystem(url);
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    watcher.stop();
    server.stop();
    indexer.stop();
    std::cout << "[Main] Done.\n";
    return 0;
}
