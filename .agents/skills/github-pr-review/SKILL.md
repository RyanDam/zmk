---
name: github-pr-review
description: Fetch and act on GitHub pull requests via the GitHub REST + GraphQL APIs — PR descriptions, diffs, commits, comment threads (with resolved state), formal reviews, and repository file/branch content — and post review findings back as comments, inline diff threads, or a full submitted review. Use this any time the user wants a GitHub pull request reviewed, wants PR info/diffs/comments fetched, wants to reply to or resolve PR review threads, or wants review feedback posted to a GitHub PR — even if they just paste a PR URL or say something like "review this PR" or "check the open threads on #128". Requires a .env file with GITHUB_TOKEN.
---

# GitHub PR Review

Talks to a GitHub repo's REST and GraphQL APIs to support two things: pulling
everything needed to understand a pull request (description, diff, commits,
existing review threads with their resolved/unresolved state, and — when the
diff alone isn't enough context — full file or branch content), and writing
back to it (top-level comments, inline diff-line threads, replies, resolving
threads, and submitting a full formal review in one call).

All of it goes through one CLI: `scripts/github_cli.py`. It's stdlib-only
Python (no pip install needed) and every subcommand prints JSON to stdout, so
pipe it through `python3 -m json.tool`, `jq`, or just read it directly.

## Setup

The CLI reads credentials from a `.env` file — it searches the current working
directory and walks upward through parent directories until it finds one, so
it works whether you're running from a repo root or a subdirectory. Real
environment variables of the same name override the `.env` file if both are
present.

Required in `.env`:
```
GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx   # classic (repo scope) or fine-grained PAT
```

Optional — lets you omit `--repo` on every call:
```
GITHUB_REPOSITORY=owner/repo
```

Only needed for GitHub Enterprise Server (leave unset for github.com):
```
GITHUB_API_URL=https://github.yourcompany.com/api/v3
```

Before doing anything else in a fresh project, sanity-check the setup:
```bash
python3 scripts/github_cli.py whoami
```
If this fails, the error message tells you exactly what's missing (no `.env`
found, no token, or the token doesn't authenticate) — fix that before
continuing rather than guessing at the cause from a later, less obvious error.

If the user gives you a PR as a URL (e.g.
`https://github.com/owner/repo/pull/128`), `--repo owner/repo` and `128` as
the PR number is everything you need — `resolve_repo()` in the client also
tolerates a pasted URL directly if you'd rather pass that through as-is.

## Commands

```bash
python3 scripts/github_cli.py --repo owner/name pr-info <pr>
python3 scripts/github_cli.py --repo owner/name pr-diff <pr> [--summary]
python3 scripts/github_cli.py --repo owner/name pr-commits <pr>
python3 scripts/github_cli.py --repo owner/name pr-threads <pr> [--unresolved-only]
python3 scripts/github_cli.py --repo owner/name pr-comments <pr>
python3 scripts/github_cli.py --repo owner/name pr-reviews <pr>
python3 scripts/github_cli.py --repo owner/name pr-comment <pr> --body "..."
python3 scripts/github_cli.py --repo owner/name pr-thread <pr> --file path --line N --body "..."
python3 scripts/github_cli.py --repo owner/name pr-reply <pr> <comment_id> --body "..."
python3 scripts/github_cli.py --repo owner/name pr-resolve <thread_id> [--unresolve]
python3 scripts/github_cli.py --repo owner/name pr-review <pr> --event COMMENT --body "..." [--comments-json '[...]']
python3 scripts/github_cli.py --repo owner/name file-content <path> --ref <branch> [--raw]
python3 scripts/github_cli.py --repo owner/name tree --ref <branch> [--path dir] [--recursive]
python3 scripts/github_cli.py --repo owner/name compare <base> <head>
```

`--repo` can be omitted anywhere it's used if `GITHUB_REPOSITORY` is set in
`.env`. `--body` can be omitted on `pr-comment`/`pr-thread`/`pr-reply`/
`pr-review` — when it is, the command reads the body from stdin, which is the
better option for anything long or multi-line (heredocs avoid shell-quoting
headaches):
```bash
python3 scripts/github_cli.py pr-comment 128 << 'EOF'
## Review summary
...
EOF
```

See `references/api_notes.md` before relying on a specific command — GitHub
splits this functionality across two APIs in a way that trips people up:
REST has no concept of a resolvable "thread" (only a flat comment list) and
no way to resolve one at all, so thread grouping/resolving always goes
through GraphQL, while everything else goes through REST. The reference also
covers `line`/`side` semantics for inline comments, comment-id vs thread-id
(they're not interchangeable), and common error causes.

## Running a code review on a PR

This is the core workflow the skill exists for. Work through it in order —
each step's output feeds the next one, and skipping straight to posting a
review without reading the actual diff produces generic, low-value feedback.

1. **Get oriented.** `pr-info` for the description, target branch, and
   author's stated intent — review *against what the author says they're
   trying to do*, not just the code in isolation. `pr-threads` and
   `pr-reviews` to see what's already been said; don't re-raise a point
   another reviewer already made unless the code still doesn't address it.

2. **Read the actual diff.** `pr-diff --summary` first on anything with more
   than a handful of files, so you know what you're dealing with before
   pulling full patches. Then `pr-diff` (no `--summary`) to get the real
   patch content to review.

3. **Pull extra context when the diff alone isn't enough to judge a change
   correctly.** A diff hunk shows the changed lines plus a few lines of
   surrounding context, which is often not enough to tell whether a change is
   correct — e.g. a modified function might be called elsewhere with
   assumptions the hunk doesn't show. Use `file-content` to read a full file
   at the PR's head branch (`pr-info`'s `head.ref` or `head.sha`) when you
   need to see more than the diff shows, and `tree` if you need to confirm
   whether a related file exists at all. Don't pull context you don't need —
   it costs tokens and time for no benefit; reach for it when a specific
   question about the change can't be answered from the diff alone.

4. **Form the actual review.** Evaluate correctness, edge cases, error
   handling, security (injection, secrets, auth checks), test coverage, and
   consistency with patterns already used elsewhere in the codebase. Be
   specific — cite the file and line, and say what's wrong and why it
   matters, not just that something looks off. Being thorough doesn't mean
   commenting on everything; a change that's genuinely fine doesn't need an
   invented nitpick to look complete.

5. **Post the findings.** GitHub gives you two ways to combine an overall
   verdict with inline points — prefer the batched one:
   - **`pr-review`** submits a formal review — an `--event` verdict
     (`COMMENT` for feedback with no gate, `REQUEST_CHANGES` for
     must-fix issues, `APPROVE` if it's genuinely ready) plus, via
     `--comments-json`/`--comments-file`, every inline point attached to that
     same review. This is the right default: it lands as one atomic review
     with one notification instead of a flurry of separate comments. Each
     comment needs `path`, `line`, `side` (default `RIGHT`), and `body`.
   - **`pr-thread`** posts a single inline comment immediately as its own
     thread, outside of any review. Use this only for a one-off follow-up
     comment after the main review is already posted — not as the primary
     way to deliver a multi-point review, since each call is a separate
     notification to the author.

   Structure the review body clearly, e.g.:
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

6. **If re-reviewing after changes**, check `pr-threads --unresolved-only`
   for prior threads still open, verify whether the new commits address them
   (`pr-commits` for what's new, `pr-diff` for the current state), and reply
   in-thread (`pr-reply`, using the comment's numeric `databaseId` from
   `pr-threads`) rather than opening a duplicate thread for the same point.
   Resolve threads (`pr-resolve`, using the thread's GraphQL `id` from
   `pr-threads`) only when you're the one who raised them and you're
   satisfied they're addressed — don't resolve another reviewer's open
   thread on their behalf.

## A note on tone

Write review comments the way a good human reviewer would: direct about
what needs to change and why, generous about acknowledging what's done well,
and never hedging so much that "must fix" and "minor nit" become
indistinguishable. The `Must fix` / `Suggestions` / `Notes` structure above
exists so the author can triage at a glance — undermine that by putting
everything in one tier and the report stops being useful.
