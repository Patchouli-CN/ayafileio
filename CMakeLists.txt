cmake_minimum_required(VERSION 3.15)
project(ayafileio VERSION 2.0.1 LANGUAGES CXX)

# ════════════════════════════════════════════════════════════════════════════
# macOS 部署目标（解决 aligned allocation 兼容性）
# ════════════════════════════════════════════════════════════════════════════
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")
endif()

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ════════════════════════════════════════════════════════════════════════════
# 构建选项
# ════════════════════════════════════════════════════════════════════════════
# 使用标准 CMake 参数控制构建类型:
#   -DCMAKE_BUILD_TYPE=Debug          — 调试构建（禁用优化、包含调试符号）
#   -DCMAKE_BUILD_TYPE=Release        — 发布构建（优化、无调试符号）
#   -DCMAKE_BUILD_TYPE=RelWithDebInfo — 带调试符号的发布构建
# pip install -e . --config-settings="cmake.args=-DCMAKE_BUILD_TYPE=Debug"
if(NOT DEFINED ENABLE_YUYUKO)
    if(MSVC)
        set(_aya_yuyuko_default ON)
    else()
        set(_aya_yuyuko_default OFF)
    endif()
else()
    set(_aya_yuyuko_default OFF)
endif()
option(ENABLE_YUYUKO "Enable Yuyuko memory tracker" ${_aya_yuyuko_default})
option(ENABLE_VERBOSE_LOGGING "Enable verbose runtime logging" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 找到 Python
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

# 找到 nanobind
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE NB_CMAKE_DIR
    ERROR_QUIET
)

if(NOT NB_CMAKE_DIR)
    message(FATAL_ERROR "Failed to get nanobind CMake directory. Make sure nanobind is installed: pip install nanobind")
endif()

list(APPEND CMAKE_PREFIX_PATH "${NB_CMAKE_DIR}")
find_package(nanobind CONFIG REQUIRED)

message(STATUS "Python executable: ${Python_EXECUTABLE}")
message(STATUS "Python version: ${Python_VERSION}")
message(STATUS "nanobind found: ${nanobind_DIR}")

# 获取 nanobind 版本
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.__version__)"
    OUTPUT_VARIABLE NANOBIND_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
message(STATUS "nanobind version: ${NANOBIND_VERSION}")

# ════════════════════════════════════════════════════════════════════════════
# io_uring 支持 (仅 Linux)
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_IO_URING "Enable io_uring support on Linux" ON)

