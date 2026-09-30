#!/usr/bin/env bash
# generate-changelog.sh -- Generate CHANGELOG.md from git history
#
# Usage:
#   bash scripts/generate-changelog.sh              # Full history (all tags)
#   bash scripts/generate-changelog.sh v0.1.0       # Since specific tag
#   bash scripts/generate-changelog.sh v0.1.0 HEAD  # Between two refs
#
# Groups commits by conventional commit prefix into categories.
# Output is written to CHANGELOG.md at the repo root.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
CHANGELOG="$ROOT/CHANGELOG.md"

# Category mapping: prefix → display name
declare -A CATEGORIES=(
    [feat]="Features"
    [feature]="Features"
    [desktop]="Desktop & UI"
    [gfx]="Graphics"
    [kernel]="Kernel"
    [boot]="Bootloader"
    [drivers]="Drivers"
    [fs]="Filesystem"
    [net]="Networking"
    [mm]="Memory"
    [sched]="Scheduler"
    [ipc]="IPC"
    [fix]="Bug Fixes"
    [bugfix]="Bug Fixes"
    [build]="Build System"
    [ci]="CI/CD"
    [docs]="Documentation"
    [agent]="Agent & Tooling"
    [tools]="Agent & Tooling"
    [license]="Documentation"
    [refactor]="Refactoring"
    [perf]="Performance"
    [test]="Testing"
    [chore]="Maintenance"
    [release]="Releases"
)

# Display order for categories
DISPLAY_ORDER=(
    "Features"
    "Kernel"
    "Bootloader"
    "Memory"
    "Scheduler"
    "Filesystem"
    "Drivers"
    "Networking"
    "IPC"
    "Desktop & UI"
    "Graphics"
    "Bug Fixes"
    "Performance"
    "Refactoring"
    "Build System"
    "CI/CD"
    "Agent & Tooling"
    "Documentation"
    "Testing"
    "Maintenance"
    "Releases"
    "Other"
)

# Get all release tags sorted by date (newest first). Only v* tags are releases:
# docs-releases-root (scripts/site/releases.py) marks the orphan docs store and
# would otherwise become a false section and a range boundary into foreign history.
get_tags() {
    git tag --list 'v*' --sort=-creatordate 2>/dev/null
}

# Generate a section for a range of commits
generate_section() {
    local from="$1"
    local to="$2"
    local title="$3"

    # Associative array: category → entries
    declare -A entries

    while IFS='|' read -r hash subject; do
        [ -z "$hash" ] && continue

        # Extract prefix (everything before first colon)
        local prefix="${subject%%:*}"
        prefix="${prefix,,}"            # lowercase
        prefix="${prefix#"${prefix%%[![:space:]]*}"}"   # trim leading
        prefix="${prefix%"${prefix##*[![:space:]]}"}"   # trim trailing

        # Look up category
        local category="${CATEGORIES[$prefix]:-Other}"

        # Extract description (everything after the colon)
        local desc="${subject#*:}"
        desc=$(echo "$desc" | sed 's/^ *//')

        # Skip empty descriptions
        [ -z "$desc" ] && desc="$subject"

        local short_hash="${hash:0:7}"
        local entry="- ${desc} (\`${short_hash}\`)"

        if [ -n "${entries[$category]+x}" ]; then
            entries[$category]="${entries[$category]}"$'\n'"$entry"
        else
            entries[$category]="$entry"
        fi
    # tformat, not format: format emits no newline after the last commit, so
    # `read` dropped the oldest commit of every section (a one-commit release
    # came out empty).
    done < <(git log --pretty=tformat:"%H|%s" "$from".."$to" 2>/dev/null)

    # Check if any entries exist
    local has_entries=0
    for cat in "${DISPLAY_ORDER[@]}"; do
        [ -n "${entries[$cat]+x}" ] && has_entries=1 && break
    done
    [ "$has_entries" = "0" ] && return

    echo "$title"
    echo ""

    for cat in "${DISPLAY_ORDER[@]}"; do
        if [ -n "${entries[$cat]+x}" ]; then
            echo "### $cat"
            echo ""
            echo "${entries[$cat]}"
            echo ""
        fi
    done
}

# --- Main ---

echo "Generating changelog..."

{
    echo "# Changelog"
    echo ""
    echo "> Auto-generated from git history by \`scripts/generate-changelog.sh\`."
    echo "> Commits are grouped by conventional commit prefix."
    echo ""
    echo "---"
    echo ""

    tags=($(get_tags))

    if [ ${#tags[@]} -gt 0 ]; then
        # Generate section for HEAD → latest tag (unreleased)
        latest_tag="${tags[0]}"
        unreleased_count=$(git rev-list --count "$latest_tag"..HEAD 2>/dev/null || echo "0")

        if [ "$unreleased_count" -gt 0 ]; then
            current_date=$(date '+%Y-%m-%d')
            generate_section "$latest_tag" "HEAD" "## Unreleased (${current_date})"
            echo "---"
            echo ""
        fi

        # Generate sections between tags
        for ((i = 0; i < ${#tags[@]} - 1; i++)); do
            newer="${tags[$i]}"
            older="${tags[$i+1]}"
            tag_date=$(git log -1 --format='%ai' "$newer" 2>/dev/null | cut -d' ' -f1)
            generate_section "$older" "$newer" "## ${newer} (${tag_date})"
            echo "---"
            echo ""
        done

        # Generate section for oldest tag → initial commit
        oldest_tag="${tags[-1]}"
        tag_date=$(git log -1 --format='%ai' "$oldest_tag" 2>/dev/null | cut -d' ' -f1)
        first_commit=$(git rev-list --max-parents=0 HEAD)
        generate_section "$first_commit" "$oldest_tag" "## ${oldest_tag} (${tag_date})"
    else
        # No tags -- generate full history
        current_date=$(date '+%Y-%m-%d')
        first_commit=$(git rev-list --max-parents=0 HEAD)
        generate_section "$first_commit" "HEAD" "## Development (${current_date})"
    fi

} > "$CHANGELOG"

lines=$(wc -l < "$CHANGELOG")
echo "✅ Generated $CHANGELOG ($lines lines)"
