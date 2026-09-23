# GitLab API notes

Details that matter for using the CLI correctly but would clutter SKILL.md.

## MR IID vs ID

Every GitLab merge request has two identifiers: a project-scoped `iid`
(what's shown in the UI and URL, e.g. `!42`) and a global `id` (unique across
the whole GitLab instance). This CLI always takes the **iid** — it's what
`mr` means in every subcommand. If a script call returns a 404 for an MR you
can clearly see in the UI, double check you didn't pass the `id` field from
a previous JSON response by mistake.

## Discussion IDs vs note IDs

A "discussion" is a thread; a "note" is one comment within it. `mr-discussions`
returns a list of discussions, each with an `id` (the discussion ID, a long
hex string) and a `notes` array where each note has its own numeric `id`.
`mr-reply` and `mr-resolve` take the **discussion ID**, not a note ID.

A discussion started via `mr-comment` (no position) still gets a discussion
ID and can be replied to — GitLab treats every top-level note as a
single-note discussion under the hood.

## Building a diff-line position for inline threads

To anchor a comment to a specific line, GitLab needs a `position` object
carrying the diff's base/start/head SHAs plus the file path and line number.
These SHAs pin the comment to *that specific version* of the diff — if new
commits are pushed, GitLab uses them to figure out whether the line still
exists and where. Always fetch a fresh `diff_refs` (via `mr-diff` or
`mr-info`) right before posting rather than reusing one from earlier in a
long session, in case new commits landed in the meantime.

`new_line` refers to the line number in the file *after* the change (use this
for added or unchanged/context lines). `old_line` refers to the line number
*before* the change (use this only when commenting on a line that was
deleted and no longer exists in the new version — you can't use `new_line`
for a deleted line since it has no position in the new file). Provide exactly
one, never both.

If GitLab rejects a position (400 error mentioning "line_code" or
"position"), the most common cause is the line number not actually
appearing in the diff hunks for that file — context lines a few rows outside
the hunk aren't valid targets even though they're visible in a full-file
view.

## Pagination

List endpoints (`mr-discussions`, `mr-commits`, `tree`) are paginated by
GitLab at 20 results/page by default. The client requests 100/page and
follows every page automatically, so CLI output is always the complete list
— you don't need to think about `page`/`per_page` yourself. On projects with
very large histories this means `mr-commits` on an MR with hundreds of
commits will make several requests; that's expected, not a bug.

## Token scope and permissions

The token needs the `api` scope (not just `read_api`) to post comments,
threads, or resolve discussions — `read_api` is enough for every read-only
subcommand (`mr-info`, `mr-diff`, `mr-discussions`, `file-content`, `tree`,
`compare`) but will fail with a 403 on `mr-comment`/`mr-thread`/`mr-reply`/
`mr-resolve`. If reads work but writes 403, that's the first thing to check
before assuming something else is wrong.

The token's user also needs at least Developer access on the project to
comment on MRs, and Reporter access is not sufficient to resolve threads
started by other users in some GitLab configurations.

## Common error causes, quickly

- **401 Unauthorized** — token is wrong, expired, or revoked.
- **403 Forbidden** — token scope or project role isn't sufficient for the action.
- **404 Not Found** — wrong project path/ID, wrong MR iid, or the token's
  user can't see the project at all (private project, no access).
- **`Could not reach <url>`** — `GITLAB_URL` is wrong, or the instance needs
  VPN/network access that isn't currently available.
- **400 on mr-thread with `--file`** — see the diff-line position section
  above; the line likely isn't in the diff hunk.
