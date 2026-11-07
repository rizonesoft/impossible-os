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
git archive --format=tar HEAD > "$TMP/head.tar"
printf '%s' "$SETUP" > "$TMP/setup.sh"
cat > "$TMP/run.sh" <<EOF
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq sudo git ca-certificates >/dev/null
bash /ci/setup.sh
useradd -m ci
mkdir /home/ci/w
tar -xf /ci/head.tar -C /home/ci/w
chown -R ci /home/ci/w
su ci -c '
  set -euo pipefail
  cd /home/ci/w
  git init -q && git -c user.name=ci -c user.email=ci@ci.invalid add -A
  git -c user.name=ci -c user.email=ci@ci.invalid commit -qm "ci-container snapshot"
  bash scripts/setup.sh --verify
  bash scripts/lint.sh
  $([ "$TOOLING" = 1 ] && echo 'bash scripts/test-tooling.sh --quiet')
  bash scripts/build.sh clean
  CI_PARITY=1 QUIET=1 bash scripts/test.sh QUIET=1
'
EOF
mkdir -p build
echo "ci-container: $IMAGE, HEAD $(git rev-parse --short HEAD), log build/ci-container.log"
rc=0
timeout 4500 docker run --rm -v "$TMP:/ci:ro" "$IMAGE" bash /ci/run.sh > build/ci-container.log 2>&1 || rc=$?
tail -5 build/ci-container.log
echo "ci-container: rc=$rc"
exit "$rc"
