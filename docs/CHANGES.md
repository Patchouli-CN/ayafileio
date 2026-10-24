# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`configure({"backend": ...})` selectsthe backend explicitly.** Values: `auto` (default, platform probing) / `iocp` / `io_uring` / `dispatch_io` / `thread_pool`. It only affects handles opened **afterwards**; already-open files keep their own backend. Forcing a backend that this build or machine does not have raises `ValueError` from `configure()` instead of surfacing at the first `open()`. `get_backend_info()` and `get_capabilities()` now answer in terms of the **effective** backend rather than always reporting the platform default, so "which backend am I actually on, and is it truly async" is finally answerable. Two drive-by corrections: a Linux build without liburing used to report `platform: "posix"`; it now reports `"linux"` (`platform` is where you run, `backend` is what you use — the two should not be conflated). And `configure({"io_uring_sqpoll": True})` is now validated against "**is the effective backend io_uring**" rather than "does the kernel support SQPOLL": after forcing `thread_pool` the option cannot take effect, so it raises `ValueError` instead of being silently accepted on a machine whose kernel happens to support SQPOLL.
- **CI now covers the thread-pool fallback backend.** `ThreadIOBackend` is the only path when io_uring / GCD / IOCP are unavailable, yet it never ran in CI (Linux with liburing uses io_uring, macOS uses GCD, Windows uses IOCP). Two new jobs: `test-fallback-backend` (Linux + macOS, switching the whole suite to the fallback backend via `AYAFILEIO_TEST_BACKEND=thread_pool`; `tests/conftest.py` verifies the switch actually took effect so a silently-ignored setting cannot produce a green run on the wrong backend) and `test-no-io-uring-build` (Linux deliberately without liburing, so `HAVE_IO_URING` is undefined and the thread pool is the only backend — which is exactly what many manylinux users get). Regression tests in `tests/test_backend_select.py`.

