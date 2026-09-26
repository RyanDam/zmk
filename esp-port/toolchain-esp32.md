# ESP32 toolchain install (Option A — Espressif cross compilers)

Phase 0 prerequisite: the ZMK ARM dev image
(`docker.io/zmkfirmware/zmk-dev-arm:4.1-branch`) ships only the **ARM**
Zephyr SDK (`/opt/zephyr-sdk-0.16.9`, `arm-zephyr-eabi`). Building the
ESP32-C3 / ESP32-S3 ports needs Espressif's RISC-V and Xtensa cross
compilers, which are **not** in that image. This document records how they
were installed (Option A from the Phase 0 plan) so the install is
reproducible on any host.

Installed: 2026-09-25.

## Why the Espressif toolchain

Upstream Zephyr's ESP32 port is built with Espressif's cross compilers,
selected through Zephyr's `espressif` toolchain variant
(`zephyr/cmake/toolchain/espressif/`). The variant is picked with two
settings:

- `ZEPHYR_TOOLCHAIN_VARIANT=espressif`
- `ESPRESSIF_TOOLCHAIN_PATH=<dir>` — a directory that directly contains the
  toolchain sub-directories.

`target.cmake` in that variant hard-codes the **exact** toolchain names per
SoC (`CROSS_COMPILE_TARGET_<arch>_<soc-series>`):

| SoC | `CROSS_COMPILE_TARGET` | required dir / binary |
| --- | --- | --- |
| ESP32-C3 (RISC-V) | `riscv32-esp-elf` | `riscv32-esp-elf/bin/riscv32-esp-elf-gcc` |
| ESP32-S3 (Xtensa) | `xtensa-esp32s3-elf` | `xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-gcc` |

So the install must provide those exact directory and binary names.

### Why version 12.2.0_20230208

Espressif's newer toolchain releases (e.g. `esp-13.2.0_20240530`) ship a
**unified** `xtensa-esp-elf` toolchain whose binaries are named
`xtensa-esp-elf-gcc` — which does **not** match the `xtensa-esp32s3-elf-gcc`
name Zephyr 4.1's `target.cmake` expects. The last release that still ships
the **separate** `xtensa-esp32s3-elf` toolchain is
`esp-12.2.0_20230208` (the toolchain set from ESP-IDF v5.1). We use the
matched 12.2.0 pair for both architectures:

- `riscv32-esp-elf-12.2.0_20230208` (GCC 12.2.0) — ESP32-C3
- `xtensa-esp32s3-elf-12.2.0_20230208` (GCC 12.2.0) — ESP32-S3

Both are verified to build the Zephyr 4.1 `hello_world` sample against the
pinned `hal_espressif` (`202c59552`, 2025-02-24).

## Install layout

The tarballs extract to unversioned top-level directories that already match
the layout Zephyr expects, so they are unpacked directly into
`ESPRESSIF_TOOLCHAIN_PATH`:

```
/opt/espressif/tools/
├── riscv32-esp-elf/
│   ├── bin/
│   │   ├── riscv32-esp-elf-gcc
│   │   ├── riscv32-esp-elf-g++
│   │   ├── riscv32-esp-elf-ld
│   │   └── ...
│   └── riscv32-esp-elf/          # sysroot
└── xtensa-esp32s3-elf/
    ├── bin/
    │   ├── xtensa-esp32s3-elf-gcc
    │   ├── xtensa-esp32s3-elf-g++
    │   ├── xtensa-esp32s3-elf-ld
    │   └── ...
    └── xtensa-esp32s3-elf/       # sysroot
```

`ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools`.

## Reproducible install steps (aarch64 host)

The host here is **aarch64** (ARM64), so the `-aarch64-linux-gnu` artifacts
are used. On an x86_64 host, substitute `-x86_64-linux-gnu` in the file
names and use the x86_64 checksums below.

