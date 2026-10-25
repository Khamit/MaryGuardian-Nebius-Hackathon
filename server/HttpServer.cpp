#include <nlohmann/json.hpp>
#include "../core/Platform.hpp"
#include "../core/Safety.hpp"
#include "HttpServer.hpp"
#include "WebAssets.hpp"
#include "../core/Consistency.hpp"
#include "../core/XrayDoc.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <set>

namespace {
// Безопасные геттеры: неверный тип поля не должен ронять процесс.
std::string getStr(const nlohmann::json& j, const char* key, const std::string& def = "") {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

// Вшитый UI вместо чтения с диска:
bool loadWeb(const std::string& webDir, const std::string& rel, std::string& body, std::string& mime) {
    namespace fs = std::filesystem;
    // Режим разработки: файл с диска
    if (!webDir.empty() && rel.find("..") == std::string::npos) {
        fs::path root(webDir), file = root / rel.substr(1);
        if (mary::safety::isInsideRoot(file, root) && fs::is_regular_file(file)) {
            std::ifstream f(file, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            body = ss.str();
            const std::string e = file.extension().string();
            mime = e == ".css" ? "text/css" : e == ".js" ? "text/javascript"
                 : e == ".svg" ? "image/svg+xml" : e == ".woff2" ? "font/woff2"
                 : e == ".html" ? "text/html; charset=utf-8" : "application/octet-stream";
            return true;
        }
    }
    // Продакшен: вшитые ассеты
    if (const mary::WebAsset* a = mary::findWebAsset(rel.c_str())) {
        body.assign((const char*)a->data, a->size);
        mime = a->mime;
        return true;
    }
    return false;
}

} // namespace

HttpServer::HttpServer(const mary::Settings& cfg,
                       mary::Services& svc,
                       mary::BookkeeperTask& task,
                       Guardian& guardian,
                       mary::Indexer& indexer,
                       mary::Library& library,
                       const std::string& workspace)
        : port_(cfg.port),
      cfg_(cfg),
      svc_(svc),
      task_(task),
      guardian_(guardian),
      extras_(svc),
      indexer_(indexer),
      library_(library),
      workspace_(workspace) {
    token_ = mary::plat::randomHex(16);
}

bool HttpServer::start() {
    if (!mary::plat::netInit()) { std::cerr << "[HTTP] network init failed\n"; return false; }
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == mary::plat::kInvalidSock) { std::cerr << "[HTTP] socket failed\n"; return false; }
    mary::plat::setExclusiveBind(server_fd_);   // вместо SO_REUSEADDR

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    if (inet_pton(AF_INET, cfg_.bindAddr.c_str(), &addr.sin_addr) != 1) addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port_);

