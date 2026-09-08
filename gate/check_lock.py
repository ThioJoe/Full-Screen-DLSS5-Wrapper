#!/usr/bin/env python3
"""Compares the installed NVIDIA SDK headers against gate/dependencies.lock (gate VIII.8)."""
import pathlib
import re
import sys


def expected(lock: str, key: str) -> dict:
    line = next((l for l in lock.splitlines() if l.startswith(key)), "")
    return dict(re.findall(r"(\w+)=(\w+)", line))


def macro_value(header: pathlib.Path, name: str) -> str:
    match = re.search(r"#define\s+" + name + r"\s+\(?\s*([\w]+)", header.read_text(encoding="utf-8", errors="replace"))
    return match.group(1) if match else "missing"


def check(sdk: str, header: str, key: str, lock: str) -> int:
    if not sdk:
        print(f"check_lock: {key}: SDK not configured, skipped")
        return 0
    found = next(pathlib.Path(sdk).rglob(header), None)
    if found is None:
        print(f"check_lock: {key}: {header} not found under {sdk}")
        return 1
    failures = 0
    for name, value in expected(lock, key).items():
        actual = macro_value(found, name)
        ok = int(actual, 0) == int(value, 0) if actual != "missing" else False
        print(f"check_lock: {name} expected {value}, found {actual}: {'ok' if ok else 'MISMATCH'}")
        failures += 0 if ok else 1
    return failures


def main() -> int:
    lock = (pathlib.Path(__file__).parent / "dependencies.lock").read_text(encoding="utf-8")
    dlss = sys.argv[1] if len(sys.argv) > 1 else ""
    nvof = sys.argv[2] if len(sys.argv) > 2 else ""
    return 1 if check(dlss, "nvsdk_ngx_defs.h", "NGX_API_VERSION", lock) + check(nvof, "nvOpticalFlowCommon.h", "NVOF_API_VERSION", lock) else 0


if __name__ == "__main__":
    sys.exit(main())