### Fixed
- **macOS and the POSIX thread-pool backend: `read()`/`read_at()` did not see data appended by other handles.** That `m_cachedFileSize` scheme (snapshotted at open, updated only by this session's writes/truncates) had the "ask the kernel at the EOF decision point" refresh only on Windows IOCP; the other two backends still treated the stale length as truth — after an external append `read()` stopped at the old EOF, `read_at` returned `b""` past it, and `seek(0, 2)` reported the old length. Both are filled in now, with the same points as Windows: `read`/`read_at`/`readinto` refresh via `fstat` at the **EOF decision point** and after a **short read**, and `seek(SEEK_END)` refreshes first; a full read adds no syscall. The macOS mincore inline fast path is unaffected — refreshing only lets ranges beyond the old EOF fall through to dispatch_io / the thread fast path, and the mapping window itself is untouched.
  Five tests were **un-gated** accordingly: the four size-cache cases in `tests/test_native_resources.py` (including `readinto`) plus the `fileno()` stability case are no longer `WIN_ONLY`. What remains gated is genuinely engine-specific: the duplicated-fd semantics (2), the single-loop bounded retention caused by `g_cachedFutureFn` (1), and the IOCP-only quarantine (1).
- **A negative `seek()` silently corrupted the logical position into 2^64-1.** `seek(-1, 0)` returned `18446744073709551615` and set the position to it; every subsequent `read()` then failed with `ReadFile failed, errno 87` on Windows (or simply read EOF on POSIX) — an error message with no relationship to the cause, and the position never recovered. Windows IOCP, io_uring and the thread-pool backend all did a bare `static_cast<uint64_t>`; only the macOS GCD backend already had a `new_pos < 0` guard. All three now reject it the way macOS always did: `ValueError("negative seek position")`, with the current position left **unchanged**. Deliberately not stdlib's `OSError(EINVAL)`: macOS has always raised `ValueError`, and cross-backend consistency matters more than matching the builtin byte for byte; the deviation from `io` is documented in the test. Regression tests are in `tests/test_io_parity.py` (five negative-offset combinations, "position unchanged after the error", "stream still usable afterwards", and "the internal rewind path is not caught by it").
- **Invalid mode strings were accepted.** `_normalize_mode` only checked that every character was in the allowed set, so `"rbb"`, `"rr"`, `"rw"`, `"rtb"` and `"r++"` all passed. `"rw"` was the dangerous one: the base-mode character was picked with `next(c for c in mode if c in "rwax")`, so a mode that reads as read-write silently opened **read-only**. Structural validation now matches the builtin `open()`: exactly one of `rwax`, at most one `+`/`b`/`t`, and `b`/`t` mutually exclusive — with the offending string included in the message.
  Also recording one **unfixed** deviation: calling `read()` on a write-only handle (`"wb"`) raises `io.UnsupportedOperation("read")` in the stdlib, while here the request reaches the OS and comes back as `OSError` (Windows `errno 5`). Both are errors, only the type differs; aligning them means adding an accessibility pre-check to eight read/write entry points in the Python layer, which is a separate change.
- **Linux: an event loop that ran any io_uring I/O could never be collected.** `UringInstance` took a `Py_INCREF` on the loop and that field was **never read** — it existed purely to keep the loop alive — while the matching `Py_DECREF` only ran in `cleanup_all()`, i.e. at process exit. So a single `aopen()` (the native `OPENAT` path) or any io_uring I/O pinned the current loop for the lifetime of the process. It is now a weak reference, which doubles as the staleness check for the cache key: `UringManager::acquire()` is keyed on a raw `PyObject*`, so when a dead loop's address is reused by a new loop the entry must be replaced — otherwise the new loop's completions are delivered to a defunct reaper/batcher (a second problem that only surfaced once the strong reference was gone). The stale path in `acquire()`, `remove()`, `cleanup_all()` and the instance-creation failure path all go through `release_loop_ref()`. `weakref_target()` moved into `globals.hpp` so `ResultBatcher` and the uring pool share one version split for the 3.13 API change. CI guards it with the new `test_loops_do_not_accumulate_across_loops`: ten distinct loops each doing open+aopen must leave at most two behind.
  Also recording one **unfixed** bounded retention: each of the three POSIX backends caches a global `loop.create_future` **bound method** (`g_cachedFutureFn`, saving one attribute lookup per op). A bound method inherently holds its instance, so "the most recently used loop" stays pinned until another loop performs I/O. That is a deliberate hot-path trade-off (not a per-operation leak); fixing it means resolving the method through a weak reference on every operation — a path that runs per op — and the cost cannot be measured on Windows for io_uring/macOS, so it is left as a separate change.
- **An event loop that performed a single I/O could never be collected.** `ResultBatcher` is cached per loop (in `IOCPContext::m_batchers` on Windows, in a global registry on the other backends) and its constructor cached `loop.call_soon_threadsafe` — a *bound method*, i.e. a strong reference — so any loop that ran one I/O stayed pinned until process exit. Code that wraps `asyncio.run()` around each request (common in a gunicorn sync stack) therefore leaked one loop object per request. The batcher now holds only a **weak** reference and resolves the loop at flush time (if the referent is gone, the batch is dropped and the dispatch flag reset — nothing can consume those results any more, and keeping them would pin the futures and the loop they reference). The cache key is still a raw `PyObject*`, so `get_batcher()` compares the weak reference's current target: when a new loop reuses a dead loop's address the comparison fails, the old batcher is **retired** (not destroyed — `Session::batcher` and `snapshot_batchers()` may still hold that raw pointer, so destroying it would be a use-after-free) and a fresh one is created. Without that comparison every result of the new loop would be silently delivered to the invalidated old batcher. Measured: after 200 iterations of `asyncio.run` + open + read, surviving loop objects went from +200 to 0 (the +200 measured immediately after the run is the quarantine's legitimate one-second hold; it reaches zero after 1.5 s of idling); random 4 KiB reads at x16–x256 remain 9.6–10.5x aiofiles, with no measurable regression (one extra attribute lookup per flush, and flushes are batched rather than per-op).
- **Windows: `fileno()` leaked a CRT descriptor per call, and `os.close()` on it took the file object down with it.** The old implementation ran `_open_osfhandle` on the session's own HANDLE: three consecutive calls returned 2003, 2004, 2005 (the first two never released), and that descriptor owned the *session's* handle — so after `os.close(f.fileno())`, `await f.read()` failed with `OSError: ReadFile failed` (errno 6). The handle is now duplicated and wrapped exactly once, matching `io.FileIO.fileno()` semantics (stable fd per object). Closing that fd needs two guards: (1) if the caller already closed it, every CRT validation call (`_get_osfhandle`/`_close`) goes through the invalid-parameter handler and `__fastfail`s the process with 0xC0000409, so the whole probe-and-close runs under a non-fatal thread-local handler; (2) the descriptor number may have been recycled by another descriptor, so a mere "is it valid" check closes someone else's fd (measured: asyncio's self-pipe socketpair) — the comparison is on **handle identity** instead. `tests/test_native_resources.py` covers the stable fd, readability after the caller's close, and the recycled-number case (run in a subprocess: a real regression is a process crash, not an exception).
- **Every `open()` / `aopen()` leaked one event-loop reference.** `WindowsIOBackend`'s constructor took the `+1` from `get_running_loop()` and never gave it back, with a comment calling it the Session's lifetime source — while `Session::loop` was documented as *borrowed* and `~Session` explicitly released nothing, so that reference simply evaporated. Ownership is now one-directional: `create_session()` does its own `Py_INCREF` and `~Session` releases it, while the constructors and the `open_async` callback each return the reference they took (`aopen` leaked an extra one; on POSIX the loop argument was ignored entirely and still held). Measured: loop refcount growth over 200 open/close cycles went from +200 to 0. This does **not** address a second, per-loop retention: `ResultBatcher` is cached per loop and caches `loop.call_soon_threadsafe`, so any loop that performed a single I/O stays referenced by `IOCPContext::m_batchers` until process exit — fixing that needs a weak reference plus staleness detection and is a separate change.
- **Windows: `read()`/`read_at()` did not see data appended by other handles.** The file size is snapshotted at open and thereafter updated only by this session's writes/truncates, so after an external append `seek(0)` + `read()` returned only the old length and `read_at` returned `b""` past it (silent short reads when tailing a file being written; `read_at` clamps at the same cached value). The kernel is now asked once at the **EOF decision point** or after a **short read** — no syscall added to the hot path, and the length change is visible on the next operation: with 7 bytes appended externally, `read_at(10, -1)` returns them immediately and `seek(0, 2)` reports the new size. `readinto`, short writes and failed writes (whose optimistic pre-update over-estimates) are corrected the same way. The macOS dispatch_io and POSIX thread-pool backends share this cache and are **not** updated here — that needs verification on the corresponding platforms.
- **Windows: the quarantine only drained under further traffic, so the last completed batch was never freed.** It previously drained only when this thread pushed 256 entries or when a worker received a completion batch, so once a burst finished and the process went idle, those `IORequest`s (with their `future`, and the event-loop reference inside it) stayed in the queue until the next traffic — memory parked at the previous peak. The worker's wait timeout now accounts for the front entry's expiry, so an idle worker wakes and drains. Measured: after 300 reads and 1.6 s of idling, `debug_quarantine_stats()['live']` drops from 300 to 0.

- **`flush()` blocked the event loop on Windows IOCP.** `submit_flush()` ran `FlushFileBuffers` on the calling thread — that is, on the event loop — costing 40–50 ms per call on a slow disk and stalling the whole loop for the duration (79.6 ms worst case, measured with an `asyncio.sleep(0)` heartbeat against a 0.9 ms idle baseline). The io_uring backend submits `IORING_OP_FSYNC`, so the same call was already truly async there; the backends disagreed. `FlushFileBuffers` now runs on the `GlobalThreadPool` and delivers its result through `ResultBatcher`, the same path `aopen` uses. Accounting follows `submit_write`: `Session::pending` +1 and `batcher->op_submitted()` at submit time, paired with `complete_request()` / `op_completed()` on completion, so `close()` still waits for this fsync before closing the handle (otherwise a recycled handle would let the fsync flush an unrelated file). The task captures a `shared_ptr<Session>` to keep the session alive and releases it while holding the GIL, so a `~Session` that happens to own the last reference runs its `Py_XDECREF` legally instead of on a bare worker thread. Ordering is unchanged: `FlushFileBuffers` covers writes already handed to the driver, so there is no extra wait for `pending` to drain. Measured on one disk: worst-case loop stall during flush **79.6 ms → 0.8 ms**, with per-call cost unchanged (23.9 → 25.0 ms — the fsync still happens, it just no longer owns the loop); `tests/` 153 passed / 1 skipped (the Linux io_uring case), plus concurrency and race checks (40 concurrent flushes on one handle, 30 files concurrently, a `flush` racing `close`) that leave `debug_batcher_outstanding()` at 0 afterwards. Windows IOCP only: `flush()` in `thread_io_backend` (`src/backends/thread_io_backend.cpp`) and `macos_gcd_backend` (`src/backends/macos_gcd_backend.cpp`) still calls `fsync` inline and still blocks its caller.

## [1.15.0] - 2026-10-20

### Changed
- **Path arguments now follow PEP 519 (`os.PathLike`), not just `pathlib.Path`.** `open()`, `aopen()`, `AsyncFile()`/`AsyncFile.open()` and every helper built on them (`read_bytes`, `read_text`, `read_bytes_many`, `write_text_many`, ...) used to accept `str | Path` and normalize with `str(path)`. That worked for `pathlib.Path` (whose `__str__` happens to be the path) but silently produced a `repr`-looking "path" for custom `os.PathLike` implementations, and accepted nonsense like `open(12345)` by stringifying it. All entry points are now annotated `str | os.PathLike[str]` and normalize through `os.fspath()` — custom path-like objects work, and non-path types fail fast with a clear `TypeError` from `os.fspath` instead of a confusing file-not-found later. Dropped a now-unused `pathlib.Path` import in `_types.py`.

### Removed
- **The Windows handle pool is gone entirely.** It was unworkable in principle: once a HANDLE is associated with an IOCP, the association cannot be revoked, so a recycled handle fails `CreateIoCompletionPort` (err=87) and the old error path cascaded into heap corruption (see the 1.3.0 fix below). In practice the pool was already dead code — nothing called `handle_pool_release`, so every acquire missed. `src/handle_pool.{cpp,hpp}`, the `poolKey` plumbing, and the `drain_handle_pool()` / `set/get_handle_pool_limits` APIs are deleted. Compatibility shims: `configure({"handle_pool_max_per_key"/"handle_pool_max_total": ...})` is accepted but silently ignored, and `get_capabilities()['backend_detail']` no longer reports `handle_pool` on Windows.

## [1.14.0] - 2026-10-19

### Changed
- **Text mode `write()` now returns characters, not bytes.** It used to return the transcoded byte count — the number the backend actually knows — which was a documented deviation from the builtin and from `aiofiles`. It now follows `io.TextIOWrapper`: text mode counts **characters consumed**, binary mode keeps returning bytes, and the text whole-file helpers agree (`write_text()`, `write_text_many()` count characters; `write_bytes*()` count bytes). The on-disk result is unchanged; only the number in the return value differs, and only in text mode. Migration: code that relied on the old byte count in text mode should use `len(text.encode(encoding))` or open the file in binary. The short-write path maps a partial byte write back onto whole characters (honouring both `newline` translation and the codec), so a short write now reports the characters that actually reached the OS. `tests/test_base.py::test_write_read_text` and `tests/test_async_open.py::TestAsyncOpen::test_write_text_many` assert the new contract (the former also checks the byte size on disk), and `tests/stress_single_file.py`'s append-write phase stops reporting every completed write as a partial write (`成功: 0 / 部分写入: 50,000` → `成功: 50,000 / 部分写入: 0`).

### Fixed
- **Windows IOCP: the deferred-free quarantine made sustained small-write throughput collapse.** The quarantine introduced in 1.10.0 (each completed `IORequest` is held `QUARANTINE_MS` = 1 s before being freed, so a redelivered `OVERLAPPED` can't hit a recycled address) was a `std::vector` drained by scanning the whole list and calling `erase(it)` per expired entry. Both halves were O(n): the scan walked the entire list on every call, and each `erase` memmoved the whole tail. At steady state the list is one second of traffic deep, so at 30k ops/s it holds ~30k entries and every completed operation inspected tens of thousands of them — measured `scanned/push` of 15k–75k, and the cost scaled with throughput, so the faster the loop ran the more it paid. Net effect: a 130-byte append loop ran at ~16k msg/s sustained while burning 2.3x the CPU per op (32.8 µs → 74.4 µs), against ~58k msg/s whenever the process had been idle for a second and the list had emptied. It was easy to misread as writeback pressure because idle was the only thing that cured it, but `GetProcessIoCounters` showed exactly 1.0x write amplification and the same work per round. The list is FIFO by construction (entries carry a non-decreasing `steady_clock` stamp), so expired entries are always a prefix: it is now a `std::deque` popped from the front in amortised O(1) per entry, with the same 1 s retention and the same CAS guard. Measured on the same 10k×129 B / 5-file / 50-worker loop: 16k → 84k msg/s sustained (`scanned/push` 15k–75k → 1.0, flat across a 500k-message continuous run), 4 KiB random writes 18.8k → 60.6k writes/s, and the loop now beats a liburing/IOCP-based peer library on both instead of losing ~2x.
- **`configure()` silently ignored the batching knobs once a loop had run I/O.** `ResultBatcher` caches `iocp_batch_size`, `adaptive_batch` and `adaptive_target_latency_us` at construction, and `AOpen`/submit paths look the batcher up once per loop, so `configure({...})` after the first async operation only affected event loops created afterwards — the most common moment to call it (during benchmark setup, or to switch adaptive batching off after seeing it misbehave) was exactly the moment it did nothing. `configure()` and `reset_config()` now push the current values into every live batcher through `refresh_batchers_config()` (threshold cap clamped, `current_threshold` / `current_idle_ms` re-clamped immediately so a lowered cap is visible at once). The setters are plain relaxed atomics and touch no Python object, so the documented "holding `m_batchersMtx` while calling `call_soon_threadsafe` deadlocks against the GIL" path is untouched: pointers are copied under the mutex and updated outside it.

### Added
- **`debug_quarantine_stats()`** (test/debug aid) exposes quarantine depth, pushes, entries inspected by drain, drain count and the depth high-water mark, so the amortised-O(1) invariant can be regression-tested rather than rediscovered.
- **`debug_batcher_state()`** (test/debug aid) reports the batching parameters a loop is actually using (`threshold`, `threshold_max`, `idle_ms`, `idle_max_ms`, `adaptive`, `target_latency_us`, `median_interval_us`, `outstanding`). This is the hook that proves `configure()` hot-updates work; before it, "did the knob take effect" was unanswerable from Python.

### CI / Tests
- **One test framework: `python -m pytest` now covers the whole suite.** The suite had grown three harnesses — `unittest.TestCase`/`IsolatedAsyncioTestCase` classes driven by `unittest discover`, hand-rolled `TestRunner` scripts that printed their own checkmarks (`test_base.py`, `test_read_at.py`, `test_wrap_file.py`, `test_capabilities.py`), and pytest — and CI invoked each of them in a separate step, so a plain `pytest tests` didn't even survive collection. `pyproject.toml` now carries `[tool.pytest.ini_options]`: `testpaths`, `python_classes = ["Test*", "*Tests", "*TestCase"]` (the historical `AcopyTests` / `WrapperTests` / `FileLifecycleTests` names never matched pytest's `Test*` default, which is why `unittest discover` was doing the work), `asyncio_mode = "auto"` with a session-scoped loop, plus a `dev` extra (pytest, pytest-asyncio, pyyaml, aiofiles, rich) that CI now installs as `pip install -e ".[dev]"` instead of a hand-maintained pip list. `tests/conftest.py` puts `tests/` on `sys.path` so `from i18n import i18n` resolves under pytest's import mode, and `tests/_console.py` holds the UTF-8 console switch that used to be a module-level `sys.stdout = io.TextIOWrapper(...)` in five files — that assignment detached pytest's capture object and killed the session with `ValueError: I/O operation on closed file`, which is exactly why `pytest tests` used to report "no tests ran". The switch now runs only under `__main__` and reconfigures in place, so script mode still prints its report while pytest captures normally. `tests/test_loguru.py` stopped being two `test_*(LOG_COUNT, CONCURRENT, NUM_FILES)` functions (pytest read the parameters as missing fixtures — the `EE` in the old matrix); it is now two zero-argument tests that assert on-disk byte counts, with the throughput comparison left to script mode, and its Linux `sqpoll` tuning no longer runs at import time where it would leak into unrelated tests. Full suite on Windows: **148 passed, 1 skipped** (the Linux-only io_uring case) in ~5 s, versus eleven separate invocations in the CI matrix. pytest also picked up 12 text-mode read/seek regressions (`test_text_read_*`, `test_*_seek_*`) that existed in `test_base.py` but were never wired into its script runner — they all pass, and without the unified harness nothing would have noticed them rotting. The new `tests/test_quarantine.py` pins both silent-degradation bugs from this release: it fails against the old scan-based drain (208M inspected entries for 20k completions) and asserts that `configure()` reaches an already-live batcher via `debug_batcher_state()`. It also documents a runner-dependent fact the CI taught us: when `SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)` engages, cache-hit small writes finish inline in `submit_write` and free the request right there, so no completion packet is posted and the quarantine never fills — on `windows-2022` only ~0.3% of small appends (59 of 20 000) reach the queue, versus 100% locally. That is safe by construction (a completion that was never posted can't be delivered twice), but it means the amortisation property is unobservable there, so those two cases skip with the ratio in the reason instead of silently passing. `tests/test_loguru.py`'s thread-pool baseline switched to binary append as well: `TextIOWrapper.write` has no lock of its own before 3.11, so 32 threads appending to one text handle lose data (the 3.10 CI legs landed 1,162,161 of 1,290,000 bytes).
- **CI now runs everything under UTF-8 (`PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`).** The console-wrapping hack in the test files existed because CI stdout is a **pipe**, not a console: Python then encodes with the locale code page — `cp1252` on the English `windows-2022` runner, `C`/ASCII on some Linux images — so the first Chinese `print()` dies with `UnicodeEncodeError`, and the old workaround (`sys.stdout = io.TextIOWrapper(...)`) is exactly what broke pytest's capture. PEP 540's UTF-8 mode fixes it one level up: the workflow sets the two variables job-wide, so the entire process speaks UTF-8, Chinese assertion messages can no longer blow up the runner *on failure* (the case that matters most), and no test needs to touch `sys.stdout`. `tests/_console.py` keeps the script-mode console switch for developers on a `cp936` console, using the object-preserving `sys.stdout.reconfigure()` instead of replacing the stream.

## [1.13.0] - 2026-10-18

### Added
- **Native Linux `aopen` via io_uring `OPENAT`.** When io_uring is available, `aopen` now submits `IORING_OP_OPENAT` directly to the shared ring for the current event loop and constructs an `AsyncFile` from the fd returned by the CQE. It no longer creates a separate local ring and synchronously waits with `io_uring_wait_cqe` for every open, and no longer depends on the userspace C++ thread pool for the native path. Existing SQ overflow submission, reaper, inline harvest, and ResultBatcher infrastructure is reused. Ring/request initialization failures retain the original C++ thread-pool fallback, while real open errors such as `ENOENT`/`EACCES` are delivered directly. Windows and macOS behavior is unchanged.
- **Decoupled native-open architecture.** `PyAsyncFile` moved to `file_handle.hpp`; native submission and completion live in `io_uring_backend`, with `file_handle` providing platform dispatch and `bindings.cpp` limited to generic registration and calls. Completion paths explicitly manage path/mode/Future lifetimes, fd handoff, and exception ownership, avoiding silent fallback, duplicate batcher accounting, and double closes.

### CI / Tests
- `tests/test_async_open.py` now runs through pytest on all three platforms and contributes to the CI exit code. The new `test_linux_native_aopen_submission` uses `debug_native_aopen_count` to prove that `aopen` actually submits through io_uring instead of silently falling back. All 20 tests pass in Linux CI; the platform-specific case is skipped as expected on Windows/macOS.

## [1.12.1] - 2026-10-17

### Changed
- **Windows IOCP `submit_close()` now wakes on completion signals instead of Sleep polling.** The zero-pending fast path, `CancelIoEx`, GIL release, close timeout, and Session lifetime semantics are unchanged. A per-session counting semaphore now wakes close after synchronous, asynchronous, inline, and failed request completions. Every wake rechecks pending, so early or redundant signals cannot close the handle early, while exponential-backoff polling latency is removed.

### CI / Tests
- `tests/test_base.py` adds a regression covering close with 128 in-flight `read_at` operations, verifying cancellation and draining, successful read contents, task completion, and the closed state.

## [1.12.0] - 2026-10-10

### Added
- **Runtime capability discovery: `ayafileio.get_capabilities()`.** Returns everything in `get_backend_info()` plus two dicts: `features` (library-level capabilities with uniform keys across platforms: `positional_io` / `batch_positional_io` / `zero_copy_readinto` / `chunked_streaming` / `async_open` / `adaptive_batching` / `fast_file_copy`) and `backend_detail` (natively probed capabilities of the active backend on this machine). The probing principle is **ask the kernel, don't parse version strings**: io_uring opcode support is probed per-op via `IORING_REGISTER_PROBE` (21 ops: read/write/statx/renameat/mkdirat/...), SQPOLL is verified by actually creating a trial ring (it needs privileges before 5.11 — version numbers can't answer that), feature bits come from `ring.features` — backported kernels report what they truly support. Windows reports `completion_model` / `batch_harvest` / `handle_pool`; macOS honestly reports `completion_model: false` (GCD file channels run blocking pread/pwrite on kernel-managed workqueue threads, not a submission/completion model) alongside its `mincore_fastpath`. Probing lives in new `src/discover.{h,cpp}`, results cached process-wide.
- **`get_backend_info()` gains an `os_version` key.** `RtlGetVersion` on Windows (immune to manifest compatibility lies), `uname(2)` elsewhere. Additive and backward compatible.
- **Capability-validated feature switches.** `configure({"io_uring_sqpoll": True})` is now validated against the probed kernel capability: raises `ValueError` with guidance when SQPOLL is unsupported, when the io_uring backend itself is unavailable, or on non-Linux platforms — instead of silently accepting a configuration that would only fail at ring-creation time. Setting `False` is always allowed, so cross-platform code can write it unconditionally. The SQPOLL trial-ring probe is now a cached `io_uring_sqpoll_supported()` shared by the capability matrix and the `configure()` validation — one probe, two consumers.
- **TypedDict annotations for the returned dicts.** New `BackendInfo` (`platform`/`backend` as Literals), `Capabilities` (extends BackendInfo with `LibraryFeatures` and a per-backend detail union), and top-level export of `AyafileioConfig` — `py.typed` users get full IDE autocomplete on the capability matrix.

### Changed
- **`warn_fake_async()` now reads the extension's authoritative probe.** The old implementation ran at import time, before the native extension loaded, so it guessed with ctypes liburing loading, kernel version string parsing, and header-path checks — and could disagree with the real backend (seccomp-blocked io_uring in containers: ctypes says yes, ring creation says no). The call site moved below the extension import and now reads `get_backend_info()`'s `is_truly_async` (on Linux, a conclusion from actually building a ring). The warning message carries platform, kernel version, and a pointer to `get_capabilities()`. ~100 lines of guesswork deleted.

### CI / Tests
- `tests/test_capabilities.py` (26 checks with per-platform branches and cache-consistency) wired into the test steps on all three platforms; the "Check backend info" step now dumps the full capability matrix — CI logs double as a kernel capability probe.
- Fixed two issues CI caught: io_uring opcode probing switched to uapi ABI numbers (`IORING_OP_*` are enum members, not macros — every `#ifdef` silently evaluated false and the matrix came back empty; numbers verified against the kernel's `include/uapi/linux/io_uring.h`, and `io_uring_opcode_supported` bounds-checks `op > last_op` at runtime, so builds no longer depend on liburing header vintage); `_winapi.CopyFile2` exists only on Python 3.12+, so `fast_file_copy` on 3.10/3.11 now honestly reports `False`, matching `acopy`'s actual behavior.

## [1.11.3] - 2026-10-08

### Fixed
- **Source builds failed on Debian 12 / GCC 12: `error: 'atomic' in namespace 'ayafileio::std' does not name a template type`.** The POSIX branch of `src/globals.hpp` included `<cstring>` *inside* `namespace ayafileio`. Pulling a standard header into a namespace injects that header's declarations there — on GCC 12's libstdc++ this materializes an `ayafileio::std` namespace, after which every later `std::` reference resolves against it and the compile dies. Newer toolchains (GCC 13+, clang, MSVC) happen to tolerate the pattern, which is why CI and the prebuilt wheels never noticed; anyone building from source on Debian 12's stock toolchain hit it immediately. `<cstring>` now sits in the global include section, above the namespace. Prebuilt-wheel users were never affected. Reported *and* fixed by [@N0zoM1z0](https://github.com/N0zoM1z0) — issue ([#5](https://github.com/Patchouli-CN/ayafileio/issues/5)) with environment, reproduction, and root cause, then the patch itself ([#6](https://github.com/Patchouli-CN/ayafileio/pull/6)), validated on Debian 12 / GCC 12.2.0 with Release builds and all 139 checks passing under io_uring both enabled and disabled. First external code contribution to the project — thank you!

## [1.11.2] - 2026-10-06

### Changed
- **`ayafileio.types` renamed to `ayafileio._types`.** The module shadowed Python's standard-library `types` module — harmless inside the package (absolute imports), but a trap whenever the source directory ends up on `sys.path`, and a lint red flag. The public import path is unchanged: `from ayafileio import AyaFileIO` works exactly as before. Code that imported the internal module directly (`from ayafileio.types import ...`) should switch to the public path. (Community feedback.)

### Fixed
- **Python 3.10 no longer needs `typing_extensions` at runtime.** `types.py` imported `Self` and `_config.py` imported `TypedDict`/`NotRequired` from `typing_extensions` on 3.10, but the package never declared that dependency — a fresh 3.10 install would fail at import time. `Self` now lives under `TYPE_CHECKING` behind lazy annotations (never evaluated at runtime), `TypedDict` comes from the stdlib (3.8+), and the `NotRequired` wrappers were dropped (the TypedDict is already `total=False`). (Community feedback.)
- **macOS: a wrap/close cycle could later kill an unrelated file's descriptor — the first write on a freshly `open()`ed file failed with `OSError: [Errno 9]` (EBADF).** 1.11.1's leak fix closed the channel's `dup()`ed descriptor in `close_impl` right after a `dispatch_barrier_sync` on the channel queue, probing with `fcntl(F_GETFD)` first. The barrier, however, only orders against blocks *already enqueued* on that queue, while the channel's cleanup handler reaches it through a multi-hop asynchronous chain (`dispatch_io_close` → channel queue → barrier queue → `close_queue` resume → trampoline → our queue): `close_impl` could probe and close the dup while libdispatch teardown was still in flight. Closing early loses in both timing directions: while the descriptor still carries libdispatch's `GUARD_CLOSE` guard the plain `close()` fails with `EPERM` and silently re-leaks the fd the fix set out to reclaim; if the close succeeds, the number returns to the kernel while teardown still references it, and teardown's deferred touches land on whatever file reused the number in the meantime. The probe cannot arbitrate any of this — `fcntl` answers "is the number valid", not "do we still own it" (textbook ABA) — so whether CI went green was pure teardown timing, which is why two Python-only commits flipped the same wrap-then-open sequence red. The dup is now closed exactly once, by the channel's cleanup handler: the point where the `dispatch_io_create(3)` contract explicitly relinquishes descriptor control ("safe for the application to `close(2)`"), scheduled after libdispatch's own fd-entry teardown (unguard, global fd-table removal) has completed, so no probe is needed and no guard can trip it. `close_impl` releases the channel and then waits on a semaphore signaled by that handler, keeping `close()`'s guarantee that the number is truly recycled when it returns; on `close_timeout_ms` expiry the handler still closes the descriptor late (its captured `shared_ptr` keeps the state alive past backend destruction) — deferred, but still exactly-once: no double-close, no leak. Regression coverage: `tests/test_wrap_file.py` gains a 100-iteration wrap → close → `os.close` → immediate-reopen-and-write loop that recycles the just-freed descriptor numbers on every cycle.

### Documentation
- README wording pass (both languages): tightened colloquial phrasing in the technical prose; the Aya signature bits stay. Added the English repository description.

## [1.11.1] - 2026-10-03

### Fixed
- **macOS: one file descriptor leaked on every open/close.** The Dispatch I/O channel is created from a `dup()`ed descriptor, but the number was kept in a constructor local and `close_impl` only called `dispatch_io_close()` before nulling the pointer — the channel's +1 reference was never `dispatch_release`d, and nobody ever closed the dup. Per the `dispatch_io` contract the system merely *relinquishes* control over the descriptor and leaves closing it to the application, so every cycle leaked one fd for the life of the process. The new CI batch-open scenario flushed this out as `EMFILE` after ~2,600 open/close cycles; for users it means long-running processes slowly marching toward "too many open files". The backend now stores the dup, releases the channel after the cleanup barrier has run, and closes the descriptor itself — probing first so a libdispatch that already closed it is not double-closed. Measured side effect on macos-15 CI: the batch scenarios that died with `EMFILE` mid-batch now run at the full 1024-file budget, and the file-touching cells run better — 4 KiB small reads measured 499K ops/s (27.2x aiofiles) and random 4 KiB at x1 432K (54.2x) post-fix, against 187K–416K for the same cell across pre-fix runs; part of that spread is the shared runner's run-to-run variance, the leak's own tax is the part that reproduces.
- **`aopen` error path: use-after-free of the returned future under concurrent failures.** `open_async` created the future with a single owned reference and then handed that *same* reference to two owners: the thread-pool worker `Py_DECREF`ed it on completion, and the binding returned it to Python via `py::steal`. In the common case a temporary reference held by the batcher's bound `set_result`/`set_exception` method covered the window, but when many opens fail at once — the batcher flushes immediately on the last-in-flight trigger — the future can be freed while the awaiting coroutine still holds it. The macOS batch-open CI scenario hit this as `OSError: EMFILE` followed by `RuntimeError: Future object is not initialized.` and a SIGSEGV (the awaiting coroutine reading a freed Future object). The worker now takes its own reference, restoring the invariant that every `Py_DECREF` in the worker balances a reference the worker owns. (Same review of the sibling captures — `set_result`, `create_future` — found them correctly owned.)

### CI / Testing
- `tests/t_compare.py` gains two batch-open scenarios: **F. Batch open** (1024 files × 64 B per batch, all opened concurrently — `read_bytes_many` vs `gather(read_bytes)` vs aiofiles' default `gather(open+read+close)`) and **G. Batch open, opens only** (`aopen` vs aiofiles open+close, two executor round-trips per file). The Windows handle pool is drained once per round so every round starts cold; later batches within a round reuse cached handles, which is each library's out-of-the-box behavior. On POSIX the soft `RLIMIT_NOFILE` is raised to the hard limit first (macOS defaults to 256 entries — fatal for a batch this size; the first CI run died with `EMFILE` on file 571 of 1024, because macOS costs *two* fds per open — the backend dups a second for its dispatch channel — and reaps closed fds asynchronously, so real usage overshoots the semaphore-based concurrency cap by an order of magnitude) — and every fd-count model loses to macOS anyway, because XNU stores whatever `RLIMIT_NOFILE` you set and clips at `MIN(RLIMIT_NOFILE, kern.maxfilesperproc)` at open time (GitHub's macOS runners sit on launchd's un-raisable default of 256), so `getrlimit` over-reports. The batch is now sized by doubling the *concurrent* batch until the library itself refuses — a sequential probe cannot see this budget, since the semaphore plus reclamation lag make peak usage a multiple of the in-flight count; the effective size lands in the scenario metadata.
- First measurements (median files/s vs aiofiles): `read_bytes_many` wins everywhere — **2.1x** on the author's Windows/NVMe box, **3.0x** on windows-2022 CI, **2.4x** on ubuntu-24.04 CI, **2.9x** on macos-15 CI. The `gather(read_bytes)` attribution row is the subtle one: where opens are expensive it collapses to ≈1.0x (local NVMe) — async reads alone do not beat the thread pool when opens serialize on the loop thread, so *there* the lead belongs to `aopen`; where opens are cheap (CI VMs, cached storage) it still wins (**2.7x** windows-2022, **3.1x** ubuntu-24.04, **3.6x** macos-15). The batch win is robust; aopen's *marginal* value tracks the per-open cost. Opens-only (G): aopen **2.5x** local, **3.3x** windows-2022, **2.1x** ubuntu-24.04, **4.3x** macos-15.

## [1.11.0] - 2026-10-01

### Added
- **Async open: `ayafileio.aopen()`.** Same arguments, mode semantics, and returned `AsyncFile` as `ayafileio.open()`, but the OS open itself runs on the C++ global thread pool: the worker constructs the backend holding the GIL while the blocking syscalls inside the constructors (`CreateFileW` / `open` / `openat`-wait / `mmap`) release it via the new `GilRelease` guard, so many workers genuinely open files in parallel. The result (a ready-to-use `AsyncFile` or an exception object) is delivered through the `ResultBatcher` — the same batched-wakeup lane as I/O completions. The returned handle is lazy (aiofiles style), so both `async with ayafileio.aopen(...) as f` and `await ayafileio.aopen(...)` work. On Windows the `loop`/`create_future` pair the constructor needs is pre-fetched on the caller's loop thread and injected (there is no running loop on a worker thread); POSIX backends bind the loop lazily at first I/O and ignore the injection. The synchronous `ayafileio.open()` path benefits too: its open syscall no longer freezes the whole interpreter for its duration.
- **Batch whole-file operations: `read_bytes_many()` / `read_text_many()` / `write_bytes_many()` / `write_text_many()`.** One call reads or writes many files concurrently — opens go through `aopen` (parallel OS opens), I/O through the platform async backend, and a semaphore (`max_concurrency`, default 64) caps in-flight files so a hundred-thousand-item input can't exhaust fds. The write variants take `(path, data)` pairs and return per-file byte counts. Results come back in input order; any failure raises the whole call (plain `asyncio.gather` semantics — batches are not atomic, a failed write batch may leave some files written). This turns the correct pattern (`gather` over files) into the shortest one to write.
- **Type-checking support: `py.typed` marker plus `@overload` pairs for `open()` / `aopen()`.** The package ships `ayafileio/py.typed` (in wheels and sdists), so mypy/pyright actually load the annotations — without the marker type checkers skipped the installed package entirely and every overload was invisible. `aopen()` mirrors `open()`'s overload pair (text modes → `AsyncFile[str]`, binary → `AsyncFile[bytes]`).
- **Bilingual usage guide: `docs/USAGE.md` / `docs/USAGE_CN.md`.** Day-to-day reference: `open()` vs `aopen()`, positioned and batched I/O, whole-file and batch operations, `chunk()` / `readinto()`, `acopy()`, the configuration table, best practices, aiofiles migration notes, and type checking. Linked from both READMEs.

### Fixed
- **Windows: in-flight op accounting leak on the synchronous-completion fast path.** Every `op_submitted()` on the IOCP submit paths must be balanced by `op_completed()`, but when `ReadFile`/`WriteFile` completed synchronously with `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` enabled, the submitting thread resolved inline and never balanced the count — one leak per sync-completed op, never healed. Once `m_outstanding` stayed inflated, the "last in-flight op flushes immediately" optimization was permanently disabled for that loop, and every asynchronously-completed op degraded to waiting for the ~5 ms idle sweep. Whether sync completion happens depends on the storage stack: bare-metal Win10 almost never takes this path (invisible locally), while the CI windows-2022 VM does — measured effect there: random 4 KiB read at x1 concurrency was **144 ops/s** (≈7 ms/op) where aiofiles on the same runner gets 4,122. The four submit sites now balance the accounting via `complete_tracked_op()` in the skip-CP branch (which also flushes when the count reaches zero with results pending). Regression coverage: `tests/test_batcher_accounting.py` asserts the per-loop outstanding count returns to zero, via the new `_ayafileio.debug_batcher_outstanding()` probe (meaningful only on machines where the sync-completion path fires). Post-fix on the same runner, that cell measures **12,012 ops/s (1.63x aiofiles)**.

### CI / Testing
- Added `tests/test_async_open.py` (aopen round-trips in binary/text/write modes, mode validation parity with `open()`, error delivery through the future, concurrent opens, batch-read ordering, concurrency limiting, error propagation).

## [1.10.0] - 2026-09-29

### Performance
- **macOS: parallel chunk fill for large reads.** Reads ≥ 1 MiB are no longer a single sequential `pread` on one worker thread: they are split into 512 KiB chunks, all submitted to the global thread pool at once, and each worker `pread`s its chunk into the same pre-built buffer (positioned writes, zero coordination); the last chunk to finish settles the request — still one completion, unchanged batcher accounting. A single-threaded sequential read is page-cache-copy bound (turbofile measured 64 MiB on its own Apple Silicon: 2.7 ms parallel vs 8.9 ms single-thread); spreading the copy across workers approaches memory bandwidth. Any failed chunk fails the whole request (partial data is never deliverable, same semantics as the pipeline path); the chunk count is capped at 256 and the chunk size grows for huge files. The Linux io_uring (already ~20 GB/s, bandwidth-bound) and Windows IOCP (single-completion structure) paths are unchanged.
- **Measurement note (honest accounting):** on the macOS-15 CI runner (shared hardware) the before/after comparison shows **no stable win** — the 1 MiB / 4 MiB sequential-read cells sit inside a ±30% runner-noise band (the below-threshold 64 KiB cell swung 0.71x on unchanged code, which calibrates the noise). Mechanism correctness is byte-exact verified by `tests/test_parallel_read.py`; the expected payoff is on memory-bandwidth-contended machines. Until a reproducible delta is measured on owned hardware, this is not claimed in the README performance table.

### Added
- **Whole-file API: `ayafileio.read_bytes()` / `write_bytes()` / `read_text()` / `write_text()`.** One await does open + read/write + close, targeting the `asyncio.gather`-over-many-files pattern (per-layer model weight loading, bulk config reads, …). Semantics are identical to hand-written `async with ayafileio.open(...)`; it just removes the boilerplate.

### CI / Testing
- Added `tests/test_parallel_read.py` (byte-exact verification of the parallel fill, short tail chunks, interleaved `read_at`, large `readinto`, whole-file API round-trips, gathered bulk reads) and wired it into the three-platform test matrices.

## [1.9.0] - 2026-09-29

### Performance
- **macOS: mincore inline fast path for small reads.** dispatch_io charges every operation a queue delivery (handler hop + wakeup); cache-hit small reads (< 256 KiB, same bound as `LARGE_IO_THRESHOLD`) now complete inline: a read-only mapping is mmap'ed at open, `mincore` checks that the whole requested range is page-cache resident, and on a full hit the calling thread (holding the GIL) `pread`s straight into the pre-built PyBytes and resolves via `complete_inline` — `await` no longer yields to the event loop. This is the macOS counterpart of the Windows `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` path (1.7.0) and the io_uring `COOP_TASKRUN` inline harvest (1.8.0), completing the hot-path story on all three platforms. Cold pages, missing mappings, and ranges beyond the open-time mapping (regions the file grew into afterwards) fall back to the dispatch_io async path with unchanged semantics; the mapping is only ever fed to `mincore`, never dereferenced (no SIGBUS risk); the residency-check-to-pread race degrades to one synchronous read at worst (EINTR transparently retried).
- **Measurements** (macOS-15 CI runner, Python 3.14, `tests/t_compare.py` methodology, 256 MiB cache-hot file; "old" = same-methodology baseline on `cb8a99f`):

  | scenario | old | new | vs aiofiles |
  | --- | ---: | ---: | ---: |
  | 4 KiB sequential read | 6,751 ops/s (0.67x) | 416,318 ops/s (**61.7x**) | **24.3x** |
  | 4 KiB random read x1 | 4,575 ops/s | 289,251 ops/s (**63.2x**) | **37.7x** |
  | 4 KiB random read x16 | 32,706 ops/s | 304,136 ops/s (9.3x) | 20.9x |
  | 4 KiB random read x64 | 37,765 ops/s | 311,607 ops/s (8.3x) | 15.4x |
  | 64 KiB sequential read | 384 MB/s (0.63x) | 7,474 MB/s (**19.5x**) | **7.3x** |

  The write path does not take the fast lane — 64 KiB sequential write is flat (1.02x, no regression); reads/writes at ≥ 256 KiB keep the existing dual-mode dispatch and thread fast path within their previous ranges (the 4 MiB sequential-read 0.86x and the large-write swings are macOS-runner noise).

### Changed (internal)
- **`complete_inline` moved up to `IOBackendBase`.** The former `IOUringBackend::complete_inline` becomes the shared inline-completion implementation for all backends; the io_uring COOP_TASKRUN harvest and the macOS mincore fast path both use it. Value construction, cleanup, and batcher accounting are unchanged.

## [1.8.0] - 2026-09-27

### Performance
- **io_uring: inline harvest of submit-time completions (page-cache-hit fast path).** With `IORING_SETUP_COOP_TASKRUN` (probed in 1.7.0), a cache-hit read completes inside the submitter's own `io_uring_enter`, yet every completion still round-tripped through the reaper thread. After each submission the submitting thread now non-blockingly peeks the CQ — `try_lock` on a new consumer-side mutex shared with the reaper, up to 32 leading data CQEs per peek, wakeup CQEs always left to the reaper — and resolves them inline with a direct `set_result`/`set_exception`, the io_uring counterpart of the Windows `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` path. The future is already resolved when the I/O call returns, so `await` no longer yields to the event loop for cache-hit I/O; batcher accounting stays balanced. Kernels without `COOP_TASKRUN` simply fall back to the reaper path.

### Added
- **`ayafileio.acopy(src, dst)` — async whole-file copy.** OS-level fast paths run first in a worker thread: in-kernel zero-copy `copy_file_range` on Linux, `CopyFile2` on Windows (which additionally preserves timestamps and metadata). Other platforms — and systems where the fast call is rejected (cross-device copies, NFS, …) — transparently fall back to a `read_at`/`write_at` pipeline that keeps up to `concurrency` chunks of `chunk_size` in flight (memory bound: `concurrency × chunk_size`, defaults 8 × 4 MiB; the destination is pre-sized to avoid repeated EOF extension). Semantics follow `shutil.copyfile`: the destination is truncated first, copying a file onto itself raises `shutil.SameFileError`, and `copy_stat=True` additionally applies `shutil.copystat`. Measured locally on a cache-hot 512 MiB file: 3,300 MiB/s, on par with `robocopy` (3,413 MiB/s).

### Build / CI
- **Python 3.15 support.** The test matrix now covers 3.15 and 3.15t (resolving to 3.15.0 release candidates via `allow-prereleases` until the October 1 final), and the wheel pipeline builds `cp315`/`cp315t` wheels — cibuildwheel 3.2.1 → 4.2.1, with `CIBW_ENABLE` switched to `cpython-prerelease` (4.x dropped the `cpython-freethreading` group; free-threaded wheels are built by default now). Shipped as a post-release wheel top-up: the `cp315`/`cp315t` wheels were uploaded to PyPI on 2026-09-27 via manual dispatch, covering Windows / Linux (x86-64, aarch64) / macOS.
- **riscv64 split out of the wheel pipeline.** The QEMU-emulated riscv64 build (slower than everything else combined) is now its own job, and manual dispatches accept a `linux_targets` input (`all`/`fast`/`riscv64`) — topping up wheels no longer means sitting through the slow path. The 1.8.0 top-up ran through the `fast` lane; the `cp315`/`cp315t` riscv64 wheels were dispatched separately on 2026-09-29 with `linux_targets=riscv64`.

## [1.7.0] - 2026-09-26

### Changed
- **Semantics note: write buffers are now pinned until the write future resolves.** All backends serve writes zero-copy straight from the caller's buffer (see below), so the buffer object must not be resized while a write is in flight (the held buffer view already raises `BufferError` on resize attempts) and its contents should not be mutated in place — the same contract as `readinto()` and raw `os.FileIO.write`.

### Performance
- **All backends now actually use the completion batcher.** `complete_ok()`/`complete_error()` on the io_uring, macOS Dispatch I/O and thread-pool backends flushed after *every* completion, silently bypassing the adaptive batching introduced earlier. They now flush only on the count threshold or when the event loop's last in-flight operation completes — the same policy the IOCP path gained in 1.6.1.
- **Zero-copy reads on all backends.** Reads I/O directly into a pre-built `PyBytes` that becomes the result object, eliminating a full data copy per read (short reads fall back to a shrinking copy).
- **Zero-copy writes on all backends.** The write paths no longer memcpy the caller's buffer into a pool buffer; the kernel reads user memory directly via a held buffer view. Large sequential writes on Windows: 1 MiB 1,363 → 2,858 MB/s, 4 MiB 929 → 3,046 MB/s (local, vs aiofiles 0.73x → 1.47x).
- **io_uring: backpressure instead of spurious EBUSY.** When the submission queue is full, requests are queued to an overflow deque and re-submitted by the reaper after each CQE batch instead of failing the operation. SQ access is now serialized with a submit mutex (concurrent submissions from Python threads and the reaper were technically racy).
- **io_uring: batched GIL acquisition in the reaper** — CQE entries are harvested without the GIL and the whole batch is then completed under a single GIL hold, matching the IOCP worker's rhythm.
- **io_uring: probe-enable `IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG`** (kernel 5.19+) with silent fallback on older kernels, avoiding a softirq dispatch per completion. `IORING_SETUP_SINGLE_ISSUER` is deliberately not used (submissions legitimately come from multiple threads).
- **macOS: dual-mode dispatch for large I/O.** `dispatch_io`'s high-water setting chunked large requests into 64 KiB pieces with a callback and a memcpy each, collapsing large sequential throughput to as low as 4% of aiofiles. Requests ≥ 256 KiB now bypass `dispatch_io` and run a single `pread`/`pwrite` on the global thread pool straight into the final buffer; small requests keep the low-latency `dispatch_io` path. Measured on CI: sequential write 1 MiB 139 → 5,502 MB/s (40×), 4 MiB 208 → 6,787 MB/s; sequential read 1 MiB 0.28x → 0.97x vs aiofiles.
- **macOS & thread-pool backends: cache the file size at open** (refreshed optimistically on write/truncate), eliminating a per-operation `fstat` syscall; `close()` now waits on the `m_close_wake` semaphore instead of exponential-backoff sleep polling, cutting close latency from ~16 ms average to immediate.
- **Windows: `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`.** Synchronously completed operations no longer post an IOCP completion packet — one less worker wakeup and context switch per op on cache-hot small I/O. Sessions where the mode cannot be set keep the legacy worker-drain path (the two paths are strictly mutually exclusive).
- **IORequest freelist.** A thread-local freelist (64 slots) with placement-new reconstruction replaces per-op `new`/`delete`, removing allocator churn on small high-frequency I/O; fully compatible with the Yuyuko memory tracker.

### Changed (internal)
- The entire C++ core now lives in `namespace ayafileio`; `bindings.cpp` pulls it in with a using-directive. No public API change.

### CI / Testing
- **Version guard on the release pipeline**: a `workflow_run` trigger whose `pyproject.toml` version already exists on PyPI skips the wheel build & publish jobs (plain test pushes: ~1 h → ~6 s). Tags and manual dispatches always build; bump the version to release.
- **Rewritten cross-platform benchmark** (`tests/t_compare.py`): time-boxed rounds with interleaved backends and median reporting, race-free concurrency scenarios (one handle per worker, positioned I/O), and JSON artifacts (`benchmark_results.json` / `benchmark_results_detailed.json`) uploaded by CI for every run on all three platforms. The previous benchmark measured a seek+read race of its own making and never wrote the artifact files.

## [1.6.1] - 2026-09-26

### Fixed
- **Windows (IOCP): serial and low-concurrency workloads no longer pay the completion-batcher idle timeout.** `ResultBatcher` coalesces completion notifications into few `loop.call_soon_threadsafe` callbacks and only flushed on a count threshold (64) or an idle timeout (~5 ms). A lone in-flight operation — the norm for serial `await` loops and low-concurrency pipelines — never reached the threshold, so every operation waited out the full idle timeout before its future resolved. The batcher now tracks in-flight IOCP requests per event loop (`op_submitted()` / `op_completed()`, carried on the previously unused `IORequest::batcher` field) and flushes immediately when the loop's last in-flight operation completes: per-op latency drops from ~5 ms to event-loop wakeup level, while high-concurrency batching behavior is unchanged. The counter is fail-soft — if it ever drifts, behavior merely degrades to the previous idle-timeout path. Measured on a 256 MB file with 4 KB random `read_at` (20k ops): 1,313 → 21,435 ops/s at 16 in-flight (16×); sequential whole-file `read_at` with 64 KB chunks: 11 → 314 MB/s (28×); large-chunk (4 MB) sequential reads now surpass aiofiles (2.17×); high-concurrency throughput unchanged. The Linux io_uring, macOS Dispatch I/O and thread-pool fallback backends are untouched.

## [1.6.0] - 2026-09-22

### Changed
- **Text-mode `read(n)` / `readline()` now follow CPython `open()` semantics character-for-character** (behavior change, hence minor bump). 1.5.x treated `size` as a byte budget; 1.6.0 ports the full three-layer I/O stack idea of `io.TextIOWrapper`:
  - **Chunked underlying reads**: `read(n)` no longer issues one syscall per call — it pulls 64 KB blocks, so chunked loops get dramatically fewer IOCP/io_uring submissions.
  - **Incremental decoding**: a `codecs` incremental decoder stitches multi-byte sequences across chunks; incomplete trailing bytes are split out by a strict probe *before* entering the decoder, so the decoder never holds internal state and `errors` modes (strict/replace/ignore) all behave correctly.
  - **Newline translation on segment feed** (as in CPython: after decoding, before return); a trailing lone `\r` is held one beat so a `\r\n` straddling chunks is never split into two line endings.
  - `read(n)` counts **post-translation characters** — `read(3)` on UTF-8 Chinese returns exactly 3 characters.
  - **`tell()` stays a plain byte offset** (better than CPython's opaque cookie): each buffer segment books its source byte count, so unconsumed bytes are always known. Byte-exact for `\n` content; `\r\n`-dense content may drift 1 byte per `\r\n` (inherent to newline compression — CPython gives up exactness entirely here and returns a cookie). Binary mode was never affected.
  - Text-mode `seek()` policy (1.5.2) is unchanged: only `seek(0)`; positional access stays binary-only (`read_at`/`write_at`).

## [1.5.3] - 2026-09-22

### Fixed
- **Text-mode `read(n)` no longer splits multi-byte characters**: `size` was passed straight to the underlying layer as a raw byte count, so `read(7)` on UTF-8 Chinese text sliced mid-character and raised `UnicodeDecodeError` — or, under `errors="replace"`, silently inserted U+FFFD (the dangerous variant: data corrupted with no error). Text-mode `read(size)` now treats `size` as a **byte budget**: a strict probe detects an incomplete trailing character, pushes those bytes back (impl re-seek / read-ahead buffer rewind), and decodes only the character-complete prefix. A budget too small for one character (e.g. `read(1)` on a 3-byte Hanzi) is topped up (bounded) so at least one whole character is returned. ASCII behavior is unchanged (byte == char); `read(-1)` and `readline()` were already boundary-safe and are untouched.

## [1.5.2] - 2026-09-22

### Changed
- **Text-mode `seek()` is now restricted to `seek(0)`** (pure Python policy, zero backend changes): an arbitrary byte offset in text mode can land mid-character and corrupt decoding — `UnicodeDecodeError` under strict, silent U+FFFD under `errors="replace"`. Text mode now raises `ValueError` for any non-zero offset or non-SEEK_SET whence. Binary mode is unaffected (positional access remains the native scenario of `read_at`/`write_at`); open in binary mode when you need positional access. The `seek(0)` rewrite flow in `r+`/`w+` text modes is unchanged.

## [1.5.1] - 2026-09-16

### Added
- **Positioned write API — `AsyncFile.write_at(offset, data)`**: writes bytes at an explicit offset without changing the logical file position. Supported by the Windows IOCP, Linux io_uring/thread-pool, and macOS Dispatch I/O backends.
- **Batch positioned writes — `AsyncFile.write_many(writes)`**: submits multiple `(offset, data)` writes concurrently and returns byte counts in input order.

### Changed
- Batch reads now gather native futures directly, avoiding one Python task per request.
- Long-line scanning avoids rescanning the already inspected portion of the read-ahead buffer.

### Fixed
- Windows file objects no longer attempt to close a stale IOCP session during destruction after the event loop has shut down.

## [1.5.0] - 2026-09-02

### Added
- **Positional read API — `AsyncFile.read_at(offset, size=-1)`**: reads up to `size` bytes starting at an explicit file `offset` without touching the logical file position (`tell()` is unchanged before and after, and there is no race with concurrent `read()`/`seek()`). Random access no longer needs the two coroutine round-trips of `await seek(off)` + `await read(n)`. Semantics follow POSIX `pread`: `size < 0` reads to EOF, reads are truncated at EOF, an out-of-range `offset` returns `b""`, and `offset < 0` raises `ValueError`. Binary mode only (text mode raises `ValueError` — positional reads are incompatible with decoding/newline translation). Implemented end-to-end on all four backends:
  - **Windows (IOCP)**: new `IOCPContext::submit_read_at` issues `ReadFile` with `OVERLAPPED.Offset/OffsetHigh` taken from the argument and never touches `Session::filePos`.
  - **Thread-pool fallback (POSIX)**: worker performs a true positional `pread` (no `lseek`+`read`, which would race with other operations).
  - **Linux (io_uring)**: `io_uring_prep_read` with an explicit offset.
  - **macOS (Dispatch I/O)**: `dispatch_io_read` with an explicit offset on the `DISPATCH_IO_RANDOM` channel.
- **Batch positional read — `AsyncFile.read_many(spans)`**: submits a batch of `(offset, size)` reads in one event-loop turn via `asyncio.gather`, returning the results in span order. Aimed at streaming model-weight loading (per-layer batched reads): all I/Os are submitted before the first suspension point, and the C++ `ResultBatcher` coalesces their completion notifications into few `loop.call_soon_threadsafe` callbacks — N-1 fewer coroutine round-trips than awaiting `read_at` one by one.

## [1.4.8] - 2026-07-02

### Fixed
- **Intermittent whole-process deadlock on Windows (CI hangs)**: `IOCPContext::flush_batchers()` held `m_batchersMtx` while calling `ResultBatcher` flush, which internally invokes `loop.call_soon_threadsafe()`. asyncio implements that via a socket `send()` that temporarily releases and re-acquires the GIL. If another thread was holding the GIL while waiting for `m_batchersMtx` at that moment (the other IOCP worker's `flush_batchers()`, or the main thread opening a file), the two threads deadlocked waiting for each other — freezing the entire process, including watchdog threads. This bug exists in 1.4.7 and earlier (both IOCP workers can collide); it surfaced as occasional GitHub CI test-step timeouts on Windows. Confirmed by native stack dumps of hung processes and a reproduction loop (hang within 1–3 runs before the fix, 100 clean runs after). Fixed by never holding a C++ lock across a Python call that may release the GIL:
  - `flush_batchers()` snapshots batcher pointers under the lock and flushes outside it.
  - `get_or_create_batcher()` (non-IOCP registry) constructs the batcher outside `g_batchersMtx`.
  - `remove_session()` destroys the `Session` (which runs `Py_DECREF`) outside `m_sessionsMtx`.
- **Data loss when mixing `readline()` with `read()`/`readinto()`/`chunk()`**: `readline()` read-ahead buffers up to 64 KB from the underlying file, but `read()`/`readinto()`/`chunk()` went straight to the C++ layer, silently skipping everything still sitting in the read-ahead buffer. All read paths now drain the read-ahead buffer first.
- **`seek()` did not invalidate the readline read-ahead buffer**: after a `readline()`, `seek(0)` followed by `readline()` returned stale buffered data instead of the line at the new position. `seek()` now clears the buffer, and relative seeks (`whence=1`) are based on the logical position the user has actually consumed to — matching built-in `open()` semantics.
- **`tell()` returned the physical (read-ahead) position**: after one `readline()` of a short line, `tell()` reported 64 KB instead of the end of that line. It now subtracts unconsumed buffered bytes. `write()` and `truncate()` likewise rewind the read-ahead so they act at the logical position (relevant for `r+`/`+` modes).
- **`async with wrap_file(...)` crashed on exit**: `AsyncFile._from_impl` left the `_auto_flush` and `_mode` slots uninitialized, so exiting the context manager raised `AttributeError`, and `.mode`/`readable()`/`writable()` were equally broken on wrapped files. All slots are now initialized and `wrap_file` passes its mode through.

### Performance
- **`readline()` is 4–5x faster** (13.4 MB / 200k-line file, Windows: text universal-newline 1640 → 405 ms, binary 1516 → 293 ms):
  - Universal-newline line scanning (the default mode) used a per-byte Python loop; it now uses C-level `find()` with the `\r` scan bounded by the first `\n`, keeping each scan O(line length).
  - Line extraction copied the entire remaining buffer twice per line (O(n²) per chunk); it now advances a cursor offset and compacts once per 64 KB refill.
- **IOCP completion hot path trimmed** (measured +8% concurrent 4 KB write throughput; reads disk-bound and unchanged):
  - Each `Session` caches its `ResultBatcher*` at open — no more mutex + hash lookup per completion.
  - The adaptive-batching median is recomputed every 16 completions instead of on every completion (previously an O(128) `nth_element` under both the batcher lock and the GIL, per I/O).
  - `flush_batchers()` uses a new single-lock `ResultBatcher::flush_if_idle()` instead of three separate lock acquisitions per batcher per worker wakeup.
  - `future.set_exception` is no longer prefetched at submit time on any backend; the error path fetches it on demand, saving one attribute lookup per successful I/O.
- **BufferPool now allocates in power-of-two size classes** (min 4 KB): previously buffers were allocated at the exact requested byte count, fragmenting the pool into incompatible sizes that rarely got reused under variable-size workloads.

## [1.4.7] - 2026-07-01

### Fixed
- **`submit_*` returned a bare `NULL` without setting an exception (Windows/IOCP)**: When a session was already gone (e.g. an operation racing with `close()`), `submit_seek`, `submit_flush`, `submit_tell`, `submit_truncate`, and `submit_readinto` returned `nullptr` without an active Python exception. The nanobind binding's `throw py::python_error()` then surfaced this as a confusing `SystemError: <built-in> returned NULL without setting an error`. These paths now return an already-failed future carrying `ValueError("File not open.")`, consistent with `submit_read`/`submit_write`. Notably `seek()`/`tell()` have no Python-layer `_closed` guard, so they actually exercised this C++ path.
- **Missing format specifier in IOCP init warning**: the `printf` for a failed `init_iocp()` used `"Warning: Failed to init IOCP:"` with no `%s`, silently discarding `e.what()`. The failure reason is now printed.

## [1.4.6] - 2026-06-16

### Added
- **Full `newline` semantics for text mode**: `AsyncFile` now follows Python's built-in `open()` newline behavior:
  - `newline=None` (default): universal newline mode — translates `\r\n` and `\r` to `\n` on read, and writes `\n` as `os.linesep`.
  - `newline=""`: universal newline mode without translation — recognizes `\r\n`, `\r`, and `\n` as line terminators and returns them as-is.
  - `newline="\n"`, `"\r"`, `"\r\n"`: fixed terminator; write translates `\n` to the specified line ending.
  - Binary mode now rejects the `newline` argument, matching the standard library.

### Fixed
- **`AsyncFile.open` class method signature**: added the missing `auto_flush` parameter so it matches the module-level `open()` function.
- **Build configuration cleanup**:
  - Unified C++ standard to C++20 across `CMakeLists.txt` and `pyproject.toml`.
  - Removed redundant `[tool.setuptools]` section from `pyproject.toml` (scikit-build-core handles packaging).
  - Replaced deprecated `cmake.minimum-version` with `cmake.version` for scikit-build-core >= 0.8.

### Changed
- **README wording**: softened the "only true async" marketing claim to a more accurate description while keeping all Touhou/Gensokyo flavor intact.

## [1.4.5] - 2026-06-09

### Added
- **Adaptive `ResultBatcher` — self-tuning batch threshold**: The completion batcher now tracks inter-completion intervals via a 128-entry ring buffer and dynamically adjusts the effective batch threshold using the rolling median interval:

  ```
  dynamic_threshold = adaptive_target_latency_us / median_interval_us
  dynamic_idle_ms    = min(idle_max, median_us * 4 / 1000)
  ```

  - **Fast disk (NVMe, ~10μs intervals)**: threshold auto-rises to 100+, batching more completions per `call_soon_threadsafe` call — reduces GIL pressure and scheduling overhead
  - **Slow disk (HDD, ~10ms intervals)**: threshold drops to 1, flushing immediately — no wasted waiting for completions that haven't arrived
  - **Idle timeout also adapts**: scales proportionally to the observed I/O rate

  Enabled by default. New config options: `adaptive_batch` (bool, default `True`) and `adaptive_target_latency_us` (unsigned, default 1000μs, range 1-10000).

- **i18n for benchmark scripts**: `tests/i18n.py` module with automatic language detection (system locale + `AYAFILEIO_LANG` env var) and YAML-based translation files in `tests/lang/`. All benchmark scripts (`bench_compare.py`, `bench_tuned.py`, `stress_single_file.py`) now support Chinese and English output.

### Changed
- **Extended performance benchmarks in README**: Added two new real-world test scenarios with detailed data:
  - **Scenario 2: Single-file high-concurrency random read** — 100K concurrent tasks sharing one file handle. ayafileio vs aiofiles measured at 1K/10K/50K/100K concurrency levels. At 100K, ayafileio is **9.1x faster** than aiofiles on HDD (19,290 vs 2,130 ops/s). aiofiles throughput *degrades* with higher concurrency (7,706 → 2,130) while ayafileio *improves* (7,487 → 46,616 at 10K).
  - **Scenario 3: 500K concurrent stress test** — 500,000 asyncio tasks reading from a single file via IOCP. **Zero errors**, 23,116 ops/s, only 2 worker threads, ~583 MB RSS.
  - **Config tuning results**: 14 config combinations tested — default config is optimal (all within ±3% on HDD). The bottleneck is the physical disk, not the software.

### Improved
- **CI: expanded `paths-ignore`**: Added `tests/bench_*.py`, `tests/stress_*.py`, `tests/t_compare.py`, `tests/test_loguru.py`, `tests/lang/**`, `tests/i18n.py`, `ignore/*.py` — benchmark/config-only changes no longer trigger full CI builds.

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
