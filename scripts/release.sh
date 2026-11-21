#!/usr/bin/env bash
# Build a production CellScope release through GitHub Actions.
#
# Usage:
#   ./scripts/release.sh --os All
#   ./scripts/release.sh --os macOS --bump patch
#   ./scripts/release.sh --os Linux --version 0.2.0
#
# The script updates CMakeLists.txt, README.md and the workflow's fallback
# version, commits and pushes them, syncs the Radio-Defense fork, dispatches the
# multi-platform workflow, and opens the resulting run in the browser.

set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

ORG="Radio-Defense"
UPSTREAM="maskirovka3301-oss/cellscope"
FORK="$ORG/cellscope"
WORKFLOW="cmake-multi-platform.yml"
BRANCH="main"
RUN_REPO="${RUN_REPO:-$UPSTREAM}"

OS="All"
VERSION_ARG=""
BUMP=""
OPEN_BROWSER=1

die() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: scripts/release.sh --os <macOS|Windows|Linux|All> [options]

Options:
  --os OS           Build target: macOS, Windows, Linux, or All (default: All)
  --version X.Y.Z   Explicit semantic version; otherwise use the current version
  --bump PART       Bump current version before release: major, minor, or patch
  --no-open         Do not open the GitHub Actions run in a browser
  -h, --help        Show this help

The script requires a clean main branch, an authenticated gh CLI, and owner
access to the Radio-Defense organisation. It pushes to the upstream repository,
syncs the Radio-Defense/cellscope fork, then dispatches the workflow from the
upstream repository. Set RUN_REPO=Radio-Defense/cellscope to dispatch from the
fork instead.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --os)
            [ "$#" -ge 2 ] || die "--os requires a value"
            OS="$2"
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] || die "--version requires a value"
            VERSION_ARG="$2"
            shift 2
            ;;
        --bump)
            [ "$#" -ge 2 ] || die "--bump requires a value"
            BUMP="$2"
            shift 2
            ;;
        --no-open)
            OPEN_BROWSER=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1 (use --help)"
            ;;
    esac
done

case "$OS" in
    macOS|macos) OS="macOS" ;;
    Windows|windows) OS="Windows" ;;
    Linux|linux) OS="Linux" ;;
    All|all) OS="All" ;;
    *) die "--os must be one of: macOS, Windows, Linux, All" ;;
esac

if [ -n "$VERSION_ARG" ] && [ -n "$BUMP" ]; then
    die "--version and --bump cannot be used together"
fi
if [ -n "$VERSION_ARG" ] && ! [[ "$VERSION_ARG" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    die "--version must be a semantic version such as 1.2.3"
fi
if [ -n "$BUMP" ] && ! [[ "$BUMP" =~ ^(major|minor|patch)$ ]]; then
    die "--bump must be major, minor, or patch"
fi

command -v git >/dev/null 2>&1 || die "git is required"
command -v gh >/dev/null 2>&1 || die "gh (GitHub CLI) is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"

[ "$(git branch --show-current)" = "$BRANCH" ] || die "release must run from the $BRANCH branch"
[ -z "$(git status --porcelain)" ] || die "working tree is dirty; commit or stash changes first"

CMakeLists="$ROOT/CMakeLists.txt"
README="$ROOT/README.md"
WORKFLOW_FILE="$ROOT/.github/workflows/$WORKFLOW"

current=$(sed -n 's/^project(CellScope VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\) LANGUAGES C)$/\1/p' "$CMakeLists" | head -n 1)
[ -n "$current" ] || die "could not read the project version from CMakeLists.txt"

VERSION="$current"
if [ -n "$VERSION_ARG" ]; then
    VERSION="$VERSION_ARG"
elif [ -n "$BUMP" ]; then
    IFS=. read -r major minor patch <<< "$current"
    case "$BUMP" in
        major) major=$((major + 1)); minor=0; patch=0 ;;
        minor) minor=$((minor + 1)); patch=0 ;;
        patch) patch=$((patch + 1)) ;;
    esac
    VERSION="$major.$minor.$patch"
fi

grep -q '\*\*Current version:\*\* `[^`]*`' "$README" ||
    die "README is missing the '**Current version:** \`x.y.z\`' marker"

if ! gh auth status >/dev/null 2>&1; then
    printf 'GitHub CLI is not authenticated; starting gh auth login...\n'
    gh auth login
fi
gh auth status >/dev/null 2>&1 || die "gh authentication failed"

