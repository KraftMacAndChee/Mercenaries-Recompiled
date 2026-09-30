"""Run repository test modules in separate processes; preserve every failure.

Some fixtures are standalone native-test scripts with import-time execution.
Discover from Python syntax without importing them into the runner process.
Use an x64 MSVC developer shell with GCC and the project Python environment.
"""
from __future__ import annotations

import argparse
import ast
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def test_plan(pattern: str) -> list[dict]:
    plan = []
    for path in sorted((ROOT / "tools").rglob("test_*.py")):
        if any(part in ("__pycache__", ".venv") for part in path.parts):
            continue
        relative = path.relative_to(ROOT).as_posix()
        if pattern not in relative:
            continue
        tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=relative)
        has_tests = any(
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name.startswith("test_") for node in ast.walk(tree)
        )
        plan.append({"file": relative, "mode": "pytest" if has_tests else "script"})
    return plan


def positive(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--match", default="", help="Substring of a test module path")
    parser.add_argument("--list", action="store_true", help="Print plan without running tests")
    parser.add_argument("--output", type=Path, help="New directory for logs and JSON results")
    parser.add_argument("--jobs", type=positive, default=1)
    parser.add_argument("--timeout", type=positive, default=180, help="Seconds per module")
    args = parser.parse_args()
    plan = test_plan(args.match)
    if not plan:
        parser.error("no matching test modules")
    if args.list:
        print(json.dumps(plan, indent=2))
        return 0
    if args.output is None:
        parser.error("--output is required when running tests")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
    # Preserve the caller's compiler/SDK environment and fixture-specific flags.
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(str(ROOT / p) for p in (
        "tools/recomp", "tools/diagnostics", "tools/symbols", "."
    ))
    env["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    for key, paths in (("CPATH", ("ports/mercenaries/src", "src", "include")),
                       ("INCLUDE", ("ports/mercenaries/src",))):
        env[key] = os.pathsep.join([*(str(ROOT / p) for p in paths), env.get(key, "")]).rstrip(os.pathsep)

    def run(row: dict) -> dict:
        stem = row["file"].replace("/", "__")
        command = [sys.executable, "-X", "utf8", "-B"]
        if row["mode"] == "pytest":
            command += ["-m", "pytest", "-q", str(ROOT / row["file"]),
                        "--junitxml=" + str(output / (stem + ".xml"))]
        else:
            command.append(str(ROOT / row["file"]))
        started = time.monotonic()
        with (output / (stem + ".log")).open("w", encoding="utf-8") as log:
            try:
                code = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
                log.write(f"\nRunner timeout after {args.timeout} seconds.\n")
        return dict(row, exit_code=code, seconds=round(time.monotonic() - started, 2),
                    command=command, log=stem + ".log")

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for row in pool.map(run, plan):
            results.append(row)
            (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(f"{len(results)}/{len(plan)} {row['file']}: {row['exit_code']}", flush=True)
    failed = sum(row["exit_code"] != 0 for row in results)
    print(f"{len(results)} modules: {len(results) - failed} successful, {failed} nonzero/timeout")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
