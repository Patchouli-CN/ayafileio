# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.4] - 2026-06-02

### Fixed
- **macOS GCD fd reuse race condition**: `dispatch_io_create` takes ownership of the file descriptor and closes it asynchronously during `dispatch_io_close`. The `dispatch_barrier_sync` previously used only guarantees dispatch queue drain, not kernel-level fd close completion. When a subsequent `open()` reused the same fd number before GCD's delayed close completed, GCD would close the **new** file's fd, causing `OSError: [Errno 9] EBADF` (bad file descriptor) on macOS. Fixed by `dup()`-ing the fd before passing to `dispatch_io_create` — GCD operates on the dup'd copy while the original fd remains fully under our control, eliminating the race entirely.

### Changed
- **CI: consolidated `release.yml` into `build_wheel_ci.yml`**: The two workflows had identical triggers but different build matrices. `build_wheel_ci.yml` is a strict superset (more architectures: riscv64, aarch64, x86, universal2; more runners: macos-15-intel; more Python versions: includes cp313t). Removed `release.yml` and added PyPI publish job and explicit `CIBW_BUILD` version pinning to `build_wheel_ci.yml`.

## [1.4.3] - 2026-05-30

### Added
- **`AsyncFile.chunk(chunk_size, *, buf=None)` — streaming chunk iterator**: Zero-copy fixed-size chunked reading built on `readinto`. Each iteration yields a `memoryview` into the filled portion of the buffer — no per-chunk allocation. Ideal for large-file streaming, network upload segmentation, and similar patterns.
  ```python
  # Built-in buffer
  async for chunk in f.chunk(4096):
      process(chunk)

  # Pre-allocated buffer (more efficient for hot paths)
  buf = bytearray(65536)
  async for chunk in f.chunk(4096, buf=buf):
      sock.send(chunk)
  ```
  Binary mode only (text mode raises `ValueError` — use `readline` instead). When the user-provided `buf` is smaller than `chunk_size`, the effective read size is automatically clamped to the buffer capacity.

### Changed
- **Yuyuko `yuyuko_memlife.hpp` fixes**:
  - `static` globals → `inline` (fixes header-only ODR issue: multiple translation units no longer get independent copies of global state)
  - `static` free functions → `inline` (eliminates per-TU code bloat)
  - Fixed `snprintf` truncation bug in `yuyuko_logln` that could read past valid data in `buf`
  - `find_soul` / `find_containing_soul` return `const SoulRecord*`, eliminating `const_cast` hack
  - Added explicit `<thread>` include
- **Yuyuko incremental CRC update**: Added `mem_snapshot_update(ptr, offset, len)` — when modifying small ranges within large tracked buffers, only affected chunks are recomputed. 4-byte write to a 1GB buffer: full recompute ~130ms → incremental ~0.5μs (260,000× faster). Convenience macro `MEM_SNAPSHOT_UPDATE`.
- **Yuyuko upgraded to C++20**: Async log engine `std::thread` → `std::jthread` with RAII auto-join, simplifying shutdown. `__cpp_lib_jthread` feature detection for AppleClang 15 compatibility.
- **`ENABLE_ASAN` → `ENABLE_YUYUKO`**: Compile definition renamed to accurately reflect what it controls (Yuyuko memory tracker, not AddressSanitizer). Auto-enabled on Windows MSVC (where ASAN support is poor). Disabled by default on Linux/macOS (where real ASAN is available); opt-in via `-DENABLE_YUYUKO=ON`.
- **`ignore/yuyuko` ↔ `src/utils` version sync**: Both copies of `yuyuko_memlife.hpp` are now functionally identical, differing only in the production version's `ENABLE_YUYUKO` conditional compilation and zero-overhead stubs.

## [1.4.2] - 2026-05-29

