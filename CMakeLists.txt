cmake_minimum_required(VERSION 3.20)
project(SteamCloudPatcher VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC 
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(version SHARED dll/version_dll.cpp)
target_link_libraries(version PRIVATE nlohmann_json::nlohmann_json Advapi32 Psapi minhook)
if(WIN32)
    target_compile_definitions(version PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()
if(MSVC)
    target_compile_options(version PRIVATE /W4 /permissive-)
else()
    target_compile_options(version PRIVATE -Wall -Wextra -Wpedantic)
endif()
