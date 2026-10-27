# Third-party components

The MIT license in `LICENSE` covers this project's own code (`native/main.cpp`,
`java/`, `module/`, `build.sh`). The module zip also contains:

| Component | Files | License |
|---|---|---|
| [LSPlant](https://github.com/LSPosed/LSPlant) 6.4 (`lsplant-standalone`, unmodified) | `native/third_party/liblsplant.so`, `lsplant.hpp`; embedded in `zygisk/arm64-v8a.so` | LGPL-3.0 — `native/third_party/LICENSE.lsplant` (supplements the GPL-3.0, `LICENSE.GPL-3.0`) |
| [And64InlineHook](https://github.com/Rprop/And64InlineHook) | `native/third_party/And64InlineHook.{cpp,hpp}` | MIT, Copyright (c) 2018 Rprop — `native/third_party/LICENSE.And64InlineHook` |
| [Magisk](https://github.com/topjohnwu/Magisk) Zygisk API header | `native/third_party/zygisk.hpp` | ISC-style, Copyright (c) 2022-2023 John "topjohnwu" Wu (header of the file) |
| Magisk module installer | `META-INF/com/google/android/update-binary` in the zip (fetched at build time) | from the Magisk repository |

LSPlant is used unmodified, as published on Maven Central (`org.lsposed.lsplant:lsplant-standalone:6.4`).
To use a different build, replace `native/third_party/liblsplant.so` and run `./build.sh`; the library
is embedded as-is and loaded at runtime.
