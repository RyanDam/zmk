# AGENTS.md

ZMK keyboard firmware (Zephyr RTOS). This is a fork: `origin` = RyanDam/zmk, `upstream` = zmkfirmware/zmk.

## West workspace layout

- `app/` is the west topdir (the only place you edit firmware code: `app/src`, `app/include`, `app/module`, `app/drivers`, `app/dts`).
- `zephyr/`, `modules/`, `tools/`, `optional/` are west-managed and gitignored — never edit; pins live in `app/west.yml`. This fork's manifest pulls `zmk-studio-messages` from the `cobanfirmware` (RyanDam) remote.
- `build/` (repo root) and `app/build/` are build artifacts, gitignored.

## Building the Coban keyboards

The working keyboards are the shields `cobanpad12b` and `cobanpad16a`, defined in `/workspaces/zmk-config/boards/shields/` (outside this repo).

Build with the script (run from the repo root; it uses `-s app`, so the build dir lands at `/workspaces/zmk/build`):

```sh
bash app/build_coban.sh
```

It builds `cobanpad16a` and copies the result to `/workspaces/zmk-config/zmk-16a.uf2`. The `cobanpad12b` lines are currently commented out in the script.

Underlying command pattern (board is `nice_nano@1`):

```sh
west build -b nice_nano@1 --pristine -S studio-rpc-usb-uart -S zmk-usb-logging -s app \
  -- -DZMK_CONFIG="/workspaces/zmk-config" -DSHIELD=cobanpad16a
```

`-DZMK_CONFIG` is required — it points west at the external config repo containing the coban shields.

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
- CI runs in container `docker.io/zmkfirmware/zmk-build-arm:4.1` after `west init -l app && west update && west zephyr-export`.

## Conventions

- Pre-commit enforces: clang-format v18.1.8 (C/C++), prettier, no tabs, trailing whitespace, gitlint commit messages.
- Keymaps are `.keymap` files (one per shield in zmk-config); behaviors are Zephyr devicetree nodes — see `docs/docs/development/` before adding hardware support.
