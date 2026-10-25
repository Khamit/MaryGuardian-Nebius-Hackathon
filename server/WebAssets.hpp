#pragma once
// Веб-интерфейс, вшитый в бинарник (генерируется cmake/EmbedWeb.cmake из папки web/).
#include <cstddef>

namespace mary {

struct WebAsset {
    const char*          path;   // "/index.html", "/privacy.js", "/vendor/bootstrap-icons/bootstrap-icons.min.css", ...
    const unsigned char* data;   // после size байт стоит ещё один нулевой байт
    std::size_t          size;
    const char*          mime;
};

// nullptr, если такого файла нет. path начинается с "/".
const WebAsset* findWebAsset(const char* path);

} // namespace mary