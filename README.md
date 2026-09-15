# Everett

Persistent storage through composable change.

Everett is a C++20 library under development for immutable blobs, shared
snapshots, branching histories and partitioned updates. Its design combines
COLA-style dynamization, compressed fractional indexes, front-coded keys,
incremental rebuilding and additive state fingerprints.

The current implementation provides byte and bit codecs, mapped object readers,
an in-memory reference world and a durability protocol model. SQLite is the
selected metadata backend for worlds, pins and merge progress. Its integration
and the incremental scheduler remain implementation work.

## Design vocabulary

| Name | Meaning |
| --- | --- |
| `multiverse` | The backing store: immutable objects, retained roots, shared merge work and reclamation. |
| `world` | A logical state, independent of the physical merge pattern representing it. |
| `timeline` | An ordered progression of changes, with a world as its current head. |
| `branch_point` | A retained point from which another timeline can begin. |

`multiverse<P>` currently implements the read side of an existing object
directory and exposes its policy-bound type family. Its `world`, `timeline` and
`branch_point` aliases are forward declarations; the persistent aggregate runtime
is still to be built. `reference_world` is the executable semantic oracle.

A logical key is a pair `(sort, local_key)`. Prefix-free sort and local-key
encodings concatenate unambiguously. The multiverse policy fixes byte or bit
units, value layout and sampling group size; associated sorts carry the same
policy. Hash/category selection may depend on the full key. The
[sort contracts](docs/keys.md) distinguish this intended registry from the
implemented codecs and policy-bound sort descriptors.

## Typed codecs and immutable objects

```cpp
#include <everett/multiverse.h>

using policy = everett::storage_policy<
  everett::profile_unit::bit, everett::fixed_values<3>, 7>;
using store = everett::multiverse<policy>;
using sort = store::sort; // everett::sort<policy>
using blob = store::blob; // everett::profile_blob<policy>
```

Both profiles implement native LPFC and separately rebuilt fractional-index
front coding. Fixed widths count policy units; `variable_values` permits varying
widths. Group sizes 3, 7, 15 and 31 are tested, with 15 the default.

Our custom files are `.kv` and `.index`, named
`ab/cd/<remaining-object-id>.<extension>`. World manifests and merge continuations
belong in SQLite. Network admission is intended to retain received `.kv` bytes
unchanged and build local fractional-index links, then perform scheduled merges.
Content-address calculation, durable admission and encoded section serialization
are still pending. The present mapped envelope reader verifies the whole body
on open; it does not yet provide lazy block-integrity checking.

## Build and test

Requires CMake 3.20+ and a C++20 compiler. The headers depend only on the standard
library.

```sh
cmake -S . -B build -DEVERETT_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

For AddressSanitizer and UndefinedBehaviorSanitizer checks on supported compilers:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DEVERETT_BUILD_TESTS=ON -DEVERETT_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

## API documentation

With Doxygen and Python 3 installed:

```sh
cmake -S . -B build-docs -DEVERETT_BUILD_DOCS=ON
cmake --build build-docs --target everett_docs
ctest --test-dir build-docs -R '^everett.doxygen$' --output-on-failure
```

The optional target generates HTML/XML and checks SPDX headers, file-footer attachment,
namespace/class ownership, overloads and source locations. It uses the original
`ein` license aliases. See [Doxygen conventions and verification](docs/doxygen.md).

## Use from CMake

Install the package:

```sh
cmake --install build --prefix /path/to/everett-install
```

Then configure your consumer with that prefix in `CMAKE_PREFIX_PATH`:

```cmake
find_package(everett CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE everett::everett)
```

An embedded checkout also supports `add_subdirectory(path/to/everett)` and the
same target. Tests default off when embedded. Version 0.1.0 has an experimental
API and does not promise a stable persisted blob format.

## Reference example

This example uses the existing replacement-valued, byte-keyed oracle:

```cpp
#include <everett/world.h>

int main() {
  auto base = everett::reference_world<>::from_records({
    {"alpha", 10}, {"beta", 20}
  });
  auto partition = [](std::string_view key) -> std::uint64_t {
    return key == "alpha" ? 0 : 1;
  };
  everett::partition_round round(base, "round-1", partition);
  auto a = round.make_batch("a", 0, {{"alpha", 12}});
  auto b = round.make_batch("b", 1, {{"beta", 25}});
  round.apply(b);
  round.apply(a);
  auto next = round.snapshot();
  return base.get("alpha") == 10 && next.get("alpha") == 12 ? 0 : 1;
}
```

Both batches use the same pinned base and modify disjoint keys. Their admission
order does not change the resulting state or its fingerprint. Earlier worlds
remain readable. The reference `save` operation exports a resolved table;
durable manifests of pinned objects are part of the planned backing store.

## Documentation

- [Storage design](docs/design.md): blobs, cascading, snapshots and shared merges.
- [Sorts and keys](docs/keys.md): prefix-free framing, key units and hash policies.
- [Sampling](docs/sampling.md): why 3 is sound, and the space/scan tradeoff.
- [Network admission](docs/network-admission.md): direct blob reuse and its work bounds.
- [Categorical updates](docs/arrows.md): dependent per-key categories and composition.
- [Strong deletion](docs/rebuild.md): live-size rebuilding and bounded replay.
- [Durability](docs/durability.md): publication, failed barriers and resumable work.
- [File lifecycle](docs/file-lifecycle.md): immutable objects, mmap and publication ordering.
- [SQLite catalog](docs/catalog.md): worlds, exact pins and resumable work in ordinary tables.
- [Implementation status](docs/implementation.md): tested components and remaining work.
- [Contributor instructions](AGENTS.md): coding style and verification.
- [Doxygen verification](docs/doxygen.md): source-footer aliases and declaration ownership.

## License

Copyright 2026 Edward Kmett. Licensed under
[BSD-2-Clause](LICENSES/BSD-2-Clause.txt) **OR**
[Apache-2.0](LICENSES/Apache-2.0.txt), at your option. See [LICENSE](LICENSE).
Both complete license texts are included in source and installed packages.
