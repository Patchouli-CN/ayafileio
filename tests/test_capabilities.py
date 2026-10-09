"""get_capabilities / get_backend_info 能力发现测试（脚本式，直接 python tests/test_capabilities.py 运行）"""

import os
import sys
from pathlib import Path

# 添加项目根目录到路径
sys.path.insert(0, str(Path(__file__).parent))

if sys.platform == "win32":
    import io

    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import ayafileio

PASSED = 0


def check(name, cond):
    global PASSED
    assert cond, f"FAILED: {name}"
    PASSED += 1
    print(f"  ✓ {name}")


def test_backend_info():
    print("[test_backend_info]")
    info = ayafileio.get_backend_info()
    check("platform 合法", info["platform"] in ("windows", "linux", "macos", "posix"))
    check("backend 非空", bool(info["backend"]))
    check("is_truly_async 是 bool", isinstance(info["is_truly_async"], bool))
    check("os_version 是非空字符串", isinstance(info["os_version"], str) and bool(info["os_version"]))
    check("description 非空", bool(info["description"]))


def test_capabilities_structure():
    print("[test_capabilities_structure]")
    caps = ayafileio.get_capabilities()
    info = ayafileio.get_backend_info()
    for key in info:
        check(f"包含 get_backend_info 的键 {key}", key in caps and caps[key] == info[key])
    check("features 是 dict", isinstance(caps["features"], dict))
    check("backend_detail 是 dict", isinstance(caps["backend_detail"], dict))


def test_library_features():
    print("[test_library_features]")
    feats = ayafileio.get_capabilities()["features"]
    for key in ("positional_io", "batch_positional_io", "zero_copy_readinto",
                "chunked_streaming", "async_open", "adaptive_batching"):
        check(f"{key} == True", feats[key] is True)
    check("fast_file_copy 是 bool", isinstance(feats["fast_file_copy"], bool))

    # fast_file_copy 必须与 acopy 实际会走的快车道一致
    if sys.platform == "win32":
        check("Windows 有 CopyFile2", feats["fast_file_copy"] is True)
    elif sys.platform == "linux":
        expect = hasattr(os, "copy_file_range")
        check(f"fast_file_copy 与 os.copy_file_range 一致 ({expect})",
              feats["fast_file_copy"] is expect)
    else:
        check("macOS/POSIX 无快车道", feats["fast_file_copy"] is False)


def test_backend_detail():
    print("[test_backend_detail]")
    caps = ayafileio.get_capabilities()
    detail = caps["backend_detail"]

    if sys.platform == "win32":
        check("iocp completion_model", detail["completion_model"] is True)
        check("iocp batch_harvest", detail["batch_harvest"] is True)
        check("iocp handle_pool", detail["handle_pool"] is True)
    elif sys.platform == "linux":
        if caps["backend"] == "io_uring":
            check("io_uring available", detail["available"] is True)
            check("io_uring completion_model", detail["completion_model"] is True)
            check("features 位矩阵非空", isinstance(detail["features"], dict) and len(detail["features"]) > 0)
            check("sqpoll 是 bool", isinstance(detail["sqpoll"], bool))
            ops = detail["opcodes"]
            if ops is None:
                check("老内核无 PROBE：opcodes 为 None", True)
            else:
                check("PROBE: read 支持", ops.get("read") is True)
                check("PROBE: write 支持", ops.get("write") is True)
        else:
            check("线程池降级 completion_model=False", detail["completion_model"] is False)
    elif sys.platform == "darwin":
        check("dispatch_io 是 bool", isinstance(detail["dispatch_io"], bool))
        if detail["dispatch_io"]:
            # GCD 文件通道是内核托管 workqueue 跑阻塞 pread，非完成模型
            check("dispatch_io completion_model=False", detail["completion_model"] is False)
            check("kernel_managed_workqueue", detail["kernel_managed_workqueue"] is True)
    else:
        check("posix 降级 completion_model=False", detail["completion_model"] is False)


def test_cached_consistency():
    print("[test_cached_consistency]")
    a = ayafileio.get_capabilities()
    b = ayafileio.get_capabilities()
    check("重复调用结果一致（探测已缓存）", a == b)


if __name__ == "__main__":
    test_backend_info()
    test_capabilities_structure()
    test_library_features()
    test_backend_detail()
    test_cached_consistency()
    print(f"\nAll {PASSED} checks passed.")