    if (::bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[HTTP] bind failed on port " << port_ << "\n";
        mary::plat::closeSock(server_fd_); 
        server_fd_ = mary::plat::kInvalidSock;
        return false;
    }
    if (::listen(server_fd_, 16) < 0) {
        std::cerr << "[HTTP] listen failed\n";
        mary::plat::closeSock(server_fd_); 
        server_fd_ = mary::plat::kInvalidSock;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&HttpServer::run, this);
    std::cout << "[HTTP] Listening on http://" << cfg_.bindAddr << ":" << port_ << "\n";
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (server_thread_.joinable()) server_thread_.join();
    if (server_fd_ != mary::plat::kInvalidSock) {
        mary::plat::closeSock(server_fd_);
        server_fd_ = mary::plat::kInvalidSock;
    }
    // Ждём активные запросы (до 3 с). Долгая генерация дождётся model.unload() через gen_mu_.
    for (int i = 0; i < 60 && active_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void HttpServer::run() {
    while (running_.load() && (!running_flag_ || running_flag_->load())) {
        if (mary::plat::waitReadable(server_fd_, 500) <= 0) continue;
        sockaddr_in client_addr{};
        mary::plat::sock_len_t len = sizeof(client_addr);
        mary::plat::sock_t fd = ::accept(server_fd_, (sockaddr*)&client_addr, &len);
        if (fd == mary::plat::kInvalidSock) continue;
        ++active_;                                   // до старта потока
        std::thread(&HttpServer::handleClient, this, fd).detach();
    }
}

static std::string headerValue(const std::string& headers, std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    std::istringstream is(headers); std::string line;
    while (std::getline(is, line)) {
        auto c = line.find(':'); if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == name) {
            std::string v = line.substr(c + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
    }
    return "";
}

static bool readRequest(mary::plat::sock_t fd, std::string& out) {
    mary::plat::setRecvTimeoutMs(fd, 5000);
    constexpr size_t MAX_REQ = 1 << 20;
    char buf[4096];
    size_t header_end = std::string::npos, content_len = 0;
    while (out.size() < MAX_REQ) {
        int n = mary::plat::recvSome(fd, buf, (int)sizeof buf);
        if (n <= 0) return false;
        out.append(buf, (size_t)n);
        if (header_end == std::string::npos && (header_end = out.find("\r\n\r\n")) != std::string::npos) {
            header_end += 4;
            std::string cl = headerValue(out.substr(0, header_end), "content-length");
            content_len = cl.empty() ? 0 : (size_t)std::strtoull(cl.c_str(), nullptr, 10);
            if (content_len > MAX_REQ) return false;
        }
        if (header_end != std::string::npos && out.size() >= header_end + content_len) return true;
    }
    return false;
}

void HttpServer::handleClient(mary::plat::sock_t fd) {
    struct Guard { std::atomic<int>& n; ~Guard() { --n; } } guard{active_};

    std::string request;
    if (!readRequest(fd, request)) { mary::plat::closeSock(fd); return; }

    std::string response;
    try {
        auto hdr_end = request.find("\r\n\r\n");
        std::string headers = request.substr(0, hdr_end);
        std::string body    = request.substr(hdr_end + 4);

        std::istringstream iss(request);
        std::string method, raw_path, version;
        iss >> method >> raw_path >> version;
        std::string path = raw_path.substr(0, raw_path.find('?'));

        const std::string p = std::to_string(port_);
        const std::string host = headerValue(headers, "host");
        const std::string origin = headerValue(headers, "origin");

        std::set<std::string> hosts = {"localhost:" + p, "127.0.0.1:" + p};
        for (const auto& h : cfg_.allowedHosts) hosts.insert(h);
        const bool host_success = hosts.count(host) > 0;
        bool origin_success = origin.empty();
        for (const auto& h : hosts) if (origin == "http://" + h || origin == "https://" + h) origin_success = true;

        const bool is_api = path.rfind("/api/", 0) == 0;
        const bool token_ok = !is_api || headerValue(headers, "x-auth-token") == token_;

        if (!host_success || !origin_success || !token_ok) {
            guardian_.record("http_rejected:" + path, false, "Invalid Host/Origin/token");
            response = httpResponse(403, "text/plain", "Forbidden");
        } else {
            response = route(method, path, body);
        }
    } catch (const std::exception& e) {
        std::cerr << "[HTTP] handler error: " << e.what() << "\n";
        response = httpResponse(500, "application/json",
                                nlohmann::json{{"ok", false}, {"error", "internal error"}}.dump());
    } catch (...) {
        response = httpResponse(500, "text/plain", "Internal Server Error");
    }
    mary::plat::sendAll(fd, response); 
    mary::plat::closeSock(fd);         
}

std::string HttpServer::route(const std::string& method,
                              const std::string& path,
                              const std::string& body) {
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string html, mime;
        if (loadWeb(cfg_.webDir, "/index.html", html, mime)) {
            // подставить __TOKEN__ как раньше
            for (size_t p = html.find("__TOKEN__"); p != std::string::npos;
                p = html.find("__TOKEN__", p + token_.size()))
                html.replace(p, 9, token_);
            return httpResponse(200, mime, html);
        }
        return httpResponse(404, "text/plain", "Not Found");
    }

    if (method == "GET" && path.rfind("/static/", 0) == 0) {
        std::string body, mime;
        if (loadWeb(cfg_.webDir, "/" + path.substr(8), body, mime))
            return httpResponse(200, mime, body);
        return httpResponse(404, "text/plain", "Not Found");
    }

    auto parseBody = [&]() -> nlohmann::json {
        try { return body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body); }
        catch (...) { return nlohmann::json::object(); }
    };

    if (path == "/api/backend"  && method == "GET") return jsonOk(extras_.backend());
    if (path == "/api/outbound" && method == "GET") return jsonOk(extras_.outbound());
    if (cfg_.demoMode && method == "POST" && path == "/api/export") return jsonError("disabled in demo mode");

    if (path == "/api/status" && method == "GET") return apiStatus();   // свой try_lock
    if (path == "/api/audit"  && method == "GET") return apiAudit();    // замок внутри Guardian
    if (method == "POST" && path.rfind("/api/lib/", 0) == 0) return apiLib(path.substr(9), parseBody());

    if (method == "POST" && path.rfind("/api/", 0) == 0) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (path == "/api/upload")  return apiUpload(parseBody());
        if (path == "/api/draft")   return apiDraft(parseBody());
        if (path == "/api/approve") return apiApprove();
        if (path == "/api/edit")    return apiEdit(parseBody());
        if (path == "/api/export")  return apiExport(parseBody());
    }
    return httpResponse(404, "text/plain", "Not Found");
}