role=$(gh api "user/memberships/orgs/$ORG" --jq '.role' 2>/dev/null || true)
[ "$role" = "admin" ] || die "the authenticated account is not an owner/admin of $ORG"

read -r is_fork parent_repo default_branch <<< "$(gh api "repos/$FORK" --jq '[.fork, .parent.full_name, .default_branch] | @tsv')"
[ "$is_fork" = "true" ] || die "$FORK is not a fork"
[ "$parent_repo" = "$UPSTREAM" ] || die "$FORK is not a fork of $UPSTREAM (found $parent_repo)"
[ "$default_branch" = "$BRANCH" ] || die "$FORK default branch is $default_branch, expected $BRANCH"

printf 'Preparing CellScope v%s for %s\n' "$VERSION" "$OS"

python3 - "$VERSION" "$CMakeLists" "$README" "$WORKFLOW_FILE" <<'PY'
from pathlib import Path
import re
import sys

version, cmake_path, readme_path, workflow_path = sys.argv[1:]
cmake = Path(cmake_path)
readme = Path(readme_path)
workflow = Path(workflow_path)

text = cmake.read_text()
text, n = re.subn(
    r"^project\(CellScope VERSION \d+\.\d+\.\d+ LANGUAGES C\)$",
    f"project(CellScope VERSION {version} LANGUAGES C)",
    text,
    count=1,
    flags=re.MULTILINE,
)
if n != 1:
    raise SystemExit("could not update project version in CMakeLists.txt")
cmake.write_text(text)

text = readme.read_text()
text, n = re.subn(
    r"(\*\*Current version:\*\* `)[^`]+(`)",
    rf"\g<1>{version}\g<2>",
    text,
    count=1,
)
if n != 1:
    raise SystemExit("could not update README version marker")
readme.write_text(text)

text = workflow.read_text()
text = re.sub(r"v\d+\.\d+\.\d+", f"v{version}", text)
workflow.write_text(text)
PY

git diff --check

git add "$CMakeLists" "$README" "$WORKFLOW_FILE"
if git diff --cached --quiet; then
    printf 'Version is already %s; no version commit needed.\n' "$VERSION"
else
    git commit -m "Release v$VERSION [skip ci]"
fi

printf 'Pushing %s...\n' "$BRANCH"
git push origin "$BRANCH"

printf 'Syncing %s from %s...\n' "$FORK" "$UPSTREAM"
gh repo sync "$FORK" --source "$UPSTREAM" --branch "$BRANCH"

ours=$(git rev-parse HEAD)
fork_sha=$(gh api "repos/$FORK/commits/$BRANCH" --jq '.sha')
[ "$fork_sha" = "$ours" ] || die "fork did not sync to $ours (fork is $fork_sha)"

before=$(gh run list --repo "$RUN_REPO" --workflow "$WORKFLOW" --event workflow_dispatch \
    --limit 1 --json databaseId --jq '.[0].databaseId // 0')

OS_INPUT="$OS"
[ "$OS" = "All" ] && OS_INPUT="all"

printf 'Dispatching %s on %s for %s...\n' "$WORKFLOW" "$RUN_REPO" "$OS"
gh workflow run "$WORKFLOW" --repo "$RUN_REPO" --ref "$BRANCH" \
    -f os="$OS_INPUT" -f version="v$VERSION"

run_url=""
for _ in $(seq 1 45); do
    line=$(gh run list --repo "$RUN_REPO" --workflow "$WORKFLOW" \
        --event workflow_dispatch --commit "$ours" --limit 5 \
        --json databaseId,url --jq '.[] | [.databaseId, .url] | @tsv' 2>/dev/null |
        head -n 1 || true)
    if [ -n "$line" ]; then
        run_id=${line%%$'\t'*}
        candidate_url=${line#*$'\t'}
        if [ "$run_id" -gt "$before" ] 2>/dev/null; then
            run_url="$candidate_url"
            break
        fi
    fi
    sleep 2
done

[ -n "$run_url" ] || die "workflow dispatched, but the run URL was not found yet; open https://github.com/$RUN_REPO/actions"

printf 'Release workflow: %s\n' "$run_url"
if [ "$OPEN_BROWSER" = 1 ]; then
    case "$(uname -s)" in
        Darwin) open "$run_url" ;;
        Linux) command -v xdg-open >/dev/null 2>&1 && xdg-open "$run_url" >/dev/null 2>&1 || true ;;
        MINGW*|MSYS*) start "" "$run_url" >/dev/null 2>&1 || true ;;
    esac
fi
