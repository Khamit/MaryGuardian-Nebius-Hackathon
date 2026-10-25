#pragma once
// Вся конфигурация — из переменных окружения (и необязательного .env). Ключи в код не зашиваются.
// Приоритет: реальное окружение > .env > значения по умолчанию.
#include <map>
#include <string>
#include <vector>

namespace mary {

class EnvFile {
public:
    // Читает KEY=VALUE (комментарии #, кавычки, "export "). Отсутствие файла — не ошибка.
    explicit EnvFile(const std::string& path = ".env");
    std::string get(const std::string& key, const std::string& def = "") const;
    int         getInt(const std::string& key, int def) const;
    bool        getBool(const std::string& key, bool def) const;
    // Список через separator (по умолчанию ';'), пустые элементы отбрасываются.
    std::vector<std::string> getList(const std::string& key, char sep = ';') const;
private:
    std::map<std::string, std::string> file_;
};

struct Settings {
    // --- сервер ---
    int         port = 8080;
    std::string bindAddr = "127.0.0.1";          
    std::vector<std::string> allowedHosts;       // доп. значения заголовка Host (для публичного демо)
    bool        demoMode = false;                // демо: данные только синтетические, лимиты на дорогие вызовы

    std::string workspace; 
    std::string webDir;   // MG_WEB_DIR: править UI без пересборки

    // --- LLM-бэкенд ---
    std::string backend = "auto";                // auto | remote | local | off
    std::string apiKey;                          // NEBIUS_API_KEY
    std::string baseUrl = "https://api.tokenfactory.nebius.com/v1/";
    std::string modelNano  = "nvidia/NVIDIA-Nemotron-3-Nano-30B-A3B";   // массовая классификация
    std::string modelSuper = "nvidia/nemotron-3-super-120b-a12b";       // извлечение полей и черновики
    std::string modelUltra = "nvidia/Nemotron-3-Ultra-550b-a55b";       // вопросы по библиотеке
    int         contextTokens = 32000;           // консервативный бюджет промпта (сами модели держат 256K)
    int         timeoutSec = 120;
    int         maxRetries = 3;
    int         concurrency = 4;                 // параллельных вызовов при классификации
    int         reasoningHeadroom = 1536;        // запас токенов под «размышления» reasoning-моделей
    std::string extraBody;                       // JSON, подмешивается в тело запроса (например chat_template_kwargs)

    // --- приватность ---
    int         snippetChars = 1200;             // сколько символов документа вообще может уйти в облако
    int         maxRequestsPerHour = 2000;       // предохранитель кредитов
    int         maxPromptChars = 60000;
    std::vector<std::string> protectedTerms;     // PII_PROTECTED_TERMS="PERSON:Иван Петров;ADDRESS:ул. Ленина 5"
    std::string piiLevel = "strict";                 // PII_MASK_LEVEL: standard | strict (strict скрывает ещё компании, номера, ФИО, адреса)

    // --- локальные модели (необязательно) ---
    std::string localModelPath, localModelUrl, localModelSha256;
    std::string ocrModelPath,   ocrModelUrl,   ocrModelSha256;
    std::string ocrMmprojPath,  ocrMmprojUrl,  ocrMmprojSha256;

    int         watchIntervalSec = 10;           // 0 = только ручной Scan

    bool remoteConfigured() const { return !apiKey.empty(); }
    // Итог: "remote" | "local" | "off" (auto выбирает remote, если есть ключ).
    std::string resolvedBackend() const;

    static Settings load(const std::string& dotenvPath = ".env");
};

} // namespace mary
