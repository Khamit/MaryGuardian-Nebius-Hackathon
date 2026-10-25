#pragma once
// Слой переносимости Windows / macOS / Linux. Header-only: в CMake ничего добавлять не нужно
// (для MinGW допишите ws2_32 и shell32 в target_link_libraries; MSVC подхватит #pragma comment).
//
// Что здесь собрано (всё, что раньше было POSIX-специфичным в main.cpp / HttpServer.cpp / Guardian.cpp / GuardedLlm.cpp):
//   сокеты (Winsock) · localtime_r · /dev/urandom · sigaction/isatty · открыть браузер/папку · папка данных пользователя
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <shellapi.h>
  #include <io.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
  #endif
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

namespace mary { namespace plat {

// ---------------------------------------------------------------------------
// Сокеты
// ---------------------------------------------------------------------------
#ifdef _WIN32
using sock_t = SOCKET;
inline constexpr sock_t kInvalidSock = INVALID_SOCKET;
using sock_len_t = int;

inline bool netInit() {                                       // WSAStartup ровно один раз
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
inline void closeSock(sock_t s) { ::closesocket(s); }
inline int  recvSome(sock_t s, char* b, int n) { return ::recv(s, b, n, 0); }
inline int  sendSome(sock_t s, const char* b, int n) { return ::send(s, b, n, 0); }
inline void setRecvTimeoutMs(sock_t s, int ms) {
    DWORD t = (DWORD)ms;                                      // на Windows таймаут — DWORD в мс, а не timeval
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}
// На Windows SO_REUSEADDR позволяет ДРУГОМУ процессу занять тот же порт; нужен именно эксклюзивный режим.
inline void setExclusiveBind(sock_t s) {
    BOOL on = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
}
inline int waitReadable(sock_t s, int ms) {                   // >0 — есть данные/соединение, 0 — таймаут
    WSAPOLLFD p{};
    p.fd = s;
    p.events = POLLRDNORM;
    return ::WSAPoll(&p, 1, ms);
}
#else
using sock_t = int;
inline constexpr sock_t kInvalidSock = -1;
using sock_len_t = socklen_t;

inline bool netInit() { return true; }
inline void closeSock(sock_t s) { ::close(s); }
inline int recvSome(sock_t s, char* b, int n) {
    int r;
    do { r = (int)::recv(s, b, (size_t)n, 0); } while (r < 0 && errno == EINTR);
    return r;
}
inline int sendSome(sock_t s, const char* b, int n) {
    int r;
    do { r = (int)::send(s, b, (size_t)n, 0); } while (r < 0 && errno == EINTR);
    return r;
}
inline void setRecvTimeoutMs(sock_t s, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
inline void setExclusiveBind(sock_t s) {                      // на POSIX SO_REUSEADDR безопасен (быстрый перезапуск)
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
}
inline int waitReadable(sock_t s, int ms) {
    pollfd p{s, POLLIN, 0};
    return ::poll(&p, 1, ms);
}
#endif

// Отправить всё (или вернуть false при ошибке/обрыве).
inline bool sendAll(sock_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = sendSome(s, data.data() + off, (int)(data.size() - off));
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Время, случайность, консоль
// ---------------------------------------------------------------------------
inline std::tm localTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);                                     // аргументы в обратном порядке по сравнению с localtime_r
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// n случайных байт в hex. std::random_device: rand_s на MSVC, /dev/urandom или arc4random на macOS/Linux.
inline std::string randomHex(size_t nbytes) {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(nbytes * 2);
    for (size_t i = 0; i < nbytes; ++i) {
        const unsigned v = rd() & 0xFFu;
        out += hex[v >> 4];
        out += hex[v & 15];
    }
    return out;
}

inline void initConsoleUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);                              // кириллица в логах
    SetConsoleCP(CP_UTF8);
#endif
}

// ---------------------------------------------------------------------------
// Остановка по Ctrl+C / SIGTERM / закрытию окна консоли. flag сбрасывается в false.
// Интерактивного «quit» больше нет: на Windows getline не прерывается обработчиком Ctrl+C, а цикл ожидания — прерывается.
// ---------------------------------------------------------------------------
namespace detail { inline std::atomic<bool>*& stopFlag() { static std::atomic<bool>* f = nullptr; return f; } }

#ifdef _WIN32
inline BOOL WINAPI consoleCtrl(DWORD ev) {
    if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT || ev == CTRL_CLOSE_EVENT || ev == CTRL_SHUTDOWN_EVENT) {
        if (auto* f = detail::stopFlag()) f->store(false);
        if (ev == CTRL_CLOSE_EVENT || ev == CTRL_SHUTDOWN_EVENT) Sleep(4000);   // дать main() корректно закрыть БД
        return TRUE;
    }
    return FALSE;
}
inline void installStopHandler(std::atomic<bool>& runningFlag) {
    detail::stopFlag() = &runningFlag;
    SetConsoleCtrlHandler(consoleCtrl, TRUE);
}
#else
inline void installStopHandler(std::atomic<bool>& runningFlag) {
    detail::stopFlag() = &runningFlag;
    struct sigaction sa{};
    sa.sa_handler = [](int) { if (auto* f = detail::stopFlag()) f->store(false); };   // store в atomic<bool> — async-signal-safe
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);                            // обрыв клиента не должен ронять процесс
}
#endif

// ---------------------------------------------------------------------------
// Папки и «открыть в системе»
// ---------------------------------------------------------------------------
// Где хранить БД, журналы и inbox по умолчанию. Двойной клик на macOS запускает процесс с cwd = ~, на Windows — с cwd = папка exe,
// поэтому относительный путь "mg_workspace" для установленного приложения не годится.
inline std::string userDataDir() {
    auto env = [](const char* k) -> std::string { const char* v = std::getenv(k); return v ? v : ""; };
#ifdef _WIN32
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) base = env("USERPROFILE");
    return base.empty() ? std::string("MaryGuardian") : base + "\\MaryGuardian";
#elif defined(__APPLE__)
    const std::string home = env("HOME");
    return home.empty() ? std::string("MaryGuardian") : home + "/Library/Application Support/MaryGuardian";
#else
    const std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return xdg + "/MaryGuardian";
    const std::string home = env("HOME");
    return home.empty() ? std::string("MaryGuardian") : home + "/.local/share/MaryGuardian";
#endif
}

// Строки приходят только от нас самих (http://127.0.0.1:<порт>, путь к inbox внутри userDataDir), не от пользователя запроса.
inline void openInSystem(const std::string& target) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + target + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + target + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

inline bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

}} // namespace mary::plat