@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

echo ========================================
echo   aiowinfile 打包脚本
echo ========================================
echo.

:: 检查 Python
echo [1/5] 检查 Python 环境...
python --version > nul 2>&1
if errorlevel 1 (
    echo ❌ Python 未安装或不在 PATH 中
    pause
    exit /b 1
)
python --version
echo.

:: 检查并安装依赖
echo [2/5] 检查打包依赖...
pip show build > nul 2>&1
if errorlevel 1 (
    echo 正在安装 build...
    pip install build
)
pip show twine > nul 2>&1
if errorlevel 1 (
    echo 正在安装 twine...
    pip install twine
)
echo ✅ 依赖检查完成
echo.

:: 清理旧文件
echo [3/5] 清理旧文件...
if exist build (
    echo 删除 build 目录...
    rmdir /s /q build
)
if exist dist (
    echo 删除 dist 目录...
    rmdir /s /q dist
)
if exist aiowinfile.egg-info (
    echo 删除 aiowinfile.egg-info...
    rmdir /s /q aiowinfile.egg-info
)
if exist aiowinfile\_aiowinfile*.pyd (
    echo 删除旧的 .pyd 文件...
    del /q aiowinfile\_aiowinfile*.pyd 2>nul
)
echo ✅ 清理完成
echo.

:: 编译扩展
echo [4/5] 编译 C++ 扩展...
echo 这可能需要几分钟，请耐心等待...
python setup.py build_ext --inplace
if errorlevel 1 (
    echo.
    echo ❌ 编译失败！请检查 Visual Studio 是否已安装
    pause
    exit /b 1
)
echo ✅ 编译完成
echo.

:: 打包 wheel
echo [5/5] 打包 wheel...
python setup.py bdist_wheel
if errorlevel 1 (
    echo.
    echo ❌ 打包失败！
    pause
    exit /b 1
)
echo ✅ 打包完成
echo.

:: 显示结果
echo ========================================
echo   📦 打包完成！
echo ========================================
echo.
echo 生成的 wheel 文件:
dir dist\*.whl /b
echo.
echo 文件大小:
dir dist\*.whl | findstr ".whl"
echo.
echo 上传到 PyPI:
echo   twine upload dist\*.whl
echo.
echo 测试安装:
echo   pip install dist\aiowinfile-*.whl
echo.

pause