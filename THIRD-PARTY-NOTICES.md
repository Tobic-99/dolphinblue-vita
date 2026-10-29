# Third-party components

Linked into `eboot.bin` or packed in the VPK:

| Component | Licence | Source |
|---|---|---|
| vitaShaRK (Rinnegatamante) | LGPL-3.0 | https://github.com/Rinnegatamante/vitaShaRK |
| SceShaccCgExt (bythos14) | LGPL-2.1 | https://github.com/bythos14/SceShaccCgExt |
| libperf (bythos14): `libperf.suprx`, `libperf.skprx`, `libScePerf_stub_weak.a` | MIT, `recomp/vita/third_party/libperf/LICENSE` | https://github.com/bythos14/libperf |
| VitaSDK stubs (vita-headers, vita-toolchain) | MIT | https://github.com/vitasdk |
| newlib (C library) | BSD-style licences (COPYING.NEWLIB) | https://github.com/vitasdk/newlib |
| libstdc++, libgcc | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org |

vitaShaRK and SceShaccCgExt are linked statically. The complete source of this port and its build scripts are in
this repository, so the PS Vita build can be rebuilt and relinked with modified versions of those libraries
(`scripts/build-vita.sh`).

Not distributed: `libshacccg.suprx` (Sony).