set(HAVE_IO_URING FALSE)
if(ENABLE_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
    find_library(LIBURING_LIBRARY NAMES uring)
    
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        set(HAVE_IO_URING TRUE)
        add_compile_definitions(HAVE_IO_URING=1)
        message(STATUS "✅ io_uring support enabled")
    else()
        message(WARNING "⚠️ liburing not found, io_uring support disabled")
    endif()
endif()

set(CORE_SOURCES
    src/core/buffer.cpp
    src/core/completion.cpp
    src/core/file_state.cpp
    src/core/backend_registry.cpp
    src/core/file_handle.cpp
    src/core/io_request.cpp
    src/core/io_backend.cpp
    src/core/global_thread_pool.cpp
)
set(BACKEND_SOURCES
    src/backend/builtin.cpp
    src/backend/thread/thread_io_backend.cpp
)
set(DISCOVERY_SOURCES
    src/discover/discover.cpp
)
set(PYTHON_SOURCES
    src/bindings/python/module.cpp
    src/bindings/python/py_globals.cpp
    src/bindings/python/result_batcher.cpp
    src/bindings/python/batcher_registry.cpp
    src/bindings/python/py_error.cpp
    src/bindings/python/py_future.cpp
    src/bindings/python/py_buffer.cpp
    src/bindings/python/py_async_file.cpp
    src/bindings/python/py_open_async.cpp
    src/bindings/python/py_config.cpp
    src/bindings/python/py_capabilities.cpp
    src/bindings/python/py_debug.cpp
)

if(WIN32)
    message(STATUS "Building for Windows (IOCP Backend)")
    list(APPEND BACKEND_SOURCES
        src/backend/windows/iocp.cpp
        src/backend/windows/iocp_request.cpp
        src/backend/windows/iocp_session.cpp
        src/backend/windows/iocp_context.cpp
        src/backend/windows/iocp_submit.cpp
        src/backend/windows/iocp_worker.cpp
        src/backend/windows/windows_io_backend.cpp
    )
    list(APPEND DISCOVERY_SOURCES src/discover/discover_windows.cpp)
elseif(APPLE)
    message(STATUS "Building for macOS (Dispatch I/O Backend + Thread Pool fallback)")
    list(APPEND BACKEND_SOURCES src/backend/macos/macos_gcd_backend.cpp)
    list(APPEND DISCOVERY_SOURCES
        src/discover/discover_posix.cpp
        src/discover/discover_mac.cpp
    )
    find_package(Threads REQUIRED)
else()
    list(APPEND DISCOVERY_SOURCES src/discover/discover_posix.cpp)
    if(HAVE_IO_URING)
        message(STATUS "Adding io_uring backend (Linux + liburing)")
        list(APPEND BACKEND_SOURCES
            src/backend/linux/io_uring_backend.cpp
            src/backend/linux/native_open.cpp
            src/backend/linux/uring_pool.cpp
        )
        list(APPEND DISCOVERY_SOURCES src/discover/discover_linux.cpp)
    else()
        message(STATUS "Building for ${CMAKE_SYSTEM_NAME} (Thread Pool Backend only)")
    endif()
    find_package(Threads REQUIRED)
endif()

add_library(aya_core OBJECT ${CORE_SOURCES})
target_include_directories(aya_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
)

add_library(aya_platform INTERFACE)
target_include_directories(aya_platform INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

add_library(aya_discovery OBJECT ${DISCOVERY_SOURCES})
target_include_directories(aya_discovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/discover
)
target_link_libraries(aya_discovery PRIVATE aya_core)

add_library(aya_backend OBJECT ${BACKEND_SOURCES})
target_include_directories(aya_backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/src/discover
    ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
)
target_link_libraries(aya_backend PRIVATE aya_core aya_platform)

add_library(aya_diagnostics OBJECT src/diagnostics/yuyuko.cpp)
target_include_directories(aya_diagnostics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
)
if(ENABLE_YUYUKO)
    target_compile_definitions(aya_diagnostics PRIVATE ENABLE_YUYUKO=1)
endif()

nanobind_add_module(_ayafileio FREE_THREADED ${PYTHON_SOURCES})
target_link_libraries(_ayafileio PRIVATE
    aya_core
    aya_backend
    aya_discovery
    aya_diagnostics
)

if(NOT TARGET nanobind-static AND TARGET nanobind-static-ft)
    add_library(nanobind-static ALIAS nanobind-static-ft)
endif()

target_include_directories(_ayafileio PRIVATE
    ${Python_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/python
    ${CMAKE_CURRENT_SOURCE_DIR}/src/discover
    ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
)

# 如果启用 io_uring，添加 liburing 链接
if(HAVE_IO_URING)
    target_link_libraries(_ayafileio PRIVATE ${LIBURING_LIBRARY})
    target_include_directories(_ayafileio PRIVATE ${LIBURING_INCLUDE_DIR})
endif()

# ════════════════════════════════════════════════════════════════════════════
# 编译选项配置
# ════════════════════════════════════════════════════════════════════════════

# 基础编译定义
target_compile_definitions(_ayafileio PRIVATE
    Py_SSIZE_T_CLEAN
    PY_SSIZE_T_CLEAN
)

# 详细日志宏
if(ENABLE_VERBOSE_LOGGING)
    target_compile_definitions(_ayafileio PRIVATE
        AYAFILEIO_VERBOSE_LOGGING=1
    )
    message(STATUS "Verbose logging enabled")
endif()

if(MSVC)
    # ── common MSVC flags ──────────────────────────────────────────────────
    target_compile_options(_ayafileio PRIVATE
        /EHsc                  # C++ exception handling
        /utf-8                 # UTF-8 source encoding
        /wd4244                # suppress conversion warning
        /wd4267                # suppress size_t conversion warning
        /wd4996                # suppress deprecated warning
    )

    # ── build-type-specific MSVC flags ─────────────────────────────────────
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(_ayafileio PRIVATE /Od /Zi)
        target_link_options(_ayafileio PRIVATE /DEBUG:FULL)
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        target_compile_options(_ayafileio PRIVATE /O2 /Zi)
        target_link_options(_ayafileio PRIVATE /DEBUG:FULL)
    else()
        target_compile_options(_ayafileio PRIVATE /O2 /W3)
    endif()

    target_compile_definitions(_ayafileio PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    target_link_libraries(_ayafileio PRIVATE ws2_32)

    # Yuyuko memory tracker (MSVC)
    if(ENABLE_YUYUKO)
        target_compile_definitions(_ayafileio PRIVATE ENABLE_YUYUKO=1)
        message(STATUS "Yuyuko memory tracker enabled (MSVC)")
    endif()
else()
    # ── GCC/Clang compiler options ─────────────────────────────────────────

    # common flags
    target_compile_options(_ayafileio PRIVATE -Wall -Wextra -fPIC)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(_ayafileio PRIVATE
            -O0 -g3 -fno-omit-frame-pointer
        )
    else()
        target_compile_options(_ayafileio PRIVATE -O3 -Wpedantic)
    endif()

    # Linux/macOS 有原生的 ASAN，幽幽子默认关闭
    # 如需开启: cmake -DENABLE_YUYUKO=ON ...
    if(ENABLE_YUYUKO)
        target_compile_definitions(_ayafileio PRIVATE ENABLE_YUYUKO=1)
        message(STATUS "Yuyuko memory tracker enabled (GCC/Clang)")
    endif()

    if(NOT WIN32)
        target_link_libraries(_ayafileio PRIVATE Threads::Threads)
    endif()
endif()

# ════════════════════════════════════════════════════════════════════════════
# macOS 特定配置
# ════════════════════════════════════════════════════════════════════════════
if(APPLE)
    target_compile_options(_ayafileio PRIVATE -stdlib=libc++)
    target_link_libraries(_ayafileio PRIVATE -stdlib=libc++)
    message(STATUS "🍎 macOS: using Dispatch I/O (GCD) for native async I/O")
    
    # 输出 macOS 版本信息
    execute_process(
        COMMAND sw_vers -productVersion
        OUTPUT_VARIABLE MACOS_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "macOS version: ${MACOS_VERSION}")
endif()

# ════════════════════════════════════════════════════════════════════════════
# 打印构建信息
# ════════════════════════════════════════════════════════════════════════════
message(STATUS "========================================")
message(STATUS "Build Configuration:")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform:     ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Yuyuko:       ${ENABLE_YUYUKO}")
message(STATUS "  Verbose log:  ${ENABLE_VERBOSE_LOGGING}")
if(APPLE)
    message(STATUS "  Backend: Dispatch I/O (GCD) + Thread Pool fallback")
elseif(WIN32)
    message(STATUS "  Backend: IOCP")
elseif(HAVE_IO_URING)
    message(STATUS "  Backend: io_uring + Thread Pool fallback")
else()
    message(STATUS "  Backend: Thread Pool")
endif()
message(STATUS "========================================")

# 安装规则
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import sysconfig; print(sysconfig.get_path('purelib'))"
    OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Python site-packages: ${PYTHON_SITE_PACKAGES}")
install(TARGETS _ayafileio LIBRARY DESTINATION ayafileio)
message(STATUS "Installation target: ayafileio/")

include(CTest)
if(BUILD_TESTING)
    add_executable(aya_core_tests tests/core/test_main.cpp)
    target_include_directories(aya_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_link_libraries(aya_core_tests PRIVATE aya_core)
    if(NOT WIN32)
        target_link_libraries(aya_core_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME core_unit COMMAND aya_core_tests)

    add_executable(aya_diagnostics_tests tests/core/test_diagnostics.cpp)
    target_include_directories(aya_diagnostics_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics
    )
    target_compile_definitions(aya_diagnostics_tests PRIVATE ENABLE_YUYUKO=1)
    if(ENABLE_YUYUKO)
        target_compile_definitions(aya_diagnostics_tests PRIVATE AYAFILEIO_TEST_YUYUKO_HOOKS=1)
    endif()
    target_link_libraries(aya_diagnostics_tests PRIVATE aya_core aya_diagnostics)
    if(NOT WIN32)
        target_link_libraries(aya_diagnostics_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME diagnostics_unit COMMAND aya_diagnostics_tests)

    add_test(
        NAME architecture_dependencies
        COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cpp_dependencies.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()