---
name: gitlab-mr-review
description: Fetch and act on GitLab merge requests via the GitLab REST API — MR descriptions, diffs, commits, comment threads/discussions, and repository file/branch content — and post review findings back as comments or inline threads. Use this any time the user wants a GitLab merge request reviewed, wants MR info/diffs/comments fetched, wants to reply to or resolve MR discussion threads, or wants review feedback posted to a GitLab MR — even if they just paste an MR URL or say something like "review this MR" or "check the open threads on !42". Requires a .env file with GITLAB_URL and GITLAB_TOKEN.
---

# GitLab MR Review

Talks to a GitLab instance's REST API to support two things: pulling everything
needed to understand a merge request (description, diff, commits, existing
discussion threads, and — when the diff alone isn't enough context — full file
or branch content), and writing back to it (top-level comments, inline
diff-line threads, replies, and resolving threads).

All of it goes through one CLI so you don't have to hand-write API calls:
`scripts/gitlab_cli.py`. It's stdlib-only Python (no pip install needed) and
every subcommand prints JSON to stdout, so pipe it through `python3 -m json.tool`,
`jq`, or just read it directly.

## Setup

The CLI reads credentials from a `.env` file — it searches the current working
directory and walks upward through parent directories until it finds one, so
it works whether you're running from a repo root or a subdirectory. Real
environment variables of the same name override the `.env` file if both are
present.

Required in `.env`:
```
GITLAB_URL=https://gitlab.example.com    # no trailing slash, no /api/v4 suffix
GITLAB_TOKEN=glpat-xxxxxxxxxxxxxxxxxxxx  # personal or project access token, `api` scope
```

Optional — lets you omit `--project` on every call:
```
GITLAB_PROJECT_ID=group/subgroup/repo    # or the numeric project ID
```

Before doing anything else in a fresh project, sanity-check the setup:
```bash
python3 scripts/gitlab_cli.py whoami
```
If this fails, the error message tells you exactly what's missing (no `.env`
found, no token, or the token doesn't authenticate) — fix that before
continuing rather than guessing at the cause from a later, less obvious error.

If the user gives you an MR as a URL (e.g.
`https://gitlab.example.com/group/repo/-/merge_requests/42`), the project path
is everything between the host and `/-/merge_requests/`, and the MR number
is the *IID* (the number the user sees in the UI, not the discussion or note
IDs) — pass `--project group/repo` and `42` as the MR argument.

## Commands

```bash
python3 scripts/gitlab_cli.py --project <id-or-path> mr-info <mr>
python3 scripts/gitlab_cli.py --project <id-or-path> mr-diff <mr> [--summary]
python3 scripts/gitlab_cli.py --project <id-or-path> mr-commits <mr>
python3 scripts/gitlab_cli.py --project <id-or-path> mr-discussions <mr> [--unresolved-only]
python3 scripts/gitlab_cli.py --project <id-or-path> mr-comment <mr> --body "..."
python3 scripts/gitlab_cli.py --project <id-or-path> mr-thread <mr> --body "..." [--file path --new-line N]
python3 scripts/gitlab_cli.py --project <id-or-path> mr-reply <mr> <discussion_id> --body "..."
python3 scripts/gitlab_cli.py --project <id-or-path> mr-resolve <mr> <discussion_id> [--unresolve]
python3 scripts/gitlab_cli.py --project <id-or-path> file-content <path> --ref <branch> [--raw]
python3 scripts/gitlab_cli.py --project <id-or-path> tree --ref <branch> [--path dir] [--recursive]
python3 scripts/gitlab_cli.py --project <id-or-path> compare <from_ref> <to_ref>
```

`--project` can be omitted anywhere it's used if `GITLAB_PROJECT_ID` is set in
`.env`. `--body` can be omitted on `mr-comment`/`mr-thread`/`mr-reply` — when
it is, the command reads the body from stdin, which is the better option for
anything long or multi-line (heredocs avoid shell-quoting headaches):
```bash
python3 scripts/gitlab_cli.py mr-comment 42 << 'EOF'
## Review summary
...
EOF
```

