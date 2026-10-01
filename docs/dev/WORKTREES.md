# Git worktrees for isolated work

A worker can develop in its own checkout instead of the shared one, and the
integrator merges the result. Opt-in per task; the shared checkout and its
READY flow are unchanged.

## Convention

- Worktrees live in `<repo-parent>/petari-worktrees/<name>` (a sibling of the
  repository, never under `~/Space`), on branch `wt/<name>`, created from `main`.
- At most **3** at a time: disk is limited and a build directory is about 3.5 GiB.
- Only committed work on `main` is in a new worktree. Uncommitted changes in the
  main checkout are not.
- Create with `build/new-worktree.sh <name> [base-ref]` and remove with
  `build/remove-worktree.sh <name>` (both untracked dev scripts in the main
  checkout's `build/`, next to the lock scripts).

## What the helper sets up

- The worktree and branch (`git worktree add -b wt/<name>`).
- `build/game-data` in the worktree: a symlink to the main checkout's disc data.
- `build/macos-gx` configured with the same options as the README, reusing the
  main checkout's already-fetched sources (Aurora, SDL, Dawn prebuilt, ImGui)
  through `FETCHCONTENT_SOURCE_DIR_*`, so nothing is downloaded again. Only these
  immutable source trees are shared; the `*-build` outputs stay per worktree,
  because two trees must not write the same Aurora objects.

## Building and running

Heavy CPU (configure, build, ctest, apps) still goes through the **main
checkout's** lock scripts, called by absolute path from the worktree directory,
so the machine-wide limits hold. The lock scripts keep their state next to
themselves, in the main checkout's `build/`, whatever the current directory is.

```sh
cd <repo-parent>/petari-worktrees/<name>
<main>/build/locked-build.sh <name> cmake --build build/macos-gx -j6 --target petari
<main>/build/locked-build.sh <name> ctest --test-dir build/macos-gx -j4 --timeout 600 -LE timing
PETARI_LOCK_CLASS=functional <main>/build/locked-app.sh <name> <command that runs the app>
```

The default `cmake --build` does not relink the app: build `--target petari`.

## Committing and merging

1. Commit on `wt/<name>` as usual (path-limited commits are fine in your own tree).
2. Send the integrator `MERGE-READY: wt/<name>; tests: ...`.
3. The integrator merges into `main` (`git merge --no-ff`, or a rebase when it is
   trivial), builds, runs the targeted tests and a broader `ctest -LE timing`,
   and resolves trivial conflicts. Behavioural conflicts go back to the owner.
4. After a successful merge the worktree is removed (`git worktree remove`) and
   the branch deleted (`git branch -d`). `main`'s history is never forced or
   rewritten.
