#!/usr/bin/env python3
"""
ayafileio vs aiofiles — stress-test comparison
Proves the difference between true kernel async and thread-pool async under extreme concurrency.

!!! Safety guards:
    - aiofiles concurrency capped at 200 (thread pool can't handle 10K)
    - Total tasks reduced to 50K (avoids thread explosion)
    - 60s hard timeout (prevents system lockup)
    - Batch task creation (avoids creating 50K coroutines at once)

Usage:
    pip install aiofiles
    python t_compare.py
"""

import asyncio
import locale
import os
import tempfile
import time
from collections import Counter

try:
    import ayafileio
except ImportError:
    print("Please install ayafileio: pip install ayafileio  /  pip install ayafileio")
    raise SystemExit(1)

try:
    import aiofiles
except ImportError:
    print("Please install aiofiles: pip install aiofiles  /  pip install aiofiles")
    raise SystemExit(1)

# ═══════════════════════════════════════════════════════════
# Locale detection
# ═══════════════════════════════════════════════════════════

def _detect_lang() -> str:
    """Return 'zh' or 'en' based on environment locale."""
    for src in (os.environ.get("LANG", ""),
                os.environ.get("LC_ALL", ""),
                os.environ.get("LANGUAGE", "")):
        if src.lower().startswith("zh"):
            return "zh"
    try:
        lc, _ = locale.getdefaultlocale()
        if lc and lc.lower().startswith("zh"):
            return "zh"
    except (ValueError, locale.Error):
        pass
    return "en"

LANG = _detect_lang()

_T = {
    "en": {
        "title":          "ayafileio vs aiofiles — comparison",
        "requires_aya":   "Please install ayafileio: pip install ayafileio",
        "requires_aio":   "Please install aiofiles: pip install aiofiles",
        "total_tasks":    "total tasks",
        "concurrency":    "concurrency",
        "hard_timeout":   "hard timeout",
        "mem_limit":      "memory limit",
        "backend_aya":    "ayafileio",
        "backend_aio":    "aiofiles",
        "truly_async":    "truly async",
        "test":           "Test",
        "progress":       "Progress",
        "results":        "Results",
        "success":        "success",
        "elapsed":        "elapsed",
        "throughput":     "throughput",
        "error_breakdown":"Error breakdown",
        "errors":         "errors",
        "summary":        "Summary",
        "benchmark":      "Benchmark",
        "completed":      "done",
        "skipped":        "Skipping aiofiles test (ayafileio timed out — system may be unstable)",
        "timeout":        "TIMEOUT",
        "timed_out_msg":  "Timed out ({timeout}s) — {completed}/{total} tasks finished",
        "terminated":     "force-terminated after {elapsed:.1f}s",
        "speedup":        "ayafileio is {speedup:.1f}x faster than aiofiles",
        "speedup_note":   "(even with aiofiles concurrency={aio} vs ayafileio={aya})",
        "ops":            "ops/s",
        "assert_mismatch":"AssertionError (data mismatch)",
        "backend_info":   "{name}: {platform} - {backend} (truly async: {async_icon})",
        "thread_pool":    "thread pool",
        "single_file":    "ayafileio — single file",
        "single_file_aio":"aiofiles  — single file",
    },
    "zh": {
        "title":          "ayafileio vs aiofiles — 对比测试",
        "requires_aya":   "请先安装 ayafileio: pip install ayafileio",
        "requires_aio":   "请先安装 aiofiles: pip install aiofiles",
        "total_tasks":    "总任务",
        "concurrency":    "并发",
        "hard_timeout":   "硬超时",
        "mem_limit":      "内存上限",
        "backend_aya":    "ayafileio",
        "backend_aio":    "aiofiles",
        "truly_async":    "真异步",
        "test":           "测试",
        "progress":       "进度",
        "results":        "结果",
        "success":        "成功",
        "elapsed":        "耗时",
        "throughput":     "吞吐量",
        "error_breakdown":"错误明细",
        "errors":         "错误",
        "summary":        "对比总结",
        "benchmark":      "Benchmark",
        "completed":      "完成",
        "skipped":        "跳过 aiofiles 测试（ayafileio 已超时，系统可能不稳定）",
        "timeout":        "超时",
        "timed_out_msg":  "超时 ({timeout}s) — 已完成 {completed}/{total}",
        "terminated":     "{elapsed:.1f}s 后强制终止",
        "speedup":        "ayafileio 比 aiofiles 快 {speedup:.1f}x",
        "speedup_note":   "(即使 aiofiles 并发={aio} vs ayafileio={aya})",
        "ops":            "ops/s",
        "assert_mismatch":"AssertionError (数据不匹配)",
        "backend_info":   "{name}: {platform} - {backend} (真异步: {async_icon})",
        "thread_pool":    "线程池",
        "single_file":    "ayafileio — 单文件",
        "single_file_aio":"aiofiles  — 单文件",
    },
}