### Fixed
- **IOCP double‑delivery use‑after‑free crash**: Under extreme concurrency (~50K coroutines), Windows IOCP can deliver the same `OVERLAPPED` completion to `GetQueuedCompletionStatusEx` multiple times — including across separate batch calls ~10–15ms apart. When the first delivery freed the `IORequest` and a later delivery arrived after the heap reused the same address for a new `IORequest`, the double‑delivery CAS guard (`IOState`) saw `PENDING` from the **new** object and incorrectly processed the stale completion, corrupting the new `IORequest`'s data and causing an access violation in `PyGILState_Release`. Fixed with a **quarantine‑based deferred‑free** mechanism: `process_one` inserts freed `IORequest` objects into a thread‑local quarantine list instead of deleting them immediately. Quarantined objects are held for 1 second before being freed, preventing address reuse during the IOCP double‑delivery window. The CAS guard order was also corrected — the `compare_exchange_strong` on `IOState` now executes **before** `Yuyuko::check_access`, so that double deliveries are silently rejected by the CAS without touching any potentially‑stale fields.
- **Yuyuko memory tracker enhanced** (used to diagnose the above bug): Added `TRACKED_NEW_ARGS` / `TRACKED_PLACEMENT_NEW` / `TRACKED_PLACEMENT_NEW_ARGS` macros for constructor‑argument and placement‑new allocation tracking. Added `reserve_souls(alive_cap, released_cap)` to pre‑size the soul‑book hash tables. Added `MEM_GUARD_BATCH_DEF` / `MEM_GUARD_BATCH_ADD` convenience macros for batch memory guards.

### CI
- **`test_critical.py` rewritten for stability**: Write test now uses unique pre‑created files per worker instead of all workers opening the same `"wb"` (`CREATE_ALWAYS`) file — eliminating concurrent‑truncation data corruption. Added `asyncio.gather(return_exceptions=True)` + failure reporting, `gc.disable()` during benchmark, `tempfile.mkdtemp`‑based cleanup, and reduced the read test file from 1GB to 100MB.

## [1.4.1] - 2026-05-29

### Changed
- **IOCP file size caching**: `submit_read`, `submit_readinto`, `submit_write` (append), and `submit_seek` (SEEK_END) no longer call `GetFileSizeEx` on every operation. The file size is now cached in the `Session` struct — initialized once at `create_session` and refreshed on write/truncate — eliminating a kernel syscall per I/O on the hot path. `windows_io_backend.cpp` constructors also use the cached value for append-mode position init.
- **`ResultBatcher` dual-trigger flush**: Replaced the unconditional `|| true` flush in `flush_batchers()` with threshold-triggered flushes from `process_one`. When `push()` reaches the batch threshold, the batcher flushes immediately; otherwise the idle timeout (5ms) controls flush timing. Reduces unnecessary `call_soon_threadsafe` calls under low-to-moderate load while preserving responsiveness under high load.
- **Thread-local `BufferPool` cache**: `pool_acquire*` / `pool_release` now use a per-thread cache (up to 8 buffers) before falling through to the global `BufferPool` mutex. The common case — release then quickly re-acquire on the same thread — avoids the global lock entirely.

### Fixed
- **`test_critical.py` write test data corruption**: The write stress test previously had all workers opening the same file path with `"wb"` (`CREATE_ALWAYS`), causing concurrent truncation and data corruption. Each worker now writes to a unique pre-created file. Added `return_exceptions=True` + failure reporting, `gc.disable()` during benchmark, `tempfile.mkdtemp`-based cleanup, and reduced the read test file from 1GB to 100MB.

## [1.4.0] - 2026-05-27