See `references/api_notes.md` for details worth knowing before you rely on a
specific command: how diff-line positions work, how discussion IDs and note
IDs differ, pagination behavior, and common error causes.

## Running a code review on an MR

This is the core workflow the skill exists for. Work through it in order —
each step's output feeds the next one, and skipping straight to posting a
review without reading the actual diff produces generic, low-value feedback.

1. **Get oriented.** `mr-info` for the description, target branch, and author's
   stated intent — review *against what the author says they're trying to do*,
   not just the code in isolation. `mr-discussions` to see what's already been
   said; don't re-raise a point another reviewer already made unless the code
   still doesn't address it.

2. **Read the actual diff.** `mr-diff --summary` first on anything with more
   than a handful of files, so you know what you're dealing with before
   pulling full patches. Then `mr-diff` (no `--summary`) to get the real
   patch content to review.

3. **Pull extra context when the diff alone isn't enough to judge a change
   correctly.** A diff hunk shows the changed lines plus a few lines of
   surrounding context, which is often not enough to tell whether a change is
   correct — e.g. a modified function might be called elsewhere with
   assumptions the hunk doesn't show. Use `file-content` to read a full file
   at the MR's source branch (from `mr-info`'s `source_branch`, or the exact
   SHAs in `mr-diff`'s `diff_refs`) when you need to see more than the diff
   shows, and `tree` if you need to confirm whether a related file exists at
   all. Don't pull context you don't need — it costs tokens and time for no
   benefit; reach for it when a specific question about the change can't be
   answered from the diff alone.

4. **Form the actual review.** Evaluate correctness, edge cases, error
   handling, security (injection, secrets, auth checks), test coverage, and
   consistency with patterns already used elsewhere in the codebase. Be
   specific — cite the file and line, and say what's wrong and why it
   matters, not just that something looks off. Being thorough doesn't mean
   commenting on everything; a change that's genuinely fine doesn't need an
   invented nitpick to look complete.

5. **Post the findings.** Two complementary ways to do this — use both when
   the review has both file-specific and overall points:
   - **Inline threads** (`mr-thread --file <path> --new-line <n>`) for
     feedback tied to a specific line — this is what makes a comment show up
     right next to the code it's about, the way a human reviewer's inline
     comment would. Building the line position requires the MR's `diff_refs`
     from `mr-diff`/`mr-info` (`gitlab_client.py`'s `build_diff_position`
     handles this — the CLI does it for you automatically when you pass
     `--file`).
   - **A summary comment** (`mr-comment`) for the overall verdict and anything
     that doesn't map to one line — overall risk assessment, missing test
     coverage, cross-cutting concerns. Structure it clearly, e.g.:
     ```
     ## Review summary
     [1-2 sentence overall assessment]

     ### Must fix
     - ...

     ### Suggestions
     - ...

     ### Notes
     - ...
     ```
   Skip categories with nothing in them rather than writing "None" — an
   empty "Must fix" section reads as a pass, which is exactly what a real
   reviewer's report should convey.

6. **If re-reviewing after changes**, check `mr-discussions --unresolved-only`
   for prior threads still open, verify whether the new commits address them
   (`mr-commits` for what's new, `mr-diff` for the current state), and reply
   in-thread (`mr-reply`) rather than opening a duplicate thread for the same
   point. Resolve threads (`mr-resolve`) only when you're the one who raised
   them and you're satisfied they're addressed — don't resolve another
   reviewer's open thread on their behalf.

## A note on tone

Write review comments the way a good human reviewer would: direct about
what needs to change and why, generous about acknowledging what's done well,
and never hedging so much that "must fix" and "minor nit" become
indistinguishable. The `Must fix` / `Suggestions` / `Notes` structure above
exists so the author can triage at a glance — undermine that by putting
everything in one tier and the report stops being useful.
