# AGENTS.md

ZMK keyboard firmware (Zephyr RTOS). This is a fork: `origin` = RyanDam/zmk, `upstream` = zmkfirmware/zmk. Workspace-level facts (two-repo layout, native macOS setup, ESP build scripts) are in the parent [AGENTS.md](../AGENTS.md).

## Workflow (user preferences)

- Always start work in the main workspace (the current checkout/folder), unless the user explicitly requests isolation — in that case, spawn a git worktree from the current branch.
- If the work is about a backlog item (see the workspace backlog
  [`../backlogs/`](../backlogs/)), always present a detailed implementation plan first and wait for the user's confirmation before implementing.
- If the user gives a topic or goal they want to achieve, also present a detailed implementation plan first and wait for confirmation.
- During implementation, do NOT commit or push anything. When the implementation is done, ask the user to review the changes.
- After review, the user may commit or discard (fully or partially) the changes themselves. Don't be surprised by that — just double-check with git history afterwards.

## Branch features (vs `main`)

Feature branches add things `main` does not have (LED indicators incl. WS2812, dynamic macros, MPR121 touchpad, Kailh Choc encoder driver, GPIO key driver, sensor keymaps, Studio RPC subsystems, ESP32 port, cobantab1a display drivers). Check `git branch --show-current` before assuming what exists. Full feature details: [FEATURES.MD](FEATURES.MD); ESP phase decisions and branch lineage: [esp-port/README.md](esp-port/README.md).

## West workspace layout

- `app/` is the west topdir (the only place you edit firmware code: `app/src`, `app/include`, `app/module`, `app/drivers`, `app/dts`).
- `zephyr/`, `modules/`, `tools/`, `optional/` are west-managed and gitignored — never edit; pins live in `app/west.yml`. This fork's manifest pulls `zmk-studio-messages` from the `cobanfirmware` (RyanDam) remote.
- `build/` (repo root) and `app/build/` are build artifacts, gitignored.

## Building the Coban keyboards

The working keyboards are the shields `cobanpad12b` and `cobanpad16a` (plus `cobantab1a`), defined in the **external** config repo — `../zmk-config/boards/shields/` (the scripts resolve it via the `ZMK_CONFIG` env var, defaulting to the sibling folder).

Build from the repo root (scripts use `-s app`, so the build dir lands at the workspace `build/`):

```sh
bash app/build_coban.sh        # -> ../zmk-config/zmk-12b.uf2 + zmk-16a.uf2
sh esp-port/build_cobantab1a.sh  # ESP32-S3, -> build/cobantab1a/zephyr/zephyr.bin
```

Underlying command pattern (board is `nice_nano@1`):

```sh
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -s app \
  -- -DZMK_CONFIG="../zmk-config" -DSHIELD=cobanpad16a
```

`-DZMK_CONFIG` is required — it points west at the external config repo containing the coban shields. ESP builds must use `-p always` so a stale CMake cache can't pin the wrong toolchain variant (Zephyr 4.4 removed the legacy `espressif` variant; use `cross-compile` + `ESPRESSIF_TOOLCHAIN_PATH` — see `esp-port/toolchain-esp32.md`).

Generic upstream build (for other boards/shields): `cd app && west build -b <board> -- -DSHIELD=<shield>`.

## Tests

Run from `app/`:

```sh
west test tests/<name>      # custom west command, wraps ./run-test.sh
./run-test.sh tests/<name>  # same thing directly
./run-test.sh all           # whole suite
```

- Tests build for `native_sim` and diff output against `keycode_events.snapshot` in the test dir.
- Set `ZMK_TESTS_AUTO_ACCEPT=1` to overwrite a stale snapshot.
- A test dir containing a `pending` file is treated as PENDING, not FAILED.
- `native_sim` tests need the `x86_64-zephyr-elf` SDK toolchain (the default setup only installs `arm-zephyr-eabi`).
- CI runs in container `docker.io/zmkfirmware/zmk-build-arm:4.4-branch` after `west init -l app && west update && west zephyr-export`. Product shields (cobanpad*, cobantab1a) have no CI entries because CI can't access the external zmk-config repo.

## Conventions

- Pre-commit enforces: clang-format v18.1.8 (C/C++), prettier, no tabs, trailing whitespace, gitlint commit messages.
- Keymaps are `.keymap` files (one per shield in zmk-config); behaviors are Zephyr devicetree nodes — see `docs/docs/development/` before adding hardware support.
