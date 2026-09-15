Cold exception paths in complete queries
=======================================

On the M2 Max, moving exception construction into a cold, non-inlined helper
reduced median complete-query time by **7.25–14.56%** in these six cases. Every
check still runs, and failures still throw the same exception type and message.
An experimental aborting helper gave mixed additional gains; these results
support outlining exceptions without changing the failure contract.

What was measured
-----------------

The runner snapshots revision `fea6afc08668032e5230b8d982dd252c58a2449c`
three times and compiles the same [query harness](query_compare.cc) against each:

* **Baseline:** the original headers.
* **Outlined:** 166 throw expressions in the ten throwing headers reachable
  from `query.h` call a `[[noreturn, gnu::cold, gnu::noinline]]` helper, which
  constructs and throws the original exception.
* **Fail-stop:** the same checks call a cold helper that aborts. This is only a
  success-path experiment; its failure behavior is deliberately different.

Conditions, catch handlers, rethrows, allocations and encoded data remain
unchanged. None of these builds uses `-fno-exceptions`. The timed queries do not
fail, so the comparison measures generated code and layout on the successful
path, not the time taken to unwind an exception.

Each fixture has four native levels, containing $N$, $N/4$, $N/16$ and $N/64$
records, and a prepared routing prefix. Each trial runs 4,096 deterministic
queries using byte and bit policies. Half target known base-level keys; the
remainder cover the key domain. An independent integer-key oracle checks every
returned source, ordinal and value before and after timing. All value bytes
contribute to the timed checksum. Match counts, catalog visits, preparation
shape and encoded array sizes agree across variants.

Five trials rotate and reverse variant order after warming the fixture. Builds
use Apple Clang 21, C++20, `-O3 -DNDEBUG` on the native M2 Max. The harness requests
user-initiated QoS. The host resource lease excludes other participating heavy
jobs; cores and clock frequencies are not pinned. These are resident owning
queries, not mmap fault, disk-I/O or native x86 measurements.

Results
-------

Times are median nanoseconds per complete query; lower is better.

| Base records | Prefix bytes | Policy | Baseline | Outlined | Change | Fail-stop |
| ---: | ---: | :--- | ---: | ---: | ---: | ---: |
| 4,096 | 0 | byte | 3,135.40 | 2,678.94 | −14.56% | 2,692.67 |
| 4,096 | 0 | bit | 4,527.50 | 4,124.71 | −8.90% | 4,095.40 |
| 4,096 | 64 | byte | 3,147.46 | 2,710.27 | −13.89% | 2,680.74 |
| 4,096 | 64 | bit | 4,695.27 | 4,355.05 | −7.25% | 4,199.84 |
| 65,536 | 64 | byte | 3,802.66 | 3,376.45 | −11.21% | 3,236.99 |
| 65,536 | 64 | bit | 5,664.42 | 5,129.51 | −9.44% | 5,166.07 |

The baseline and outlined trial ranges do not overlap in any case. Fail-stop
ranges overlap the outlined ranges in several cases and its median ranges from
4.13% faster to 0.71% slower than outlined. That does not justify replacing
recoverable errors with process termination.

Whole-executable Mach-O sizes are recorded below. They include construction,
validation and harness code as well as the timed query loop.

| Bytes | Baseline | Outlined | Fail-stop |
| :--- | ---: | ---: | ---: |
| `__text` | 181,812 | 165,764 | 163,092 |
| `__gcc_except_tab` | 9,808 | 5,312 | 4,464 |
| `__unwind_info` | 2,368 | 1,600 | 1,528 |
| Entire executable file | 382,448 | 396,144 | 445,360 |

All three have no `__eh_frame` section. The executable files grow because
`__LINKEDIT` grows, even though their code and exception metadata sections
shrink. Section sizes alone do not isolate which code-generation change causes
the timing improvement.

Validation and implementation scope
-----------------------------------

All **90 release rows** and **18 ASan/UBSan rows** pass the full-result oracle.
The sanitizer run uses smaller fixtures and is not timing evidence. Separate
sanitized probes compare dynamic exception types, exact messages, and malformed
count decoder offsets for baseline, outlined, and the first production patch.
The production checkpoint passes the profile, groups, query and separate
installed-package CTest checks under ASan/UBSan: **4/4**.
The production helper also has a guarded MSVC `__declspec(noinline)` spelling;
that compiler path has not been run here.

The initial production checkpoint `dbc56d8a37db194884457cb09adcf8a22034bc8a`
applies the helper to 135 sites outside the rank and select headers, which are
being changed independently. It preserves the later comparison-state code in
its `13c914d` base. The table above measures the exact 166-site transformation
of `fea6afc`; it does not establish a separate gain for that partial checkpoint,
subsequent Elias–Fano changes, native writers or disk readers.

Reproduction and artifacts
--------------------------

The measured runner is committed at `de7389136c7136e28b265b563ca1ee5fa590da06`.
The later runner revision checks unsupported throw syntax even in files without
a recognized throw expression; this does not alter the pinned experiment.
The JSON records the measured runner, harness, every header and executable hash,
compiler command, process order and full `size -m` output. The two patches
preserve the exact generated variants.

```sh
python3 bench/query_errors.py --build-dir build-query-errors/release
python3 bench/query_errors.py --build-dir build-query-errors/sanitize \
  --sanitize --records 128 --larger-records 1024 --queries 128 --trials 1
```

Run each command under the host's CPU/build-directory lease when configured.

* [Release rows](results/query_errors_m2max.csv) and [metadata](results/query_errors_m2max.json).
* [Sanitizer rows](results/query_errors_sanitizer.csv) and [metadata](results/query_errors_sanitizer.json).
* [Outlined patch](results/query_errors_outlined.patch) and [fail-stop patch](results/query_errors_fail_stop.patch).
* [Error probes](results/query_errors_probes.cc.txt), [check driver](results/query_errors_checks.py.txt),
  and [check commands, hashes and CTest log](results/query_errors_checks.json).
