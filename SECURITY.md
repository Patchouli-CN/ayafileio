# Security Policy

## Supported Versions

Only the latest release receives security updates. Versions older than the latest release are **not supported**.

| Version | Supported          |
| ------- | ------------------ |
| 1.4.x   | :white_check_mark: |
| < 1.4    | :x:                |

## Reporting a Vulnerability

**Please do not open a public issue.** Instead, report vulnerabilities privately via one of:

- Email: [3072252442@qq.com](mailto:3072252442@qq.com)
- GitHub: [Security Advisories](https://github.com/Patchouli-CN/ayafileio/security/advisories/new) (recommended)

You should receive a response within 7 days. We aim to triage all reports and release a fix within 30 days of confirmation.

### What to include

- A clear description of the vulnerability
- Steps to reproduce (minimal code example if possible)
- Affected platform(s) (Windows, Linux, macOS)
- Any suggested fix (optional)

### Disclosure policy

Once a fix is released, the vulnerability will be publicly disclosed in the changelog. We follow [coordinated disclosure](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/about-coordinated-disclosure-of-security-vulnerabilities).

## Security Boundaries

ayafileio is a **kernel-level async file I/O library** implemented in C++20 with platform-native async primitives. Security-sensitive boundaries include:

| Boundary | Risk |
| -------- | ---- |
| C++ extension ↔ Python | Native code with manual memory management (nanobind bindings) |
| File descriptor operations | fd ownership, close-after-free, double-close races |
| Kernel I/O primitives | IOCP, io_uring, Dispatch I/O — each with platform-specific lifetime rules |
| Buffer pool | Raw `malloc`/`free` with thread-local caching |
| I/O request lifecycle | Concurrency, use-after-free, double-completion (IOCP) |

### What is in scope

- Memory corruption (buffer overflow, use-after-free, double-free)
- File descriptor leaks or races (fd reuse after close, double-close)
- Data corruption or silent data loss
- GIL-related deadlocks or crashes
- Sandbox escape via file I/O

### What is out of scope

- Vulnerabilities in dependencies (report upstream)
- Issues requiring arbitrary code execution to exploit (the library runs inside the Python process)
- Denial-of-service via resource exhaustion (infinite file size, unbounded I/O)
- Social engineering or phishing

## Acknowledgments

We thank everyone who responsibly discloses vulnerabilities. With your permission, contributors will be acknowledged in the changelog.
