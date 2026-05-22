cmake_minimum_required(VERSION 3.15)
project(ayafileio VERSION 1.3.2 LANGUAGES CXX)

# ════════════════════════════════════════════════════════════════════════════
# macOS 部署目标（解决 aligned allocation 兼容性）
# ════════════════════════════════════════════════════════════════════════════
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")
endif()

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ════════════════════════════════════════════════════════════════════════════
# 构建选项
# ════════════════════════════════════════════════════════════════════════════
# 使用标准 CMake 参数控制构建类型:
#   -DCMAKE_BUILD_TYPE=Debug          — 调试构建（禁用优化、包含调试符号）
#   -DCMAKE_BUILD_TYPE=Release        — 发布构建（优化、无调试符号）
#   -DCMAKE_BUILD_TYPE=RelWithDebInfo — 带调试符号的发布构建
# pip install -e . --config-settings="cmake.args=-DCMAKE_BUILD_TYPE=Debug"
option(ENABLE_ASAN "Enable AddressSanitizer for memory error detection" OFF)
option(ENABLE_VERBOSE_LOGGING "Enable verbose runtime logging" OFF)

if(DEFINED ENV{AYAFILEIO_ASAN})
    set(ENABLE_ASAN ON)
endif()

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

# 收集源文件
set(SOURCES
    src/bindings.cpp
    src/globals.cpp
    src/global_thread_pool.cpp
    src/result_batcher.cpp
    src/file_handle.cpp
    src/io_backend.cpp
)

# 根据平台添加后端源文件
if(WIN32)
    message(STATUS "Building for Windows (IOCP Backend)")
    list(APPEND SOURCES
        src/backends/windows_io_backend.cpp
        src/handle_pool.cpp
        src/iocp.cpp
        src/iocp_context.cpp
    )
elseif(APPLE)
    message(STATUS "Building for macOS (Dispatch I/O Backend + Thread Pool fallback)")
    list(APPEND SOURCES
        src/backends/macos_gcd_backend.cpp
        src/backends/thread_io_backend.cpp
    )
    find_package(Threads REQUIRED)
else()
    # Linux / 其他 POSIX
    list(APPEND SOURCES
        src/backends/thread_io_backend.cpp
    )
    
    if(HAVE_IO_URING)
        message(STATUS "Adding io_uring backend (Linux + liburing)")
        list(APPEND SOURCES
            src/backends/io_uring_backend.cpp
            src/uring_globals.cpp
        )
    else()
        message(STATUS "Building for ${CMAKE_SYSTEM_NAME} (Thread Pool Backend only)")
    endif()
    
    find_package(Threads REQUIRED)
endif()

# 创建 Python 扩展模块
nanobind_add_module(_ayafileio FREE_THREADED ${SOURCES})

# 兼容 old/new nanobind target 命名
if(NOT TARGET nanobind-static AND TARGET nanobind-static-ft)
    add_library(nanobind-static ALIAS nanobind-static-ft)
endif()

# 添加 include 目录
target_include_directories(_ayafileio PRIVATE
    ${Python_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends
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
    if(ENABLE_ASAN)
        target_compile_definitions(_ayafileio PRIVATE ENABLE_ASAN=1)
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

    # AddressSanitizer
    if(ENABLE_ASAN)
        target_compile_options(_ayafileio PRIVATE
            -fsanitize=address -fno-omit-frame-pointer
        )
        target_link_options(_ayafileio PRIVATE -fsanitize=address)
        target_compile_definitions(_ayafileio PRIVATE AYAFILEIO_ASAN=1)
        message(STATUS "AddressSanitizer enabled")
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
message(STATUS "  ASan:         ${ENABLE_ASAN}")
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