```sh
set -euo pipefail
TC=/opt/espressif/tools
VER=12.2.0_20230208
BASE="https://github.com/espressif/crosstool-NG/releases/download/esp-${VER}"
mkdir -p "$TC"

# 1. Download (aarch64).
curl -fL -o riscv32-esp-elf.tar.xz     "$BASE/riscv32-esp-elf-${VER}-aarch64-linux-gnu.tar.xz"
curl -fL -o xtensa-esp32s3-elf.tar.xz  "$BASE/xtensa-esp32s3-elf-${VER}-aarch64-linux-gnu.tar.xz"

# 2. Verify checksums (aarch64). Values are from ESP-IDF v5.1 tools/tools.json.
echo "aefbf1e6f2c91a10e8995399d2003502e167e8c95e77f40957309e843700906a  riscv32-esp-elf.tar.xz"     | sha256sum -c -
echo "30a1fed3ab6341feb1ae986ee55f227df6a594293ced13c65a0136eb4681087d  xtensa-esp32s3-elf.tar.xz"  | sha256sum -c -

# 3. Extract (top-level dirs are already the names Zephyr expects).
tar -xJf riscv32-esp-elf.tar.xz     -C "$TC"
tar -xJf xtensa-esp32s3-elf.tar.xz  -C "$TC"

# 4. Sanity-check the compilers.
"$TC/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"       --version
"$TC/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-gcc" --version
```

x86_64 checksums (same manifest), for reference:

| file | sha256 (x86_64) |
| --- | --- |
| `riscv32-esp-elf-12.2.0_20230208-x86_64-linux-gnu.tar.xz` | `21694e5ee506f5e52908b12c6b5be7044d87cf34bb4dfcd151d0a10ea09dedc1` |
| `xtensa-esp32s3-elf-12.2.0_20230208-x86_64-linux-gnu.tar.xz` | `29b5ea6b30d98231f0c17f2327404109e0abf59b48d0f2890d9d9899678a89a3` |

## Using the toolchain

The easiest way is the wrapper script `esp-port/build_esp32.sh` (modelled on
`app/build_coban.sh`). It sets the toolchain env vars for you, builds with a
clean (`-p always`) build dir so a stale cache can't pin the wrong variant,
and works from anywhere in the repo:

```sh
bash esp-port/build_esp32.sh            # build C3 + S3
bash esp-port/build_esp32.sh c3         # build C3 only
bash esp-port/build_esp32.sh s3         # build S3 only
bash esp-port/build_esp32.sh c3 flash   # build C3, then west flash it
```

To build manually, set the two variables for the build. **Do not export them
globally** — the ARM ZMK boards (cobanpad16a, etc.) still build with the
Zephyr SDK ARM toolchain, and a global `ZEPHYR_TOOLCHAIN_VARIANT=espressif`
would break them. Set them per ESP32 build (inline, or via a wrapper).

From the workspace root (`/workspaces/zmk`):

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=espressif
export ESPRESSIF_TOOLCHAIN_PATH=/opt/espressif/tools

# ESP32-C3
west build -b esp32c3_devkitc/esp32c3 -d build/c3-hello zephyr/samples/hello_world

# ESP32-S3 (dual-core: pick a core; procpu = primary core 0)
west build -b esp32s3_devkitc/esp32s3/procpu -d build/s3-hello zephyr/samples/hello_world
```

### Gotcha: a stale CMake cache pins the variant

`ZEPHYR_TOOLCHAIN_VARIANT` is read with precedence **cache > environment**
(see `zephyr_get()` in `zephyr/cmake/modules/extensions.cmake`). A build
directory that was first configured with the default (Zephyr SDK) toolchain
keeps `ZEPHYR_TOOLCHAIN_VARIANT:STRING=zephyr` in `CMakeCache.txt`, which
silently overrides the environment variable. If a build suddenly looks for
`riscv64-zephyr-elf-gcc` inside the Zephyr SDK, delete the build directory
(or pass `-DZEPHYR_TOOLCHAIN_VARIANT=espressif`) and rebuild:

```sh
rm -rf build/c3-hello   # then rebuild with the env vars set
```

## Verification (2026-09-25)

Both samples build and link with the installed toolchains against the pinned
`hal_espressif`:

| board | toolchain | result |
| --- | --- | --- |
| `esp32c3_devkitc/esp32c3` | riscv32-esp-elf 12.2.0 | OK — `zephyr.elf` + esptool image, FLASH 132484 B (3.16%) |
| `esp32s3_devkitc/esp32s3/procpu` | xtensa-esp32s3-elf 12.2.0 | OK — `zephyr.elf` + esptool image, FLASH 135284 B (1.61%) |

This unblocks Step 3 (flash/monitor) of Phase 0.
