cmake_minimum_required(VERSION 3.16)
project(MaryGuardian LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# MSVC флаги (вместо -Wall -Wextra):
if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX NOGDI)
else()
  add_compile_options(-Wall -Wextra -Werror=return-type -fstack-protector-strong)
endif()

option(MG_BUILD_APP   "Build the MaryGuardian application (needs MuPDF + SQLite3)" ON)
option(MG_LOCAL_LLM   "Optional offline Qwen/OCR through llama.cpp (needs third_party/llama.cpp built)" OFF)
option(MG_BUILD_TESTS "Build unit and integration tests" ON)

# Встраивание веб-ассетов:
file(GLOB_RECURSE MG_WEB_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/web/*)
set(MG_WEB_CPP ${CMAKE_BINARY_DIR}/gen/WebAssets.cpp)

add_custom_command(OUTPUT ${MG_WEB_CPP}
  COMMAND ${CMAKE_COMMAND} -DWEB_DIR=${CMAKE_SOURCE_DIR}/web -DOUT=${MG_WEB_CPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/EmbedWeb.cmake
  DEPENDS ${MG_WEB_FILES} ${CMAKE_SOURCE_DIR}/cmake/EmbedWeb.cmake
  COMMENT "Embedding web/ into the binary")

list(APPEND APP_SOURCES ${MG_WEB_CPP})

# ---------------------------------------------------------------------------
# Windows: vcpkg ставит mupdf в installed/x64-windows-static/
#
# ВАЖНО:
# Этот блок должен быть после option(MG_BUILD_APP ...) и до if(MG_BUILD_APP),
# иначе MG_BUILD_APP ещё не определён и условие всегда ложно.
# ---------------------------------------------------------------------------
if(WIN32 AND MG_BUILD_APP)
  find_path(MUPDF_INCLUDE_DIRS NAMES mupdf/fitz.h
    PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")

  if(NOT MUPDF_INCLUDE_DIRS)
    message(FATAL_ERROR
      "MuPDF (vcpkg) include dir not found. Expected mupdf/fitz.h in "
      "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
  endif()

  # Все библиотеки MuPDF + его зависимости (статические)
  set(MUPDF_LIBS
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/mupdf.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/mupdf-third.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/freetype.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/harfbuzz.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/openjp2.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/jbig2dec.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libjpeg.lib"
    "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/zlib.lib"
  )

  set(MUPDF_STATIC_LDFLAGS ${MUPDF_LIBS})

  # Чтобы последующий pkg-config/manual поиск не перезаписал Windows-конфигурацию.
  set(MUPDF_FOUND TRUE)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 QUIET) # иначе берётся third_party/nlohmann/json.hpp

# ---------------------------------------------------------------------------
# SQLite3: найти и создать compatibility target
# ---------------------------------------------------------------------------
find_package(SQLite3)
if(NOT SQLite3_FOUND)
  message(FATAL_ERROR "SQLite3 not found. Install libsqlite3-dev or sqlite3 via Homebrew.")
endif()

if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
  set_target_properties(SQLite3::SQLite3 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${SQLite3_LIBRARIES}"
  )
endif()

include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Новый слой: конфигурация, маскирование PII, клиент Nemotron, шлюз Guardian,
# загрузчик, уточнение полей.
# Не зависит ни от MuPDF, ни от llama.cpp — собирается и тестируется за секунды.
# ---------------------------------------------------------------------------
add_library(mg_core STATIC
  core/Config.cpp
  core/PiiMasker.cpp
  core/RemoteLlm.cpp
  core/GuardedLlm.cpp
  core/Downloader.cpp
  core/FieldRefiner.cpp)

target_link_libraries(mg_core PUBLIC CURL::libcurl Threads::Threads)

if(nlohmann_json_FOUND)
  target_link_libraries(mg_core PUBLIC nlohmann_json::nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# Тесты
# ---------------------------------------------------------------------------
if(MG_BUILD_TESTS)
  enable_testing()

  add_executable(sha256_test tests/sha256_test.cpp)
  add_executable(pii_test tests/pii_test.cpp)
  target_link_libraries(pii_test mg_core)

  add_executable(support_test tests/support_test.cpp)
  target_link_libraries(support_test mg_core)

  add_executable(guarded_e2e_test tests/guarded_e2e_test.cpp)
  target_link_libraries(guarded_e2e_test mg_core)

  add_executable(refiner_test tests/refiner_test.cpp)
  target_link_libraries(refiner_test mg_core)

  add_executable(pii_strict_test tests/pii_strict_test.cpp)
  target_link_libraries(pii_strict_test mg_core)

  add_test(NAME pii_strict COMMAND pii_strict_test)

  # Тесты, которым нужна реальная БД проекта (core/Db.cpp из вашего репозитория).
  if(SQLite3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/core/Db.cpp AND EXISTS ${CMAKE_SOURCE_DIR}/core/Extract.cpp)
    add_library(mg_data STATIC core/Db.cpp core/Extract.cpp core/Corrections.cpp)
    target_link_libraries(mg_data PUBLIC SQLite3::SQLite3)

    if(nlohmann_json_FOUND)
      target_link_libraries(mg_data PUBLIC nlohmann_json::nlohmann_json)
    endif()

    add_executable(corrections_test tests/corrections_test.cpp)
    target_link_libraries(corrections_test mg_data)

    add_executable(api_extras_test tests/api_extras_test.cpp server/ApiExtras.cpp)
    target_link_libraries(api_extras_test mg_core mg_data)

    add_library(mg_consistency STATIC core/Consistency.cpp)
    target_link_libraries(mg_consistency PUBLIC mg_data mg_core)

    add_executable(consistency_test tests/consistency_test.cpp)
    target_link_libraries(consistency_test mg_consistency)

    add_test(NAME consistency COMMAND consistency_test)
  endif()

  add_test(NAME sha256 COMMAND sha256_test)
  add_test(NAME pii    COMMAND pii_test)

  # Остальные тесты запускаются скриптом: он поднимает mock-API и локальный
  # HTTP-сервер для проверки загрузчика.
  add_test(NAME integration COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_tests.sh ${CMAKE_BINARY_DIR})
endif()

# ---------------------------------------------------------------------------
# Приложение
# ---------------------------------------------------------------------------
if(MG_BUILD_APP)
  # ------ check MuPDF
  #
  # Если MuPDF уже найден через Windows/vcpkg-блок выше, не запускаем
  # повторный поиск, чтобы не перезаписать MUPDF_STATIC_LDFLAGS.
  if(NOT MUPDF_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MUPDF QUIET mupdf)
  endif()

  # ------ if not find MuPDF
  if(NOT MUPDF_FOUND)
    message(STATUS "pkg-config mupdf not found, trying manual search...")

    find_path(MUPDF_INCLUDE_DIRS
      NAMES mupdf/fitz.h
      PATHS
        /opt/homebrew/include
        /opt/homebrew/opt/mupdf/include
        /usr/local/include
        /usr/include                  # <-- Добавлено для Ubuntu/Debian
    )

    find_library(MUPDF_LIBRARY
      NAMES mupdf
      PATHS
        /opt/homebrew/lib
        /opt/homebrew/opt/mupdf/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu     # <-- Добавлено для Ubuntu/Debian
        /usr/lib
    )

    find_library(MUPDF_THIRD_LIBRARY
      NAMES mupdf-third
      PATHS
        /opt/homebrew/lib
        /opt/homebrew/opt/mupdf/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu     # <-- Добавлено для Ubuntu/Debian
        /usr/lib
    )

    if(NOT MUPDF_INCLUDE_DIRS OR NOT MUPDF_LIBRARY)
      message(FATAL_ERROR
        "MuPDF not found.\n"
        "macOS: brew install mupdf pkg-config\n"
        "Ubuntu/Debian: sudo apt install libmupdf-dev pkg-config\n"
        " configure with -DMG_BUILD_APP=OFF to build only core/tests."
      )
    endif()

    set(MUPDF_STATIC_LDFLAGS ${MUPDF_LIBRARY})

    if(MUPDF_THIRD_LIBRARY)
      list(APPEND MUPDF_STATIC_LDFLAGS ${MUPDF_THIRD_LIBRARY})
    endif()

    # Для статической линковки на macOS нужны зависимости MuPDF из Homebrew
    if(APPLE AND MUPDF_LIBRARY MATCHES "\\.a$")
      foreach(dep freetype harfbuzz openjp2 jpeg jbig2dec z)
        find_library(MUPDF_DEP_${dep}
          NAMES ${dep}
          PATHS
            /opt/homebrew/lib
            /opt/homebrew/opt/${dep}/lib
            /usr/local/lib
        )

        if(MUPDF_DEP_${dep})
          list(APPEND MUPDF_STATIC_LDFLAGS ${MUPDF_DEP_${dep}})
        endif()
      endforeach()
    endif()

    # Для статической линковки на Linux (Ubuntu) нужны системные зависимости
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MUPDF_LIBRARY MATCHES "\\.a$")
      foreach(dep z jpeg freetype openjp2 jbig2dec harfbuzz png mujs gumbo)
        find_library(MUPDF_DEP_${dep} NAMES ${dep})
        if(MUPDF_DEP_${dep})
          list(APPEND MUPDF_STATIC_LDFLAGS ${MUPDF_DEP_${dep}})
        endif()
      endforeach()
    endif()

    message(STATUS "Manual MuPDF include dirs: ${MUPDF_INCLUDE_DIRS}")
    message(STATUS "Manual MuPDF libraries: ${MUPDF_STATIC_LDFLAGS}")
  endif()

  # Debian/Ubuntu: mupdf.pc не перечисляет harfbuzz, а libmupdf.a его требует
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(HARFBUZZ QUIET harfbuzz)
  endif()

  # Если MuPDF найден через pkg-config, но статические флаги не были заданы явно.
  if(NOT MUPDF_STATIC_LDFLAGS)
    set(MUPDF_STATIC_LDFLAGS ${MUPDF_LDFLAGS})
  endif()

  list(APPEND APP_SOURCES
    main.cpp
    core/PdfToolEngine.cpp core/DocReader.cpp core/Guardian.cpp core/Verifier.cpp
    core/Db.cpp core/Extract.cpp core/Corrections.cpp core/Indexer.cpp core/Library.cpp
    core/ClassificationRules.cpp
    core/Consistency.cpp core/Xray.cpp core/XrayDoc.cpp
    task/BookkeeperTask.cpp
    server/HttpServer.cpp server/ApiExtras.cpp)

  if(MG_LOCAL_LLM)
    list(APPEND APP_SOURCES core/ModelRuntime.cpp core/OcrEngine.cpp core/LocalLlm.cpp)
  endif()

  add_executable(MaryGuardian ${APP_SOURCES})

  target_include_directories(MaryGuardian PRIVATE ${MUPDF_INCLUDE_DIRS})

  # Include для server/
  target_include_directories(MaryGuardian PRIVATE ${CMAKE_SOURCE_DIR}/server)

  target_link_libraries(MaryGuardian PRIVATE
    mg_core
    SQLite3::SQLite3
    ${MUPDF_STATIC_LDFLAGS}
    ${HARFBUZZ_LDFLAGS}
  )

  if(MG_LOCAL_LLM)
    set(LLAMA_DIR ${CMAKE_SOURCE_DIR}/third_party/llama.cpp)

    target_compile_definitions(MaryGuardian PRIVATE MG_LOCAL_LLM=1)

    target_include_directories(MaryGuardian PRIVATE
      ${LLAMA_DIR}/include
      ${LLAMA_DIR}/ggml/include
      ${LLAMA_DIR}/common
      ${LLAMA_DIR}/tools/mtmd
    )

    target_link_directories(MaryGuardian PRIVATE ${LLAMA_DIR}/build/bin)

    target_link_libraries(MaryGuardian PRIVATE
      llama
      llama-common
      ggml
      ggml-base
      ggml-cpu
      mtmd
    )

    set_target_properties(MaryGuardian PROPERTIES BUILD_RPATH ${LLAMA_DIR}/build/bin)

    if(APPLE)
      target_link_libraries(MaryGuardian PRIVATE
        ggml-metal
        "-framework Accelerate"
        "-framework Metal"
        "-framework MetalKit"
        "-framework Foundation"
      )
    endif()
  endif()

  install(TARGETS MaryGuardian RUNTIME DESTINATION bin)
  install(DIRECTORY web/ DESTINATION share/maryguardian/web)

  # В конце блока `if(MG_BUILD_APP)` (после `install(...)`), т.к. здесь уже известны флаги MuPDF:
  if(MG_BUILD_TESTS)
    foreach(t xray_test demo_data_test)
      add_executable(${t} tests/${t}.cpp core/Xray.cpp core/Verifier.cpp)
      target_include_directories(${t} PRIVATE ${MUPDF_INCLUDE_DIRS})
      target_link_libraries(${t} PRIVATE mg_core ${MUPDF_STATIC_LDFLAGS} ${HARFBUZZ_LDFLAGS})
      add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(xray_doc_test
      tests/xray_doc_test.cpp
      core/XrayDoc.cpp
      core/Xray.cpp
      core/Verifier.cpp
      core/Consistency.cpp
    )

    target_include_directories(xray_doc_test PRIVATE ${MUPDF_INCLUDE_DIRS})

    target_link_libraries(xray_doc_test PRIVATE
      mg_data
      mg_core
      ${MUPDF_STATIC_LDFLAGS}
      ${HARFBUZZ_LDFLAGS}
    )

    add_test(NAME xray_doc COMMAND xray_doc_test)
  endif()

  # ---------------------------------------------------------------------------
  # Windows: манифест + библиотеки.
  #
  # ВАЖНО: этот блок должен быть после add_executable(MaryGuardian),
  # иначе CMake выдаст:
  # "Cannot specify sources for target MaryGuardian which is not built by this project".
  # ---------------------------------------------------------------------------
  if(WIN32)
    target_sources(MaryGuardian PRIVATE packaging/app.rc)

    if(MSVC)
      target_link_options(MaryGuardian PRIVATE /MANIFEST:NO)
    endif()

    target_link_libraries(MaryGuardian PRIVATE ws2_32 shell32)
  endif()
endif()