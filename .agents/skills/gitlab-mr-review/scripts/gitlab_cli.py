#!/usr/bin/env python3
"""
Command-line interface over gitlab_client.py. Every subcommand prints a
single JSON value to stdout on success, so output is easy to pipe, jq,
or read directly. Errors go to stderr with a non-zero exit code.

Run `python3 gitlab_cli.py <subcommand> --help` for per-command options.
Run `python3 gitlab_cli.py whoami` first to confirm .env is set up right.
"""

from __future__ import annotations

import argparse
import json
import sys

from gitlab_client import GitLabAPIError, GitLabConfigError, load_client


def out(value) -> None:
    print(json.dumps(value, indent=2, ensure_ascii=False))


def cmd_whoami(client, args):
    out(client._request("GET", "/user"))


def cmd_mr_info(client, args):
    out(client.get_merge_request(args.project, args.mr))


def cmd_mr_diff(client, args):
    changes = client.get_merge_request_changes(args.project, args.mr)
    if args.summary:
        summary = {
            "title": changes.get("title"),
            "source_branch": changes.get("source_branch"),
            "target_branch": changes.get("target_branch"),
            "diff_refs": changes.get("diff_refs"),
            "files_changed": [
                {
                    "path": c.get("new_path"),
                    "old_path": c.get("old_path"),
                    "new_file": c.get("new_file"),
                    "deleted_file": c.get("deleted_file"),
                    "renamed_file": c.get("renamed_file"),
                }
                for c in changes.get("changes", [])
            ],
        }
        out(summary)
    else:
        out(changes)


def cmd_mr_commits(client, args):
    out(client.get_merge_request_commits(args.project, args.mr))


def cmd_mr_discussions(client, args):
    discussions = client.list_discussions(args.project, args.mr)
    if args.unresolved_only:
        discussions = [
            d for d in discussions
            if any(n.get("resolvable") and not n.get("resolved") for n in d.get("notes", []))
        ]
    out(discussions)


def cmd_mr_comment(client, args):
    body = args.body if args.body else sys.stdin.read()
    out(client.create_note(args.project, args.mr, body))


def cmd_mr_thread(client, args):
    body = args.body if args.body else sys.stdin.read()
    position = None
    if args.file:
        changes = client.get_merge_request_changes(args.project, args.mr)
        position = client.build_diff_position(
            changes, args.file, new_line=args.new_line, old_line=args.old_line,
        )
    out(client.create_discussion(args.project, args.mr, body, position=position))


def cmd_mr_reply(client, args):
    body = args.body if args.body else sys.stdin.read()
    out(client.reply_to_discussion(args.project, args.mr, args.discussion_id, body))


def cmd_mr_resolve(client, args):
    out(client.resolve_discussion(args.project, args.mr, args.discussion_id,
                                   resolved=not args.unresolve))


def cmd_file_content(client, args):
    content = client.get_file_content(args.project, args.path, args.ref)
    if args.raw:
        sys.stdout.write(content)
    else:
        out({"path": args.path, "ref": args.ref, "content": content})


def cmd_tree(client, args):
    out(client.list_repo_tree(args.project, args.ref, path=args.path,
                               recursive=args.recursive))


def cmd_compare(client, args):
    out(client.compare_refs(args.project, args.from_ref, args.to_ref))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="GitLab MR review CLI")
    parser.add_argument("--project", "-p", default=None,
                         help="Project ID or 'group/subgroup/repo' path. "
                              "Falls back to GITLAB_PROJECT_ID from .env if omitted.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("whoami", help="Verify .env/token setup by fetching the authenticated user").set_defaults(func=cmd_whoami)

    p = sub.add_parser("mr-info", help="Fetch MR title/description/state/author/etc.")
    p.add_argument("mr", type=int, help="MR IID (the number shown in the GitLab UI)")
    p.set_defaults(func=cmd_mr_info)

    p = sub.add_parser("mr-diff", help="Fetch the MR's file diffs")
    p.add_argument("mr", type=int)
    p.add_argument("--summary", action="store_true",
                    help="Print only file list + diff_refs instead of full patches "
                         "(useful first pass on large MRs before reading specific files)")
    p.set_defaults(func=cmd_mr_diff)

    p = sub.add_parser("mr-commits", help="List commits included in the MR")
    p.add_argument("mr", type=int)
    p.set_defaults(func=cmd_mr_commits)

    p = sub.add_parser("mr-discussions", help="Fetch all comment threads on the MR")
    p.add_argument("mr", type=int)
    p.add_argument("--unresolved-only", action="store_true",
                    help="Only show threads with at least one unresolved resolvable note")
    p.set_defaults(func=cmd_mr_discussions)

    p = sub.add_parser("mr-comment", help="Post a top-level comment (e.g. an overall review report)")
    p.add_argument("mr", type=int)
    p.add_argument("--body", help="Comment text. If omitted, read from stdin (use for long/multi-line reports).")
    p.set_defaults(func=cmd_mr_comment)

    p = sub.add_parser("mr-thread", help="Start a new discussion thread, optionally anchored to a diff line")
    p.add_argument("mr", type=int)
    p.add_argument("--body", help="Thread text. If omitted, read from stdin.")
    p.add_argument("--file", help="Path of the file to anchor the comment to (omit for a general thread)")
    p.add_argument("--new-line", type=int, help="Line number in the new version of the file")
    p.add_argument("--old-line", type=int, help="Line number in the old version of the file (for deleted lines)")
    p.set_defaults(func=cmd_mr_thread)

    p = sub.add_parser("mr-reply", help="Reply to an existing discussion thread")
    p.add_argument("mr", type=int)
    p.add_argument("discussion_id", help="Discussion ID, from mr-discussions output")
    p.add_argument("--body", help="Reply text. If omitted, read from stdin.")
    p.set_defaults(func=cmd_mr_reply)

    p = sub.add_parser("mr-resolve", help="Mark a discussion thread resolved (or unresolved)")
    p.add_argument("mr", type=int)
    p.add_argument("discussion_id")
    p.add_argument("--unresolve", action="store_true", help="Mark unresolved instead")
    p.set_defaults(func=cmd_mr_resolve)

    p = sub.add_parser("file-content", help="Fetch a file's raw content at a given branch/ref")
    p.add_argument("path", help="File path within the repo")
    p.add_argument("--ref", required=True, help="Branch name, tag, or commit SHA")
    p.add_argument("--raw", action="store_true", help="Print raw file content instead of a JSON wrapper")
    p.set_defaults(func=cmd_file_content)

    p = sub.add_parser("tree", help="List files/directories in the repo at a given branch/ref")
    p.add_argument("--ref", required=True)
    p.add_argument("--path", default="", help="Subdirectory to list (default: repo root)")
    p.add_argument("--recursive", action="store_true")
    p.set_defaults(func=cmd_tree)

    p = sub.add_parser("compare", help="Diff between two branches/commits/tags")
    p.add_argument("from_ref")
    p.add_argument("to_ref")
    p.set_defaults(func=cmd_compare)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    try:
        client = load_client(project=args.project)
        args.func(client, args)
    except (GitLabConfigError, GitLabAPIError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
