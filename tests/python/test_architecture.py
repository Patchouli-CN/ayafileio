"""Architecture dependency checks and checker regression cases."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "tools" / "check_cpp_dependencies.py"
spec = importlib.util.spec_from_file_location("aya_cpp_dependency_check", CHECKER)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def write(root: Path, relative: str, content: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def test_component_names():
    assert checker.component(Path("src/core/a.hpp")) == "core"
    assert checker.component(Path("src/backend/builtin.cpp")) == "backend"
    assert checker.component(Path("src/backend/linux/a.cpp")) == "backend/linux"
    assert checker.component(Path("src/bindings/python/a.cpp")) == "bindings/python"


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
    assert checker.check(tmp_path) == []


def test_python_include_is_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/bad.cpp",
        '#include <Python.h>\nnamespace ayafileio::core {}\n',
    )
    errors = checker.check(tmp_path)
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
    errors = checker.check(tmp_path)
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
    errors = checker.check(tmp_path)
    assert any("forbidden dependency" in error for error in errors)


def test_namespace_and_missing_include_are_rejected(tmp_path):
    write(
        tmp_path,
        "src/core/a.cpp",
        '#include "../../core/missing.hpp"\nnamespace wrong {}\n',
    )
    errors = checker.check(tmp_path)
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
