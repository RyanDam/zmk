# GitHub API notes

Details that matter for using the CLI correctly but would clutter SKILL.md.

## REST vs GraphQL — why both

GitHub's REST API models PR review comments as a flat list: every comment has
an `id`, and a reply has an `in_reply_to_id` pointing at the thread's root
comment, but REST never groups them into a thread object or exposes whether
a thread is resolved. That grouping and resolved/unresolved state only
exists in GraphQL (`reviewThreads` on a `PullRequest`), and *resolving* a
thread is a GraphQL-only mutation — there is no REST endpoint for it at all.

So: `pr-threads` and `pr-resolve` go through GraphQL. Everything else
(`pr-info`, `pr-diff`, `pr-commits`, `pr-comments`, `pr-reviews`,
`pr-comment`, `pr-thread`, `pr-reply`, `pr-review`, `file-content`, `tree`,
`compare`) goes through REST. You generally don't need to think about which
is which — the CLI handles it — but it explains why thread IDs and comment
IDs look so different (see below) and why `pr-threads` output has different
field names (`isResolved`, camelCase) than the rest of the CLI's REST-derived
output (`snake_case`).

## Comment ID vs thread ID — not interchangeable

- **Comment ID** (a plain integer, e.g. `123456789`): identifies one REST
  review comment. Used by `pr-reply` (as `comment_id`) and appears as
  `databaseId` inside each comment in `pr-threads` output, or as `id` in
  `pr-comments`/REST comment listings.
- **Thread ID** (a GraphQL node ID string, e.g. `PRRT_kwDOABcd1M4...`):
  identifies the thread as a whole. Used only by `pr-resolve`. It's the `id`
  field on each thread object from `pr-threads` — not the `databaseId` on
  any of the comments inside it.

Passing a comment ID to `pr-resolve` or a thread ID to `pr-reply` will fail;
they are not interchangeable even though both eventually mean "this thread."

## `line` / `side` semantics for inline comments

`pr-thread` and `pr-review`'s inline comments both need `path`, `line`, and
`side` to anchor a comment to a spot in the diff:

- `side: RIGHT` (the default) means the line as it appears in the *new*
  version of the file — use this for added or unchanged/context lines.
- `side: LEFT` means the line in the *old* version — use this only when
  commenting on a line that was deleted and no longer exists on the right
  side.
- `line` is the line number *in that version of the file*, not a diff hunk
  offset.
- For a comment spanning multiple lines, also pass `start_line` (and
  `start_side` if it differs from `side`) — `line`/`side` then mark the end
  of the range.

`commit_id` should be the PR's current head SHA (`pr-info` → `head.sha`).
`pr-thread` fetches this automatically if you don't pass `--commit`, but
fetch a fresh one rather than reusing an old value if new commits may have
landed since you last checked — GitHub will reject a comment anchored to a
line that no longer exists at an outdated SHA.

If GitHub rejects a comment with something like "pull_request_review_thread
... line must be part of the diff", the line you picked isn't actually
within a diff hunk for that file — comment-able lines are limited to what
the diff view shows, even though the full file has many more lines.

## Single comment vs full review

`pr-thread` creates one inline comment immediately, as its own standalone
thread — useful for a quick follow-up, but each call is a separate
notification to the PR author. `pr-review` submits a review: one overall verdict
(`APPROVE`/`REQUEST_CHANGES`/`COMMENT`) plus, optionally, a whole batch of
inline comments attached to it, landing as a single atomic action with one
notification. For a full code review with multiple points, prefer
`pr-review` — it's both fewer API calls and a better experience for whoever
receives it.

The `--comments-json`/`--comments-file` array for `pr-review` takes the same
`path`/`line`/`side` (and optionally `start_line`/`start_side`) fields as
`pr-thread`, just one object per comment:
```json
[
  {"path": "src/app.py", "line": 42, "body": "This can raise if `items` is empty."},
  {"path": "src/app.py", "line": 88, "side": "LEFT", "body": "Was this removed intentionally?"}
]
```

## Pagination

REST list endpoints (`pr-diff`, `pr-commits`, `pr-comments`, `pr-reviews`)
paginate via a `Link` response header, not a page-count parameter — the
client follows `rel="next"` automatically, so CLI output is always the
complete list. `pr-threads` paginates via GraphQL cursors instead, also
handled automatically. Neither requires the caller to think about paging.

## Token scope and permissions

A classic PAT needs the `repo` scope for private repos (or no extra scope
for public-repo read access) to do everything here; write actions
(comments, reviews, resolving threads) need `repo` regardless of visibility.
A fine-grained PAT needs "Pull requests: Read and write" and "Contents:
Read-only" (or "Read and write" if you'll never need write, drop back to
read-only) on the target repo.

## Common error causes, quickly

- **401 Unauthorized** — token is wrong, expired, or revoked.
- **403 Forbidden** — token scope isn't sufficient for the action, or (rarely)
  a secondary rate limit was hit — the client retries these automatically a
  few times before giving up.
- **404 Not Found** — wrong `owner/repo`, wrong PR number, or the token's
  user can't see the repo at all (private repo, no access, or an
  organization with SSO enforcement the token isn't authorized for).
- **`Could not reach <url>`** — `GITHUB_API_URL` is wrong (Enterprise Server
  only), or the instance needs VPN/network access that isn't currently
  available.
- **422 on `pr-thread`/`pr-review` inline comments** — see the `line`/`side`
  section above; the line likely isn't in a diff hunk, or `commit_id` is
  stale.