def i18n(key: str) -> str:
    return _T[LANG].get(key, _T["en"].get(key, key))

# ═══════════════════════════════════════════════════════════
# Safety limits
# ═══════════════════════════════════════════════════════════
TOTAL_TASKS = 50_000
FILE_SIZE = 128
HARD_TIMEOUT = 60
MAX_MEMORY_MB = 1500
AYAFILEIO_CONCURRENT = 1000
AIOFILES_CONCURRENT = 200


def format_number(n: int) -> str:
    if n >= 1_000_000:
        return f"{n/1_000_000:.2f}M"
    if n >= 1_000:
        return f"{n/1_000:.2f}K"
    return str(n)


async def worker_ayafileio(f, task_id: int):
    """ayafileio worker — true kernel-async I/O"""
    data = f"aya_{task_id}".ljust(FILE_SIZE, "x").encode()
    offset = task_id * FILE_SIZE
    await f.seek(offset)
    await f.write(data)
    await f.seek(offset)
    read_back = await f.read(len(data))
    assert read_back == data, f"data mismatch: {read_back!r} != {data!r}"


async def worker_aiofiles(f, task_id: int):
    """aiofiles worker — thread-pool async (seek+write not atomic)"""
    data = f"aio_{task_id}".ljust(FILE_SIZE, "x").encode()
    offset = task_id * FILE_SIZE
    await f.seek(offset)
    await f.write(data)
    await f.seek(offset)
    read_back = await f.read(len(data))
    assert read_back == data, f"data mismatch: {read_back!r} != {data!r}"


async def run_benchmark(name: str, concurrent: int, worker_func, *args):
    """Run one benchmark and return result dict with error breakdown."""
    print(f"\n{'='*60}")
    print(f"🧪 {name}")
    print(f"   {i18n( 'total_tasks')}: {format_number(TOTAL_TASKS)} | {i18n( 'concurrency')}: {concurrent}")
    print(f"{'='*60}")

    sem = asyncio.Semaphore(concurrent)
    completed = 0
    error_counts: Counter[str] = Counter()
    start_time = time.perf_counter()
    timed_out = False

    async def bounded(task_id: int):
        nonlocal completed, error_counts
        async with sem:
            try:
                await worker_func(*args, task_id)
            except AssertionError:
                error_counts[i18n("assert_mismatch")] += 1
            except OSError as e:
                err_desc = os.strerror(e.errno) if e.errno else str(e)
                error_counts[f"OSError({e.errno}): {err_desc}"] += 1
            except ValueError:
                error_counts["ValueError"] += 1
            except Exception as e:
                error_counts[f"{type(e).__name__}: {e}"] += 1
            completed += 1
            if completed % 10000 == 0:
                elapsed = time.perf_counter() - start_time
                print(f"   📍 {i18n( 'progress')}: {format_number(completed)}/{format_number(TOTAL_TASKS)} "
                      f"({completed/TOTAL_TASKS*100:.0f}%) - "
                      f"{format_number(int(completed/elapsed))} {i18n( 'ops')}")

    try:
        batch_size = concurrent * 2
        for start in range(0, TOTAL_TASKS, batch_size):
            end = min(start + batch_size, TOTAL_TASKS)
            workers = [bounded(i) for i in range(start, end)]
            await asyncio.wait_for(
                asyncio.gather(*workers),
                timeout=HARD_TIMEOUT - (time.perf_counter() - start_time)
            )
    except asyncio.TimeoutError:
        timed_out = True
        print(f"   ⏰ {i18n( 'timeout')}! ({HARD_TIMEOUT}s)")

    elapsed = time.perf_counter() - start_time
    total_errors = sum(error_counts.values())

    print(f"\n   📊 {i18n( 'results')}:")
    if timed_out:
        print(f"   ❌ {i18n( 'timed_out_msg').format(timeout=HARD_TIMEOUT, completed=format_number(completed), total=format_number(TOTAL_TASKS))}")
        print(f"   ⏱️  {i18n( 'terminated').format(elapsed=elapsed)}")
        return {"name": name, "completed": completed, "elapsed": elapsed,
                "ops": int(completed/elapsed) if elapsed > 0 else 0,
                "error_counts": error_counts, "timed_out": True}
    else:
        ops = int(completed / elapsed) if elapsed > 0 else 0
        icon = "✅" if total_errors == 0 else "⚠️"
        print(f"   {icon} {i18n( 'success')}: {format_number(completed - total_errors)} / {format_number(completed)}")
        print(f"   ⏱️  {i18n( 'elapsed')}: {elapsed:.1f}s")
        print(f"   🚀 {i18n( 'throughput')}: {format_number(ops)} {i18n( 'ops')}")
        if error_counts:
            print(f"   ❌ {i18n( 'error_breakdown')} ({format_number(total_errors)} {i18n( 'errors')}):")
            for err_type, count in error_counts.most_common():
                print(f"      [{count:>6}] {err_type}")
        return {"name": name, "completed": completed, "elapsed": elapsed,
                "ops": ops, "error_counts": error_counts, "timed_out": False}


