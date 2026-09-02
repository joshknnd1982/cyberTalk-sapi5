cmake_minimum_required(VERSION 3.15)
project(CyberTalkSAPI VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BUILD_X64 TRUE)
    message(STATUS "Building x64 (SAPI DLL only)")
else()
    set(BUILD_X64 FALSE)
    message(STATUS "Building x86 (host, SAPI DLL, configuration utility, tools)")
endif()

set(COMMON_DEFS _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# pipe client (shared by the SAPI DLL and the configuration utility)
# ---------------------------------------------------------------------------
add_library(pipe_client STATIC src/pipe_client.cpp)
target_include_directories(pipe_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pipe_client PUBLIC shlwapi advapi32 shell32)
target_compile_definitions(pipe_client PRIVATE ${COMMON_DEFS})

# ---------------------------------------------------------------------------
# SAPI 5 engine DLL (built for both architectures)
# ---------------------------------------------------------------------------
add_library(CyberTalkSAPI SHARED
    src/sapi_main.cpp
    src/com.cpp
    src/registry.cpp
    src/ISpDataKeyImpl.cpp
    src/IEnumSpObjectTokensImpl.cpp
    src/ISpTTSEngineImpl.cpp
    src/voice_token.cpp
    src/stl_text.cpp
    src/cybertalk_sapi.def
)
target_include_directories(CyberTalkSAPI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(CyberTalkSAPI PRIVATE pipe_client ole32 oleaut32 advapi32 shell32 shlwapi)
target_compile_definitions(CyberTalkSAPI PRIVATE ${COMMON_DEFS})
if(BUILD_X64)
    target_compile_definitions(CyberTalkSAPI PRIVATE BUILD_X64)
endif()

if(NOT BUILD_X64)
    include(FetchContent)
    FetchContent_Declare(
        sonic
        GIT_REPOSITORY https://github.com/waywardgeek/sonic.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(sonic)
    add_library(sonic STATIC ${sonic_SOURCE_DIR}/sonic.c)
    target_include_directories(sonic PUBLIC ${sonic_SOURCE_DIR})

    # engine protocol client (32-bit; talks to STLTTS.EXE)
    add_library(stl_engine STATIC src/stl_engine.cpp src/stl_text.cpp)
    target_include_directories(stl_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(stl_engine PUBLIC psapi advapi32 shell32)
    target_compile_definitions(stl_engine PRIVATE ${COMMON_DEFS})

    # host process
    add_executable(CyberTalkHost WIN32 src/cybertalk_host.cpp)
    target_include_directories(CyberTalkHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(CyberTalkHost PRIVATE stl_engine sonic user32 shell32)
    target_compile_definitions(CyberTalkHost PRIVATE ${COMMON_DEFS})

    # configuration utility
    add_executable(CyberTalkConfig WIN32
        src/config/CyberTalkConfig.cpp
        src/config/CyberTalkConfig.rc
        src/stl_text.cpp
    )
    target_include_directories(CyberTalkConfig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/config)
    target_link_libraries(CyberTalkConfig PRIVATE pipe_client comctl32 winmm user32 gdi32 shell32 ole32)
    target_compile_definitions(CyberTalkConfig PRIVATE ${COMMON_DEFS})
    if(MSVC)
        target_link_options(CyberTalkConfig PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    endif()

    # engine probe / sample generator
    add_executable(cybertalk_probe test/cybertalk_probe.cpp)
    target_include_directories(cybertalk_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(cybertalk_probe PRIVATE stl_engine user32)
    target_compile_definitions(cybertalk_probe PRIVATE ${COMMON_DEFS})

endif()

# SAPI 5 end-to-end test client (built for both architectures)
add_executable(sapi_test test/sapi_test.cpp)
target_link_libraries(sapi_test PRIVATE ole32 oleaut32)
target_compile_definitions(sapi_test PRIVATE ${COMMON_DEFS})

if(MSVC)
    foreach(t pipe_client CyberTalkSAPI sapi_test)
        target_compile_options(${t} PRIVATE /W3 /MP)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
    if(NOT BUILD_X64)
        foreach(t sonic stl_engine CyberTalkHost CyberTalkConfig cybertalk_probe)
            target_compile_options(${t} PRIVATE /W3 /MP)
            set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()
