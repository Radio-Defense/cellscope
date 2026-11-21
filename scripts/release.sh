#!/usr/bin/env bash
# Build a CellScope release through GitHub Actions.
#
# Normal commits bump the patch version via .githooks/pre-commit.
# Running this script bumps the minor version.
# Adding --publish bumps the major version and marks the release production-ready.

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
PUBLISH=0
OPEN_BROWSER=1

die() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: scripts/release.sh --os <macOS|Windows|Linux|All> [--publish] [--no-open]

Options:
  --os OS      Build target: macOS, Windows, Linux, or All (default: All)
  --publish    Bump major version and label the release production-ready
  --no-open    Do not open the GitHub Actions run in a browser
  -h, --help   Show this help

Without --publish, the minor version is bumped and the release is marked
non-production ready. Install the patch-bump hook once with:

  git config core.hooksPath .githooks
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --os)
            [ "$#" -ge 2 ] || die "--os requires a value"
            OS="$2"
            shift 2
            ;;
        --publish)
            PUBLISH=1
            shift
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

command -v git >/dev/null 2>&1 || die "git is required"
command -v gh >/dev/null 2>&1 || die "gh (GitHub CLI) is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"

[ "$(git branch --show-current)" = "$BRANCH" ] || die "release must run from the $BRANCH branch"
[ -z "$(git status --porcelain)" ] || die "working tree is dirty; commit or stash changes first"

git config core.hooksPath .githooks

current=$(python3 "$ROOT/scripts/version.py" read)
[ -n "$current" ] || die "could not read the current project version"

IFS=. read -r major minor patch <<< "$current"
if [ "$PUBLISH" = 1 ]; then
    VERSION="$((major + 1)).0.0"
else
    VERSION="$major.$((minor + 1)).0"
fi

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

printf 'Preparing CellScope v%s for %s (%s)\n' \
    "$VERSION" "$OS" "$([ "$PUBLISH" = 1 ] && echo production || echo non-production)"

python3 "$ROOT/scripts/version.py" set "$VERSION" >/dev/null
git diff --check
git add "$ROOT/CMakeLists.txt" "$ROOT/README.md" \
    "$ROOT/.github/workflows/$WORKFLOW"

if git diff --cached --quiet; then
    printf 'Version is already %s; no version commit needed.\n' "$VERSION"
else
    CELLSCOPE_SKIP_VERSION_BUMP=1 git commit -m "Release v$VERSION [skip ci]"
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

PUBLISH_INPUT=false
[ "$PUBLISH" = 1 ] && PUBLISH_INPUT=true

printf 'Dispatching %s on %s for %s...\n' "$WORKFLOW" "$RUN_REPO" "$OS"
gh workflow run "$WORKFLOW" --repo "$RUN_REPO" --ref "$BRANCH" \
    -f os="$OS_INPUT" -f version="v$VERSION" -f publish="$PUBLISH_INPUT"

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
