# Migrating to ayafileio 2.0.0

ayafileio 2.0.0 is an architecture-layering release. It does not introduce a public C++ or C ABI. File-I/O calls remain compatible, with the API metadata exception listed below.

## What stays compatible

The following remain part of the Python public contract unless a future major release explicitly changes them:

- `ayafileio.__all__` and documented imports;
- `open()` / `aopen()` and their lazy async-context-manager behavior;
- text and binary modes, encoding, errors, newline translation and character counts;
- `read()`, `readline()`, `readinto()`, `chunk()`, `read_at()`, `write_at()`, `read_many()` and `write_many()`;
- `seek()`, `tell()`, `truncate()`, `flush()`, `close()` and `fileno()`;
- `wrap_file()`, `acopy()` and whole-file/batch helpers;
- configuration keys, backend names, and the remaining backend-information and capability fields;
- native buffer pinning and short-write behavior;
- the existing debug entry points.

Exceptions retain their intended types. In particular, native open failures remain `FileNotFoundError` where appropriate; they are not converted to a generic `RuntimeError`. On Windows, wrapping an existing fd continues to use IOCP even when path opens are forced to another backend.

Deprecated `handle_pool_max_per_key` and `handle_pool_max_total` configuration keys remain accepted and ignored for compatibility. This is not a new promise to restore the removed handle pool.

## Removed in 2.0

- `is_truly_async` is removed from `get_backend_info()` and `get_capabilities()`. Use `info["backend"]` to identify the I/O mechanism: `iocp`, `io_uring`, `dispatch_io` or `thread_pool`. Code that checked `not info["is_truly_async"]` should check `info["backend"] == "thread_pool"` instead. Asynchronous Python APIs and kernel completion are different layers; a thread pool is not "fake async".

## What changed internally

- The Python package now lives under `python/ayafileio/`.
- `_async_file.py` was split into mode, text-stream and batch helpers.
- Native code now has `core`, `platform`, `backend`, `discovery`, `diagnostics` and `bindings/python` layers with matching namespaces.
- `Buffer`, `Completion`, `Operation`, `FileState` and `BackendRegistry` are native types. Python objects are confined to the Python adapter.
- `ResultBatcher` and its registry are unified across all backends.
- IOCP is split into context, session, submission, worker and request modules.
- Yuyuko is connected through lifecycle hooks instead of being included by the hot request path. Allocation records, asynchronous logging, CRC checks and HTML reports are separate internal headers.
- Tests moved to `tests/python/`; benchmark scripts moved to `benchmarks/`.
- CMake now builds separate core, backend, discovery and diagnostics object libraries, plus native `aya_core_tests` and `aya_diagnostics_tests` targets.

The private `_ayafileio` extension, C++ classes, file layout and namespaces are not a stable C++ ABI. No public C++ headers, C handles or other-language bindings are added by this release.

## Test and build changes

Use the same Python entry points as before:

```bash
pip install -e ".[dev]"
python -m pytest -q
AYAFILEIO_TEST_BACKEND=thread_pool python -m pytest -q
```

Native checks and the dependency guard:

```bash
ctest --test-dir build/<wheel-tag> --output-on-failure
python tools/check_cpp_dependencies.py
```

The benchmark command is now:

```bash
python benchmarks/t_compare.py
```

The concurrent logging correctness test is at `tests/python/test_loguru.py`.

## Semver from 2.0 onward

2.0 establishes the compatibility boundary explicitly:

- **Major**: a documented backward-incompatible change to the public contract.
- **Minor**: backward-compatible features and implementation improvements.
- **Patch**: backward-compatible fixes.

Internal restructuring, file moves and namespace changes alone do not require another major bump. Deliberate behavior changes must be listed under upgrade notes instead of being hidden inside a refactor.

## Validation status

The 2.0 worktree is covered by CI on Windows, Linux and macOS, including native backends, thread-pool fallbacks and free-threaded interpreters. Backend-specific behavior is exercised on the corresponding runners.