// =========================================================
// API handlers
// =========================================================
std::string HttpServer::apiLib(const std::string& op, const nlohmann::json& b) {
    auto wrap = [&](const nlohmann::json& r) {
        if (r.is_object() && r.contains("error") && r["error"].is_string())
            return jsonError(r["error"].get<std::string>());
        return jsonOk(r);
    };
    auto id = [&]() -> long long {
        return (b.is_object() && b.contains("id") && b["id"].is_number_integer()) ? b["id"].get<long long>() : 0;
    };
    
    nlohmann::json extra;
    if (extras_.handle(op, b, extra)) return wrap(extra);
        if (op == "risk_overview") return jsonOk(svc_.consistency ? svc_.consistency->overview() : nlohmann::json::object());
    if (op == "doc_flags")     return jsonOk({{"flags", svc_.consistency ? svc_.consistency->flags(id()) : nlohmann::json::array()}});
    if (op == "payee_confirm") {
        if (!svc_.consistency) return jsonError("consistency checks are not enabled");
        auto r = svc_.consistency->confirmPayee(id());
        if (r.contains("error")) return wrap(r);
        guardian_.record("organize:payee_confirm", true, "doc id=" + std::to_string(id()));   // подтверждение пользователя попадает в аудит
        return jsonOk(r);
    }
    if (op == "xray") return wrap(xrayJson(b));

    if (op == "scan") {
        bool started = indexer_.start();
        bool already_running = !started && indexer_.isRunning();
        return jsonOk({
            {"started", started},
            {"already_running", already_running}
        });
    }
    if (op == "scan_status")       return jsonOk(indexer_.status());
    if (op == "list")              return wrap(library_.list(b));
    if (op == "doc")               return wrap(library_.doc(id()));
    if (op == "update")            return wrap(library_.updateDoc(b));
    if (op == "collections")       return wrap(library_.collections());
    if (op == "collection_save")   return wrap(library_.saveCollection(b));
    if (op == "collection_delete") return wrap(library_.deleteCollection(b));
    if (op == "resort")            return wrap(library_.resort());
    if (op == "companies")         return wrap(library_.companies());
    if (op == "company_rename")    return wrap(library_.renameCompany(b));
    if (op == "profile_get")       return wrap(library_.profileGet());
    if (op == "profile_set")       return wrap(library_.profileSet(b));
    if (op == "cleanup")           return wrap(library_.cleanup());
    if (op == "ask")               return wrap(library_.ask(getStr(b, "question")));
    return httpResponse(404, "text/plain", "Not Found");
}

// Не блокируемся на mutex_: пока идёт долгая генерация, отвечаем "generating".
std::string HttpServer::apiStatus() {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return jsonOk({{"step","generating"}, {"model_loaded", svc_.llm->available()},
                                        {"pii_warning", false}, {"pending_approval", false}, {"error", ""}});

    const auto& s = task_.state();
    static const char* step_names[] = {"idle","extracted","draft_ready","approved","error"};
    nlohmann::json j;
    j["step"]             = step_names[static_cast<int>(s.step)];
    j["model_loaded"]     = svc_.llm->available();
    j["pii_warning"]      = s.pii_warning;
    j["pending_approval"] = guardian_.isPending();
    j["error"]            = s.error;
    return jsonOk(j);
}

std::string HttpServer::apiUpload(const nlohmann::json& body) {
    std::vector<std::string> paths;
    if (body.contains("paths") && body["paths"].is_array())
        for (const auto& p : body["paths"])
            if (p.is_string())   // только имя файла, всегда из inbox
                paths.push_back(workspace_ + "/inbox/" + std::filesystem::path(p.get<std::string>()).filename().string());
    if (paths.empty()) return jsonError("paths is empty");

    if (!task_.extract(paths)) return jsonError(task_.state().error);

    return jsonOk({
        {"full_text", task_.state().extracted},
        {"length", task_.state().extracted.size()}
    });
}

