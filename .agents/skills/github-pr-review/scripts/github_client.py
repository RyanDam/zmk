"""
Minimal GitHub REST + GraphQL client, stdlib-only (no pip installs required).

Auth is read from a .env file (GITHUB_TOKEN, optional GITHUB_REPOSITORY,
optional GITHUB_API_URL for GitHub Enterprise) so this works the same way
in every project without hardcoding credentials anywhere. See
find_and_load_env() for the search order.

Two APIs are used deliberately: REST for everything it covers well (PR
info, diffs, commits, comments, reviews, file/tree content), and GraphQL
for the one thing REST cannot do at all — reading/resolving PR *review
threads* as GitHub's UI shows them (REST only exposes a flat list of
review comments; thread grouping and the resolved/unresolved state are
GraphQL-only).
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


class GitHubConfigError(RuntimeError):
    """Raised when GITHUB_TOKEN / repo can't be found or are invalid."""


class GitHubAPIError(RuntimeError):
    """Raised when the GitHub API returns an error response."""

    def __init__(self, status: int, method: str, url: str, body: str):
        self.status = status
        self.method = method
        self.url = url
        self.body = body
        super().__init__(f"GitHub API {method} {url} -> HTTP {status}: {body[:500]}")


def find_and_load_env(start_dir: str | None = None) -> dict[str, str]:
    """
    Look for a .env file starting at start_dir (default: current working
    directory) and walking up through parent directories until one is
    found or the filesystem root is reached. Real environment variables
    of the same name take precedence over the .env file, so a user can
    override values with real env vars if they want.
    """
    env: dict[str, str] = {}
    current = Path(start_dir or os.getcwd()).resolve()

    for directory in [current, *current.parents]:
        candidate = directory / ".env"
        if candidate.is_file():
            for raw_line in candidate.read_text(encoding="utf-8").splitlines():
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.strip()
                value = value.strip().strip('"').strip("'")
                env[key] = value
            break

    for key in ("GITHUB_TOKEN", "GITHUB_REPOSITORY", "GITHUB_API_URL"):
        if os.environ.get(key):
            env[key] = os.environ[key]

    return env


_LINK_RE = re.compile(r'<([^>]+)>;\s*rel="([^"]+)"')


