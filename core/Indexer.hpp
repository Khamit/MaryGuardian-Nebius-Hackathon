#pragma once

#ifdef MG_LOCAL_LLM
#include "OcrEngine.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

#include "Db.hpp"
#include "Guardian.hpp"
#include "ModelRuntime.hpp"
#include "Verifier.hpp"
#include "LlmClient.hpp"
#include "Corrections.hpp"


namespace mary {

class Consistency;

struct ScanReport { int total = 0,
                        known = 0,
                        added = 0, 
                        renamed = 0, 
                        missing = 0, 
                        skipped = 0, 
                        duplicates = 0; };

class Indexer {

public:
    static constexpr int CLASSIFIER_VER = 5; 
    // было 4: библиотека переклассифицируется один раз (ответы LLM берутся из кеша)

    Indexer(Db& db, Guardian& g, LlmClient& llm, Verifier& v, Corrections& corr, const std::string& inbox,
        const std::string& ocr_model = "", const std::string& ocr_mmproj = "");
    ~Indexer() { stop(); }

    bool start();
    void stop();
    nlohmann::json status() const;
    bool isRunning() const { return running_.load(); }
    void setConsistency(Consistency* c) { consistency_ = c; }
    void configure(int concurrency, int snippetChars) { concurrency_ = concurrency; snippetChars_ = snippetChars; }
    void setOcrPaths(const std::string& model, const std::string& mmproj) {   // после скачивания моделей из Settings
        std::lock_guard<std::mutex> lk(smu_); ocrModel_ = model; ocrMmproj_ = mmproj;
    }


private:

    Consistency* consistency_ = nullptr;
    // Строка из таблицы documents ровно в том объёме, который нужен сканеру.
    struct Rec {
        std::string sha, name;
        long long size = -1, mtime = -1;
        bool present = true;
    };

    void run();
    ScanReport scan();
    void loadIndex();
    int runOcrPhase();                     // сколько документов получили текст
    void classifyPending();
    void classify(long long id, const std::string& name);
    std::string readLeading(const std::string& path, safety::FileKind kind) const;
    static std::string fileKey(const std::string& name, long long size, long long mtime);

    Db& db_;
    Guardian& guardian_;
    LlmClient& llm_;
    Verifier& verifier_;
    Corrections& corr_;
    std::string inbox_;
    int concurrency_ = 1, snippetChars_ = 1200;

    std::string ocrError_;
    std::string ocrModel_;
    std::string ocrMmproj_;

    #ifdef MG_LOCAL_LLM
    OcrEngine ocr_;
    #endif

    // --- RAM-индекс сканера. Источник истины — SQL; здесь рабочая копия. ---
    // Трогает только поток индексатора (запуски разделены join), поэтому без замков.
    std::vector<Rec> recs_;
    std::unordered_map<std::string, uint32_t> byKey_;   // имя+размер+mtime → индекс в recs_
    std::unordered_map<std::string, uint32_t> bySha_;   // sha256 → индекс в recs_
    bool indexLoaded_ = false;
    std::unordered_map<std::string, std::string> dupSha_;   // дубликаты: имя+размер+mtime → sha (чтобы не хешировать снова)
    std::unordered_set<std::string> rejected_;               // отклонённые Guardian: имя+размер+mtime

    std::thread th_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<int> done_{0}, total_{0};
    std::atomic<unsigned long long> rev_{0};   // растёт при каждом изменении данных; UI перечитывает список только по нему
    // Счётчики последнего запуска: сколько работы сделано на самом деле и сколько сэкономили кеши (показывает вкладка Activity).
    struct RunStats {
        std::atomic<unsigned long long> hashed{0}, 
                                        llmCalls{0}, 
                                        llmHits{0}, 
                                        batches{0}, 
                                        classified{0}, 
                                        scanMs{0}, 
                                        tokensIn{0}, 
                                        tokensOut{0}, 
                                        refined{0}, 
                                        learned{0};
    } stats_;
    mutable std::mutex smu_;
    ScanReport report_;
    std::string current_, phase_ = "idle";
};
} // namespace mary