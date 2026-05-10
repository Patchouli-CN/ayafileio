# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.4] - 2026-05-10

### Fixed
- **macOS seek + read returns empty data**: `seek()` called `lseek()` on the raw file descriptor, interfering with the Dispatch I/O channel's internal state on first use. Removed `lseek()` — the macOS backend uses `DISPATCH_IO_RANDOM` with explicit offsets, so the fd position is irrelevant.
- **macOS `dispatch_io_read` multi-callback data loss**: `dispatch_io_read` may deliver data across multiple callbacks (first with `data` + `done=false`, then with `NULL` + `done=true`). The accumulated byte count is now tracked across callbacks via `__block` variable.
- **Heap corruption from `malloc`/`free` mismatch**: `PoolBuf` and `make_req` allocated buffers with `new[]`, but `dispatch_data_create(..., DISPATCH_DATA_DESTRUCTOR_DEFAULT)` freed them with `free()`. Changed all buffer allocations to `std::malloc`/`std::free`.
- **CI test steps always passing**: Removed `continue-on-error: true` from test run steps in `test.yml`, so test failures now correctly fail the workflow. Release workflow now depends on test workflow success via `workflow_run` trigger.
- **CI verbose debug logging**: Changed test builds from Debug to Release mode with debug/verbose logging disabled, reducing log output noise.

## [1.1.2.post1] - 2026-05-06

### Fixed
- **POSIX `wb+` mode regression**: `parse_mode()` set `O_RDWR` via `if (mi.plus) flags = O_RDWR`, which overwrote `O_CREAT` / `O_TRUNC` / `O_APPEND` flags. This caused `FileNotFoundError` when opening files with `wb+`, `ab+`, etc. Fixed in all three POSIX backends by using `(flags & ~O_ACCMODE) | O_RDWR`.
- **Double `[Errno N]` in exception messages**: `throw_os_error()` / `set_os_error()` manually formatted `[Errno %d]` into the message string, but Python's `OSError` constructor also adds the prefix — resulting in `FileNotFoundError: [Errno 2] [Errno 2] Failed to open file: '/path'`. Now uses the proper 2-arg/3-arg `OSError` constructor forms.
- **atexit segfault on failed open (Windows)**: `WindowsIOBackend` inserted `this` into `g_openFiles` in the constructor before `CreateFileW` / `CreateIoCompletionPort` could fail. If construction threw, the destructor never ran, leaving a dangling pointer in the global set. `atexit` cleanup (`close_all_files`) then crashed iterating over it. Insertion is now deferred until all fallible operations succeed.

## [1.1.2] - 2026-05-06

### Changed
- **`ThreadIOBackend`: global shared thread pool** — Worker threads are now created once and shared across all `ThreadIOBackend` instances, eliminating per-file-open thread creation/destruction (~16 threads per cycle). On Linux/macOS fallback paths, repeated open-close cycles are now orders of magnitude faster.
- **`WindowsIOBackend::close_impl()` optimized** — Removed unnecessary `SetFilePointerEx` (all I/O uses explicit `OVERLAPPED.Offset`). `CancelIoEx` is now skipped when there is no pending I/O (`m_pending == 0`), saving one syscall per close in the common case.
- **`handle_pool_release` lock optimization** — `CloseHandle` syscall moved outside the writer lock to avoid blocking concurrent `handle_pool_acquire` operations. Replaced `operator[]` with `find()` to avoid creating empty map entries for handles that won't be pooled.
- **`handle_pool_release` uses atomics** — Pool size limits are now read from lock-free atomics (`g_hpMaxPerKey` / `g_hpMaxTotal`) instead of `ConfigManager` (which requires a `shared_mutex` lock on every call).
- **`BufferPool::release` uses atomic cache** — `buffer_pool_max` is cached in an atomic, refreshed only on `configure()`, avoiding `ConfigManager` reads on every buffer release.
- **`WindowsIOBackend` skips `make_pool_key` for non-poolable modes** — `CREATE_ALWAYS` / `CREATE_NEW` modes (w/x) no longer compute handle pool keys, saving filesystem path normalization overhead.
- **`AsyncFile.readline()` uses `bytearray` internally** — Replaced `bytes += bytes` concatenation with `bytearray.extend()`, eliminating O(n²) memory allocation for large files read line-by-line.
- **`AsyncFile.writelines()` batched** — All lines are now joined and written in a single `write()` call instead of N individual async calls.

### Added
- **`drain_handle_pool()` / `drain_buffer_pool()` public API** — New functions to drain cached handles and I/O buffers at runtime (useful between benchmark rounds or after bulk tempfile operations).
- **`GlobalThreadPool` singleton** — Cross-platform shared thread pool for `ThreadIOBackend`. Created at `src/global_thread_pool.hpp` / `.cpp` and compiled on all platforms for consistent cleanup.

