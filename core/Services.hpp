#pragma once
// Всё, что нужно новым HTTP-эндпоинтам, одним объектом (вместо глобальных переменных и россыпи параметров конструктора).
#include "Config.hpp"
#include "Corrections.hpp"
#include "Downloader.hpp"
#include "LlmSwitch.hpp"
#include "OutboundLog.hpp"
#include "RemoteLlm.hpp"
#include <functional>
#include <string>
#include <vector>

namespace mary {

class Consistency; 

struct Services {
    Settings      settings;
    std::string   workspace;

    LlmSwitch*    llm = nullptr;          // активный бэкенд (его видят Indexer/Library/BookkeeperTask)
    LlmClient*    remote = nullptr;       // GuardedLlm поверх RemoteLlm; nullptr — ключ не задан
    LlmClient*    local = nullptr;        // LocalLlm; nullptr — сборка без -DMG_LOCAL_LLM
    RemoteLlm*    rawRemote = nullptr;    // только для диагностики (/models); наружу не отдаётся
    OutboundLog*  outbound = nullptr;
    Downloader*   downloader = nullptr;
    Corrections*  corrections = nullptr;
    Consistency*  consistency = nullptr; 

    // Локальные модели (заполняются только при MG_LOCAL_LLM).
    std::function<bool(const std::string& modelPath)>                    loadLocal;
    std::function<bool()>                                                localLoaded;
    std::function<void(const std::string& model, const std::string& mmproj)> setOcr;

    std::function<nlohmann::json()> watchInfo; // для живого индикатора «приложение работает»
    std::function<void()> afterDemoLoad; // добавить защищённый термин для демо
    std::function<bool(const std::string& key, std::string* err)> setApiKey; // ключ из Settings без перезапуска

    // Защищённые термины: env + поле профиля. Читается на каждый запрос, чтобы правки применялись сразу.
    std::function<std::vector<std::string>()> protectedTerms;
};

} // namespace mary
