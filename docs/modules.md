# C++26 modules

I use named modules for the public C++ interface. The package supplies compiled
objects and module source files; CMake rebuilds the compiler-specific module
interfaces for each consumer. Compiler, standard library, runtime and exception
settings must agree with the installed `simd` package.

| Import | CMake target | Purpose |
| --- | --- | --- |
| `everett` | `everett::everett` | Storage, codecs, snapshots and low-level operations |
| `everett.neon` | `everett::neon` | The common API and `neon_policy<>` |
| `everett.avx2` | `everett::avx2` | The common API and `avx2_policy<>` |
| `everett.avx512` | `everett::avx512` | The common API and `avx512_policy<>` |
| `everett.sqlite` | `everett::sqlite` | Durable catalogs, named connections and transactions |

Native modules are built only for the configured profiles. SQLite is optional
and does not become a dependency of the core module. Import both a native module
and `everett.sqlite` to use that profile with durable named tables.

## Build and consume

The qualified module toolchain follows `simd`: upstream Clang 23, CMake 4.4
and Ninja. The local module and installed-consumer checks use Clang 23.1.1,
CMake 4.4.3 and Ninja 1.13.2. The compiler must implement structured-binding
packs and explicit-object named properties; merely accepting `-std=c++26` is
insufficient. On Windows the SIMD compiler configuration uses `clang-cl`;
Everett's durable file operations currently require POSIX.

Build [simd](https://github.com/ekmett/simd) with exceptions enabled, because
Everett reports failed operations through exceptions. For ARM NEON:

```sh
cmake -S /path/to/simd-source -B build-simd -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/simd \
  -DSIMD_PROFILES=NEON -DSIMD_ENABLE_EXCEPTIONS=ON \
  -DSIMD_BUILD_TESTS=OFF
cmake --build build-simd --parallel 4
cmake --install build-simd
```

On x86 choose `SIMD_PROFILES=AVX2`, or `AVX2;AVX512` when supplying both native
modules. Everett builds the profiles available from that package; set
`EVERETT_PROFILES` to a subset when needed. The baseline module remains available.

On macOS I pair upstream Clang with Apple's SDK C++ headers and system runtime.
Use the following options consistently when configuring SIMD, Everett and its
consumers:

```sh
everett_sdk="$(xcrun --sdk macosx --show-sdk-path)"
# Add these to each cmake configure command:
# -DCMAKE_OSX_SYSROOT="$everett_sdk"
# -DCMAKE_CXX_FLAGS="-nostdinc++ -isystem $everett_sdk/usr/include/c++/v1"
```

This prevents upstream libc++ headers from requiring symbols absent from the
system runtime. Use a separate build directory for a different compiler or SDK.
Then configure Everett:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/path/to/simd -DCMAKE_BUILD_TYPE=Release \
  -DEVERETT_ENABLE_SQLITE=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/everett
```

An installed consumer uses the same toolchain:

```cmake
cmake_minimum_required(VERSION 4.4)
project(example LANGUAGES CXX)
find_package(everett CONFIG REQUIRED COMPONENTS sqlite neon)
add_executable(example example.cc)
target_link_libraries(example PRIVATE everett::neon everett::sqlite)
simd_target_profile(example NEON)
```

Both installed packages must be discoverable when configuring that consumer:

```sh
cmake -S /path/to/example -B build-example -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="/path/to/everett;/path/to/simd"
cmake --build build-example --parallel 4
```

Apply the same SDK options or toolchain file used for the libraries. Requesting
`neon` in `find_package` checks that the installed Everett package contains that
profile. Use the `avx2` or `avx512` component for the corresponding x86 example.

```cpp
#include <optional>
#include <string>
import everett.neon;
import everett.sqlite;

int main() {
  auto storage = everett::multiverse<everett::neon_policy<>>::create("data");
  auto db = storage.connect("earth-616");
  db.put("name", "Everett");
  return db.get("name") != "Everett";
}
```

The source headers under `include/everett` are implementation inputs for module
construction and focused internal tests. Applications import the modules.
Include standard-library headers for the standard types and free functions used
in application code; importing Everett does not re-export the standard library.
Attribute macros require a textual `#include <simd/attributes.h>` because
imports do not carry macros.

## Explicit execution profiles

The default `storage_policy<>` uses `simd::scalar`. Its type does not change
with compiler ISA flags or the profiles another translation unit imports.
`backend_policy<Arch, Registry, GroupSize, BackspaceCode, CodecBlockSize>` chooses
an architecture explicitly. Each native module offers the corresponding short
alias, preserving the remaining defaults:

```cpp
import everett.neon;
import everett.sqlite;

using policy = everett::neon_policy<>;
using storage = everett::multiverse<policy>;
```

```cmake
target_link_libraries(example PRIVATE everett::neon everett::sqlite)
simd_target_profile(example NEON)
```

The architecture controls kernels, not serialized bytes or schema identity.
Rank and Elias–Fano representations retain common types; their hot operations
accept an architecture template argument. The policy passes its architecture
to the operations used during table construction and lookup.

CRC kernels are compiled into the matching archive. `crc32c<Arch>` selects that
entry point; `crc32c(bytes)` remains the baseline function. Native file writers
and recovery scans select through their policy. Generated third-party kernels
stay outside consumer module interfaces.

ISA flags apply only to the selected target. A baseline dispatcher can link a
native archive without importing its module or gaining its ISA flags. On x86,
admit both CPU features and OS vector-state support before calling native code.
`AVX2` requires AVX2, FMA and BMI2. `AVX512` additionally requires AVX-512 F,
DQ, BW and VL. `NEON` targets AArch64. Linking a profile is not a runtime
feature check, and the profile tag does not add compiler flags by itself.
Separate baseline and native translation units preserve that boundary; disable
cross-boundary IPO on the dispatcher when that boundary matters.

Everett imports granular `simd` modules. It does not import the dependency's
combined omnibus: a package containing AVX2 and AVX-512 would otherwise require
AVX-512 even for a consumer using only scalar or AVX2 operations.

## Optional experiments

Standalone CMake experiments use the same compiler, Ninja generator and installed
SIMD dependency as the library. For example, a new space-only run from the current
source uses:

```sh
cmake -S optional/profile_space_compare -B build-space -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=/path/to/simd \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-space --parallel 1
ctest --test-dir build-space --output-on-failure
```

The experiment normally builds the compiled Everett core from this checkout.
`EVERETT_EXPERIMENT_INSTALLED=ON` selects an installed package instead; make both
package prefixes discoverable in that case. Objective-C++ Metal hosts use the
same compiler and standard-library flags as C++.

Retained measurements identify their original source and toolchain. Use those
revisions, including their build files, when reproducing a historical run.
Building an old header tree with the current compiled package is not the same
measurement configuration. A run against the current source needs its own
source hashes, correctness checks and observations.

## Definition ownership and checks

System headers and textual implementation definitions live in the global module
fragment. Explicit export declarations expose those same entities from each
module, so importing a profile does not create a second rank, offset or policy
type. Native modules re-export the common module and their matching SIMD module.
SQLite exports its additional API separately.

Package tests rebuild installed interfaces from a relocated prefix, exercise a
separate embedded consumer, and link multiple importers to the compiled archive.
Native consumers check cross-translation-unit type identity and byte-identical
serialization against the scalar policy. The SQLite consumer checks durable
writes, snapshots, saves, forks and transactions; a separate core consumer
disables SQLite discovery.
