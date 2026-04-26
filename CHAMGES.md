# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] — 2026-04-25

### Added
- Added `CHANGES.md` and `CHANGES_CN.md` for bilingual changelog tracking.
- Added `TypedDict`-based `AyafileioConfig` for IDE-friendly `configure()` autocompletion.
- Added `asyncio.sleep(0)` latency calibration baseline to `test_speed.py` benchmark.

### Changed
- Refactored `__init__.py`: separated concerns into `_async_file.py`, `_open.py`, `_config.py`, `_compat.py`, `_cleanup.py`.
- Improved `configure()` to accept `TypedDict` for better type checking and autocompletion.
- CMake now emits a `WARNING` instead of `STATUS` when `liburing` is not found, so users know they're on the thread pool fallback.
- Improved `warn_fake_async()` message with install instructions for `liburing-dev` / `liburing-devel`.

### Fixed
- Fixed `MANIFEST.in` filename typo.
- Fixed `test_loguru.py` missing `import io` on Windows.
- Fixed `ThreadIOBackend` potential deadlock on Linux under concurrent read workloads.
- Fixed `print_stats`/`print_latency_detail` Rich markup errors when Rich is not installed.

---

## [1.0.0] — 2026-04-24

### Added
- Initial public release.
- Full async file I/O on Windows (IOCP), Linux (io_uring), and macOS (Dispatch I/O / GCD).
- `AsyncFile` class with familiar aiofiles-compatible API.
- Unified `configure()` runtime configuration system with hot-reloading.
- `get_backend_info()` for runtime backend detection.
- `BufferPool` with size-bucketed allocation for memory efficiency.
- `LoopHandle` batched dispatch mechanism for reduced GIL contention.
- Cross-platform benchmark suite (`test_speed.py`) comparing against aiofiles.
- Loguru async sink example and benchmark (`test_loguru.py`).
- Precompiled wheels for Python 3.10–3.14 on Windows, Linux, and macOS via GitHub Actions.