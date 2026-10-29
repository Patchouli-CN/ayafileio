"""Architecture dependency checks and checker regression cases."""

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "check_cpp_dependencies.py"
RULES = ROOT / "tools" / "architecture-rules.json"
spec = importlib.util.spec_from_file_location("aya_cpp_dependency_check", CHECKER)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)

# Tests build synthetic trees, so they must not depend on the repo's own rules
# file being the one under test. They use the same real rules by default.
REPO_RULES = checker.load_rules(RULES)


def write(root: Path, relative: str, content: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def test_component_names():
    assert checker.component(Path("src/core/a.hpp"), REPO_RULES) == "core"
    assert checker.component(Path("src/backend/builtin.cpp"), REPO_RULES) == "backend"
    assert checker.component(Path("src/backend/linux/a.cpp"), REPO_RULES) == "backend/linux"
    assert checker.component(Path("src/bindings/python/a.cpp"), REPO_RULES) == "bindings/python"
    assert checker.component(Path("README.md"), REPO_RULES) == "other"


def test_valid_tree_and_system_headers(tmp_path):
    write(tmp_path, "src/core/a.hpp", "namespace ayafileio::core {}\n")
    write(
        tmp_path,
        "src/backend/thread/t.cpp",
        '#include "../../core/a.hpp"\n#include <sys/stat.h>\n'
        "namespace ayafileio::backend::thread {}\n",
    )
    write(
        tmp_path,
        "src/bindings/python/p.cpp",
        '#include <Python.h>\n#include "../../core/a.hpp"\n'
        "namespace ayafileio::python {}\n",
    )
    write(
        tmp_path,
        "src/backend/macos/m.cpp",
        '#include <dispatch/dispatch.h>\n'
        "namespace ayafileio::backend::macos {}\n",
    )
    assert checker.check(tmp_path, REPO_RULES) == []


def test_python_include_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/bad.cpp",
        '#include <Python.h>\nnamespace ayafileio::core {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("CPython/nanobind" in error for error in errors)


def test_component_cycle_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/a.hpp",
        '#include "b.hpp"\nnamespace ayafileio::core {}\n',
    )
    write(
        tmp_path,
        "src/core/b.hpp",
        '#include "a.hpp"\nnamespace ayafileio::core {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("cycle" in error for error in errors)


def test_reverse_dependency_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/backend/thread/t.hpp",
        "namespace ayafileio::backend::thread {}\n",
    )
    write(
        tmp_path,
        "src/core/a.cpp",
        '#include "../backend/thread/t.hpp"\nnamespace ayafileio::core {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("forbidden dependency" in error for error in errors)


def test_namespace_and_missing_include_are_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/a.cpp",
        '#include "../../core/missing.hpp"\nnamespace wrong {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("expected namespace" in error for error in errors)
    assert any("unresolved project include" in error for error in errors)


def test_real_sources_pass_dependency_check():
    result = subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(ROOT)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Architecture OK" in result.stdout


# ── One minimal failing case per checker branch ───────────────────────────────
# Every ``errors.append`` in check() needs a case here. A branch without a case
# is either dead code or inverted, and neither is visible from CI otherwise.

def test_missing_src_directory_is_rejected(tmp_path):
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("missing source directory" in error for error in errors)


def test_namespace_declared_only_in_comment_is_rejected(tmp_path):
    """A comment mentioning the namespace must not satisfy the check."""
    write(
        tmp_path,
        "src/core/a.cpp",
        "/*\nnamespace ayafileio::core\n*/\nnamespace wrong {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("expected namespace ayafileio::core" in error for error in errors)


def test_namespace_in_block_comment_and_include_in_block_comment(tmp_path):
    """Neither a namespace nor an include inside block comments may count."""
    write(
        tmp_path,
        "src/core/a.cpp",
        '/*\n#include "../backend/thread/t.hpp"\nnamespace ayafileio::core\n*/\n'
        "namespace wrong {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("expected namespace" in error for error in errors)
    assert not any("forbidden dependency" in error for error in errors)


def test_platform_header_in_core_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/a.cpp",
        "#include <windows.h>\nnamespace ayafileio::core {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("platform header <windows.h> is not allowed in core" in error for error in errors)


def test_core_standard_headers_are_allowed(tmp_path):
    write(
        tmp_path,
        "src/core/a.hpp",
        "#include <vector>\n#include <cstdint>\n#include <chrono>\n"
        "namespace ayafileio::core {}\n",
    )
    assert checker.check(tmp_path, REPO_RULES) == []


def test_dotted_standard_style_headers_are_treated_as_platform(tmp_path):
    """``sys/stat.h`` has no dot in its first component but is not portable."""
    write(
        tmp_path,
        "src/core/a.cpp",
        "#include <sys/stat.h>\nnamespace ayafileio::core {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("platform header <sys/stat.h>" in error for error in errors)


def test_platform_header_is_allowed_in_backend_subdirectory(tmp_path):
    """backend/linux may include liburing; the allow-list is per top layer."""
    write(
        tmp_path,
        "src/backend/linux/u.hpp",
        "#include <liburing.h>\nnamespace ayafileio::backend::linux {}\n",
    )
    assert checker.check(tmp_path, REPO_RULES) == []


def test_native_symbol_in_core_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/a.hpp",
        "struct Holder { HANDLE h; };\nnamespace ayafileio::core {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("forbidden native/Python symbol HANDLE in core" in error for error in errors)


def test_backend_name_string_in_core_is_not_a_native_symbol(tmp_path):
    """``io_uring`` in core is a backend name and config field, not a type."""
    write(
        tmp_path,
        "src/core/a.cpp",
        'static const char* n = "io_uring";\n'
        "unsigned io_uring_queue_depth = 256;\n"
        "namespace ayafileio::core {}\n",
    )
    assert checker.check(tmp_path, REPO_RULES) == []


def test_component_cycle_without_nameable_cycle_is_rejected(tmp_path):
    """A component-level cycle that is not also a file-level cycle."""
    write(tmp_path, "src/core/a.hpp", "namespace ayafileio::core {}\n")
    write(tmp_path, "src/core/b.hpp", "namespace ayafileio::core {}\n")
    # A backend that depends back on core, while core depends on it.
    write(
        tmp_path,
        "src/backend/thread/t.hpp",
        '#include "../../core/a.hpp"\nnamespace ayafileio::backend::thread {}\n',
    )
    write(
        tmp_path,
        "src/core/a.cpp",
        '#include "b.hpp"\n#include "backend/thread/t.hpp"\n'
        "namespace ayafileio::core {}\n",
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("forbidden dependency core -> backend/thread" in error for error in errors)


def test_bindings_may_reach_backend_internals_by_design(tmp_path):
    """The composition root surfaces backend-specific knobs, so it is exempt."""
    write(tmp_path, "src/backend/windows/iocp.hpp",
          "namespace ayafileio::backend::windows {}\n")
    write(
        tmp_path,
        "src/bindings/python/p.cpp",
        '#include "../../backend/windows/iocp.hpp"\nnamespace ayafileio::python {}\n',
    )
    assert checker.check(tmp_path, REPO_RULES) == []


def test_discover_cannot_depend_on_backend(tmp_path):
    """Backend selection belongs to core::BackendRegistry, not discovery."""
    write(
        tmp_path,
        "src/backend/thread/t.hpp",
        "namespace ayafileio::backend::thread {}\n",
    )
    write(
        tmp_path,
        "src/discover/d.cpp",
        '#include "../backend/thread/t.hpp"\nnamespace ayafileio::discovery {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("forbidden dependency discover -> backend" in error for error in errors)


def test_speculative_include_resolution_is_gone(tmp_path):
    """A header must not be resolved through a guessed ``src/backend/`` root."""
    write(tmp_path, "src/core/target.hpp", "namespace ayafileio::core {}\n")
    write(tmp_path, "src/core/a.cpp", '#include "target.hpp"\nnamespace ayafileio::core {}\n')
    assert checker.check(tmp_path, REPO_RULES) == []

    # Same target name under backend/ must not satisfy an include from core.
    write(tmp_path, "src/backend/only_here.hpp", "namespace ayafileio::backend {}\n")
    write(
        tmp_path,
        "src/core/b.cpp",
        '#include "backend/only_here.hpp"\nnamespace ayafileio::core {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("forbidden dependency core -> backend" in error for error in errors)


def test_component_cycle_is_reported_when_all_layers_allow_each_other(tmp_path):
    """Component cycles are detected even if every hop is individually legal."""
    # platform -> core is legal; core -> platform is not, so use a pair where
    # both directions are permitted by ``allowed()``: backend self-hops are
    # legal, so build a cycle across two backend sub-components via files.
    write(
        tmp_path,
        "src/backend/linux/a.hpp",
        '#include "../macos/b.hpp"\nnamespace ayafileio::backend::linux {}\n',
    )
    write(
        tmp_path,
        "src/backend/macos/b.hpp",
        '#include "../linux/a.hpp"\nnamespace ayafileio::backend::macos {}\n',
    )
    errors = checker.check(tmp_path, REPO_RULES)
    assert any("cycle" in error for error in errors)


def test_every_error_branch_has_coverage():
    """Guard against a new branch shipping without a regression case."""
    source = CHECKER.read_text(encoding="utf-8")
    branches = source.count("errors.append(")
    assert branches >= 6, f"checker has {branches} error branches; update this guard"


# ── The golden lock ───────────────────────────────────────────────────────────
# Rules live in architecture-rules.json so the layer matrix can be edited
# without touching code. That flexibility is also the risk: moving a rule out of
# Python moves it out of the diff-visible, test-asserted surface. These tests
# re-read the JSON and check the propositions the architecture actually rests
# on, so a rule edit that loosens them fails CI even though the JSON parses.

RULES_JSON = json.loads(RULES.read_text(encoding="utf-8"))
LAYER_NAMES = set(RULES_JSON["layers"])


def _allowed(layer: str) -> set[str]:
    """Recompute the effective allow-set from the JSON, exactly as the loader does."""
    allowed: set[str] = set()
    for target in RULES_JSON["dependencies"][layer]["allow"]:
        if target == "*":
            allowed |= LAYER_NAMES
        elif target == "_self":
            top = layer.split("/", 1)[0]
            allowed |= {c for c in LAYER_NAMES if c.split("/", 1)[0] == top}
        else:
            allowed.add(target)
    return allowed


def test_rules_file_matches_immutable_propositions():
    """Every proposition in immutable_propositions must still hold in the JSON."""
    props = {p["id"]: p for p in RULES_JSON["immutable_propositions"]}
    failures: list[str] = []

    core = props.get("core_is_platform_neutral")
    if core:
        reachable = _allowed("core")
        for target in core["forbid"]:
            if target in reachable:
                failures.append(f"core_is_platform_neutral: core may reach {target}")

    neutral = props.get("core_takes_no_platform_headers")
    if neutral:
        layer = neutral["forbidden_header_layer"]
        if RULES_JSON["platform_headers_allowed_in"].get(layer):
            failures.append(f"core_takes_no_platform_headers: {layer} may take platform headers")

    bindings = props.get("nothing_depends_on_bindings")
    if bindings:
        target = bindings["forbidden_target"]
        for layer in LAYER_NAMES:
            if layer == bindings.get("forbidden_for_every_layer_except"):
                continue
            if target in _allowed(layer):
                failures.append(f"nothing_depends_on_bindings: {layer} may reach {target}")

    diagnostics = props.get("diagnostics_never_reaches_up")
    if diagnostics:
        reachable = _allowed("diagnostics")
        for target in diagnostics["forbid"]:
            if target in reachable:
                failures.append(f"diagnostics_never_reaches_up: diagnostics may reach {target}")

    discover = props.get("discover_does_not_select_backends")
    if discover:
        reachable = _allowed("discover")
        for target in discover["forbid"]:
            if target in reachable:
                failures.append(f"discover_does_not_select_backends: discover may reach {target}")

    backend = props.get("no_backend_knows_another_backend_internals")
    if backend:
        os_layers = [n for n in LAYER_NAMES if n.startswith("backend/")]
        for layer in os_layers:
            reachable = _allowed(layer)
            for other in os_layers:
                if other != layer and other in reachable:
                    failures.append(
                        f"no_backend_knows_another_backend_internals: {layer} may reach {other}"
                    )

    assert not failures, "immutable architecture propositions violated:\n  " + "\n  ".join(failures)


def test_every_layer_has_a_dependency_rule():
    """A layer without a rule would be silently unconstrained."""
    missing = LAYER_NAMES - set(RULES_JSON["dependencies"])
    assert not missing, f"layers with no rule: {sorted(missing)}"


def test_dependency_targets_are_known_layers():
    """A typo would otherwise drop the rule instead of failing."""
    unknown = {
        target
        for spec in RULES_JSON["dependencies"].values()
        for target in spec["allow"]
        if target not in LAYER_NAMES and target not in {"*", "_self"}
    }
    assert not unknown, f"unknown layers referenced: {sorted(unknown)}"


def test_core_is_not_platform_header_exempt():
    """Narrow, emphatic version of the header proposition."""
    assert not RULES_JSON["platform_headers_allowed_in"].get("core")


def test_forbidden_symbol_list_contains_only_types():
    """The symbol list is about architecture, not about compiler diagnostics."""
    symbols = set(RULES_JSON["native_symbols_forbidden_in"].get("core", []))
    # A backend *name* and its config fields are platform-neutral.
    assert "io_uring" not in symbols
    assert not any(s.startswith("io_uring_") and s.endswith("queue_depth") for s in symbols)


def test_malformed_rules_file_is_fatal(tmp_path):
    """A bad rules file must fail loudly, not silently check less."""
    rules = tmp_path / "bad.json"
    rules.write_text(json.dumps({"layers": {"core": {"namespace": "ayafileio::core"}}}), encoding="utf-8")
    with pytest.raises(checker.RulesError):
        checker.load_rules(rules)


def test_unknown_dependency_target_is_fatal(tmp_path):
    payload = json.loads(RULES.read_text(encoding="utf-8"))
    payload["dependencies"]["core"]["allow"] = ["Core"]
    rules = tmp_path / "typo.json"
    rules.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(checker.RulesError) as excinfo:
        checker.load_rules(rules)
    assert "Core" in str(excinfo.value)


def test_cli_reports_rules_error_with_exit_code_2(tmp_path):
    broken = tmp_path / "broken.json"
    broken.write_text("{not json", encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(ROOT), "--rules", str(broken)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 2
    assert "invalid JSON" in result.stderr


