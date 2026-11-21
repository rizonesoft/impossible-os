# shellcheck shell=bash
# Sourced by .githooks/pre-push (TODO-08 section 48). git writes each ref
# update as "<local ref> <local oid> <remote ref> <remote oid>". The local ref
# is the source as typed and can hold a space (HEAD@{0 seconds ago}); the
# other three fields cannot. push_records prints stdin's records with the
# local ref's spaces written as %20, so a loop that reads four fields split
# on ASCII space alone (IFS=' ' read) reads the right ones; any other
# whitespace, a tab included, stays inside its field.
push_records() {
  local line head
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    head="${line% * * *}"
    printf '%s%s\n' "${head// /%20}" "${line#"$head"}"
  done
}
