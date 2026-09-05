#!/usr/bin/env python3
"""
Command-line interface over github_client.py. Every subcommand prints a
single JSON value to stdout on success, so output is easy to pipe, jq,
or read directly. Errors go to stderr with a non-zero exit code.

Run `python3 github_cli.py <subcommand> --help` for per-command options.
Run `python3 github_cli.py whoami` first to confirm .env is set up right.
"""

from __future__ import annotations

import argparse
import json
import sys

from github_client import GitHubAPIError, GitHubConfigError, load_client


def out(value) -> None:
    print(json.dumps(value, indent=2, ensure_ascii=False))


def cmd_whoami(client, args):
    user, _ = client._request("GET", "/user")
    out(user)


def cmd_pr_info(client, args):
    out(client.get_pull_request(args.repo, args.pr))


def cmd_pr_diff(client, args):
    files = client.get_pr_files(args.repo, args.pr)
    if args.summary:
        out([
            {
                "path": f.get("filename"),
                "status": f.get("status"),
                "additions": f.get("additions"),
                "deletions": f.get("deletions"),
                "previous_filename": f.get("previous_filename"),
            }
            for f in files
        ])
    else:
        out(files)


def cmd_pr_commits(client, args):
    out(client.get_pr_commits(args.repo, args.pr))


def cmd_pr_threads(client, args):
    threads = client.get_review_threads(args.repo, args.pr)
    if args.unresolved_only:
        threads = [t for t in threads if not t.get("isResolved")]
    out(threads)


def cmd_pr_comments(client, args):
    out(client.list_issue_comments(args.repo, args.pr))


def cmd_pr_reviews(client, args):
    out(client.list_reviews(args.repo, args.pr))


def cmd_pr_comment(client, args):
    body = args.body if args.body else sys.stdin.read()
    out(client.create_issue_comment(args.repo, args.pr, body))


def cmd_pr_thread(client, args):
    body = args.body if args.body else sys.stdin.read()
    commit_id = args.commit
    if not commit_id:
        pr = client.get_pull_request(args.repo, args.pr)
        commit_id = pr["head"]["sha"]
    out(client.create_review_comment(
        args.repo, args.pr, body, commit_id=commit_id, path=args.file,
        line=args.line, side=args.side,
        start_line=args.start_line, start_side=args.start_side,
    ))


def cmd_pr_reply(client, args):
    body = args.body if args.body else sys.stdin.read()
    out(client.reply_to_review_comment(args.repo, args.pr, args.comment_id, body))


def cmd_pr_resolve(client, args):
    out(client.resolve_review_thread(args.thread_id, resolved=not args.unresolve))


def cmd_pr_review(client, args):
    body = args.body if args.body else sys.stdin.read()
    comments = None
    if args.comments_json:
        comments = json.loads(args.comments_json)
    elif args.comments_file:
        with open(args.comments_file) as f:
            comments = json.load(f)
    out(client.submit_review(args.repo, args.pr, body, event=args.event, comments=comments))


def cmd_file_content(client, args):
    content = client.get_file_content(args.repo, args.path, args.ref)
    if args.raw:
        sys.stdout.write(content)
    else:
        out({"path": args.path, "ref": args.ref, "content": content})


def cmd_tree(client, args):
    tree = client.list_repo_tree(args.repo, args.ref, recursive=args.recursive)
    if args.path:
        prefix = args.path.rstrip("/") + "/"
        tree = [t for t in tree if t.get("path", "").startswith(prefix)]
    out(tree)


