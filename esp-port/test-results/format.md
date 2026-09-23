# Test-results format

Phase 0 deliverable: the agreed format for every hardware test result from
Phase 0 onward (bring-up, Phase 5/6 functional tests, Phase 7 power
qualification, Phase 8 USB gate). One file per test run:
`test-results/<phase>-<chip>-<seq>-<slug>.md` (e.g.
`05-c3-001-ble-pair-linux.md`).

## Required fields

Every entry starts with this front-matter block. Fields marked *(hardware)*
apply to on-board tests; mark them `n/a` for host-only or build-only entries.

```markdown
# <Phase>-<seq> — <test case name>

- Date: YYYY-MM-DD
- Test case: <stable ID, e.g. flash-monitor | matrix-scan-ble | pair-linux | idle-current>
- Result: PASS | FAIL | PARTIAL | BLOCKED
- Duration: <e.g. 24h00m / 12m30s / n/a>

## Environment

- Board: <reference-boards file name>
- Board/module revision: <PCB revision>
- Chip marking: <transcription>
- Flash / PSRAM: <sizes>
- USB-PHY / eFuse state: <unburned | burned | n/a> *(hardware)*
- Firmware: <repo> @ <full SHA>, target <board/target>, image SHA-256 <hash>
- Zephyr: <SHA or tag> ; hal_espressif: <SHA>
- Toolchain: Zephyr SDK <ver>, west <ver>, Python <ver>
- Host OS: <e.g. Linux 6.8 / macOS 15 / Windows 11 / Android 15 / iOS 18>
- Power: <e.g. USB 5 V / 3.3 V supply> *(hardware)*

## Procedure

<numbered steps actually executed>

## Observations / measurements

- <key log lines, measured current with conditions, timings, disconnect reasons>
- Measured current: <mA> @ <state> *(hardware, when applicable)*

## Artifacts

- logs: <path or link to captured serial/monitor log>
- photos: <paths, if any>

## Notes / follow-ups

- <issues filed, next steps>
```

## Rules

1. **Result is binary per test case.** A run that partially passes is
   `PARTIAL` with the failing sub-steps listed; it is not a PASS.
2. **Identifiers are full, not abbreviated.** Full SHAs, full chip markings.
   "A SuperMini" is not a board identifier.
3. **Current measurements are conditional.** Always record the state
   (boot / advertising / connected-idle / typing / sleep), the supply
   voltage, and the measurement instrument.
4. **Logs are mandatory for FAIL/BLOCKED.** The log path must resolve to a
   file in this tree or an archived artifact.
5. **eFuse state is recorded before and after any USB experiment** (Phase 8).
