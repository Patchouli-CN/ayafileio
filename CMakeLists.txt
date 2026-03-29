cmake_minimum_required(VERSION 3.15)
project(ayafileio VERSION 0.1.5 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 设置构建类型（默认 Release）
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

# 收集源文件
set(SOURCES
    src/bindings.cpp
    src/globals.cpp
    src/loop_handle.cpp
    src/pool.cpp
    src/file_handle.cpp
    src/io_backend.cpp
)

# 根据平台添加后端源文件
if(WIN32)
    message(STATUS "Building for Windows (IOCP backend)")
    list(APPEND SOURCES
        src/backends/windows_io_backend.cpp
        src/handle_pool.cpp
        src/iocp.cpp
    )
else()
    message(STATUS "Building for Linux (thread pool backend)")
    list(APPEND SOURCES
        src/backends/linux_io_backend.cpp
        src/handle_pool_posix.cpp
    )
    # Linux 需要 pthread
    find_package(Threads REQUIRED)
endif()

# 创建 Python 扩展模块
nanobind_add_module(_ayafileio ${SOURCES})

# 设置模块属性 - 关键修复点
if(WIN32)
    # Windows: 强制使用 .pyd 扩展名
    set_target_properties(_ayafileio PROPERTIES
        PREFIX ""
        SUFFIX ".pyd"
        OUTPUT_NAME "_ayafileio"
    )
else()
    # Linux/Mac: 使用标准共享库扩展名
    set_target_properties(_ayafileio PROPERTIES
        PREFIX ""
        SUFFIX "${Python_SOABI}${CMAKE_SHARED_MODULE_SUFFIX}"
    )
endif()

# 添加 include 目录
target_include_directories(_ayafileio PRIVATE
    ${Python_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends
)

# 设置编译选项
if(MSVC)
    # MSVC 编译选项
    target_compile_options(_ayafileio PRIVATE 
        /O2                    # 优化
        /W3                    # 警告级别 3
        /EHsc                  # C++ 异常处理
        /utf-8                 # UTF-8 编码
        /wd4244                # 忽略转换警告
        /wd4267                # 忽略 size_t 转换警告
        /wd4996                # 忽略 deprecated 警告
    )
    target_compile_definitions(_ayafileio PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    target_link_libraries(_ayafileio PRIVATE
        ws2_32
    )
else()
    # GCC/Clang 编译选项
    target_compile_options(_ayafileio PRIVATE 
        -O3                    # 优化级别 3
        -Wall                  # 所有警告
        -Wextra                # 额外警告
        -Wpedantic             # 严格标准
        -fPIC                  # 位置无关代码
    )
    if(NOT WIN32)
        target_link_libraries(_ayafileio PRIVATE
            Threads::Threads
        )
    endif()
endif()

# 添加 Python 特定的编译定义
target_compile_definitions(_ayafileio PRIVATE
    # Do not use the limited Python stable ABI here because the code
    # relies on full CPython C-API symbols (Py_buffer, PyObject_GetBuffer, etc.).
    Py_SSIZE_T_CLEAN
    PY_SSIZE_T_CLEAN
)

# 打印构建信息
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ flags: ${CMAKE_CXX_FLAGS}")

# 安装规则
# 获取 Python site-packages 路径
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import sysconfig; print(sysconfig.get_path('purelib'))"
    OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Python site-packages: ${PYTHON_SITE_PACKAGES}")

# 安装扩展模块到 ayafileio 包目录
install(TARGETS _ayafileio
    DESTINATION ayafileio
    LIBRARY DESTINATION ayafileio
    RUNTIME DESTINATION ayafileio
)

# 安装 Python 包文件
install(FILES 
    ayafileio/__init__.py
    DESTINATION ayafileio
)

# 可选：安装 README 和许可证
install(FILES
    README.md
    DESTINATION .
    OPTIONAL
)

# 打印安装信息
message(STATUS "Installation target: ayafileio/")