COLA comparison layout
======================

Aligning the out-of-line comparator removes the observed long-prefix regression
from reusing the carried LCP in the two-target COLA builder. With both versions
aligned, their observation ranges overlap in every fixture. I do not claim a
demonstrated build speedup from LCP reuse.

The production change gives `compare_common_bits` a 64-byte entry alignment on
Apple Clang/AArch64. It does not force the function out of line, change its
instructions or relax any check. Other compiler/target combinations retain the
existing declaration. Complete-query collateral measurements show overlapping
ranges, and the existing profile tests pass under strict O3 ASan/UBSan.

Whole-build control
-------------------

The four variants use the same [C++ fixture](cola_layout.cc):

* **B:** `111a2d6`, checked borrowed-writer append.
* **C:** `f12d708`, append using the already computed exact LCP.
* **B+A / C+A:** the corresponding headers with the alignment declaration.

Five fresh-process trials per variant, each with one warm-up and three timed
rounds, produce 480 observations. The timed node has 2,048 native records and
samples a main node containing 4,096 native records plus its own borrowed samples,
and a 4,096-record secondary leaf. The main node's two routes share one native
leaf. Keys have a fixed-width ordered integer suffix,
with either zero or 4,096 common prefix bytes; bit policies add three trailing
bits. Values are variable-width. The bit K=3 case uses Golomb-3 controls.

The timer includes builder construction, `step` and `finish`; serialization,
result/builder destruction and fixture construction are outside it. Every process
dumps its eight warm-up index files, which the runner compares byte-for-byte
across all variants and trials. Timed rounds serialize afterward and check CRC
and encoded size. These are exact-wire controls against the checked implementation,
not a new independent full-key oracle.

The M2 Max run uses Apple Clang 21.0.0, `-O3 -DNDEBUG -std=c++20`, under the shared
exclusive CPU lease. The runner records its rotating/reversed process order;
five trials are not a fully position-balanced schedule. Ranges below are observed
minima and maxima, not confidence intervals. All observations, including short
fixture outliers, are retained.

Nanoseconds per virtual occurrence, **median [minimum, maximum]**:

| Unit | K | Prefix bytes | B | C | B+A | C+A |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| byte | 3 | 0 | 148.0 [142.2, 329.6] | 142.9 [135.2, 344.0] | 146.9 [138.5, 226.7] | 141.6 [134.3, 283.6] |
| byte | 15 | 0 | 159.2 [153.3, 282.0] | 158.6 [147.2, 287.5] | 163.5 [151.9, 267.4] | 162.5 [155.2, 273.4] |
| bit | 3 | 0 | 244.4 [231.8, 362.3] | 249.1 [228.2, 386.6] | 247.1 [229.9, 309.0] | 238.1 [229.0, 368.6] |
| bit | 15 | 0 | 277.0 [261.1, 368.1] | 275.0 [257.7, 370.3] | 273.2 [261.5, 356.5] | 265.7 [251.9, 370.3] |
| byte | 3 | 4096 | 1048.2 [997.4, 1189.9] | 1348.7 [1328.4, 1463.8] | 1043.8 [1017.8, 1097.3] | 968.0 [927.6, 1080.6] |
| byte | 15 | 4096 | 1191.6 [1106.6, 1220.1] | 1593.7 [1512.7, 1697.0] | 1164.0 [1108.4, 1297.4] | 1130.3 [1076.9, 1232.7] |
| bit | 3 | 4096 | 1124.1 [1095.1, 1195.4] | 1424.9 [1398.5, 1487.2] | 1131.9 [1092.0, 1175.6] | 1074.3 [1027.6, 1104.8] |
| bit | 15 | 4096 | 1288.6 [1232.2, 1418.3] | 1692.6 [1641.5, 1813.3] | 1275.0 [1230.0, 1361.6] | 1244.3 [1198.2, 1301.6] |

Without alignment, C is 26.8–33.7% slower on long prefixes and all four range
pairs are disjoint. Alignment removes those regressions. C+A medians are
2.4–7.3% below B+A on long prefixes, but their ranges overlap; short-prefix
comparisons overlap too. The useful demonstrated result is removal of this
compiler-layout regression.

Instruction and size evidence
-----------------------------

The benchmark include closure contains only two changed headers between B and C:
the COLA append call and its borrowed-writer friend declaration. The revision also
changes `multiverse.h`, which this fixture does not include. `append_known` remains
out of line in both builds, and there is no new full-prefix copy.

