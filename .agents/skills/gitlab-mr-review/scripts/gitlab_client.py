"""
Minimal GitLab REST API client, stdlib-only (no pip installs required).

Auth is read from a .env file (GITLAB_URL, GITLAB_TOKEN, optional
GITLAB_PROJECT_ID) so this works the same way in every project without
hardcoding credentials anywhere. See find_and_load_env() for the search
order.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterable


class GitLabConfigError(RuntimeError):
    """Raised when GITLAB_URL / GITLAB_TOKEN can't be found or are invalid."""


class GitLabAPIError(RuntimeError):
    """Raised when the GitLab API returns an error response."""

    def __init__(self, status: int, method: str, url: str, body: str):
        self.status = status
        self.method = method
        self.url = url
        self.body = body
        super().__init__(f"GitLab API {method} {url} -> HTTP {status}: {body[:500]}")


def find_and_load_env(start_dir: str | None = None) -> dict[str, str]:
    """
    Look for a .env file starting at start_dir (default: current working
    directory) and walking up through parent directories until one is
    found or the filesystem root is reached. This mirrors how most
    dotenv-style tools behave, so the skill works no matter which
    subdirectory of a project the agent happens to be running in.

    Returns the parsed key/value pairs. Values already set in the real
    process environment take precedence over the .env file, so a user
    can override GITLAB_URL/GITLAB_TOKEN with real env vars if they want.
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

    # Real environment variables win over .env file values.
    for key in ("GITLAB_URL", "GITLAB_TOKEN", "GITLAB_PROJECT_ID"):
        if os.environ.get(key):
            env[key] = os.environ[key]

    return env


class GitLabClient:
    def __init__(self, base_url: str | None = None, token: str | None = None,
                 default_project: str | None = None, timeout: int = 30):
        env = find_and_load_env()

        self.base_url = (base_url or env.get("GITLAB_URL") or "").rstrip("/")
        self.token = token or env.get("GITLAB_TOKEN")
        self.default_project = default_project or env.get("GITLAB_PROJECT_ID")
        self.timeout = timeout

        if not self.base_url:
            raise GitLabConfigError(
                "GITLAB_URL is not set. Add it to a .env file in the project "
                "root (e.g. GITLAB_URL=https://gitlab.example.com) or export "
                "it as an environment variable."
            )
        if not self.token:
            raise GitLabConfigError(
                "GITLAB_TOKEN is not set. Add a personal/project access "
                "token with `api` scope to a .env file (GITLAB_TOKEN=glpat-...) "
                "or export it as an environment variable."
            )

        self.api_root = f"{self.base_url}/api/v4"

    # ------------------------------------------------------------------
    # low-level request handling
    # ------------------------------------------------------------------

    def _request(self, method: str, path: str, params: dict | None = None,
                 json_body: Any = None, raw: bool = False,
                 max_retries: int = 3) -> Any:
        query = f"?{urllib.parse.urlencode(params)}" if params else ""
        url = f"{self.api_root}{path}{query}"

        data = None
        headers = {"PRIVATE-TOKEN": self.token}
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
                        return None
                    return json.loads(body)
            except urllib.error.HTTPError as e:
                err_body = e.read().decode("utf-8", errors="replace")
                # Retry on rate-limiting / transient server errors only.
                if e.code == 429 or e.code >= 500:
                    last_error = GitLabAPIError(e.code, method, url, err_body)
                    time.sleep(min(2 ** attempt, 8))
                    continue
                raise GitLabAPIError(e.code, method, url, err_body) from e
            except urllib.error.URLError as e:
                raise GitLabConfigError(
                    f"Could not reach {self.base_url} ({e.reason}). "
                    "Check GITLAB_URL and your network/VPN access."
                ) from e

        raise last_error  # exhausted retries on a transient error

    def _paginated(self, path: str, params: dict | None = None,
                   max_pages: int = 50) -> list:
        """GitLab list endpoints are paginated (default 20/page, max 100).
        Fetch every page up to max_pages so callers get complete results
        without needing to think about pagination themselves."""
        params = dict(params or {})
        params.setdefault("per_page", 100)
        results: list = []
        page = 1
        while page <= max_pages:
            params["page"] = page
            batch = self._request("GET", path, params=params)
            if not batch:
                break
            results.extend(batch)
            if len(batch) < params["per_page"]:
                break
            page += 1
        return results

    # ------------------------------------------------------------------
    # project resolution
    # ------------------------------------------------------------------

    def resolve_project(self, project: str | None) -> str:
        """GitLab accepts either the numeric project ID or the URL-encoded
        namespace/path (e.g. 'group%2Fsubgroup%2Frepo'). This accepts a
        human-typed 'group/subgroup/repo' or a numeric ID or already-encoded
        string and returns the URL-safe form to drop into a path."""
        project = project or self.default_project
        if not project:
            raise GitLabConfigError(
                "No project specified and GITLAB_PROJECT_ID is not set. "
                "Pass --project <id-or-path> or set GITLAB_PROJECT_ID in .env."
            )
        if project.isdigit():
            return project
        if "%2F" in project:
            return project
        return urllib.parse.quote(project, safe="")

    # ------------------------------------------------------------------
    # merge requests: metadata, diffs, discussions
    # ------------------------------------------------------------------

    def get_merge_request(self, project: str, mr_iid: int) -> dict:
        pid = self.resolve_project(project)
        return self._request("GET", f"/projects/{pid}/merge_requests/{mr_iid}")

    def get_merge_request_changes(self, project: str, mr_iid: int) -> dict:
        """Returns MR metadata plus the full list of per-file diffs."""
        pid = self.resolve_project(project)
        return self._request(
            "GET", f"/projects/{pid}/merge_requests/{mr_iid}/changes",
            params={"access_raw_diffs": "false"},
        )

    def get_merge_request_diffs(self, project: str, mr_iid: int) -> list:
        """Same diff content as get_merge_request_changes but paginated;
        prefer this for MRs with a very large number of changed files."""
        pid = self.resolve_project(project)
        return self._paginated(f"/projects/{pid}/merge_requests/{mr_iid}/diffs")

    def get_merge_request_commits(self, project: str, mr_iid: int) -> list:
        pid = self.resolve_project(project)
        return self._paginated(f"/projects/{pid}/merge_requests/{mr_iid}/commits")

    def list_discussions(self, project: str, mr_iid: int) -> list:
        """All threads on the MR, each containing its notes/comments in
        chronological order, including resolved state for review threads."""
        pid = self.resolve_project(project)
        return self._paginated(f"/projects/{pid}/merge_requests/{mr_iid}/discussions")

    def list_notes(self, project: str, mr_iid: int) -> list:
        """Flat list of all notes (comments) on the MR, including system
        notes (e.g. 'changed the description'). Usually list_discussions()
        is more useful since it groups notes into threads."""
        pid = self.resolve_project(project)
        return self._paginated(f"/projects/{pid}/merge_requests/{mr_iid}/notes")

    # ------------------------------------------------------------------
    # merge requests: posting comments / threads
    # ------------------------------------------------------------------

    def create_note(self, project: str, mr_iid: int, body: str) -> dict:
        """Post a plain top-level comment on the MR (not tied to a diff
        line). Use this for an overall review summary/report."""
        pid = self.resolve_project(project)
        return self._request(
            "POST", f"/projects/{pid}/merge_requests/{mr_iid}/notes",
            json_body={"body": body},
        )

    def create_discussion(self, project: str, mr_iid: int, body: str,
                           position: dict | None = None) -> dict:
        """Start a new discussion thread. If `position` is given, the
        thread is anchored to a specific line in the diff (see
        build_diff_position() for how to construct it) — this is what
        makes a comment show up inline on the changed line, the way a
        human reviewer's inline comment would. Without `position` this
        behaves like a regular top-level note but still creates a
        resolvable/repliable thread."""
        pid = self.resolve_project(project)
        payload: dict[str, Any] = {"body": body}
        if position:
            payload["position"] = position
        return self._request(
            "POST", f"/projects/{pid}/merge_requests/{mr_iid}/discussions",
            json_body=payload,
        )

    def reply_to_discussion(self, project: str, mr_iid: int, discussion_id: str,
                             body: str) -> dict:
        pid = self.resolve_project(project)
        return self._request(
            "POST",
            f"/projects/{pid}/merge_requests/{mr_iid}/discussions/{discussion_id}/notes",
            json_body={"body": body},
        )

    def resolve_discussion(self, project: str, mr_iid: int, discussion_id: str,
                            resolved: bool = True) -> dict:
        pid = self.resolve_project(project)
        return self._request(
            "PUT",
            f"/projects/{pid}/merge_requests/{mr_iid}/discussions/{discussion_id}",
            json_body={"resolved": resolved},
        )

    def build_diff_position(self, changes: dict, file_path: str,
                             new_line: int | None = None,
                             old_line: int | None = None) -> dict:
        """Build the `position` object required to anchor a discussion to a
        specific line of a specific file in the diff. `changes` must be the
        dict returned by get_merge_request_changes() for the same MR — it
        carries the base/head/start SHAs GitLab needs to place the comment
        correctly even as the MR's diff evolves.

        Pass new_line for a line added/unchanged in the new version of the
        file, or old_line for a line only present in the old version
        (i.e. commenting on a deletion). Provide exactly one of the two."""
        if (new_line is None) == (old_line is None):
            raise ValueError("Provide exactly one of new_line or old_line")

        diff_refs = changes.get("diff_refs") or {}
        if not diff_refs:
            raise GitLabAPIError(
                0, "GET", "changes",
                "Response has no diff_refs; cannot build a line position.",
            )

        position = {
            "position_type": "text",
            "base_sha": diff_refs.get("base_sha"),
            "start_sha": diff_refs.get("start_sha"),
            "head_sha": diff_refs.get("head_sha"),
            "old_path": file_path,
            "new_path": file_path,
        }
        if new_line is not None:
            position["new_line"] = new_line
        else:
            position["old_line"] = old_line
        return position

    # ------------------------------------------------------------------
    # repository content (for pulling extra context beyond the diff)
    # ------------------------------------------------------------------

    def get_file_content(self, project: str, file_path: str, ref: str) -> str:
        """Fetch the full raw content of a file at a given branch/commit ref.
        Useful when a diff hunk alone doesn't give the reviewer enough
        context (e.g. needing to see a function's full body, or an
        untouched file that the changed code calls into)."""
        pid = self.resolve_project(project)
        encoded_path = urllib.parse.quote(file_path, safe="")
        body, _headers = self._request(
            "GET", f"/projects/{pid}/repository/files/{encoded_path}/raw",
            params={"ref": ref}, raw=True,
        )
        return body.decode("utf-8", errors="replace")

    def list_repo_tree(self, project: str, ref: str, path: str = "",
                        recursive: bool = False) -> list:
        """List files/directories in the repo at a given branch/ref."""
        pid = self.resolve_project(project)
        params = {"ref": ref, "recursive": str(recursive).lower()}
        if path:
            params["path"] = path
        return self._paginated(f"/projects/{pid}/repository/tree", params=params)

    def compare_refs(self, project: str, from_ref: str, to_ref: str) -> dict:
        """Diff between any two branches/commits/tags — handy for reviewing
        a branch against its target before an MR exists yet, or for
        double-checking what an MR's diff will look like."""
        pid = self.resolve_project(project)
        return self._request(
            "GET", f"/projects/{pid}/repository/compare",
            params={"from": from_ref, "to": to_ref},
        )


def load_client(project: str | None = None) -> GitLabClient:
    """Convenience factory used by the CLI; raises GitLabConfigError with a
    clear message if setup is incomplete."""
    return GitLabClient(default_project=project)


if __name__ == "__main__":
    # Lightweight self-check: confirm .env can be found and the token can
    # authenticate, without needing a specific MR/project.
    try:
        client = load_client()
        user = client._request("GET", "/user")
        print(f"OK: authenticated to {client.base_url} as {user.get('username')}")
    except (GitLabConfigError, GitLabAPIError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        sys.exit(1)
