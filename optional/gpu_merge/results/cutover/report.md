# Header-only GPU cutover calibration

Device: Apple M2 Max|registryID=4294968459; backend: metal.
Variant: `c7042a4fdd4d9207e106916f9fdc97fc0b058ba7fb89a2bc9a8ab4493a5f4653`.
CPU: Apple M2 Max; OS: macOS-26.6.2-arm64-arm-64bit-Mach-O; driver: OS-provided; see os_version.
The device token is observed for this run; stability across boots is not assumed.

Held-out validation: **passed**.

This is an empirical heuristic for this device, backend, and implementation. It is not a performance guarantee.
Input generation and one verified warm-up of each implementation are excluded. Both measured totals include mapped output construction and CRC, without durability fsync barriers.
The 34 training cases fit depth at most three; a GPU leaf needs two cases whose slowest GPU repetition is at least 10% faster than their fastest CPU repetition. The 16 held-out cases do not change the rule.
The observed training envelope is checked first. Unknown device/backend/variant, inputs outside it, and failed held-out validation select CPU.

## Frozen GPU regions

- records > 51200

## Held-out decisions

| Case | In envelope | Frozen rule GPU | CPU median ms | GPU median ms | Speedup | Robust 10% win |
|---:|:---:|:---:|---:|---:|---:|:---:|
| 34 | True | False | 1.17325 | 6.05233 | 0.194x | False |
| 35 | True | False | 0.707791 | 4.37592 | 0.162x | False |
| 36 | True | False | 1.15792 | 2.68408 | 0.431x | False |
| 37 | True | False | 0.587792 | 5.64288 | 0.104x | False |
| 38 | True | False | 3.98937 | 6.28821 | 0.634x | False |
| 39 | True | False | 2.69954 | 5.35517 | 0.504x | False |
| 40 | True | False | 4.32833 | 4.60542 | 0.940x | False |
| 41 | True | False | 1.91121 | 5.7725 | 0.331x | False |
| 42 | True | True | 15.6526 | 7.692 | 2.035x | True |
| 43 | True | False | 10.2419 | 8.94475 | 1.145x | False |
| 44 | True | False | 16.1906 | 8.99946 | 1.799x | True |
| 45 | True | False | 6.76908 | 7.18846 | 0.942x | False |
| 46 | True | True | 63.6656 | 11.001 | 5.787x | True |
| 47 | True | True | 39.7075 | 11.7701 | 3.374x | True |
| 48 | True | True | 61.8584 | 17.4174 | 3.552x | True |
| 49 | True | True | 26.9972 | 7.21408 | 3.742x | True |

Terminal key length is a header feature, not an assumed maximum over keys. The execution backend retains all framing, ordering, resource, and output checks.
