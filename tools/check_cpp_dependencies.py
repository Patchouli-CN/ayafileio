#!/usr/bin/env python3
"""Check ayafileio's C++ dependency and namespace boundaries.

Policy and data live in ``tools/architecture-rules.json`` so that the layer
matrix can be edited without touching code. Mechanism stays here on purpose:
comment stripping, the fail-closed header classifier and cycle detection must
not be switchable off by editing a data file.

The rules file is validated strictly. A misspelled layer name is an error, not
a silently dropped rule -- an architecture checker that silently checks less
than you think is worse than no checker.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]")
DEFAULT_RULES = Path(__file__).resolve().parent / "architecture-rules.json"

# Keys starting with "$" are documentation for humans reading the JSON.
RULES_SCHEMA = {
    "layers": dict,
    "dependencies": dict,
    "namespace_exemptions": list,
    "platform_headers_allowed_in": dict,
    "python_headers_allowed_in": list,
    "native_symbols_forbidden_in": dict,
    "immutable_propositions": list,
}


@dataclass(frozen=True)
class Include:
    source: Path
    target: str
    line: int
    resolved: Path | None
    delimiter: str


@dataclass(frozen=True)
class Rules:
    """The loaded policy, already reduced to what the checker needs."""

    layers: dict[str, str]                 # component name -> expected namespace
    dependencies: dict[str, set[str]]       # component -> components it may include
    namespace_exemptions: dict[str, str]    # repo-relative path -> why it is exempt
    platform_headers_allowed: frozenset[str]
    python_headers_allowed: frozenset[str]
    forbidden_symbols: dict[str, frozenset[str]]
    immutable: list[dict]

    def symbol_re(self) -> re.Pattern[str]:
        symbols = sorted({s for group in self.forbidden_symbols.values() for s in group})
        if not symbols:
            return re.compile(r"(?!x)x")  # never matches
        return re.compile(r"\b(" + "|".join(map(re.escape, symbols)) + r")\b")


class RulesError(Exception):
    """The rules file is malformed. Always fatal, never a warning."""


def load_rules(path: Path) -> Rules:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise RulesError(f"{path}: rules file not found")
    except json.JSONDecodeError as exc:
        raise RulesError(f"{path}: invalid JSON: {exc}") from exc

    if not isinstance(raw, dict):
        raise RulesError(f"{path}: top level must be an object")

    unknown = {k for k in raw if not k.startswith("$")} - set(RULES_SCHEMA)
    if unknown:
        raise RulesError(f"{path}: unknown top-level keys {sorted(unknown)}")
    for key, expected in RULES_SCHEMA.items():
        if key not in raw:
            raise RulesError(f"{path}: missing required key '{key}'")
        if not isinstance(raw[key], expected):
            raise RulesError(f"{path}: '{key}' must be {expected.__name__}")

    layers_raw = raw["layers"]
    if not layers_raw:
        raise RulesError(f"{path}: 'layers' must not be empty")
    layers: dict[str, str] = {}
    for name, spec in layers_raw.items():
        if not isinstance(spec, dict) or not isinstance(spec.get("namespace"), str):
            raise RulesError(f"{path}: layer '{name}' needs a string 'namespace'")
        layers[name] = spec["namespace"]

    # Every component must have a rule. A missing key means a component would
    # be silently unconstrained, which is the exact failure mode this file is
    # meant to prevent.
    deps_raw = raw["dependencies"]
    dependencies: dict[str, set[str]] = {}
    layer_names = set(layers)
    for name, spec in deps_raw.items():
        if name not in layer_names:
            raise RulesError(f"{path}: dependency rule for unknown layer '{name}'")
        if not isinstance(spec, dict) or not isinstance(spec.get("allow"), list):
            raise RulesError(f"{path}: layer '{name}' needs an 'allow' list")
        allowed: set[str] = set()
        for target in spec["allow"]:
            if not isinstance(target, str):
                raise RulesError(f"{path}: layer '{name}' has a non-string target")
            if target == "*":
                allowed |= layer_names
            elif target == "_self":
                top = name.split("/", 1)[0]
                allowed |= {c for c in layer_names if c.split("/", 1)[0] == top}
            elif target in layer_names:
                allowed.add(target)
            else:
                raise RulesError(
                    f"{path}: layer '{name}' allows unknown layer '{target}' "
                    f"(known: {sorted(layer_names)})"
                )
        dependencies[name] = allowed

    missing = layer_names - set(dependencies)
    if missing:
        raise RulesError(f"{path}: no dependency rule for {sorted(missing)}")

    exemptions_raw = raw["namespace_exemptions"]
    exemptions: dict[str, str] = {}
    for item in exemptions_raw:
        if not isinstance(item, dict):
            raise RulesError(
                f"{path}: 'namespace_exemptions' entries must be objects "
                f"{{'path': ..., 'reason': ...}}; a bare path is an unreviewable exemption"
            )
        missing_keys = {"path", "reason"} - set(item)
        if missing_keys:
            raise RulesError(
                f"{path}: namespace exemption {item!r} is missing {sorted(missing_keys)}"
            )
        if not isinstance(item["path"], str) or not isinstance(item["reason"], str):
            raise RulesError(f"{path}: namespace exemption {item!r} needs string path and reason")
        # An exemption without a reason is exactly the "it's just this once"
        # case that erodes an architecture checker. Require it, and require it
        # to be a sentence rather than a placeholder.
        if len(item["reason"].strip()) < 20:
            raise RulesError(
                f"{path}: exemption for '{item['path']}' needs a real reason, got {item['reason']!r}"
            )
        exemptions[item["path"]] = item["reason"]

    platform_raw = raw["platform_headers_allowed_in"]
    for key, value in platform_raw.items():
        if not isinstance(value, bool):
            raise RulesError(f"{path}: 'platform_headers_allowed_in.{key}' must be a boolean")
        top_layers = {n.split("/", 1)[0] for n in layer_names}
        if key not in top_layers:
            raise RulesError(f"{path}: 'platform_headers_allowed_in' names unknown layer '{key}'")

    python_allowed = raw["python_headers_allowed_in"]
    for item in python_allowed:
        if item not in layer_names:
            raise RulesError(f"{path}: 'python_headers_allowed_in' names unknown layer '{item}'")

    symbols_raw = raw["native_symbols_forbidden_in"]
    forbidden: dict[str, frozenset[str]] = {}
    for name, group in symbols_raw.items():
        if name not in layer_names:
            raise RulesError(f"{path}: 'native_symbols_forbidden_in' names unknown layer '{name}'")
        if not isinstance(group, list) or not all(isinstance(s, str) for s in group):
            raise RulesError(f"{path}: symbols for '{name}' must be a list of strings")
        forbidden[name] = frozenset(group)

    immutable = raw["immutable_propositions"]
    if not immutable:
        raise RulesError(f"{path}: 'immutable_propositions' must not be empty")
    for prop in immutable:
        if not isinstance(prop, dict) or "id" not in prop:
            raise RulesError(f"{path}: every immutable proposition needs an 'id'")

    return Rules(
        layers=layers,
        dependencies=dependencies,
        namespace_exemptions=exemptions,
        platform_headers_allowed=frozenset(k for k, v in platform_raw.items() if v),
        python_headers_allowed=frozenset(python_allowed),
        forbidden_symbols=forbidden,
        immutable=immutable,
    )


def component(path: Path, rules: Rules | None = None) -> str:
    """Map a repo-relative path to its component name.

    With rules, the result is the deepest configured layer that prefixes the
    path, so ``backend/linux/x.hpp`` resolves to ``backend/linux`` rather than
    the parent ``backend`` and the mapping is data-driven rather than
    hard-coded.
    """
    parts = path.parts
    if not parts or parts[0] != "src":
        return "other"
    if len(parts) == 1:
        return "src"
    if rules is not None:
        # Deepest first: a sub-layer must win over its parent layer.
        for depth in sorted(range(2, len(parts) + 1), reverse=True):
            candidate = "/".join(parts[1:depth])
            if candidate in rules.layers:
                return candidate
        return parts[1]
    # Fallback used only before rules are loaded (unit tests of this helper).
    if parts[1] in {"core", "discover", "diagnostics"}:
        return parts[1]
    if parts[1] in {"platform", "backend"}:
        return parts[1] if len(parts) == 3 else "/".join(parts[1:3])
    if parts[1] == "bindings":
        return "bindings/python" if len(parts) >= 3 and parts[2] == "python" else "bindings"
    return parts[1]


def expected_namespace(path: Path, rules: Rules) -> str | None:
    if path.as_posix() in rules.namespace_exemptions:
        return None
    name = component(path, rules)
    return rules.layers.get(name)


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


def code_only(text: str) -> str:
    """Comment-stripped source, for checks that must not be fooled by prose."""
    return "\n".join(line for _, line in code_lines(text))


def is_platform_header(target: str) -> bool:
    """True for any header that is not provided by the C++ standard library.

    Standard headers are dotless names (``vector``, ``chrono``, ``bit``).
    Everything else -- ``<sys/stat.h>``, ``<windows.h>``, ``<dispatch/dispatch.h>``
    -- is platform-bound. Deliberately heuristic-free: a header counts as
    standard only if *no* path component contains a dot, so a new OS header
    fails closed instead of silently widening ``core``.
    """
    return any("." in part for part in target.split("/"))


def resolve_include(root: Path, source: Path, target: str) -> Path | None:
    """Resolve a quoted include against the directories CMake actually adds.

    Only two forms are real in this tree: a relative path from the including
    file, and a path relative to one of the ``src`` include directories.
    Deliberately no ``src/backend/`` guessing: a speculative candidate can
    resolve to the wrong file and make the dependency graph lie.
    """
    candidates = [root / source.parent / target, root / "src" / target]
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
        includes.append(Include(path, target, number, resolved, delimiter))
    return includes, text


def allowed(source: str, target: str, rules: Rules) -> bool:
    if source == target:
        return True
    if target == "other":
        return False
    return target in rules.dependencies.get(source, set())


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


def check(root: Path, rules: Rules | None = None) -> list[str]:
    root = root.resolve()
    if rules is None:
        top = root.name
        rules = load_rules(root / "tools" / "architecture-rules.json")
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
    symbol_re = rules.symbol_re()

    for path in files:
        source_name = path.as_posix()
        source_component = component(path, rules)
        includes, text = parse_file(root, path)
        # Namespace must be declared in code, not merely mentioned in a comment.
        namespace = expected_namespace(path, rules)
        if namespace and f"namespace {namespace}" not in code_only(text):
            errors.append(f"{source_name}: expected namespace {namespace}")

        forbidden_group = rules.forbidden_symbols.get(source_component)
        if forbidden_group:
            for number, line in code_lines(text):
                match = symbol_re.search(line)
                if match and match.group(1) in forbidden_group:
                    errors.append(
                        f"{source_name}:{number}: forbidden native/Python symbol "
                        f"{match.group(1)} in {source_component}"
                    )

        for include in includes:
            target_name = include.target
            is_python_header = target_name == "Python.h" or target_name.startswith("nanobind/")
            if is_python_header:
                if source_component not in rules.python_headers_allowed:
                    errors.append(
                        f"{source_name}:{include.line}: CPython/nanobind include is only "
                        f"allowed in {sorted(rules.python_headers_allowed)}"
                    )
                continue

            if include.resolved is None:
                # An angle include can never be a project file: it is either a
                # standard header or a platform header, and only the latter has
                # to respect the component allow-list.
                if (
                    is_platform_header(target_name)
                    and include.delimiter == "<"
                    and source_component.split("/", 1)[0] not in rules.platform_headers_allowed
                ):
                    errors.append(
                        f"{source_name}:{include.line}: platform header <{target_name}> "
                        f"is not allowed in {source_component}"
                    )
                elif target_name.endswith((".h", ".hh", ".hpp", ".hxx")) and target_name.startswith(
                    tuple(f"{name.split('/', 1)[0]}/" for name in rules.layers)
                    + ("./", "../")
                ):
                    errors.append(
                        f"{source_name}:{include.line}: unresolved project include {target_name}"
                    )
                continue

            target_path = include.resolved
            if target_path not in files:
                continue
            target_component = component(target_path, rules)
            dependency_count += 1
            file_graph[source_name].add(target_path.as_posix())
            if source_component != target_component:
                component_graph[source_component].add(target_component)
            if not allowed(source_component, target_component, rules):
                errors.append(
                    f"{source_name}:{include.line}: forbidden dependency "
                    f"{source_component} -> {target_component} ({target_path.as_posix()})"
                )

    errors.extend(cycle_messages(file_graph, "file include cycle"))
    errors.extend(cycle_messages(component_graph, "component dependency cycle"))
    if not errors:
        print(
            f"Architecture OK: {len(files)} C++ files, {dependency_count} resolved includes, "
            f"{len(rules.layers)} layers"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of tools/)",
    )
    parser.add_argument(
        "--rules",
        type=Path,
        default=None,
        help="rules file (defaults to <root>/tools/architecture-rules.json)",
    )
    args = parser.parse_args()
    rules_path = args.rules or (args.root / "tools" / "architecture-rules.json")
    try:
        rules = load_rules(rules_path)
    except RulesError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    errors = check(args.root, rules)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