### Changed
- **Upgraded C++ standard from C++17 to C++20**: Modernized the entire codebase to leverage C++20 features for both performance and safety. The minimum compiler requirement is now GCC 10+, Clang 10+, or MSVC 19.28+ (Visual Studio 2019 16.8+).
- **`counting_semaphore` for `close_impl()` wait loop**: The io_uring backend's `close_impl()` previously used an exponential-backoff `sleep_for()` polling loop (starting at 1ms) to wait for pending I/O completions. Replaced with `std::counting_semaphore::try_acquire_for()` — the semaphore is released by `complete_ok()`/`complete_error()` in the base class, so when the last I/O completes, `close()` wakes immediately instead of waiting for the next sleep cycle. Worst-case close latency drops from ~1ms to ~10μs.
- **`[[likely]]` / `[[unlikely]]` branch prediction hints**: Added C++20 standard branch prediction attributes to key hot paths: `complete_ok()` switch on Read/Write, `IOUringBackend::read()`/`write()` early-exit paths (closed file, EOF, zero-size), `BufferPool::acquire()` cache hit, and `HandlePool::acquire()` cache miss. The CPU branch predictor now biases correctly on these frequently-executed paths, reducing pipeline flushes.
- **`std::jthread` for thread lifecycle management**: Replaced `std::thread` with `std::jthread` in `GlobalThreadPool` and Yuyuko's async log engine. `jthread` provides RAII auto-join on destruction and integrated `std::stop_token` support, simplifying shutdown logic and eliminating manual `join()` calls.
- **`std::span` for buffer passing**: The io_uring backend's `submit_io()` now accepts `std::span<const std::byte>` instead of separate `(const void*, size_t)` parameters. Zero-overhead abstraction — the compiler generates identical code — but eliminates buffer/length mismatch bugs at compile time.

### Fixed
- **`global_thread_pool` shutdown race**: Replaced `std::atomic<bool> m_stop` with `std::atomic<unsigned> m_running_workers`, eliminating a TOCTOU race between `ensure_started()` and `shutdown()`.
- **GCC 11 compilation error on `[[unlikely]]` catch clause**: GCC 11 does not support the `[[unlikely]]` attribute on `catch` clauses (supported since GCC 12). Removed the attribute from the `catch (const std::runtime_error&)` sites in the io_uring backend to restore compatibility with GCC 11.
- **`std::jthread` / `<stop_token>` unavailable on AppleClang 15**: AppleClang 15 (Xcode 15) ships libc++ without `<stop_token>` and `std::jthread` support. Added `__cpp_lib_jthread` feature test macro guards in `GlobalThreadPool` and Yuyuko's async log engine, with automatic fallback to `std::thread` + `std::atomic<bool>` when the feature is unavailable.

### CI
- **Upgraded CI runner images**: Linux `ubuntu-22.04` → `ubuntu-24.04` (GCC 11 → GCC 14), macOS `macos-14` → `macos-15` (AppleClang 15 → 16).
- **Skip CI on docs-only changes**: Added `paths-ignore` to push/PR triggers in `test.yml` to skip the Build and Test pipeline when only documentation files (`**/*.md`, LICENSE, .gitignore, MANIFEST.in) are changed. Since `build_wheel_ci.yml` and `release.yml` chain off `workflow_run` on test success, they are automatically skipped too.

## [1.3.1] - 2026-05-22

### Changed
- **Removed `LoopHandle`, unified on `ResultBatcher`**: Deleted the `LoopHandle` class and its `loop_handle.hpp`/`.cpp` files (~150 lines). `LoopHandle` and `ResultBatcher` had completely overlapping responsibilities — both batch-collect future results and dispatch them to the event loop via `call_soon_threadsafe`. After unification, all three non-Windows backends (io_uring, macOS GCD, ThreadIO) use a global batcher registry (`get_or_create_batcher`), and `IORequest::loop_handle` was renamed to `batcher`. The `ResultBatcher::push()` + `flush()` pattern replaces `LoopHandle::push()`'s immediate-schedule approach, naturally inheriting threshold batching, idle timeout, retry-on-failure, and `InvalidStateError` suppression for all backends.

