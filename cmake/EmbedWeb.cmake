# Встраивает все файлы из WEB_DIR в один .cpp, чтобы бинарник не зависел от папки web/ на диске.
# Запускается на этапе сборки: cmake -DWEB_DIR=<dir> -DOUT=<file.cpp> -P EmbedWeb.cmake

if(NOT WEB_DIR OR NOT OUT)
    message(FATAL_ERROR "usage: cmake -DWEB_DIR=<dir> -DOUT=<file.cpp> -P EmbedWeb.cmake")
endif()

file(GLOB_RECURSE FILES RELATIVE "${WEB_DIR}" "${WEB_DIR}/*")
list(SORT FILES)

function(mime_for ext out)
    set(m "application/octet-stream")
    if(ext STREQUAL ".html")
        set(m "text/html; charset=utf-8")
    elseif(ext STREQUAL ".js")
        set(m "text/javascript; charset=utf-8")
    elseif(ext STREQUAL ".mjs")
        set(m "text/javascript; charset=utf-8")
    elseif(ext STREQUAL ".css")
        set(m "text/css; charset=utf-8")
    elseif(ext STREQUAL ".json")
        set(m "application/json")
    elseif(ext STREQUAL ".svg")
        set(m "image/svg+xml")
    elseif(ext STREQUAL ".png")
        set(m "image/png")
    elseif(ext STREQUAL ".ico")
        set(m "image/x-icon")
    elseif(ext STREQUAL ".woff2")
        set(m "font/woff2")
    elseif(ext STREQUAL ".woff")
        set(m "font/woff")
    elseif(ext STREQUAL ".txt")
        set(m "text/plain; charset=utf-8")
    endif()
    set(${out} "${m}" PARENT_SCOPE)
endfunction()

set(BODY "")
set(TABLE "")
set(i 0)

foreach(rel IN LISTS FILES)
    file(READ "${WEB_DIR}/${rel}" HEX_CONTENTS HEX)
    string(LENGTH "${HEX_CONTENTS}" HEX_LEN)
    math(EXPR SIZE "${HEX_LEN} / 2")
    
    set(ARR "")
    set(pos 0)
    while(pos LESS HEX_LEN)
        string(SUBSTRING "${HEX_CONTENTS}" ${pos} 4096 CHUNK)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CHUNK "${CHUNK}")
        string(APPEND ARR "${CHUNK}\n")
        math(EXPR pos "${pos} + 4096")
    endwhile()
    
    get_filename_component(ext "${rel}" LAST_EXT)
    string(TOLOWER "${ext}" ext)
    mime_for("${ext}" MIME)
    
    string(APPEND BODY "static const unsigned char kAsset${i}[] = {\n${ARR}0x00};\n")
    string(APPEND TABLE "  {\"/${rel}\", kAsset${i}, ${SIZE}u, \"${MIME}\"},\n")
    math(EXPR i "${i} + 1")
endforeach()

file(WRITE "${OUT}.tmp"
"// СГЕНЕРИРОВАНО cmake/EmbedWeb.cmake — не редактировать вручную.
#include \"WebAssets.hpp\"
#include <cstring>

namespace mary {
namespace {
${BODY}
const WebAsset kAssets[] = {
${TABLE}  {nullptr, nullptr, 0u, nullptr}
};
} // namespace

const WebAsset* findWebAsset(const char* path) {
    for (const WebAsset* a = kAssets; a->path; ++a) {
        if (std::strcmp(a->path, path) == 0) return a;
    }
    return nullptr;
}
} // namespace mary
")

execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")

list(LENGTH FILES N)
message(STATUS "EmbedWeb: ${N} files -> ${OUT}")
