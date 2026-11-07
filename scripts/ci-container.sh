#!/usr/bin/env bash
# ci-container.sh [--tooling] -- run the suites from a clean clone of HEAD inside the
# pinned CI image, before a push. `CI_CONTAINER=1 bash scripts/test.sh` lands here.
#
# CI_PARITY=1 pins QEMU and the engine on THIS host. This tier pins the rest: the OS
# image, the packages `build.yml` installs, and a tree with no build leftovers. It
# would have caught the `apt-key` removal and the clean-only link failures locally.
# It predicts the verdict; CI stays the authority.
#
# HEAD, not the working tree: it tests what the push will carry, so commit first.
# Output goes to build/ci-container.log. Bounded at 75 minutes.
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
TOOLING=0
[ "${1:-}" = "--tooling" ] && TOOLING=1
command -v docker >/dev/null || { echo "ci-container: docker is required" >&2; exit 2; }

IMAGE="$(python3 - <<'PY'
import yaml
print(yaml.safe_load(open(".github/workflows/build.yml"))["jobs"]["build"]["runs-on"].replace("-", ":", 1))
PY
)"
SETUP="$(python3 scripts/ci_container_setup.py)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# A full-history bundle, as CI's `fetch-depth: 0` has: lint and the site build judge
# changes against history, and a one-commit snapshot reads every old line as new.
git bundle create -q "$TMP/head.bundle" HEAD main
printf '%s' "$SETUP" > "$TMP/setup.sh"
cat > "$TMP/run.sh" <<EOF
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq sudo git ca-certificates >/dev/null
bash /ci/setup.sh
useradd -m ci
su ci -c '
  set -euo pipefail
  git clone -q --branch main /ci/head.bundle /home/ci/w
  cd /home/ci/w
  git config user.name ci
  git config user.email ci@ci.invalid
  bash scripts/setup.sh --verify
  bash scripts/lint.sh
  $([ "$TOOLING" = 1 ] && echo 'bash scripts/test-tooling.sh --quiet')
  bash scripts/build.sh clean
  CI_PARITY=1 QUIET=1 bash scripts/test.sh QUIET=1
'
EOF
# mktemp -d is 0700, and the suites run as an unprivileged user, as CI's do.
chmod -R a+rX "$TMP"
mkdir -p build
echo "ci-container: $IMAGE, HEAD $(git rev-parse --short HEAD), log build/ci-container.log"
rc=0
timeout 4500 docker run --rm -v "$TMP:/ci:ro" "$IMAGE" bash /ci/run.sh > build/ci-container.log 2>&1 || rc=$?
tail -5 build/ci-container.log
echo "ci-container: rc=$rc"
exit "$rc"
