# 安全策略

## 支持版本

仅最新版本接收安全更新。早于最新版本的版本**不受支持**。

| 版本   | 状态          |
| ------ | ------------- |
| 1.4.x  | :white_check_mark: |
| < 1.4  | :x:                |

## 报告漏洞

**请勿公开提 Issue。** 请通过以下方式私下报告漏洞：

- 邮箱：[3072252442@qq.com](mailto:3072252442@qq.com)
- GitHub：[安全通告](https://github.com/Patchouli-CN/ayafileio/security/advisories/new)（推荐）

我们将在 7 天内回复，确认后 30 天内发布修复。

### 应包含的内容

- 漏洞的清晰描述
- 复现步骤（最好附带最小代码示例）
- 影响平台（Windows / Linux / macOS）
- 修复建议（可选）

### 披露策略

修复发布后，漏洞将在更新日志中公开披露。我们遵循[协调披露](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/about-coordinated-disclosure-of-security-vulnerabilities)原则。

## 安全边界

ayafileio 是基于 C++20 实现的**内核级异步文件 I/O 库**，使用各平台原生异步原语。安全敏感边界包括：

| 边界 | 风险 |
| ---- | ---- |
| C++ 扩展 ↔ Python | 原生代码手动内存管理（nanobind 绑定） |
| 文件描述符操作 | fd 所有权、close 后使用、双重 close 竞态 |
| 内核 I/O 原语 | IOCP / io_uring / Dispatch I/O — 各有平台专属生命周期规则 |
| 缓冲池 | 原始 `malloc`/`free` + 线程本地缓存 |
| I/O 请求生命周期 | 并发、use-after-free、双重完成（IOCP） |

### 在范围内

- 内存损坏（缓冲区溢出、use-after-free、double-free）
- 文件描述符泄漏或竞态（fd close 后复用、双重 close）
- 数据损坏或静默数据丢失
- GIL 相关的死锁或崩溃
- 通过文件 I/O 的沙箱逃逸

### 不在范围内

- 依赖库的漏洞（请向上游报告）
- 需要任意代码执行才能利用的问题（该库运行在 Python 进程内）
- 资源耗尽型拒绝服务（无限文件大小、无限制 I/O）
- 社会工程学或钓鱼攻击

## 致谢

感谢所有负责任披露漏洞的安全研究人员。经您许可，贡献者将在更新日志中致谢。
