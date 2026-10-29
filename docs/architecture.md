# ayafileio 2.0 architecture

ayafileio 2.0 separates the native I/O engine from Python semantics. The goal is to make dependency direction explicit instead of relying on file names or historical includes.

## Layers

```text
Python API and text/binary semantics
              ↓
CPython / asyncio bindings
              ↓
Native file handle and backend registry
              ↓
Core state, requests, buffers and completions
              ↓
Platform backends and OS primitives
```

- `python/ayafileio/`: public Python API, `AsyncFile`, text decoding, newline handling, open helpers and compatibility entry points.
- `src/bindings/python/`: the only code allowed to include `Python.h` or nanobind. It owns `PyAsyncFile`, `PyOperation`, Future creation, Python buffer ownership, `ResultBatcher` and the unified `BatcherRegistry`.
- `src/core/`: platform-neutral native types. `FileState`, `BackendRegistry`, `FileHandle`, `Buffer`, `Completion`, `IORequest`, configuration, buffer pool and global thread pool. No CPython types, concrete backend includes or OS handle types.
- `src/platform/`: thin native file and error primitives shared by backends.
- `src/backend/`: IOCP, io_uring, Dispatch I/O and thread-pool implementations. Backends submit native operations and produce `core::Completion`; they do not create or resolve Python Futures.
- `src/discover/`: runtime probing and native capability values. Backend selection belongs to `core::BackendRegistry`, not discovery.
- `src/diagnostics/`: debug logging and Yuyuko integration through `RequestLifecycleHooks`. `yuyuko/memory_record.hpp` owns allocation tracking; `async_log.hpp`, `crc.hpp` and `report.hpp` contain the independent logging, integrity-checking and HTML-reporting facilities. `memory_tracker.hpp` is the compatibility entry point for their tracking macros. Diagnostics depends on core, never the other way around.

Routine allocation/free traces are written only when asynchronous logging is explicitly enabled. Memory-safety diagnostics still use the logging sink, with synchronous `stderr` as the fallback.

## Namespace rules

The directory and namespace structures mirror each other:

```text
ayafileio::core
ayafileio::platform::{windows,posix}
ayafileio::backend::{windows,linux,macos,thread}
ayafileio::discovery
ayafileio::python
ayafileio::diagnostics::yuyuko
```

Do not add `using namespace` to shared headers. `NB_MODULE` and macro-only files are explicit exceptions in the dependency checker, not a general exemption.

## Native completion model

`core::Operation` is the native boundary for an asynchronous operation. It supports submission, asynchronous completion, inline completion and completion-only accounting. `PyOperation` implements this interface using a Python Future and a per-loop `ResultBatcher`.

`core::Buffer` owns native or type-erased storage without knowing its host. This preserves:

- preallocated `PyBytes` as zero-copy read results;
- held `Py_buffer` views for zero-copy writes and `readinto`;
- pooled and heap buffers used by native operations;
- platform-specific ownership transfer such as `dispatch_data`.

`core::Completion` carries a native result, an optional buffer, an `IoError`, and an optional type-erased exception object. Backends do not need to know what a Future is. The Python adapter converts the completion into bytes, counts, `None`, a file object or an exception while preserving its original type.

`CompletionScheduler` is the small interface used by IOCP workers for idle timeouts and flushes. `BatcherRegistry` implements it for all platforms; IOCP no longer has a second batcher registry.

## State and backend registration

`core::FileState` owns the logical position, cached size, append behavior, read planning, write reservation and seek/truncate updates. It is shared by all backends and removes the old platform-specific copies.

`core::BackendRegistry` stores backend factories and availability callbacks. `backend::register_builtin_backends()` is called explicitly by the Python composition root; there is no static self-registration. Windows fd wrapping intentionally remains IOCP-based, matching the established behavior.

## Build targets

CMake builds separate object libraries:

- `aya_core`
- `aya_platform`
- `aya_backend`
- `aya_discovery`
- `aya_diagnostics`

Only `_ayafileio` receives Python and nanobind include/link requirements. `aya_core_tests` and `aya_diagnostics_tests` run native CTest cases without Python; the latter exercises allocation tracking, CRC snapshots, HTML reports and asynchronous-log flushing.

## Dependency guard

Run the same check locally, in CTest and in CI:

```bash
python tools/check_cpp_dependencies.py
```

Policy and data live in `tools/architecture-rules.json`: the layer list, the
allowed-dependency matrix, the namespace exemptions, which layers may take
platform headers, and the symbol blacklist. Mechanism stays in the checker —
comment stripping, the fail-closed header classifier and cycle detection are not
switchable off from a data file.

The rules file is validated strictly. An unknown layer name, a dependency target
that is not a declared layer, a missing rule for a declared layer, or a wrong
value type is a hard error (exit code 2), never a silently dropped rule.

`immutable_propositions` in that file names the propositions the architecture
actually rests on: `core` stays platform-neutral and takes no platform headers,
nothing depends on `bindings/python`, `diagnostics` never reaches upward,
`discovery` does not know concrete backends, and no backend includes another
backend's internals. `tests/python/test_architecture.py` re-reads the JSON and
asserts every one of them, so a rule edit that loosens the architecture fails CI
even though the JSON still parses.

The checker resolves repository includes and enforces:

- CPython/nanobind only in `bindings/python`;
- platform headers only in the layers listed in the rules file;
- allowed layer dependencies and no backend-to-Python or backend-to-discovery shortcuts;
- no file or component dependency cycles;
- directory/namespace consistency;
- no unresolved project-relative includes.

`tests/python/test_architecture.py` contains valid and deliberately invalid synthetic trees, so a broken checker cannot silently pass. `architecture_dependencies` is also a CTest test and a dedicated GitHub Actions job.

## Validation

Use the platform's normal commands:

```bash
python -m pytest -q
AYAFILEIO_TEST_BACKEND=thread_pool python -m pytest -q
ctest --test-dir build/<wheel-tag> --output-on-failure
```

Cross-platform behavior must be verified on Windows, Linux and macOS. A change compiling on one OS does not prove the other backends work.