### Fixed
- **Windows Ctrl+C access violation crash**: The console control handler (`ctrl_handler`) was calling `close_all_sessions()` from a Windows system thread that does not hold the Python GIL. The wait loop for pending IOCP completions deadlocked because the IOCP workers needed the GIL to process cancellation packets, but the GIL was held by the main thread running the event loop. After the 500ms timeout, `CloseHandle` was called with I/O still in flight → undefined behavior → access violation. Fixed by only setting `trigger_ctrlc()` in the handler for `CTRL_C_EVENT`/`CTRL_BREAK_EVENT` and returning `FALSE` to let Python's own console handler raise `KeyboardInterrupt` normally, so cleanup proceeds through the GIL-safe `submit_close()` path (which already releases the GIL during its wait loop since v1.3.0).
- **Windows Ctrl+C `InvalidStateError` noise**: During asyncio task cancellation, `Task.cancel()` calls `future.cancel()` on the inner future, transitioning it to `CANCELLED` state. When the IOCP worker later completed the I/O and attempted `future.set_result()`, it raised `asyncio.exceptions.InvalidStateError` which was printed to stderr via `PyErr_Print()`. Fixed by caching `asyncio.exceptions.InvalidStateError` in the module globals and silently suppressing it in both `ResultBatcher` and `LoopHandle` drain callbacks — a cancelled future means no one is waiting for the result, so discarding is correct.
- **All platforms Ctrl+C GIL deadlock in `close_impl()`**: The `close_impl()` wait loop in the io_uring, macOS GCD, and ThreadIO backends called `std::this_thread::sleep_for()` while holding the GIL, preventing the I/O completion callbacks (io_uring reaper, GCD dispatch callbacks, thread pool workers) from acquiring the GIL via `PyGILState_Ensure()` to finish processing completions. The `pending` counter was already decremented atomically before the GIL acquire, so the wait loop would see `pending == 0` and proceed, but the completion callbacks would remain blocked. On the last `close()`, the io_uring `UringInstance` destructor called `reaper_thread.join()` while the reaper was still blocked on `PyGILState_Ensure()` → deadlock. Fixed by wrapping the sleep in `Py_BEGIN_ALLOW_THREADS`/`Py_END_ALLOW_THREADS` in all three backends, matching the v1.3.0 fix for Windows `submit_close()`.
- **macOS GCD EBADF (errno 9) after `close()`**: `dispatch_io_close(m_channel, DISPATCH_IO_STOP)` is an asynchronous call — the system's cleanup handler closes the underlying fd on a later GCD callback. If the fd number is reused by a subsequent `open()` before that callback runs (POSIX fd reuse), the GCD cleanup incorrectly closes the new file's fd, causing subsequent I/O to fail with EBADF. Fixed by adding `dispatch_barrier_sync` on the channel's serial queue after `dispatch_io_close` to wait for all pending callbacks and the cleanup handler to finish before `close_impl()` returns. The GIL is released during the barrier wait (`Py_BEGIN_ALLOW_THREADS`) so GCD callbacks can make progress.
- **Windows IOCP mixed read/write hang**: When `call_soon_threadsafe` failed during `ResultBatcher::flush()` or `LoopHandle::push()`, both would silently discard all pending batch entries (DECREF-ing `set_result`/`set_exception` without calling them), leaving the associated futures unresolved and their awaiting coroutines permanently hung. Scene 4 (mixed read/write) activates both IOCP workers simultaneously, which under high concurrency can trigger an occasional `call_soon_threadsafe` failure → dropped batch → hang. Fixed by not discarding the batch on failure; instead only resetting the `dispatch_pending` flag so the next `flush()`/`push()` retries scheduling.

### CI
- **Added Python 3.14t free-threading builds** to the test matrix for Linux, Windows, and macOS.

## [1.3.0] - 2026-05-21