All four emitted comparator bodies contain the same 209 instructions after
expressing branch addresses relative to the function entry. The normalized bodies
have the same SHA-256:
`c5f83f67e218e9f94b53e415234c3c8f8e8ad26e727d1ab01a3c56cf8560a346`.
The aligned bodies have another four unreachable padding instructions afterward.

| Variant | Function entry | NEON loop start | Loop start mod 64 |
| --- | --- | --- | ---: |
| B | `0x100009db0` | `0x100009fc0` | 0 |
| C | `0x100009dcc` | `0x100009fdc` | 28 |
| B+A | `0x100009e40` | `0x10000a050` | 16 |
| C+A | `0x100009e40` | `0x10000a050` | 16 |

The 40-byte NEON equality loop crosses a 64-byte boundary only in C. The identical
instructions, repeated regression and alignment intervention strongly support a
code-layout explanation. They do not isolate a particular cache or fetch mechanism;
alignment also shifts other code, and no hardware-counter experiment was performed.

Alignment has a code-size cost in this complete executable:

| Variant | `__text` bytes | `__TEXT` segment bytes | Exception table bytes | Unwind bytes |
| --- | ---: | ---: | ---: | ---: |
| B | 216,860 | 245,760 | 7,612 | 2,128 |
| C | 216,964 | 245,760 | 7,612 | 2,120 |
| B+A | 219,644 | 245,760 | 7,612 | 2,128 |
| C+A | 219,616 | 245,760 | 7,612 | 2,120 |

The 2.7 KiB increase includes padding beyond the comparator itself. The mapped
text segment size is unchanged. Other compilers, compiler versions, inlined copies
and caller mixes may lay out this code differently; this is a local measured
alignment choice, not a general instruction-cache bound.

Query collateral and correctness
--------------------------------

The unchanged [whole-query harness](query_compare.cc) compares `f12d708` against
the alignment-only commit `a4c823e`, with 4,096 records and 4,096 queries, five
alternating trials, and byte/bit K=W=15 policies. Every process checks exact
source, ordinal and value against its independent integer-key oracle before and
after timing. Cross-version checksums, catalog visits, matches and encoded-array
sizes agree.

Nanoseconds per complete query:

| Unit | Prefix bytes | Before, median [min, max] | Aligned, median [min, max] | Median change |
| --- | ---: | ---: | ---: | ---: |
| byte | 0 | 1869.3 [1827.0, 3026.9] | 1909.2 [1824.8, 2277.4] | +2.1% |
| bit | 0 | 2920.7 [2821.3, 3200.6] | 2870.1 [2801.1, 2963.3] | −1.7% |
| byte | 4096 | 2271.4 [2208.1, 2341.7] | 2218.2 [2195.9, 2267.0] | −2.3% |
| bit | 4096 | 3785.9 [3731.6, 3862.1] | 3850.7 [3758.9, 3914.8] | +1.7% |

All query ranges overlap. Preparation medians increase 3.3–28.5%, but those short
single preparations also have wide overlapping ranges; the raw data retains them.
There is no demonstrated query or preparation speedup. The existing profile suite
passes with `-O3 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`;
[validation metadata](results/cola_layout_validation.json) records the source,
header, compiler and command.

Reproduction and raw data
-------------------------

[The runner](cola_layout.py) snapshots fresh headers and records every header,
source, executable and warm-up file hash. The archived candidate patch reconstructs
all candidate header hashes exactly, so reproduction does not depend on retaining
the experimental Git branch:

```sh
python3 bench/cola_layout.py --baseline 111a2d6 \
  --candidate-patch bench/results/cola_layout_candidate.patch \
  --build-dir build-cola-layout --trials 5 --rounds 3
```

Run under the host's exclusive CPU/build-directory lease. The measured runner is
also preserved byte-for-byte with the hash recorded in its metadata; the reusable
runner additionally accepts the archived header patch.

* Final control: [480 CSV rows](results/cola_layout_m2max.csv),
  [metadata and commands](results/cola_layout_m2max.json),
  [exact measured runner](results/cola_layout_measured.py),
  [instruction excerpts and section sizes](results/cola_layout_codegen.json).
* Query collateral: [40 CSV rows](results/cola_layout_query_m2max.csv),
  [metadata and commands](results/cola_layout_query_m2max.json).
* Earlier measurements, retained separately rather than pooled into these tables:
  [original 240 rows](results/cola_layout_original_m2max.csv),
  [original metadata](results/cola_layout_original_m2max.json),
  [initial four-way 288 rows](results/cola_layout_initial_m2max.csv),
  [initial metadata](results/cola_layout_initial_m2max.json).