class GitHubClient:
    def __init__(self, token: str | None = None, default_repo: str | None = None,
                 api_url: str | None = None, timeout: int = 30):
        env = find_and_load_env()

        self.token = token or env.get("GITHUB_TOKEN")
        self.default_repo = default_repo or env.get("GITHUB_REPOSITORY")
        # api.github.com for github.com; https://HOST/api/v3 for Enterprise Server.
        self.api_url = (api_url or env.get("GITHUB_API_URL") or "https://api.github.com").rstrip("/")
        self.timeout = timeout

        if not self.token:
            raise GitHubConfigError(
                "GITHUB_TOKEN is not set. Add a personal access token (classic, "
                "`repo` scope, or a fine-grained token with PR read/write) to a "
                ".env file (GITHUB_TOKEN=ghp_...) or export it as an environment "
                "variable."
            )

        # Derive the GraphQL endpoint. github.com -> api.github.com/graphql.
        # Enterprise Server -> https://HOST/api/graphql (note: NOT under /api/v3).
        if env.get("GITHUB_GRAPHQL_URL"):
            self.graphql_url = env["GITHUB_GRAPHQL_URL"]
        elif self.api_url.rstrip("/").endswith("/api/v3"):
            self.graphql_url = self.api_url.rstrip("/")[: -len("/api/v3")] + "/api/graphql"
        else:
            self.graphql_url = "https://api.github.com/graphql"

    # ------------------------------------------------------------------
    # low-level request handling
    # ------------------------------------------------------------------

    def _headers(self) -> dict:
        return {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        }

    def _request(self, method: str, path_or_url: str, params: dict | None = None,
                 json_body: Any = None, raw: bool = False,
                 max_retries: int = 3) -> Any:
        url = path_or_url if path_or_url.startswith("http") else f"{self.api_url}{path_or_url}"
        if params:
            url = f"{url}?{urllib.parse.urlencode(params)}"

        data = None
        headers = self._headers()
        if json_body is not None:
            data = json.dumps(json_body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = urllib.request.Request(url, data=data, headers=headers, method=method)

        last_error = None
        for attempt in range(max_retries):
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    body = resp.read()
                    if raw:
                        return body, dict(resp.headers)
                    if not body:
                        return None, dict(resp.headers)
                    return json.loads(body), dict(resp.headers)
            except urllib.error.HTTPError as e:
                err_body = e.read().decode("utf-8", errors="replace")
                # Retry on secondary rate limits / transient server errors only.
                if e.code == 403 and "rate limit" in err_body.lower():
                    last_error = GitHubAPIError(e.code, method, url, err_body)
                    time.sleep(min(2 ** attempt * 2, 16))
                    continue
                if e.code >= 500:
                    last_error = GitHubAPIError(e.code, method, url, err_body)
                    time.sleep(min(2 ** attempt, 8))
                    continue
                raise GitHubAPIError(e.code, method, url, err_body) from e
            except urllib.error.URLError as e:
                raise GitHubConfigError(
                    f"Could not reach {self.api_url} ({e.reason}). Check "
                    "GITHUB_API_URL and your network/VPN access."
                ) from e

        raise last_error  # exhausted retries on a transient error

    def _paginated(self, path: str, params: dict | None = None,
                   max_pages: int = 50) -> list:
        """GitHub REST list endpoints paginate via a Link header (not a
        simple page count), so follow rel="next" until it's absent rather
        than guessing page numbers."""
        params = dict(params or {})
        params.setdefault("per_page", 100)
        results: list = []
        url = f"{self.api_url}{path}?{urllib.parse.urlencode(params)}"
        page_count = 0

        while url and page_count < max_pages:
            body, headers = self._request("GET", url)
            if body:
                results.extend(body)
            link_header = headers.get("Link", "")
            next_url = None
            for target, rel in _LINK_RE.findall(link_header):
                if rel == "next":
                    next_url = target
                    break
            url = next_url
            page_count += 1

        return results

    def graphql(self, query: str, variables: dict | None = None) -> dict:
        payload = {"query": query, "variables": variables or {}}
        data = json.dumps(payload).encode("utf-8")
        headers = self._headers()
        headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.graphql_url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                result = json.loads(resp.read())
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            raise GitHubAPIError(e.code, "POST", self.graphql_url, err_body) from e
        if "errors" in result and result["errors"]:
            raise GitHubAPIError(0, "POST", self.graphql_url, json.dumps(result["errors"]))
        return result["data"]

    # ------------------------------------------------------------------
    # repo resolution
    # ------------------------------------------------------------------

    def resolve_repo(self, repo: str | None) -> tuple[str, str]:
        """Accepts 'owner/name', falling back to GITHUB_REPOSITORY. Also
        accepts a full PR/repo URL fragment if the caller passes one
        through unchanged."""
        repo = repo or self.default_repo
        if not repo:
            raise GitHubConfigError(
                "No repository specified and GITHUB_REPOSITORY is not set. "
                "Pass --repo owner/name or set GITHUB_REPOSITORY in .env."
            )
        repo = repo.strip().strip("/")
        if repo.startswith("http"):
            # tolerate a pasted URL like https://github.com/owner/repo/...
            parts = urllib.parse.urlparse(repo).path.strip("/").split("/")
            repo = "/".join(parts[:2])
        if "/" not in repo:
            raise GitHubConfigError(f"Repository must be 'owner/name', got: {repo!r}")
        owner, name = repo.split("/", 1)
        return owner, name

    # ------------------------------------------------------------------
    # pull requests: metadata, diff, commits
    # ------------------------------------------------------------------

    def get_pull_request(self, repo: str, number: int) -> dict:
        owner, name = self.resolve_repo(repo)
        body, _ = self._request("GET", f"/repos/{owner}/{name}/pulls/{number}")
        return body

    def get_pr_files(self, repo: str, number: int) -> list:
        """Per-file diffs (patch text, additions/deletions, status). GitHub
        caps this at the first 3000 changed files; a PR that large needs a
        different strategy (e.g. compare_refs on subpaths) which is rare
        enough not to special-case here."""
        owner, name = self.resolve_repo(repo)
        return self._paginated(f"/repos/{owner}/{name}/pulls/{number}/files")

    def get_pr_commits(self, repo: str, number: int) -> list:
        owner, name = self.resolve_repo(repo)
        return self._paginated(f"/repos/{owner}/{name}/pulls/{number}/commits")

    # ------------------------------------------------------------------
    # comments and reviews (REST — flat lists)
    # ------------------------------------------------------------------

    def list_issue_comments(self, repo: str, number: int) -> list:
        """General (non-line-specific) PR comments. PRs are issues under
        the hood in GitHub's model, hence the /issues/ path here."""
        owner, name = self.resolve_repo(repo)
        return self._paginated(f"/repos/{owner}/{name}/issues/{number}/comments")

    def list_review_comments(self, repo: str, number: int) -> list:
        """Flat list of line-anchored review comments. Each reply has an
        `in_reply_to_id` pointing at the thread's root comment — use
        get_review_threads() instead if you want them already grouped
        into threads with resolved state."""
        owner, name = self.resolve_repo(repo)
        return self._paginated(f"/repos/{owner}/{name}/pulls/{number}/comments")

    def list_reviews(self, repo: str, number: int) -> list:
        """Formal reviews (APPROVE/REQUEST_CHANGES/COMMENT submissions),
        distinct from individual comments."""
        owner, name = self.resolve_repo(repo)
        return self._paginated(f"/repos/{owner}/{name}/pulls/{number}/reviews")

    def get_review_threads(self, repo: str, number: int) -> list:
        """Review comment threads as GitHub's UI shows them, via GraphQL —
        REST has no concept of thread grouping or resolved state, only a
        flat comment list. Each returned thread has: id (needed to
        resolve/unresolve), isResolved, path, line, and its comments."""
        owner, name = self.resolve_repo(repo)
        query = """
        query($owner: String!, $name: String!, $number: Int!, $after: String) {
          repository(owner: $owner, name: $name) {
            pullRequest(number: $number) {
              reviewThreads(first: 100, after: $after) {
                pageInfo { hasNextPage endCursor }
                nodes {
                  id
                  isResolved
                  isOutdated
                  path
                  line
                  comments(first: 50) {
                    nodes {
                      id
                      databaseId
                      body
                      author { login }
                      createdAt
                    }
                  }
                }
              }
            }
          }
        }
        """
        threads = []
        after = None
        while True:
            data = self.graphql(query, {"owner": owner, "name": name, "number": number, "after": after})
            rt = data["repository"]["pullRequest"]["reviewThreads"]
            threads.extend(rt["nodes"])
            if not rt["pageInfo"]["hasNextPage"]:
                break
            after = rt["pageInfo"]["endCursor"]
        return threads

    # ------------------------------------------------------------------
    # posting comments / reviews
    # ------------------------------------------------------------------

    def create_issue_comment(self, repo: str, number: int, body: str) -> dict:
        """Post a plain top-level comment on the PR. Use this for an
        overall review summary/report."""
        owner, name = self.resolve_repo(repo)
        result, _ = self._request(
            "POST", f"/repos/{owner}/{name}/issues/{number}/comments",
            json_body={"body": body},
        )
        return result

    def create_review_comment(self, repo: str, number: int, body: str,
                               commit_id: str, path: str, line: int,
                               side: str = "RIGHT", start_line: int | None = None,
                               start_side: str | None = None) -> dict:
        """Post a new inline comment anchored to a specific line of a
        specific file, creating a new thread immediately (not a pending
        review). `line`/`side` refer to the line as shown in the PR diff:
        side='RIGHT' for the new version of the file (added/unchanged
        lines), side='LEFT' for the old version (only valid for removed
        lines). For a multi-line comment, also pass start_line/start_side.
        `commit_id` should be the PR's current head SHA (from
        get_pull_request()['head']['sha']) so the comment lands on the
        latest diff."""
        owner, name = self.resolve_repo(repo)
        payload: dict[str, Any] = {
            "body": body, "commit_id": commit_id, "path": path,
            "line": line, "side": side,
        }
        if start_line is not None:
            payload["start_line"] = start_line
            payload["start_side"] = start_side or side
        result, _ = self._request(
            "POST", f"/repos/{owner}/{name}/pulls/{number}/comments",
            json_body=payload,
        )
        return result

    def reply_to_review_comment(self, repo: str, number: int, comment_id: int,
                                 body: str) -> dict:
        """Reply within an existing review thread. `comment_id` is a REST
        comment database ID (the numeric `id` from list_review_comments()
        or a thread's comments in get_review_threads(), i.e. `databaseId`
        there — not the GraphQL node `id` string)."""
        owner, name = self.resolve_repo(repo)
        result, _ = self._request(
            "POST",
            f"/repos/{owner}/{name}/pulls/{number}/comments/{comment_id}/replies",
            json_body={"body": body},
        )
        return result

    def resolve_review_thread(self, thread_id: str, resolved: bool = True) -> dict:
        """Resolve or unresolve a review thread. `thread_id` is the
        GraphQL node id from get_review_threads() (looks like
        'PRRT_kwDO...'), not a numeric comment id — REST has no resolve
        endpoint at all, so this always goes through GraphQL."""
        mutation_name = "resolveReviewThread" if resolved else "unresolveReviewThread"
        mutation = f"""
        mutation($threadId: ID!) {{
          {mutation_name}(input: {{threadId: $threadId}}) {{
            thread {{ id isResolved }}
          }}
        }}
        """
        data = self.graphql(mutation, {"threadId": thread_id})
        return data[mutation_name]["thread"]

    def submit_review(self, repo: str, number: int, body: str, event: str,
                       comments: list[dict] | None = None) -> dict:
        """Submit a formal review in one call — an overall verdict
        (APPROVE / REQUEST_CHANGES / COMMENT) plus, optionally, a batch of
        line comments attached to it at once. Prefer this over several
        separate create_review_comment() calls when posting a full review
        report with multiple inline points, since it lands as one atomic
        review instead of N separate notifications. Each item in
        `comments` needs at least path, line/side (or position for the
        legacy diff-position style), and body."""
        owner, name = self.resolve_repo(repo)
        payload: dict[str, Any] = {"body": body, "event": event}
        if comments:
            payload["comments"] = comments
        result, _ = self._request(
            "POST", f"/repos/{owner}/{name}/pulls/{number}/reviews",
            json_body=payload,
        )
        return result

    # ------------------------------------------------------------------
    # repository content
    # ------------------------------------------------------------------

    def get_file_content(self, repo: str, file_path: str, ref: str) -> str:
        """Fetch a file's decoded text content at a given branch/ref."""
        import base64
        owner, name = self.resolve_repo(repo)
        encoded_path = urllib.parse.quote(file_path)
        result, _ = self._request(
            "GET", f"/repos/{owner}/{name}/contents/{encoded_path}",
            params={"ref": ref},
        )
        if isinstance(result, list):
            raise GitHubAPIError(0, "GET", file_path, "Path is a directory, not a file.")
        if result.get("encoding") != "base64":
            raise GitHubAPIError(0, "GET", file_path, f"Unexpected encoding: {result.get('encoding')}")
        return base64.b64decode(result["content"]).decode("utf-8", errors="replace")

    def list_repo_tree(self, repo: str, ref: str, recursive: bool = False) -> list:
        owner, name = self.resolve_repo(repo)
        params = {"recursive": "1"} if recursive else {}
        result, _ = self._request(
            "GET", f"/repos/{owner}/{name}/git/trees/{urllib.parse.quote(ref)}",
            params=params,
        )
        return result.get("tree", [])

    def compare_refs(self, repo: str, base: str, head: str) -> dict:
        owner, name = self.resolve_repo(repo)
        result, _ = self._request(
            "GET", f"/repos/{owner}/{name}/compare/{urllib.parse.quote(base)}...{urllib.parse.quote(head)}",
        )
        return result


def load_client(repo: str | None = None) -> GitHubClient:
    """Convenience factory used by the CLI; raises GitHubConfigError with a
    clear message if setup is incomplete."""
    return GitHubClient(default_repo=repo)


if __name__ == "__main__":
    try:
        client = load_client()
        user, _ = client._request("GET", "/user")
        print(f"OK: authenticated to {client.api_url} as {user.get('login')}")
    except (GitHubConfigError, GitHubAPIError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        sys.exit(1)
