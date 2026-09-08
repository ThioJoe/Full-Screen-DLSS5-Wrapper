#!/usr/bin/env bash
# The gate (VIII) for the portable targets: warnings as errors, lint, formatter, property tests under
# several seeds, sanitizers, mutation testing and the inventories. Run from the repository root.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
mutants=${MUTANTS:-40}
threshold=${MUTATION_THRESHOLD:-0.8}
build=${BUILD_DIR:-build-gate}
asan=${ASAN_DIR:-build-gate-asan}

echo "== configure and build (warnings as errors, tracing on)"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$build"
cmake --build "$build" --target rules_lint

echo "== formatter"
if command -v clang-format >/dev/null; then
  git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run --Werror --style=file
else
  echo "clang-format not installed" >&2; exit 1
fi

echo "== rules lint, function index and inventories"
"$build/rules_lint" . "$build/gate"

echo "== property tests and seed fuzzer"
ctest --test-dir "$build" --output-on-failure
for seed in ${SEEDS:-4000 5000 6000}; do "$build/dscreen_tests" "$seed"; done

echo "== address and undefined-behaviour sanitizers"
cmake -S . -B "$asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDSCREEN_SANITIZE=ON >/dev/null
cmake --build "$asan"
ctest --test-dir "$asan" --output-on-failure

echo "== mutation testing ($mutants mutants, threshold $threshold)"
python3 gate/mutate.py --build-dir "$build" --sample "$mutants" --threshold "$threshold"

echo "== dependency lock"
python3 gate/check_lock.py "${DLSS_SDK_DIR:-}" "${NVOF_SDK_DIR:-}"

echo "gate: passed"