### Fixed
- **Windows IOCP double‑delivery crash**: Under extreme concurrency (~50K concurrent readinto operations) `GetQueuedCompletionStatusEx` could return the same `OVERLAPPED` completion twice, causing use‑after‑free and `STATUS_HEAP_CORRUPTION` (0xC0000374). Added `IOState` atomic flag to `IORequest` with CAS at `process_one` entry to safely skip duplicate completions. (Captured duplicates in 12/20 stress runs.)
- **Pending counter underflow on sync I/O**: Synchronous completions were double‑decremented (once in the submission path, once by the IOCP worker), causing `pending` to go negative and `close()` to skip `CancelIoEx` + wait. Fixed by removing the submission‑path decrement and letting the worker handle it uniformly.
- **GIL deadlock in `submit_close`**: The `Sleep` wait loop held the GIL, preventing the IOCP worker from acquiring it to process cancelled completions. Added `Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS` around `Sleep`.
- **Handle pool / IOCP association conflict causing heap corruption**: `submit_close` returned IOCP‑associated HANDLEs to the pool cache. When `create_session` later retrieved the same HANDLE and called `CreateIoCompletionPort` to update the completion key, it failed with err=87 (`ERROR_INVALID_PARAMETER`). The error path called `CloseHandle`, after which the kernel reused the same HANDLE value, causing cascading invalid HANDLE (err=6) and heap corruption crashes. Fixed by closing IOCP HANDLEs directly instead of pooling them, and added `handle_pool_evict()` to purge all cached HANDLEs for a key on IOCP association failure. Stress test: 500K readinto ops (50K concurrent) passed with zero crashes.
- **Yuyuko memory tracker integration and fixes**: Integrated the project's built‑in Yuyuko (yuyuko_memlife.hpp) memory lifecycle tracker into the IOCP `IORequest` allocation/deallocation paths. Fixed several issues in the tracker itself: `ShadowMemory` array out‑of‑bounds on 64‑bit address spaces (replaced with soul‑book hash‑map lookup), `TRACKED_NEW`/`TRACKED_DELETE` macros bypassing constructors/destructors (switched to `new`/`delete`). Added MSVC support for the `ENABLE_ASAN` compile option. Yuyuko caught one use‑after‑free from IOCP double‑delivery during stress testing (correctly defended by the `IOState` CAS guard).

### Changed
- **CMakeLists.txt debug control unified**: Replaced `ENABLE_DEBUG` option with standard `CMAKE_BUILD_TYPE` (Debug / Release / RelWithDebInfo). Extracted common compiler flags for MSVC and GCC/Clang, reducing duplication.
- **`LoopHandle` global cache cleanup**: Added `clear_loop_handles()` to free all cached `LoopHandle` objects at module cleanup, fixing a memory leak on repeated init/shutdown cycles.

## [1.2.0] - 2026-05-17

### Changed
- **IOCP batch completion harvesting**: Replaced single `GetQueuedCompletionStatus` with `GetQueuedCompletionStatusEx` using a dual-phase strategy: Phase 1 blocks on a single entry with `INFINITE` timeout, Phase 2 non-blocking drains up to `iocp_batch_size - 1` additional completions. Significantly reduces syscall overhead under high concurrency.
- **`iocp_batch_size` config**: New configuration parameter (default 64, range 1-256) controls the maximum number of completion entries harvested per IOCP worker cycle. Configurable via `ayafileio.configure({"iocp_batch_size": N})` and readable via `ayafileio.get_config()`. Python `AyafileioConfig` TypedDict and `configure()` docstring synced accordingly.

## [1.1.6] - 2026-05-15

### Changed
- **`wrap_fd` → `wrap_file`**: Renamed to reflect that it now accepts both file descriptors (`int`) and file-like objects with a `fileno()` method. Added `is_real_file()` validation to reject non-regular files (sockets, pipes, ttys).
- **`FileObj` type**: New `_HasFileno` Protocol exported as `FileObj` in `types.py`, representing any object with `fileno() -> int`.
- **`drain_handle_pool` / `drain_buffer_pool`**: Re-exported through `_compat.py` with proper Python wrapper functions instead of direct C++ imports.
- **Development Status**: Updated from "4 - Beta" to "5 - Production/Stable" in `pyproject.toml`.

### Fixed
- **macOS DW_BAD_DESCRIPTOR write errors**: `dispatch_data_create(DISPATCH_DATA_DESTRUCTOR_DEFAULT)` and `IORequest` destructor both attempted to free the same buffer, causing heap corruption and intermittent EBADF on macOS. Fixed by adding `IORequest::release_buffer_ownership()` to properly transfer ownership to `dispatch_data`.
- **`malloc`/`free` mismatch**: `PoolBuf` and `make_req` allocated buffers with `new[]`/`delete[]`, but `dispatch_data_create` released them with `free()`. Changed all buffer allocations to `std::malloc`/`std::free`.

## [1.1.5] - 2026-05-11

### Added
- **`auto_flush` parameter**: `open()` and `AsyncFile` now accept an `auto_flush` keyword (default `False`). When `True`, the file is automatically flushed before `close()` inside `__aexit__`, ensuring buffered data is persisted to disk before the context manager exits.

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