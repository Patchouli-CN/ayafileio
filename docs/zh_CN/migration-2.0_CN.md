# ayafileio 2.0.0 迁移指南

[English](../migration-2.0.md)

ayafileio 2.0.0 是架构分层版本，不新增公共 C++ 或 C ABI。文件 I/O 调用保持兼容，但下方列出的 API 元数据有一项删除。

## 保持兼容的内容

除非未来的 major 版本明确调整，以下仍属于 Python 公共契约：

- `ayafileio.__all__` 与文档中的导入方式；
- `open()` / `aopen()` 及其惰性异步上下文管理器行为；
- 文本与二进制模式、编码、错误处理、换行转换和字符计数；
- `read()`、`readline()`、`readinto()`、`chunk()`、`read_at()`、`write_at()`、`read_many()`、`write_many()`；
- `seek()`、`tell()`、`truncate()`、`flush()`、`close()`、`fileno()`；
- `wrap_file()`、`acopy()` 及整文件、批量辅助函数；
- 配置键、后端名称，以及保留的后端信息与能力字段；
- 原生缓冲固定与短写行为；
- 现有调试入口。

异常保持原有类型。原生打开失败在适用时仍是 `FileNotFoundError`，不会统一转换成 `RuntimeError`。在 Windows 上，即使路径打开被强制为其他后端，包装已有 fd 仍使用 IOCP。

已弃用的 `handle_pool_max_per_key` 与 `handle_pool_max_total` 配置键仍会接受并忽略，以保持兼容；这不代表会恢复已删除的句柄池。

## 2.0 删除的 API

`get_backend_info()` 与 `get_capabilities()` 不再返回 `is_truly_async`。请通过 `info["backend"]` 判断实际 I/O 机制：`iocp`、`io_uring`、`dispatch_io` 或 `thread_pool`。

旧代码：

```python
if not info["is_truly_async"]:
    ...
```

迁移为：

```python
if info["backend"] == "thread_pool":
    ...
```

Python 异步 API 与内核完成模型是两个不同层次；线程池后端并不意味着“假异步”。

## 内部变化

- Python 包迁到 `python/ayafileio/`。
- `_async_file.py` 拆出模式、文本流和批量辅助模块。
- 原生代码按 `core`、`platform`、`backend`、`discovery`、`diagnostics`、`bindings/python` 分层，并使用对应 namespace。
- `Buffer`、`Completion`、`Operation`、`FileState`、`BackendRegistry` 是共享原生类型，Python 对象只存在于 Python 适配层。
- `ResultBatcher` 与其注册表在各后端统一。
- IOCP 拆成上下文、Session、提交、worker 和请求模块。
- Yuyuko 通过生命周期钩子接入；内存记录、异步日志、CRC 和 HTML 报告分别独立，不再进入热路径公共头。
- pytest 位于 `tests/python/`，基准脚本位于 `benchmarks/`。
- CMake 构建独立的 core、backend、discovery、diagnostics 对象库，以及 `aya_core_tests`、`aya_diagnostics_tests`。

私有 `_ayafileio` 扩展、C++ 类、文件布局和 namespace 不构成稳定 C++ ABI。本版本不新增公共 C++ 头文件、C 句柄或其他语言绑定。

## 测试与构建变化

Python 入口保持不变：

```bash
pip install -e ".[dev]"
python -m pytest -q
AYAFILEIO_TEST_BACKEND=thread_pool python -m pytest -q
```

原生测试和依赖检查：

```bash
ctest --test-dir build/<wheel-tag> --output-on-failure
python tools/check_cpp_dependencies.py
```

基准入口：

```bash
python benchmarks/t_compare.py
```

并发日志正确性测试位于 `tests/python/test_loguru.py`。

## 从 2.0 开始的 semver

- **Major**：公共契约出现文档明确的向后不兼容变化。
- **Minor**：向后兼容的新功能与实现改进。
- **Patch**：向后兼容的修复。

仅内部重构、文件移动或 namespace 调整不需要再次提升 major。任何有意的行为变化都必须写入迁移说明，不能藏在重构中。

## 验证范围

2.0 工作区由 Windows、Linux 和 macOS CI 覆盖，包括原生后端、线程池回退和自由线程解释器。各后端在对应平台的 runner 上执行实际测试。