async def main():
    print("╔" + "═" * 58 + "╗")
    title = i18n( "title")
    print("║" + title.center(58) + "║")
    print("╠" + "═" * 58 + "╣")
    print(f"║   {i18n( 'total_tasks')}: {format_number(TOTAL_TASKS):>12}    {i18n( 'concurrency')}(aya/aio): {AYAFILEIO_CONCURRENT}/{AIOFILES_CONCURRENT:<3} ║")
    print(f"║   {i18n( 'hard_timeout')}: {HARD_TIMEOUT}s    {i18n( 'mem_limit')}: {MAX_MEMORY_MB}MB              ║")
    print("╚" + "═" * 58 + "╝")

    info = ayafileio.get_backend_info()
    aya_icon = "✅" if info["is_truly_async"] else "❌"
    print(f"\n🔧 {i18n( 'backend_info').format(name=i18n( 'backend_aya'), platform=info['platform'], backend=info['backend'], async_icon=aya_icon)}")
    print(f"🔧 {i18n( 'backend_info').format(name=i18n( 'backend_aio'), platform=info['platform'], backend=i18n( 'thread_pool'), async_icon='❌')}")

    results = []

    # ── Test 1: ayafileio single file ──
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp_path = tmp.name
    try:
        async with ayafileio.open(tmp_path, "wb+") as f:
            r = await run_benchmark(
                i18n("single_file"),
                AYAFILEIO_CONCURRENT, worker_ayafileio, f)
            results.append(r)
    finally:
        os.unlink(tmp_path)

    # ── Test 2: aiofiles single file ──
    if results[-1]["timed_out"]:
        print(f"\n   ⏭️ {i18n( 'skipped')}")
    else:
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp_path = tmp.name
        try:
            async with aiofiles.open(tmp_path, "wb+") as f:
                r = await run_benchmark(
                    i18n("single_file_aio"),
                    AIOFILES_CONCURRENT, worker_aiofiles, f)
                results.append(r)
        finally:
            os.unlink(tmp_path)

    # ── Summary ──
    print("\n" + "=" * 60)
    print(f"📊 {i18n( 'summary')}")
    print("=" * 60)
    print(f"{i18n( 'benchmark'):<30} {i18n( 'completed'):>8} {i18n( 'elapsed'):>8} {i18n( 'throughput'):>10} {i18n( 'truly_async'):>6}")
    print("-" * 60)
    for r in results:
        total_err = sum(r["error_counts"].values())
        status = format_number(r["completed"])
        if r["timed_out"]:
            status += "*"
        err_flag = f" ({format_number(total_err)} {i18n( 'errors')})" if total_err else ""
        aya_icon = "✅" if "aiofiles" not in r["name"] else "❌"
        print(f"{r['name']:<30} {status:>8} {r['elapsed']:>7.1f}s "
              f"{format_number(r['ops']):>9}{i18n( 'ops')}  {aya_icon} {err_flag}")

    # ── Error details ──
    for r in results:
        if r["error_counts"]:
            total_err = sum(r["error_counts"].values())
            print(f"\n📋 {r['name']} — {i18n( 'error_breakdown')} ({format_number(total_err)} {i18n( 'errors')}):")
            for err_type, count in r["error_counts"].most_common():
                pct = count / total_err * 100
                print(f"   [{count:>6} | {pct:5.1f}%] {err_type}")

    if len(results) >= 2 and not results[0]["timed_out"] and not results[1]["timed_out"]:
        speedup = results[0]["ops"] / max(results[1]["ops"], 1)
        print(f"\n💡 {i18n( 'speedup').format(speedup=speedup)}")
        print(f"   {i18n( 'speedup_note').format(aio=AIOFILES_CONCURRENT, aya=AYAFILEIO_CONCURRENT)}")


if __name__ == "__main__":
    asyncio.run(main())
