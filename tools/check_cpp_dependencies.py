#!/usr/bin/env python3
"""Check ayafileio's C++ dependency and namespace boundaries."""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]")
NATIVE_ONLY_SYMBOLS = {"PyObject", "Py_buffer", "OVERLAPPED", "HANDLE", "DWORD"}
SYMBOL_RE = re.compile(r"\b(" + "|".join(sorted(NATIVE_ONLY_SYMBOLS)) + r")\b")
NAMESPACE_EXEMPTIONS = {
    "src/bindings/python/module.cpp",
    "src/diagnostics/debug_log.hpp",
}


@dataclass(frozen=True)
class Include:
    source: Path
    target: str
    line: int
    resolved: Path | None


def component(path: Path) -> str:
    parts = path.parts
    if not parts or parts[0] != "src":
        return "other"
    if len(parts) == 1:
        return "src"
    if parts[1] in {"core", "discover", "diagnostics"}:
        return parts[1]
    if parts[1] in {"platform", "backend"}:
        if len(parts) == 3:
            return parts[1]
        return "/".join(parts[1:3])
    if parts[1] == "bindings":
        if len(parts) >= 3 and parts[2] == "python":
            return "bindings/python"
        return parts[1]
    return parts[1]


def expected_namespace(path: Path) -> str | None:
    relative = path.as_posix()
    if relative in NAMESPACE_EXEMPTIONS:
        return None
    parts = path.parts
    if len(parts) < 3 or parts[0] != "src":
        return None
    if parts[1] == "discover":
        return "ayafileio::discovery"
    if parts[1] in {"core", "diagnostics"}:
        return f"ayafileio::{parts[1]}"
    if parts[1] in {"platform", "backend", "bindings"}:
        if len(parts) == 3:
            return f"ayafileio::{parts[1]}"
        if parts[1] == "bindings" and parts[2] == "python":
            return "ayafileio::python"
        return f"ayafileio::{parts[1]}::{parts[2]}"
    return None


def code_lines(text: str):
    in_block_comment = False
    for number, line in enumerate(text.splitlines(), 1):
        output: list[str] = []
        index = 0
        while index < len(line):
            if in_block_comment:
                end = line.find("*/", index)
                if end < 0:
                    index = len(line)
                    continue
                in_block_comment = False
                index = end + 2
                continue
            start = line.find("/*", index)
            line_comment = line.find("//", index)
            if line_comment >= 0 and (start < 0 or line_comment < start):
                output.append(line[index:line_comment])
                index = len(line)
                continue
            if start < 0:
                output.append(line[index:])
                index = len(line)
                continue
            output.append(line[index:start])
            in_block_comment = True
            index = start + 2
        yield number, "".join(output)


def resolve_include(root: Path, source: Path, target: str) -> Path | None:
    candidates = [
        root / source.parent / target,
        root / "src" / target,
        root / "src" / "backend" / target,
        root / target,
    ]
    for candidate in candidates:
        if candidate.is_file():
            resolved = candidate.resolve()
            try:
                return resolved.relative_to(root)
            except ValueError:
                return None
    return None


def parse_file(root: Path, path: Path) -> tuple[list[Include], str]:
    text = (root / path).read_text(encoding="utf-8", errors="replace")
    includes: list[Include] = []
    for number, line in code_lines(text):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        delimiter, target = match.groups()
        resolved = resolve_include(root, path, target) if delimiter == '"' else None
        includes.append(Include(path, target, number, resolved))
    return includes, text


def allowed(source: str, target: str) -> bool:
    if source == target:
        return True
    if target == "other":
        return False
    if source == "core":
        return target in {"core", "diagnostics"}
    if source == "platform" or source.startswith("platform/"):
        return target == "core" or target == "platform" or target.startswith("platform/")
    if source == "backend":
        return True
    if source.startswith("backend/"):
        return (
            target == "core"
            or target == "platform"
            or target.startswith("platform/")
            or target == "diagnostics"
            or target == source
            or target == "backend"
        )
    if source == "discover":
        return target in {"core", "discover", "platform", "diagnostics"} or target.startswith(
            ("platform/", "backend")
        )
    if source == "bindings/python":
        return True
    if source == "diagnostics":
        return target in {"core", "diagnostics"}
    return False


def cycle_messages(graph: dict[str, set[str]], prefix: str) -> list[str]:
    messages: list[str] = []
    visited: set[str] = set()
    active: list[str] = []
    active_set: set[str] = set()
    reported: set[tuple[str, ...]] = set()

    def visit(node: str) -> None:
        if node in active_set:
            start = active.index(node)
            cycle = tuple(active[start:] + [node])
            if cycle not in reported:
                reported.add(cycle)
                messages.append(f"{prefix}: " + " -> ".join(cycle))
            return
        if node in visited:
            return
        visited.add(node)
        active.append(node)
        active_set.add(node)
        for dependency in sorted(graph.get(node, set())):
            visit(dependency)
        active.pop()
        active_set.remove(node)

    for node in sorted(graph):
        visit(node)
    return messages


def check(root: Path) -> list[str]:
    root = root.resolve()
    src = root / "src"
    errors: list[str] = []
    if not src.is_dir():
        return [f"{src}: missing source directory"]

    files = sorted(
        path.resolve().relative_to(root)
        for path in src.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )
    file_graph: dict[str, set[str]] = defaultdict(set)
    component_graph: dict[str, set[str]] = defaultdict(set)
    dependency_count = 0

    for path in files:
        source_name = path.as_posix()
        source_component = component(path)
        includes, text = parse_file(root, path)
        namespace = expected_namespace(path)
        if namespace and f"namespace {namespace}" not in text:
            errors.append(f"{source_name}: expected namespace {namespace}")

        if source_component == "core":
            for number, line in code_lines(text):
                match = SYMBOL_RE.search(line)
                if match:
                    errors.append(
                        f"{source_name}:{number}: forbidden native/Python symbol {match.group(1)} in core"
                    )

        for include in includes:
            target_name = include.target
            if target_name == "Python.h" or target_name.startswith("nanobind/"):
                if source_component != "bindings/python":
                    errors.append(
                        f"{source_name}:{include.line}: CPython/nanobind include is only allowed in bindings/python"
                    )
                continue

            if include.resolved is None:
                if target_name.endswith((".h", ".hh", ".hpp", ".hxx")) and target_name.startswith(
                    ("core/", "backend/", "platform/", "bindings/", "discover/", "diagnostics/", "./", "../")
                ):
                    errors.append(
                        f"{source_name}:{include.line}: unresolved project include {target_name}"
                    )
                continue

            target_path = include.resolved
            if target_path not in files:
                continue
            target_component = component(target_path)
            dependency_count += 1
            file_graph[source_name].add(target_path.as_posix())
            if source_component != target_component:
                component_graph[source_component].add(target_component)
            if not allowed(source_component, target_component):
                errors.append(
                    f"{source_name}:{include.line}: forbidden dependency "
                    f"{source_component} -> {target_component} ({target_path.as_posix()})"
                )

    errors.extend(cycle_messages(file_graph, "file include cycle"))
    errors.extend(cycle_messages(component_graph, "component dependency cycle"))
    if not errors:
        print(f"Architecture OK: {len(files)} C++ files, {dependency_count} resolved includes")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of tools/)",
    )
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
