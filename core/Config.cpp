#include "Config.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace mary {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace

EnvFile::EnvFile(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 7, "export ") == 0) line = trim(line.substr(7));
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front()) {
            val = val.substr(1, val.size() - 2);
        } else {
            const size_t hash = val.find(" #");           // инлайн-комментарий только у значений без кавычек
            if (hash != std::string::npos) val = trim(val.substr(0, hash));
        }
        file_[key] = val;
    }
}

std::string EnvFile::get(const std::string& key, const std::string& def) const {
    if (const char* e = std::getenv(key.c_str())) {
        if (*e) return e;                                  // пустая переменная = не задана
    }
    auto it = file_.find(key);
    if (it != file_.end() && !it->second.empty()) return it->second;
    return def;
}

int EnvFile::getInt(const std::string& key, int def) const {
    const std::string v = get(key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

bool EnvFile::getBool(const std::string& key, bool def) const {
    const std::string v = lowerAscii(get(key));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::vector<std::string> EnvFile::getList(const std::string& key, char sep) const {
    std::vector<std::string> out;
    const std::string v = get(key);
    size_t start = 0;
    while (start <= v.size()) {
        const size_t p = v.find(sep, start);
        std::string item = trim(v.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (!item.empty()) out.push_back(std::move(item));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

std::string Settings::resolvedBackend() const {
    const std::string b = lowerAscii(backend);
    if (b == "remote") return remoteConfigured() ? "remote" : "off";   // без ключа удалённый режим невозможен
    if (b == "local")  return "local";
    if (b == "off")    return "off";
    return remoteConfigured() ? "remote" : "local";                    // auto
}

Settings Settings::load(const std::string& dotenvPath) {
    EnvFile e(dotenvPath);
    Settings s;
    s.workspace   = e.get("MG_WORKSPACE", s.workspace);
    s.port        = e.getInt("MG_PORT", s.port);
    s.bindAddr    = e.get("MG_BIND", s.bindAddr);
    s.allowedHosts = e.getList("MG_ALLOWED_HOSTS", ',');
    s.demoMode    = e.getBool("MG_DEMO_MODE", s.demoMode);

    s.webDir = e.get("MG_WEB_DIR");

    s.backend     = e.get("LLM_BACKEND", s.backend);
    s.apiKey      = e.get("NEBIUS_API_KEY");
    s.baseUrl     = e.get("NEBIUS_BASE_URL", s.baseUrl);
    s.modelNano   = e.get("NEMOTRON_MODEL_NANO", s.modelNano);
    s.modelSuper  = e.get("NEMOTRON_MODEL_SUPER", s.modelSuper);
    s.modelUltra  = e.get("NEMOTRON_MODEL_ULTRA", s.modelUltra);
    s.contextTokens = e.getInt("LLM_CONTEXT_TOKENS", s.contextTokens);
    s.timeoutSec  = e.getInt("LLM_TIMEOUT_SEC", s.timeoutSec);
    s.maxRetries  = e.getInt("LLM_MAX_RETRIES", s.maxRetries);
    s.concurrency = std::max(1, e.getInt("LLM_CONCURRENCY", s.concurrency));
    s.reasoningHeadroom = e.getInt("LLM_REASONING_HEADROOM", s.reasoningHeadroom);
    s.extraBody   = e.get("LLM_EXTRA_BODY");

    s.snippetChars = e.getInt("PII_SNIPPET_CHARS", s.snippetChars);
    s.maxRequestsPerHour = e.getInt("LLM_MAX_REQUESTS_PER_HOUR", s.maxRequestsPerHour);
    s.maxPromptChars = e.getInt("LLM_MAX_PROMPT_CHARS", s.maxPromptChars);
    s.piiLevel = lowerAscii(e.get("PII_MASK_LEVEL", s.piiLevel));
    s.protectedTerms = e.getList("PII_PROTECTED_TERMS", ';');

    s.localModelPath   = e.get("LOCAL_MODEL_PATH");
    s.localModelUrl    = e.get("LOCAL_MODEL_URL");
    s.localModelSha256 = e.get("LOCAL_MODEL_SHA256");
    s.ocrModelPath     = e.get("OCR_MODEL_PATH");
    s.ocrModelUrl      = e.get("OCR_MODEL_URL");
    s.ocrModelSha256   = e.get("OCR_MODEL_SHA256");
    s.ocrMmprojPath    = e.get("OCR_MMPROJ_PATH");
    s.ocrMmprojUrl     = e.get("OCR_MMPROJ_URL");
    s.ocrMmprojSha256  = e.get("OCR_MMPROJ_SHA256");

    s.watchIntervalSec = e.getInt("MG_WATCH_INTERVAL_SEC", s.watchIntervalSec);
    return s;
}

} // namespace mary
