cmake_minimum_required(VERSION 3.15)
project(ayafileio VERSION 1.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ════════════════════════════════════════════════════════════════════════════
# 调试选项：通过环境变量或 CMake 参数控制
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_DEBUG "Enable debug build with detailed logging" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer for memory error detection" OFF)
option(ENABLE_VERBOSE_LOGGING "Enable verbose runtime logging" OFF)

# 如果通过环境变量设置，自动启用
if(DEFINED ENV{AYAFILEIO_DEBUG})
    set(ENABLE_DEBUG ON)
    set(ENABLE_VERBOSE_LOGGING ON)
endif()

if(DEFINED ENV{AYAFILEIO_ASAN})
    set(ENABLE_ASAN ON)
endif()

# 设置构建类型
if(ENABLE_DEBUG)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    message(STATUS "🔧 Debug mode enabled")
elseif(NOT CMAKE_BUILD_TYPE)
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
    src/loop_handle.cpp
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
    )
elseif(APPLE)
    message(STATUS "Building for macOS (Dispatch I/O Backend + Thread Pool fallback)")
    list(APPEND SOURCES
        src/backends/macos_gcd_backend.cpp
        src/backends/thread_io_backend.cpp
        src/handle_pool_posix.cpp
    )
    find_package(Threads REQUIRED)
else()
    # Linux / 其他 POSIX
    list(APPEND SOURCES
        src/backends/thread_io_backend.cpp
        src/handle_pool_posix.cpp
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
nanobind_add_module(_ayafileio ${SOURCES})

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
        AYAFILEIO_DEBUG=1
    )
    message(STATUS "📝 Verbose logging enabled")
endif()

if(MSVC)
    # MSVC 编译选项
    if(ENABLE_DEBUG)
        target_compile_options(_ayafileio PRIVATE 
            /Od                    # 禁用优化
            /Zi                    # 生成完整调试信息
            /EHsc                  # C++ 异常处理
            /utf-8                 # UTF-8 编码
            /wd4244                # 忽略转换警告
            /wd4267                # 忽略 size_t 转换警告
            /wd4996                # 忽略 deprecated 警告
        )
        target_link_options(_ayafileio PRIVATE /DEBUG:FULL)
    else()
        target_compile_options(_ayafileio PRIVATE 
            /O2                    # 优化
            /W3                    # 警告级别 3
            /EHsc                  # C++ 异常处理
            /utf-8                 # UTF-8 编码
            /wd4244                # 忽略转换警告
            /wd4267                # 忽略 size_t 转换警告
            /wd4996                # 忽略 deprecated 警告
        )
    endif()
    target_compile_definitions(_ayafileio PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    target_link_libraries(_ayafileio PRIVATE ws2_32)
else()
    # GCC/Clang 编译选项
    if(ENABLE_DEBUG)
        target_compile_options(_ayafileio PRIVATE 
            -O0                    # 禁用优化，便于调试
            -g3                    # 最大调试信息
            -ggdb                  # GDB 格式调试信息
            -Wall                  # 所有警告
            -Wextra                # 额外警告
            -fPIC                  # 位置无关代码
            -fno-omit-frame-pointer # 保留栈帧指针
            -rdynamic              # 导出所有符号
        )
    else()
        target_compile_options(_ayafileio PRIVATE 
            -O3                    # 优化级别 3
            -Wall                  # 所有警告
            -Wextra                # 额外警告
            -Wpedantic             # 严格标准
            -fPIC                  # 位置无关代码
        )
    endif()
    
    # AddressSanitizer (内存错误检测)
    if(ENABLE_ASAN)
        target_compile_options(_ayafileio PRIVATE 
            -fsanitize=address
            -fno-omit-frame-pointer
        )
        target_link_options(_ayafileio PRIVATE 
            -fsanitize=address
        )
        target_compile_definitions(_ayafileio PRIVATE AYAFILEIO_ASAN=1)
        message(STATUS "🧹 AddressSanitizer enabled")
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
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Debug: ${ENABLE_DEBUG}")
message(STATUS "  ASan: ${ENABLE_ASAN}")
message(STATUS "  Verbose logging: ${ENABLE_VERBOSE_LOGGING}")
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