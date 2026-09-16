Generated Space Tables
======================

Complete native plus index files at 131,072 logical rows. Positive deltas mean byte files are larger.

Typed
-----

| Fixture | Byte `.kv` | Bit `.kv` | Byte `.index` | Bit `.index` | Complete byte delta | Bytes/row delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| integer-ordered-fixed | 1,452,968 | 2,130,608 | 158,520 | 152,808 | -29.43% | -5.126 |
| integer-ordered-variable | 34,511,744 | 34,880,176 | 158,520 | 152,808 | -1.04% | -2.767 |
| integer-random-fixed | 2,215,504 | 2,130,608 | 213,984 | 218,024 | +3.44% | +0.617 |
| integer-random-variable | 35,272,512 | 34,880,168 | 213,984 | 218,024 | +1.11% | +2.963 |
| string-binary-fixed | 4,576,960 | 4,565,136 | 288,976 | 297,552 | +0.07% | +0.025 |
| string-binary-variable | 36,127,816 | 36,117,520 | 288,976 | 297,552 | +0.00% | +0.013 |
| string-prefix-fixed | 2,809,048 | 2,534,592 | 171,088 | 165,288 | +10.38% | +2.138 |
| string-prefix-variable | 34,360,632 | 34,090,368 | 171,088 | 165,288 | +0.81% | +2.106 |
| string-structured-fixed | 3,595,864 | 3,511,368 | 227,376 | 230,608 | +2.17% | +0.620 |
| string-structured-variable | 35,147,120 | 35,064,712 | 227,376 | 230,608 | +0.22% | +0.604 |

Typed-known
-----------

| Fixture | Byte `.kv` | Bit `.kv` | Byte `.index` | Bit `.index` | Complete byte delta | Bytes/row delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| string-binary-fixed | 4,444,752 | 4,565,136 | 288,976 | 297,552 | -2.65% | -0.984 |
| string-binary-variable | 36,127,816 | 36,117,520 | 288,976 | 297,552 | +0.00% | +0.013 |
| string-prefix-fixed | 2,675,024 | 2,534,592 | 171,088 | 165,288 | +5.42% | +1.116 |
| string-prefix-variable | 34,360,632 | 34,090,368 | 171,088 | 165,288 | +0.81% | +2.106 |
| string-structured-fixed | 3,463,136 | 3,511,368 | 227,376 | 230,608 | -1.38% | -0.393 |
| string-structured-variable | 35,147,120 | 35,064,712 | 227,376 | 230,608 | +0.22% | +0.604 |

Raw
---

| Fixture | Byte `.kv` | Bit `.kv` | Byte `.index` | Bit `.index` | Complete byte delta | Bytes/row delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| integer-ordered-fixed | 1,452,968 | 1,271,856 | 158,520 | 152,808 | +13.11% | +1.425 |
| integer-ordered-variable | 34,187,048 | 34,121,560 | 158,520 | 152,808 | +0.21% | +0.543 |
| integer-random-fixed | 2,215,496 | 2,238,824 | 213,984 | 218,024 | -1.11% | -0.209 |
| integer-random-variable | 34,947,816 | 35,085,824 | 213,984 | 218,024 | -0.40% | -1.084 |
| string-binary-fixed | 4,313,672 | 4,400,392 | 288,976 | 297,552 | -2.03% | -0.727 |
| string-binary-variable | 35,996,448 | 36,197,792 | 288,976 | 297,552 | -0.58% | -1.602 |
| string-prefix-fixed | 2,543,952 | 2,369,768 | 171,080 | 165,288 | +7.10% | +1.373 |
| string-prefix-variable | 34,229,272 | 34,170,552 | 171,080 | 165,288 | +0.19% | +0.492 |
| string-structured-fixed | 3,332,064 | 3,346,520 | 227,368 | 230,608 | -0.49% | -0.135 |
| string-structured-variable | 35,015,744 | 35,144,896 | 227,368 | 230,608 | -0.37% | -1.010 |

Typed Size Sweep
----------------

| Fixture | 1,024 | 8,192 | 32,768 | 131,072 |
| --- | ---: | ---: | ---: | ---: |
| integer-ordered-fixed | -25.51% | -28.79% | -29.31% | -29.43% |
| integer-ordered-variable | -1.14% | -1.05% | -1.04% | -1.04% |
| integer-random-fixed | +5.08% | +4.71% | +4.22% | +3.44% |
| integer-random-variable | +1.28% | +1.20% | +1.16% | +1.11% |
| string-binary-fixed | -0.84% | -0.56% | -0.20% | +0.07% |
| string-binary-variable | -0.13% | -0.08% | -0.03% | +0.00% |
| string-prefix-fixed | +7.43% | +9.93% | +10.29% | +10.38% |
| string-prefix-variable | +0.68% | +0.79% | +0.80% | +0.81% |
| string-structured-fixed | +0.94% | +2.01% | +2.14% | +2.17% |
| string-structured-variable | +0.10% | +0.21% | +0.22% | +0.22% |
