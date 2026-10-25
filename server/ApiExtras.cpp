#include "ApiExtras.hpp"
#include "../core/PiiMasker.hpp"
#include "../core/DemoData.hpp"
#include <filesystem>
#include <set>

namespace mary {
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

NullLlm& nullLlm() { static NullLlm n; return n; }

std::string hostOf(const std::string& url) {
    const size_t a = url.find("://");
    const size_t from = a == std::string::npos ? 0 : a + 3;
    const size_t to = url.find('/', from);
    return url.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

std::string strOf(const json& b, const char* k) {
    return (b.is_object() && b.contains(k) && b[k].is_string()) ? b[k].get<std::string>() : "";
}

} // namespace

ApiExtras::Slot ApiExtras::slot(const std::string& what) const {
    const Settings& c = s_.settings;
    Slot s;
    s.id = what;
    if (what == "llm")         { s.url = c.localModelUrl; s.path = c.localModelPath; s.sha = c.localModelSha256; s.defName = "Qwen3.5-2B-Q4_K_M.gguf"; }
    else if (what == "ocr")    { s.url = c.ocrModelUrl;   s.path = c.ocrModelPath;   s.sha = c.ocrModelSha256;   s.defName = "OvisOCR2-Q6_K.gguf"; }
    else if (what == "mmproj") { s.url = c.ocrMmprojUrl;  s.path = c.ocrMmprojPath;  s.sha = c.ocrMmprojSha256;  s.defName = "mmproj-F16.gguf"; }
    else s.id.clear();
    if (!s.id.empty() && s.path.empty()) s.path = s_.workspace + "/models/" + s.defName;
    return s;
}

json ApiExtras::backend() const {
    const Settings& c = s_.settings;
    json out;
    out["backend"] = s_.llm ? s_.llm->mode() : "off";
    out["demo_mode"] = c.demoMode;
    out["remote"] = {
        {"configured", s_.remote != nullptr && s_.remote->available()},
        {"provider", "Nebius Token Factory"},
        {"host", hostOf(c.baseUrl)},
        {"models", {{"nano", c.modelNano}, {"super", c.modelSuper}, {"ultra", c.modelUltra}}},
        {"roles", {{"nano", "bulk classification"}, {"super", "field extraction and drafts"}, {"ultra", "questions over the library"}}}};
    json local = {{"compiled", s_.local != nullptr}, {"loaded", s_.localLoaded ? s_.localLoaded() : false}};
    json warn = json::array();
        if (!s_.llm || !s_.llm->available())
            warn.push_back("No AI backend: documents are classified by rules only. Add Nebius API key in Settings.");
        out["warnings"] = warn;
    for (const char* w : {"llm", "ocr", "mmproj"}) {
        const Slot sl = slot(w);
        std::error_code ec;
        local[w] = {{"path", sl.path}, {"present", fs::exists(sl.path, ec)}, {"url_configured", !sl.url.empty()}};
    }
    out["local"] = local;
    out["privacy"] = {{"snippet_chars", c.snippetChars},{"mask_level", c.piiLevel},
                      {"protected_terms", s_.protectedTerms ? s_.protectedTerms().size() : 0},
                      {"requests_per_hour_limit", c.maxRequestsPerHour}};
    out["downloads"] = s_.downloader ? s_.downloader->status() : json::object();
    return out;
}

json ApiExtras::outbound(size_t n) const {
    if (!s_.outbound) return json{{"recent", json::array()}, {"totals", json::object()}};
    return json{{"recent", s_.outbound->recent(n)}, {"totals", s_.outbound->totals()}};
}

bool ApiExtras::handle(const std::string& op, const json& b, json& r) {
    static const std::set<std::string> mutating = {"backend_set", "model_download", "model_cancel", "model_load"};
    if (op == "backend")  { r = backend(); return true; }
    if (op == "outbound") { r = outbound(); return true; }
    if (op == "downloads"){ r = json{{"downloads", s_.downloader ? s_.downloader->status() : json::object()}}; return true; }

    if (op == "demo_load") {
        r = json{{"created", demo::writeDemoInbox(s_.workspace + "/inbox")}};
        if (s_.afterDemoLoad) s_.afterDemoLoad();
        return true;
    }
    if (op == "open_inbox") {
        if (s_.settings.demoMode) { r = err("disabled in demo mode"); return true; }
        plat::openInSystem(s_.workspace + "/inbox");
        r = json::object();
        return true;
    }

    if (op == "learning") { r = s_.corrections ? s_.corrections->stats() : json::object(); return true; }

    if (op == "mask_preview") {
        // Только локально: показывает, что ушло бы в облако. Сеть не задействуется.
        std::string text = strOf(b, "text");
        if (text.size() > 20000) text.resize(20000);

        MaskSession session(PiiMasker::parseTerms(s_.protectedTerms ? s_.protectedTerms() : std::vector<std::string>{}),
        s_.settings.piiLevel == "standard" ? MaskLevel::Standard : MaskLevel::Strict);

        const std::string masked = session.mask(text);
        json counts = json::object();
        for (const auto& kv : session.counts()) counts[kv.first] = kv.second;
        r = json{{"masked", masked}, {"counts", counts}, {"total", session.total()}};
        return true;
    }

    if (op == "models_check") {
        if (!s_.rawRemote) { r = err("remote backend is not configured (set NEBIUS_API_KEY)"); return true; }
        const json list = s_.rawRemote->listModels();
        if (list.contains("error")) { r = err("could not list models: " + list["error"].get<std::string>()); return true; }
        std::set<std::string> have;
        if (list.contains("data") && list["data"].is_array())
            for (const auto& m : list["data"]) if (m.is_object() && m.contains("id") && m["id"].is_string()) have.insert(m["id"].get<std::string>());
        json cfg = json::object();
        bool all = true;
        for (auto [role, id] : {std::pair<const char*, std::string>{"nano", s_.settings.modelNano},
                                std::pair<const char*, std::string>{"super", s_.settings.modelSuper},
                                std::pair<const char*, std::string>{"ultra", s_.settings.modelUltra}}) {
            const bool ok = have.count(id) > 0;
            all = all && ok;
            cfg[role] = {{"id", id}, {"available", ok}};
        }
        r = json{{"models", cfg}, {"all_available", all}, {"catalog_size", have.size()}};
        return true;
    }

    if (mutating.count(op)) {
        if (s_.settings.demoMode) { r = err("disabled in demo mode"); return true; }

        if (op == "backend_set") {
            const std::string mode = strOf(b, "backend");
            if (mode == "remote") {
                if (!s_.remote || !s_.remote->available()) { r = err("remote backend is not configured (set NEBIUS_API_KEY)"); return true; }
                s_.llm->set(s_.remote, "remote");
            } else if (mode == "local") {
                if (!s_.local || !s_.local->available()) { r = err("no local model is loaded; download and load one in Settings first"); return true; }
                s_.llm->set(s_.local, "local");
            } else if (mode == "off") {
                s_.llm->set(&nullLlm(), "off");
            } else { r = err("unknown backend"); return true; }
            r = backend();
            return true;
        }

        if (op == "model_download" || op == "model_cancel") {
            const Slot sl = slot(strOf(b, "what"));
            if (sl.id.empty()) { r = err("unknown model slot (use llm, ocr or mmproj)"); return true; }
            if (!s_.downloader) { r = err("downloads are not available"); return true; }
            if (op == "model_cancel") { s_.downloader->cancel(sl.id); r = json::object(); return true; }
            if (sl.url.empty()) { r = err("set the download URL in .env first (LOCAL_MODEL_URL / OCR_MODEL_URL / OCR_MMPROJ_URL)"); return true; }
            std::string e;
            if (!s_.downloader->start({sl.id, sl.url, sl.path, sl.sha}, &e)) { r = err(e); return true; }
            r = json{{"downloads", s_.downloader->status()}};
            return true;
        }

        if (op == "model_load") {
            if (!s_.loadLocal) { r = err("this build has no local model support (rebuild with -DMG_LOCAL_LLM=ON)"); return true; }
            const Slot m = slot("llm");
            std::error_code ec;
            if (!fs::exists(m.path, ec)) { r = err("the model file is missing: download it first"); return true; }
            if (!s_.loadLocal(m.path)) { r = err("the model failed to load"); return true; }
            const Slot o = slot("ocr"), p = slot("mmproj");
            if (s_.setOcr && fs::exists(o.path, ec) && fs::exists(p.path, ec)) s_.setOcr(o.path, p.path);
            r = backend();
            return true;
        }
    }
    return false;
}

} // namespace mary
