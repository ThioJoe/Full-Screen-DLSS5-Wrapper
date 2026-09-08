#!/usr/bin/env python3
"""Mutation testing for the property tests (R26).

Applies one deliberate defect at a time to the interior sources, rebuilds the test binary and runs one
seed. A mutant that survives with the tests still passing is a hole in the tests. Sources are restored
after every mutant, on interruption too.
"""
import argparse
import pathlib
import random
import re
import subprocess
import sys
import time

# Comparison operators are written with spaces around them; template brackets are not, so the
# patterns require the spaces and never touch a template argument list.
OPERATORS = [
    (re.compile(r"(?<= )<(?= )"), "<=", "< to <="),
    (re.compile(r"(?<= )<=(?= )"), "<", "<= to <"),
    (re.compile(r"(?<= )>(?= )"), ">=", "> to >="),
    (re.compile(r"(?<= )>=(?= )"), ">", ">= to >"),
    (re.compile(r"(?<= )==(?= )"), "!=", "== to !="),
    (re.compile(r"(?<= )!=(?= )"), "==", "!= to =="),
    (re.compile(r"(?<=[\w)\]] )\+(?= [\w(])"), "-", "+ to -"),
    (re.compile(r"(?<=[\w)\]] )-(?= [\w(])"), "+", "- to +"),
    (re.compile(r"\btrue\b"), "false", "true to false"),
    (re.compile(r"\bfalse\b"), "true", "false to true"),
    (re.compile(r"&&"), "||", "&& to ||"),
    (re.compile(r"\|\|"), "&&", "|| to &&"),
]


def is_code(line: str) -> bool:
    stripped = line.strip()
    return bool(stripped) and not stripped.startswith("//") and not stripped.startswith("#") and "WAIVER" not in stripped


def sites(path: pathlib.Path):
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    for number, line in enumerate(lines):
        if not is_code(line) or 'return "' in line or "static_assert" in line:
            continue
        code = line.split("//")[0]
        for pattern, replacement, name in OPERATORS:
            for match in pattern.finditer(code):
                yield (path, number, match.start(), match.end(), replacement, name)


def mutate(lines, site):
    _, number, start, end, replacement, _ = site
    line = lines[number]
    return lines[:number] + [line[:start] + replacement + line[end:]] + lines[number + 1:]


def run(command, cwd, timeout):
    try:
        completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return completed.returncode, completed.stdout + completed.stderr
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--build-dir", default="build-linux")
    parser.add_argument("--target", default="dscreen_tests")
    parser.add_argument("--seed", type=int, default=1000, help="property-test seed run against every mutant")
    parser.add_argument("--sample", type=int, default=40, help="number of mutants to try (0 = all)")
    parser.add_argument("--shuffle-seed", type=int, default=1, help="seed of the mutant sample")
    parser.add_argument("--threshold", type=float, default=0.8, help="minimum kill ratio")
    parser.add_argument("--timeout", type=int, default=600, help="seconds allowed per build or test run")
    parser.add_argument("paths", nargs="*", default=["src/interior"], help="files or directories to mutate")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    files = []
    for entry in args.paths:
        p = root / entry
        files.extend(sorted(p.rglob("*.cpp")) if p.is_dir() else [p])
    all_sites = [s for f in files for s in sites(f)]
    random.Random(args.shuffle_seed).shuffle(all_sites)
    chosen = all_sites if args.sample == 0 else all_sites[: args.sample]
    print(f"mutate: {len(all_sites)} candidate sites in {len(files)} files, trying {len(chosen)}", flush=True)

    build = ["cmake", "--build", str(root / args.build_dir), "--target", args.target]
    test = [str(root / args.build_dir / args.target), str(args.seed)]
    killed, survived, stillborn = 0, 0, 0
    survivors = []
    started = time.time()
    for index, site in enumerate(chosen):
        path, number, _, _, _, name = site
        original = path.read_text(encoding="utf-8")
        lines = original.splitlines(keepends=True)
        mutated = "".join(mutate(lines, site))
        label = f"{path.relative_to(root)}:{number + 1} {name}"
        try:
            path.write_text(mutated, encoding="utf-8")
            code, output = run(build, root, args.timeout)
            if code != 0:
                stillborn += 1
                verdict = "stillborn (does not compile)"
            else:
                code, output = run(test, root, args.timeout)
                if code != 0:
                    killed += 1
                    verdict = "killed"
                else:
                    survived += 1
                    survivors.append(label)
                    verdict = "SURVIVED"
        finally:
            path.write_text(original, encoding="utf-8")
        print(f"[{index + 1}/{len(chosen)}] {label}: {verdict}", flush=True)

    code, output = run(build, root, args.timeout)
    if code != 0:
        print("mutate: rebuild of the restored sources failed:\n" + output)
        return 2
    scored = killed + survived
    ratio = killed / scored if scored else 1.0
    print(f"mutate: {killed} killed, {survived} survived, {stillborn} stillborn; score {ratio:.2f} in {time.time() - started:.0f}s")
    for label in survivors:
        print(f"  survivor: {label}")
    return 0 if ratio >= args.threshold else 1


if __name__ == "__main__":
    sys.exit(main())
