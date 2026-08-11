# Upstream synchronization

The remotes for a normal checkout are:

```text
origin        https://github.com/AELovelace/Doll-OS-Tab5.git
upstream-fnk  https://github.com/AELovelace/Doll-OS-FNK0104.git
```

The scheduled workflow creates or refreshes a draft pull request from
`automation/upstream-fnk` into `main`. It merges the complete FNK history; it
does not cherry-pick, rebase, or auto-merge the result.

## Conflict policy

Keep shared files as close to FNK as practical. Put Tab5 hardware behavior in
narrow backends and avoid unrelated formatting changes. During a sync:

1. Resolve shared-logic conflicts in favor of the new FNK behavior.
2. Reapply the Tab5 behavior at the hardware boundary.
3. Compile the root `tab5` Arduino profile and run input/storage smoke tests.
4. Record any intentionally omitted FNK feature in the pull request.
5. Merge the pull request with a merge commit so the next merge base is clear.

Fixes that apply to both products should be committed to FNK first. A later
upstream merge then carries the same commit into Tab5 without maintaining two
independent versions.

## Manual sync

```powershell
git fetch upstream-fnk main
git switch -c sync/fnk-YYYYMMDD origin/main
git merge --no-ff upstream-fnk/main
```

Never force-push `main`. The automation branch may be replaced by the scheduled
workflow because it exists only to stage the next reviewed merge.
