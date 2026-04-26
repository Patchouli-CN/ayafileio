# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.2 & 1.0.2.post1] - 2026-04-26

### Added
- Linux backend now supports asynchronous file opening via `io_uring`'s `IORING_OP_OPENAT`, using a dedicated local ring to avoid contention with the reaper thread.
- New benchmark scenario: "Tempfile storm" (open-read-close without handle reuse, 2000 files × 4KB) in `test_speed.py`.

### Changed
- **Complete rewrite of `IOUringBackend` architecture**:
  - File opening (`open`) now uses a standalone `io_uring` instance (`local_ring`), fully isolated from the reaper thread's shared ring.
  - The shared ring (managed by `UringManager`) is now lazily initialized on the first `read()` or `write()` call, rather than during construction.
  - This "dual-ring" design eliminates all CQE contention between the constructor and the reaper, resolving the persistent segfault on Linux.
- `ensure_loop_initialized()` simplified: now solely responsible for acquiring the shared ring for read/write operations.
- Reaper loop streamlined: no longer needs to handle `char*` user_data (since OPENAT uses its own ring), reducing branching in the hot path.

### Fixed
- Fixed segfault on Linux caused by `io_uring_wait_cqe` contention between the constructor's `IORING_OP_OPENAT` and the reaper thread on the same ring.

## [1.0.1.post2] - 2026-04-26

### Fixed
- **ThreadIOBackend**: Fixed a deadlock in `close_impl()` where worker threads were not notified to wake up before being joined. Now calls `m_cv.notify_all()` prior to `join()` to ensure workers see the stop flag and exit cleanly.
- **MANIFEST.in**: Fixed filename typo (was `MAIFEST.in`).

### Added
- **CHANGES.md**: Started maintaining a changelog.

### Changed
- **config.hpp**: Removed unused `enable_debug_log` and `enable_perf_stats` configuration options. Removed empty `from_env()` method and the unimplemented callback system (`register_callback`, `on_config_changed`). Reduced code by approximately 130 lines.
- **CMakeLists.txt**: Elevated the liburing detection message from `STATUS` to `WARNING` when the library is not found, prompting users to install the appropriate development package.

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