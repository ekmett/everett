COLA Linux Portability Check
===========================

I checked five COLA suites on Linux x86-64 using the headers from
`e13d0309557594c0becee3c1a5e32ce1f3b4771d`, with the test-only oracle correction
`9336a9ce4ad166c1aa7147fef2e31701f9194d91`. No production header changed for this
check. The same test correction is available on main as
`a66f7de471695628cc00048a1a4efd554bb80b3d`. This is correctness and portability
evidence, not a benchmark.

Environment and scope
---------------------

- Host: eak-quartus, Intel Core i9-12900K.
- Linux 6.8.0-100-generic, x86-64, glibc 2.35.
- Ubuntu Clang 20.0.0, build `b74e588e1f46`, using the installed libstdc++ 12 headers.
- C++20, `-O2 -g -Wall -Wextra -Wpedantic -Werror -UNDEBUG`,
  `-fsanitize=address,undefined -fno-omit-frame-pointer -pthread`.
- Runtime: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- Default compiler ISA target; no `-march=native`, AVX2 or AVX-512 override.

I copied `include/everett` and the five standalone test sources directly by SCP
to a fresh temporary directory. The snapshot contained 55 files and 686,068
bytes before the oracle correction. Compilation and execution used the host's
`cpu-heavy` resource gate and a reserved build directory, with at most two
concurrent compiler processes. The test programs ran sequentially.

Results
-------

| Suite | Result |
| --- | --- |
| `cola_index` | Passed ASan/UBSan |
| `mapped_cola` | Passed ASan/UBSan |
| `mapped_cola_builder` | Passed ASan/UBSan |
| `cola_terminal` | Passed ASan/UBSan |
| `cola_local_merge` | Passed ASan/UBSan |

The initial strict `cola_index` compile failed on a deprecation diagnostic in
libstdc++ 12's `std::stable_sort` implementation: its temporary-buffer helper
was marked deprecated. The independent oracle had already appended each
origin's entries in ordinal order. I made that tie break explicit and used
`std::sort` on key, origin, then ordinal, preserving its occurrence order.
The original compiler diagnostic is retained; no warning suppression was used.

The suites exercise owning and mapped dual-route indexes, exact targets,
metadata-only terminal construction, malformed sections, query results and
local merge stages. This check does not cover SQLite, the complete package or
Doxygen build, every Everett component, or every x86 SIMD target.

Reproduction and evidence
-------------------------

The original and corrected source manifests, exact compiler
commands, toolchain output, executable hashes and test output are retained in
[original results](results/cola_linux/results.json) and
[corrected results](results/cola_linux/results-patched.json). I verified every
remote source hash against the pinned Git objects; only `tests/cola_index.cc`
differs between the manifests. The original
[compiler diagnostic](results/cola_linux/cola_index.compile.txt) is also retained.
The remote temporary directory was `/tmp/everett-cola-linux-YR37tcNZ`. The host
check did not edit a checkout; its resource gate wrote the normal run logs.

Each compiler invocation uses this form, with the full argument arrays recorded
in the JSON evidence:

```sh
/usr/bin/clang++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  -I/tmp/everett-cola-linux-YR37tcNZ/include \
  /tmp/everett-cola-linux-YR37tcNZ/tests/cola_index.cc \
  -o /tmp/everett-cola-linux-YR37tcNZ/build/cola_index
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/everett-cola-linux-YR37tcNZ/build/cola_index
```

The host lease wrapped compilation and execution:

```sh
python3 /home/ekmett/cult/tools/resource_run.py --resource cpu-heavy \
  --build-dir /tmp/everett-cola-linux-YR37tcNZ/build -- python3 -
```

The two host logs were
`20260915-164154-psa10pzj/output.log` and
`20260915-164356-n87dlnhm/output.log` under
`/home/ekmett/cult/build-agent-logs/`. Test runtimes in the raw records are
verification metadata and are not comparisons with another implementation.
