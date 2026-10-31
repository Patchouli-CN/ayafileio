#!/usr/bin/env python3
"""Run isolated pytest iterations with bounded wall time and native hang diagnostics."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


def tail(path: Path, count: int = 200) -> str:
    try:
        return "\n".join(deque(path.read_text(encoding="utf-8", errors="replace").splitlines(), count))
    except OSError:
        return ""


def append_env(name: str, value: str) -> None:
    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a", encoding="utf-8") as handle:
            handle.write(f"{name}={value}\n")


def pytest_command(selection: str, dump_dir: Path) -> list[str]:
    command = [sys.executable, "-m", "pytest"]
    if selection:
        command.append(selection)
    if os.name == "nt":
        command.append("-q")
        procdump = os.environ.get("PROCDUMP")
        if procdump:
            return [procdump, "-accepteula", "-ma", "-e", "-x", str(dump_dir), *command]
    else:
        command.extend(["-vv", "--capture=no", "-o", "faulthandler_timeout=30"])
    return command


def capture_native_stack(pid: int, path: Path) -> None:
    if os.name == "nt":
        return
    if sys.platform == "linux":
        command = ["sudo", "-n", "gdb", "-batch", "-ex", "thread apply all bt full", "-p", str(pid)]
    else:
        command = ["sudo", "-n", "lldb", "-p", str(pid), "-o", "thread backtrace all", "-o", "detach", "-o", "quit"]
    with path.open("w", encoding="utf-8") as output:
        output.write(f"pytest is still running; capturing native stacks for pid {pid}\n")
        output.flush()
        try:
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=45, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass


def terminate_process(proc: subprocess.Popen) -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, check=False)
        return
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        try:
            proc.kill()
        except OSError:
            pass


def run_iteration(iteration: int, selection: str, dump_dir: Path, timeout: int, processes: dict[int, subprocess.Popen]) -> tuple[int, bool, Path]:
    log_path = Path("soak") / f"run-{iteration}.log"
    native_path = Path("soak") / f"native-{iteration}.log"
    before = len(list(dump_dir.glob("*.dmp")))
    with log_path.open("w", encoding="utf-8") as output:
        proc = subprocess.Popen(pytest_command(selection, dump_dir), stdout=output, stderr=subprocess.STDOUT)
        processes[iteration] = proc
        try:
            return_code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            capture_native_stack(proc.pid, native_path)
            terminate_process(proc)
            return_code = proc.wait()
        finally:
            processes.pop(iteration, None)
    after = len(list(dump_dir.glob("*.dmp")))
    passed = return_code == 0 and after == before and re.search(r"[0-9]+ passed", tail(log_path)) is not None
    return iteration, passed, log_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=int(os.environ.get("ITERATIONS", "100")))
    parser.add_argument("--workers", type=int, default=int(os.environ.get("SOAK_WORKERS", "2")))
    parser.add_argument("--timeout", type=int, default=int(os.environ.get("SOAK_TIMEOUT", "120")))
    parser.add_argument("--test-selection", default=os.environ.get("TEST_SELECTION", ""))
    args = parser.parse_args()
    if args.iterations < 1 or args.workers < 1 or args.timeout < 1:
        parser.error("iterations, workers and timeout must be positive")

    dump_dir = Path(os.environ.get("DUMP_DIR", os.path.join(os.environ.get("RUNNER_TEMP", "."), "crash_dumps")))
    Path("soak").mkdir(exist_ok=True)
    dump_dir.mkdir(parents=True, exist_ok=True)
    processes: dict[int, subprocess.Popen] = {}
    try:
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = [
                executor.submit(run_iteration, iteration, args.test_selection, dump_dir, args.timeout, processes)
                for iteration in range(1, args.iterations + 1)
            ]
            for future in as_completed(futures):
                iteration, passed, log_path = future.result()
                if passed:
                    print(f"iteration {iteration} ok: {tail(log_path, 1)}", flush=True)
                    continue
                for proc in list(processes.values()):
                    terminate_process(proc)
                print(f"::error::iteration {iteration} of {args.iterations} failed — stopping here", flush=True)
                print(f"----- tail of {log_path} -----", flush=True)
                print(tail(log_path), flush=True)
                append_env("failed_iteration", str(iteration))
                append_env("SOAK_FAILED", "1")
                return 1
    except KeyboardInterrupt:
        for proc in list(processes.values()):
            terminate_process(proc)
        raise
    print(f"all {args.iterations} iterations passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
