#!/usr/bin/env python3
"""Compile-check native game sources; this does not measure runtime completeness."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--output", type=Path, default=ROOT / "build/native-audit.json")
    parser.add_argument("paths", nargs="*", default=["src/Game"])
    args = parser.parse_args()
    sources = set()
    for path in args.paths:
        path = ROOT / path
        sources.update(path.rglob("*.cpp") if path.is_dir() else [path])
    command = [args.compiler, "-std=c++17", "-DPETARI_NATIVE=1", "-fsyntax-only",
               "-Wno-register", "-Wno-inconsistent-missing-override", "-Werror=return-type",
               "-ferror-limit=5", "-fno-color-diagnostics"]
    for path in ["native/include", "include", "libs/JSystem/include", "libs/RVL_SDK/include",
                 "libs/nw4r/include", "libs/RVLFaceLib/include"]:
        command += ["-I", str(ROOT / path)]

    def compile_source(path):
        result = subprocess.run(command + [str(path)], cwd=ROOT, text=True, capture_output=True)
        return {"source": str(path.relative_to(ROOT)), "passed": result.returncode == 0,
                "diagnostics": result.stderr}

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(compile_source, sorted(sources)):
            results.append(result)
            if len(results) % 100 == 0:
                print(f"Checked {len(results)}/{len(sources)}", flush=True)
    passed = sum(r["passed"] for r in results)
    report = {"checked": len(results), "passed": passed, "failed": len(results) - passed,
              "command": command, "results": results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{passed}/{len(results)} compile checks passed; report: {args.output}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