std::string HttpServer::apiDraft(const nlohmann::json& body) {
    std::string instruction = getStr(body, "instruction");
    if (!task_.draft(instruction)) return jsonError(task_.state().error);
    return jsonOk({
        {"draft",       task_.state().draft},
        {"pii_warning", task_.state().pii_warning},
        {"truncated",   task_.state().truncated}
    });
}

std::string HttpServer::apiApprove() {
    task_.approve();
    return jsonOk({{"step", "approved"}});
}

std::string HttpServer::apiEdit(const nlohmann::json& body) {
    std::string draft = getStr(body, "draft");
    if (draft.empty()) return jsonError("draft is empty");
    task_.editAndApprove(draft);
    return jsonOk({{"step", "approved"}, {"pii_warning", task_.state().pii_warning}});
}

std::string HttpServer::apiExport(const nlohmann::json& body) {
    std::string name = getStr(body, "name", "draft.md");
    name = std::filesystem::path(name).filename().string();
    for (char& c : name) if (!std::isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') c = '_';
    if (name.empty() || name[0] == '.') name = "draft.md";
    std::string output_path = workspace_ + "/output/" + name;
    if (!task_.exportTo(output_path)) return jsonError("Not approved, blocked by Guardian, or write error.");
    return jsonOk({{"saved_to", output_path}});
}

std::string HttpServer::apiAudit() {
    return jsonOk({{"logs", guardian_.logJson()}});
}

// =========================================================
// Helpers
// =========================================================
nlohmann::json HttpServer::xrayJson(const nlohmann::json& b) {
    const long long docId = (b.is_object() && b.contains("id") && b["id"].is_number_integer()) ? b["id"].get<long long>() : 0;
    const int page = (b.is_object() && b.contains("page") && b["page"].is_number_integer()) ? b["page"].get<int>() : 0;
    const auto d = library_.doc(docId);
    if (d.contains("error")) return d;
    const std::string name = std::filesystem::path(getStr(d["doc"], "filename")).filename().string();
    auto v = guardian_.checkFile("read", workspace_ + "/inbox/" + name, false, /*log_allowed=*/false);
    if (!v.allowed) return nlohmann::json{{"error", v.reason}};
    return mary::xray::viewForDocument(d["doc"], workspace_ + "/inbox", page, svc_.consistency,
                                       svc_.protectedTerms ? svc_.protectedTerms() : std::vector<std::string>{},
                                       cfg_.piiLevel == "standard" ? mary::MaskLevel::Standard : mary::MaskLevel::Strict);
}

std::string HttpServer::loadHtml() const {
    std::ifstream f2(workspace_ + "/web/index.html");
    if (f2.is_open()) {
        std::stringstream ss;
        ss << f2.rdbuf();
        return ss.str();
    }
    return R"(<!DOCTYPE html><html><head><meta charset="utf-8">
            <title>Sift</title></head>
            <body style="font-family: system-ui, sans-serif; padding: 40px; color: #333; background: #f5f5f5;">
            <h2>index.html not found</h2>
            <p>Expected at: <code>)" + workspace_ + R"(/web/index.html</code></p>
            <p>Run <code>make install-web</code>.</p>
            </body></html>)";
}

std::string HttpServer::jsonOk(const nlohmann::json& data) {
    nlohmann::json j = {{"ok", true}};
    if (data.is_object())
        for (auto& [k, v] : data.items()) j[k] = v;
    return httpResponse(200, "application/json", j.dump());
}

std::string HttpServer::jsonError(const std::string& msg) {
    return httpResponse(200, "application/json",
                        nlohmann::json{{"ok", false}, {"error", msg}}.dump());
}

std::string HttpServer::httpResponse(int code,
                                     const std::string& content_type,
                                     const std::string& body) {
    static const std::unordered_map<int,std::string> phrases = {
        {200,"OK"},{400,"Bad Request"},{403,"Forbidden"},{404,"Not Found"},{500,"Internal Server Error"}
    };
    auto it = phrases.find(code);
    std::string phrase = it != phrases.end() ? it->second : "Unknown";

    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << phrase << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return ss.str();
}