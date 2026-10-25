#include "Downloader.hpp"
#include "Sha256.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace mary {
namespace fs = std::filesystem;

bool Downloader::defaultPolicy(const std::string& url) {
    static const std::string prefix = "https://huggingface.co/";
    return url.rfind(prefix, 0) == 0 && url.size() > prefix.size();
}

namespace {

struct Ctx {
    std::ofstream out;
    std::atomic<unsigned long long>* done;
    std::atomic<unsigned long long>* total;
    std::atomic<bool>* cancel;
};

size_t onWrite(char* p, size_t sz, size_t n, void* user) {
    auto* c = static_cast<Ctx*>(user);
    c->out.write(p, (std::streamsize)(sz * n));
    return c->out ? sz * n : 0;
}

int onProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* c = static_cast<Ctx*>(user);
    c->done->store((unsigned long long)dlnow);
    if (dltotal > 0) c->total->store((unsigned long long)dltotal);
    return c->cancel->load() ? 1 : 0;       // ненулевое значение прерывает передачу
}

std::string sha256OfFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    Sha256 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        if (f.gcount() > 0) h.update(buf.data(), (size_t)f.gcount());
    }
    return h.hex();
}

std::string lowerHex(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    return s;
}

} // namespace

Downloader::~Downloader() {
    std::vector<std::shared_ptr<State>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : jobs_) { kv.second->cancel = true; all.push_back(kv.second); }
    }
    for (auto& s : all) if (s->th.joinable()) s->th.join();
}

bool Downloader::start(const Job& job, std::string* error) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    if (job.id.empty() || job.url.empty() || job.dest.empty()) return fail("url or destination is not configured");
    if (!policy_(job.url)) return fail("this URL is not allowed (only https://huggingface.co/ by default)");

    std::shared_ptr<State> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = jobs_.find(job.id);
        if (it != jobs_.end()) {
            if (it->second->state == "downloading" || it->second->state == "verifying") return fail("already downloading");
            old = it->second;
        }
    }
    if (old && old->th.joinable()) old->th.join();   // прежняя загрузка уже завершилась

    auto st = std::make_shared<State>();
    st->path = job.dest;
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_[job.id] = st;
    }
    st->th = std::thread([this, job, st] { run(job, st); });
    return true;
}

void Downloader::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it != jobs_.end()) it->second->cancel = true;
}

nlohmann::json Downloader::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : jobs_) {
        const auto& s = *kv.second;
        j[kv.first] = {{"state", s.state}, {"done", s.done.load()}, {"total", s.total.load()},
                       {"path", s.path}, {"error", s.error}};
    }
    return j;
}

void Downloader::run(Job job, std::shared_ptr<State> st) {
    auto finish = [&](const std::string& state, const std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        st->state = state;
        st->error = err;
    };

    std::error_code ec;
    fs::create_directories(fs::path(job.dest).parent_path(), ec);
    const std::string part = job.dest + ".part";

    Ctx ctx;
    ctx.out.open(part, std::ios::binary | std::ios::trunc);
    ctx.done = &st->done; ctx.total = &st->total; ctx.cancel = &st->cancel;
    if (!ctx.out) { finish("error", "cannot write to " + part); return; }

    static const bool once = [] { curl_global_init(CURL_GLOBAL_DEFAULT); return true; }();
    (void)once;
    CURL* c = curl_easy_init();
    if (!c) { finish("error", "curl init failed"); return; }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);          // HuggingFace отдаёт файлы через редирект на CDN
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);      // сорвалось соединение — не висим вечно
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MaryGuardian/1.0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    ctx.out.close();

    if (st->cancel) { fs::remove(part, ec); finish("cancelled", ""); return; }
    if (rc != CURLE_OK) { fs::remove(part, ec); finish("error", errbuf[0] ? errbuf : curl_easy_strerror(rc)); return; }

    if (!job.sha256.empty()) {
        { std::lock_guard<std::mutex> lk(mu_); st->state = "verifying"; }
        if (lowerHex(sha256OfFile(part)) != lowerHex(job.sha256)) {
            fs::remove(part, ec);
            finish("error", "SHA-256 mismatch: the downloaded file was deleted");
            return;
        }
    }
    fs::rename(part, job.dest, ec);
    if (ec) { finish("error", "cannot move the file into place: " + ec.message()); return; }
    st->done = fs::file_size(job.dest, ec);
    finish("done", "");
}

} // namespace mary
