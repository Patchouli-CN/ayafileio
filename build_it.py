#!/usr/bin/env python3
"""跨平台的构建脚本（替代 build_it.bat），使用 argparse 提供灵活构建选项。

用法示例:
  python build_it.py --versions 310,311,312 --output dist --clean-cache

该脚本会：
 - 检查临时目录可用空间
 - 安装 cibuildwheel（可选）
 - 清理旧构建产物
 - 调用 cibuildwheel 构建 wheel
 - 可选清理构建缓存
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')


DEFAULT_TEMP = Path("D:/temp/cibw_build") if os.name == "nt" else Path.cwd() / ".cibw_build"


def check_disk_space(path: Path) -> int:
    path.mkdir(parents=True, exist_ok=True)
    usage = shutil.disk_usage(path)
    return usage.free // (1024 * 1024)


def pip_install(package: str, cache_dir: Path | None = None) -> None:
    cmd = [sys.executable, "-m", "pip", "install", package]
    if cache_dir:
        cmd[4:4] = ["--cache-dir", str(cache_dir)]
    subprocess.check_call(cmd)


def remove_path(p: Path) -> None:
    if p.exists():
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        else:
            p.unlink()


def main() -> int:
    p = argparse.ArgumentParser(description="Build wheels using cibuildwheel (argparse wrapper)")
    p.add_argument("--versions", default="310,311,312,313,314",
                   help="comma-separated python minor versions, e.g. 310,311 -> maps to cp310-* cp311-*")
    p.add_argument("--output", default="dist", help="output directory for wheels")
    p.add_argument("--temp", default=str(DEFAULT_TEMP), help="临时目录/缓存目录")
    p.add_argument("--skip-install", action="store_true", help="跳过 pip install cibuildwheel")
    p.add_argument("--clean-cache", action="store_true", help="构建完成后清理临时缓存目录")
    p.add_argument("--yes", "-y", action="store_true", help="自动确认提示")
    p.add_argument("--platform", default="windows", help="传给 cibuildwheel 的 --platform 参数")
    args = p.parse_args()
    
    if os.environ.get("CI"):  # GitHub Actions、GitLab CI 等环境
        print("Running in CI mode")
        # 自动确认所有交互
        args.yes = True

    temp_dir = Path(args.temp)
    cache_dir = temp_dir / "pip_cache"
    cibw_cache = temp_dir / "cibw_cache"

    free_mb = check_disk_space(temp_dir)
    print(f"临时目录: {temp_dir}")
    print(f"可用空间: {free_mb} MB")
    if free_mb < 2048:
        print("⚠️ 警告: 可用空间小于2GB，可能不够用")

    if not args.skip_install:
        print("[1/4] 安装/更新 cibuildwheel...")
        try:
            pip_install("cibuildwheel", cache_dir=cache_dir)
        except subprocess.CalledProcessError:
            print("❌ pip install cibuildwheel 失败")
            return 1

    # 清理旧文件
    print("[2/4] 清理旧文件...")
    for pth in (Path("build"), Path("dist"), Path("ayafileio.egg-info")):
        remove_path(pth)

    # 环境变量准备
    versions = [v.strip() for v in args.versions.split(",") if v.strip()]
    cibw_build = " ".join(f"cp{v}-*" for v in versions)

    env = os.environ.copy()
    env["CIBW_BUILD"] = cibw_build
    env["CIBW_SKIP"] = "*-win32 *-musllinux* pp*"
    env["CIBW_ARCHS"] = "auto64"
    env["CIBW_ENVIRONMENT"] = "CMAKE_BUILD_PARALLEL_LEVEL=2"
    env["CIBW_BUILD_VERBOSITY"] = "1"
    env["CIBW_CACHE_DIR"] = str(cibw_cache)

    # Hooks: 用户若需自定义可通过环境变量覆盖
    env.setdefault("CIBW_BEFORE_BUILD", 'echo "Building for Python"')
    env.setdefault("CIBW_AFTER_BUILD", "rmdir /s /q build 2>nul")
    env.setdefault("CIBW_BEFORE_ALL", 'echo "Starting build"')
    env.setdefault("CIBW_AFTER_ALL", 'echo "All builds completed"')

    print("[3/4] 开始构建...")
    outdir = Path(args.output)
    outdir.mkdir(parents=True, exist_ok=True)

    cmd = [sys.executable, "-m", "cibuildwheel", "--platform", args.platform, "--output-dir", str(outdir)]
    try:
        subprocess.check_call(cmd, env=env)
    except subprocess.CalledProcessError as e:
        print("❌ 构建失败！")
        return e.returncode

    # 展示结果
    print("[4/4] 构建完成，统计结果：")
    wheels = list(outdir.glob("*.whl"))
    if not wheels:
        print("❌ 未生成任何 wheel 文件")
    else:
        total = sum(w.stat().st_size for w in wheels)
        print("生成的 wheel 文件:")
        for w in wheels:
            print(" -", w.name)
        print(f"总大小: {total // (1024*1024)} MB")

    # 可选清理
    if args.clean_cache:
        do_clean = True
    elif args.yes:
        do_clean = True
    else:
        resp = input("是否清理构建缓存? (y/N): ").strip().lower()
        do_clean = resp == "y"

    if do_clean:
        print("清理缓存...")
        remove_path(temp_dir)
        print("✅ 缓存已清理")
    else:
        print("保留缓存以便下次构建加速")

    return 0


if __name__ == "__main__":
    sys.exit(main())