def cmd_compare(client, args):
    out(client.compare_refs(args.repo, args.base, args.head))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="GitHub PR review CLI")
    parser.add_argument("--repo", "-r", default=None,
                         help="Repository as 'owner/name'. Falls back to "
                              "GITHUB_REPOSITORY from .env if omitted.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("whoami", help="Verify .env/token setup by fetching the authenticated user").set_defaults(func=cmd_whoami)

    p = sub.add_parser("pr-info", help="Fetch PR title/description/state/author/head-sha/etc.")
    p.add_argument("pr", type=int, help="PR number (as shown in the GitHub UI)")
    p.set_defaults(func=cmd_pr_info)

    p = sub.add_parser("pr-diff", help="Fetch the PR's changed files and diffs")
    p.add_argument("pr", type=int)
    p.add_argument("--summary", action="store_true",
                    help="Print only filenames + status + add/delete counts instead of "
                         "full patches (useful first pass on large PRs)")
    p.set_defaults(func=cmd_pr_diff)

    p = sub.add_parser("pr-commits", help="List commits included in the PR")
    p.add_argument("pr", type=int)
    p.set_defaults(func=cmd_pr_commits)

    p = sub.add_parser("pr-threads", help="Fetch review comment threads (grouped, with resolved state)")
    p.add_argument("pr", type=int)
    p.add_argument("--unresolved-only", action="store_true")
    p.set_defaults(func=cmd_pr_threads)

    p = sub.add_parser("pr-comments", help="Fetch general (non-line-specific) PR comments")
    p.add_argument("pr", type=int)
    p.set_defaults(func=cmd_pr_comments)

    p = sub.add_parser("pr-reviews", help="Fetch formal review submissions (approve/request-changes/comment)")
    p.add_argument("pr", type=int)
    p.set_defaults(func=cmd_pr_reviews)

    p = sub.add_parser("pr-comment", help="Post a top-level comment (e.g. an overall review report)")
    p.add_argument("pr", type=int)
    p.add_argument("--body", help="Comment text. If omitted, read from stdin (use for long/multi-line reports).")
    p.set_defaults(func=cmd_pr_comment)

    p = sub.add_parser("pr-thread", help="Post a single inline comment anchored to a diff line, starting a new thread")
    p.add_argument("pr", type=int)
    p.add_argument("--file", required=True, help="Path of the file to comment on")
    p.add_argument("--line", required=True, type=int, help="Line number as shown in the PR diff")
    p.add_argument("--side", default="RIGHT", choices=["LEFT", "RIGHT"],
                    help="RIGHT=new version of the file (default), LEFT=old version (for removed lines)")
    p.add_argument("--start-line", type=int, help="For a multi-line comment: the first line of the range")
    p.add_argument("--start-side", choices=["LEFT", "RIGHT"], help="Side for --start-line, defaults to --side")
    p.add_argument("--commit", help="Commit SHA to anchor to (defaults to the PR's current head SHA)")
    p.add_argument("--body", help="Comment text. If omitted, read from stdin.")
    p.set_defaults(func=cmd_pr_thread)

    p = sub.add_parser("pr-reply", help="Reply within an existing review comment thread")
    p.add_argument("pr", type=int)
    p.add_argument("comment_id", type=int, help="Numeric comment id (from pr-threads' comments[].databaseId, or pr-comments)")
    p.add_argument("--body", help="Reply text. If omitted, read from stdin.")
    p.set_defaults(func=cmd_pr_reply)

    p = sub.add_parser("pr-resolve", help="Mark a review thread resolved (or unresolved)")
    p.add_argument("thread_id", help="GraphQL thread id, from pr-threads output (the 'id' field, e.g. 'PRRT_...')")
    p.add_argument("--unresolve", action="store_true", help="Mark unresolved instead")
    p.set_defaults(func=cmd_pr_resolve)

    p = sub.add_parser("pr-review", help="Submit a full review (verdict + optional batch of inline comments) in one call")
    p.add_argument("pr", type=int)
    p.add_argument("--event", required=True, choices=["APPROVE", "REQUEST_CHANGES", "COMMENT"])
    p.add_argument("--body", help="Overall review summary text. If omitted, read from stdin.")
    p.add_argument("--comments-json", help='Inline comments as a JSON array string, e.g. \'[{"path":"a.py","line":10,"body":"..."}]\'')
    p.add_argument("--comments-file", help="Path to a JSON file containing the comments array (alternative to --comments-json)")
    p.set_defaults(func=cmd_pr_review)

    p = sub.add_parser("file-content", help="Fetch a file's content at a given branch/ref")
    p.add_argument("path", help="File path within the repo")
    p.add_argument("--ref", required=True, help="Branch name, tag, or commit SHA")
    p.add_argument("--raw", action="store_true", help="Print raw file content instead of a JSON wrapper")
    p.set_defaults(func=cmd_file_content)

    p = sub.add_parser("tree", help="List files/directories in the repo at a given branch/ref")
    p.add_argument("--ref", required=True)
    p.add_argument("--path", default="", help="Only show entries under this subdirectory")
    p.add_argument("--recursive", action="store_true")
    p.set_defaults(func=cmd_tree)

    p = sub.add_parser("compare", help="Diff between two branches/commits/tags")
    p.add_argument("base")
    p.add_argument("head")
    p.set_defaults(func=cmd_compare)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    try:
        client = load_client(repo=args.repo)
        args.func(client, args)
    except (GitHubConfigError, GitHubAPIError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
