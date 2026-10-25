cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)

project(Clipped VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enlazar el runtime de C/C++ de forma estática (/MT en Release, /MTd en Debug)
# para que Clipped.exe sea 100% autónomo y no requiera vcruntime140.dll en otras PCs
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

include(FetchContent)

# 1. Descargar stb (para comprimir/descomprimir píxeles crudos a PNG en memoria)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_SHALLOW    TRUE
)

# 2. Descargar SQLite3 Amalgamation oficial
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 3. Descargar Dear ImGui (Win32 + DirectX 11)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb sqlite3 imgui)

add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3_lib PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_DEPRECATED
)
if(MSVC)
    # Desactivar advertencias de terceros dentro de la amalgamación C de SQLite3
    target_compile_options(sqlite3_lib PRIVATE /W0)
endif()

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
if(MSVC)
    target_compile_options(imgui_lib PRIVATE /W0)
endif()

add_executable(Clipped WIN32
    src/main.cpp
    src/Clipboard.cpp
    src/Database.cpp
    src/resources.rc
)

if(MSVC)
    target_link_options(Clipped PRIVATE /ENTRY:mainCRTStartup)
endif()

target_include_directories(Clipped PRIVATE 
    src
    ${stb_SOURCE_DIR}
)

target_link_libraries(Clipped PRIVATE
    user32
    shell32
    advapi32
    d3d11
    dxgi
    dwmapi
    sqlite3_lib
    imgui_lib
)
