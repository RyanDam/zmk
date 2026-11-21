# Backlogs — moved

The backlog for this workspace (this `zmk/` repo, `../zmk-config/`, and the
workspace tooling) now lives at the **workspace root**: [`../../backlogs/`](../../backlogs/).

This merge happened because product work routinely spans both repos and the old
repo-local backlog couldn't cover `zmk-config/` or the tooling. All ESP-port
items and their history were moved unchanged; ESP-port context remains in
[`../report.md`](../report.md) and [`../implement_plan.md`](../implement_plan.md).

Use the helper script from the workspace root:

```sh
python3 ../.agents/skills/backlog-manager/scripts/backlog.py list --backlogs ../backlogs --open
```

Do not add new items here.