### Fixed
- **`test_base.py`: async test exceptions silently swallowed** — `run_async()` previously caught all exceptions with `except Exception as e: ...` without reporting them or incrementing the failure counter. Now properly reports `AssertionError` and other exceptions.
- **`_config.py`: `NameError` on `reset_config()`** — `_CACHE_MAX_SIZE` and `_CACHE_ENABLED` were referenced as `global` but never defined at module level. Now properly declared.
- **`_compat.py`: removed `globals()` anti-pattern** — Replaced `globals()["_io_worker_count"] = count` with a proper module-level variable and `global` declaration.
- **`util.py`: replaced bare `except:` with `except Exception:`** — Three bare `except` clauses could swallow `KeyboardInterrupt` and `SystemExit`.

### Test & Benchmark
- **`test_speed.py` Scene 5 (tempfile storm) Windows fix** — Scene 5 previously hung on Windows due to: (a) excessive concurrency (1000 → 200), (b) each file opened twice (write then read → now single `wb+`), (c) zombie handles accumulating in the pool across rounds (now drained before each round). Scene 5 now completes in ~3s on Windows.

## [1.1.1.post1] - 2026-05-02

### Removed
- **`handle_pool_posix.cpp`**: Removed the empty handle pool stub for POSIX platforms. On POSIX, `open()`/`close()` are microsecond-level operations, making handle caching unnecessary. The API surface is preserved via inline stubs in the header for cross-platform compatibility.

### Changed
- **`handle_pool.hpp`**: Added inline stub functions for POSIX platforms, replacing the deleted `handle_pool_posix.cpp`.
- **`CMakeLists.txt`**: Removed references to `handle_pool_posix.cpp` from macOS and Linux builds, reducing the number of compiled source files.

## [1.1.1] - 2026-05-01

### Added
- **Python 3.14+ free-threading support**: Added `FREE_THREADED` flag to `nanobind_add_module()`. `ayafileio` now runs natively on `python3.14t` (free-threading builds) without GIL.
- **CMake build enhancement**: `nanobind_add_module` now passes `FREE_THREADED` argument, automatically handling free-threaded compilation targets.
- **CI matrix expansion**: Added Python 3.14 free-threading builds to CI, covering Windows / Linux / macOS.

### Tested & Verified
- **Windows (IOCP)**: 45/45 tests passed, Scenario 3 append write **3.06x** speedup vs aiofiles
- **Linux (io_uring)**: 45/45 tests passed, Scenario 5 tempfile storm **1.92x** speedup vs aiofiles
- **macOS (Dispatch I/O)**: 45/45 tests passed, loguru concurrent write **2.1x** speedup vs thread pool
- All tests run stably under free-threading environment — no data races, no GIL reentrancy issues

### Changed
- `CMakeLists.txt`: Added `FREE_THREADED` parameter to `nanobind_add_module` invocation

### Notes for packagers
- Pre-built wheels for Python 3.14t are now included in the release assets
- Source distribution remains unchanged; free-threading support is enabled at build time


## [1.1.0] - 2026-04-30

### Added
- **`tell()`**: Return the current file position. Implemented in all four backends
  as a pure in-memory operation (no syscall needed).
- **`truncate(size)`**: Truncate/expand the file to the given size.
- **`fileno()`**: Return the underlying file descriptor (POSIX) or CRT fd (Windows).
- **`readinto(buf)`**: Zero-copy read directly into a pre-allocated `bytearray` or
  `memoryview`. Returns the number of bytes read instead of a new `bytes` object.
  Only available in binary mode.
- **`readable()` / `writable()` / `seekable()`**: Query file access modes.
- **`writelines(lines)`**: Write multiple lines in batch.
- **`readall()`**: Alias for `read(-1)`.
- **`isatty()`**: Check if the file is a TTY.
- **`mode` property**: Now returns the original mode string (e.g., `"rb"`, `"w+"`).

### Changed
- **Refactored `IOBackendBase`**: Four shared methods (`complete_ok`, `complete_error`,
  `make_req`, `complete_error_inline`) are now implemented once in the base class,
  eliminating ~240 lines of duplicate code across the four backends.
- **`IORequest` extended**: Added `isReadinto`, `userBuf`, and `userBufView` fields
  to support zero-copy `readinto()`. `buf()` and destructor automatically handle
  the new readinto path.
- **`complete_ok` in `io_backend.cpp`**: Now uses `switch-case` on `ReqType` for
  cleaner dispatch. `readinto` requests return `int` instead of `bytes`.
- **Backend `.hpp` files cleaned up**: Removed redundant declarations of `m_pending`,
  `m_loop_handle`, `m_cached_buffer_size`, `m_cached_buffer_pool_max`,
  `m_cached_close_timeout_ms`, and unused `m_barrierMtx`/`m_barrierCv`.
  All now inherited from `IOBackendBase` (protected).

## [1.0.5.post1] - 2026-04-29

### Fixed
- **MacOSGCDBackend**: Fixed a critical `ContextVar` reentrancy error and sporadic
  `Segmentation fault` on macOS when calling `seek()` or `flush()`.
  These methods previously used `dispatch_io_barrier`, whose callback could
  execute on an arbitrary GCD thread and attempt to acquire the Python GIL via
  `PyGILState_Ensure`, causing a conflict with `asyncio`'s internal context
  manager state. In concurrent scenarios (e.g., rapid `open()` / `close()` of
  the same file), this also led to use-after-free of file descriptors.
  Both `seek()` and `flush()` now execute synchronously on the main thread,
  eliminating all cross-thread GIL contention and object lifetime issues.

### Changed
- **MacOSGCDBackend**: `seek()` and `flush()` no longer use `dispatch_io_barrier`.
  They now perform `lseek` and `fsync` directly on the calling thread. Since
  both operations complete in microseconds, the synchronous approach is actually
  faster by avoiding GCD scheduling overhead while being fully safe.
- `MacOSGCDBackend` no longer requires the unused `m_barrierMtx` and
  `m_barrierCv` synchronization primitives (can be removed from the header).

## [1.0.5] - 2026-04-28

### Added
- `AsyncFile` is now generic: `AsyncFile[str]` for text mode, `AsyncFile[bytes]`
  for binary mode. IDE autocompletion and mypy now know the exact return type
  of `read()` at compile time.
- `open()` now uses `@overload` to return `AsyncFile[str]` or `AsyncFile[bytes]`
  based on the mode argument.

### Changed
- `wrap_fd()` return type changed from `AyaFileIO[bytes]` to `AsyncFile[bytes]`
  for improved type inference.
- `AyaFileIO` protocol type is now generic (`AyaFileIO[T]`) and kept as an
  internal type in `ayafileio.types` rather than exported in `__init__.py`.
  Users should type-annotate with `AsyncFile[str]` or `AsyncFile[bytes]`
  directly.
- Simplified public API surface: `ayafileio` now exports only `AsyncFile` as
  the main type, removing the redundant `AyaFileIO` from `__all__`.

### Fixed
- `wrap_fd()` now raises `ValueError` at runtime if given a non-binary mode,
  matching the documented restriction.

## [1.0.4] - 2026-04-26

### Added
- **`wrap_fd(fd, mode, *, owns_fd)`**: Wrap an existing **file** descriptor as an
  async I/O object backed by the optimal platform backend (io_uring / IOCP /
  Dispatch I/O). On Windows, the fd is transparently upgraded to an
  overlapped-capable handle. Only file descriptors are supported; sockets and
  pipes should be managed by the event loop.
- **`AyaIO` protocol type** (`ayafileio.types`): A unified async I/O interface
  (`read()`, `write()`, `seek()`, `flush()`, `close()`, `closed`) that both
  `AsyncFile` and `wrap_fd()` return values satisfy.

### Changed
- All backends (`IOUringBackend`, `MacOSGCDBackend`, `ThreadIOBackend`,
  `WindowsIOBackend`) now support construction from a raw file descriptor via
  `FileHandle(int fd, const std::string& mode, bool owns_fd)`.
- `close_impl()` respects the `owns_fd` flag—externally provided file descriptors
  are not closed by ayafileio unless `owns_fd=True`.
- On Windows, `wrap_fd()` obtains the file path from the CRT fd via
  `GetFinalPathNameByHandleW`, closes the original fd if `owns_fd=True`, and
  re-opens the file with `FILE_FLAG_OVERLAPPED` to enable true async IOCP I/O.

### Fixed
- Fixed `PermissionError` on Windows when calling `read()` after `wrap_fd()` with
  write-only mode—the reopened handle now always requests
  `GENERIC_READ | GENERIC_WRITE`.

## [1.0.3] - 2026-04-27

### Added
- `open()` now accepts a `newline` parameter for custom line ending conversion (`None`, `''`, `'\n'`, `'\r\n'`, etc.)
- `open()` now accepts an `errors` parameter for non-strict encoding error handling (e.g., `'ignore'`, `'replace'`, `'strict'`)

### Changed
- `AsyncFile.__slots__` updated to include `_newline` and `_errors` attributes

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