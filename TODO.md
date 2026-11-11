# aslgc TODO

The road to a complete desktop Git client. Every operation runs the `git` CLI.

Legend: `[x]` done · `[ ]` to do · size **S** < 1 day, **M** 1–3 days, **L** about a week

---

## ★ High priority
- [x] **git-flow**
  - Set up (production / develop branches, prefixes, version tag prefix), edit settings, remove
  - Start feature / bugfix / release / hotfix, with a suggested next version for releases and hotfixes
  - Finish: merge back, tag releases and hotfixes, delete the branch, optionally push; re-running Finish after resolving a conflict picks up where it stopped
  - Uses the `gitflow.*` settings of the git-flow (AVH) command line tool, so both work side by side
  - Toolbar button, Repository menu and sidebar branch menus
- [x] git-flow: support branches and fetch before finishing
  - Support branches start from a release (a version tag or a production commit) and stay open; their hotfixes go back only into them and continue their version line
  - The branch a flow branch starts from is recorded (`gitflow.branch.<name>.base`), so a feature started from another feature finishes back into it
  - Finish can fetch first; branches behind their remote copy are fast-forwarded before merging, diverged ones stop the finish with a hint to pull

## 0. Housekeeping
- [x] Connected to github.com/kyleschouw/aslgc (working copy = project root)
- [x] First commit and push
- [x] MIT license, third-party notices, SPDX headers, `.clang-format`, `.editorconfig`
- [x] README for users and packagers, CONTRIBUTING, CHANGELOG, screenshots
- [x] CI: build, tests, UI smoke test (offscreen screenshots) and staged install on Arch, Fedora, Debian 13, Ubuntu 24.04
- [x] Packaging metadata: reverse-DNS app id, desktop entry, AppStream metainfo (validates cleanly), man page, relocatable libexec helper
- [x] Arch `PKGBUILD` (`packaging/arch`, builds the latest commit) — publish to the AUR after the first push
- [ ] Flatpak manifest (needs QtKeychain and git modules) and AppImage — M
- [x] Translations with Qt Linguist: `i18n/*.ts` compiled and embedded, English plural forms, Qt's own translations loaded
- [ ] First real translations (any language) — S each
- [x] Log file (`~/.local/state/aslgc/aslgc.log`) + Help → Open Log Folder

## 1. Core: done ✔
- [x] Repository tabs, session restore, recent repositories, welcome page
- [x] Open, clone (progress, recursive submodules), init
- [x] Auto-refresh on window focus and on `.git` changes
- [x] Open repository in terminal / file manager
- [x] Unstaged / staged lists; stage, unstage, discard files (untracked files go to trash)
- [x] Stage / unstage / discard **hunks** and **selected lines**
- [x] Commit, amend, "stage all & commit"
- [x] Ignore file or extension via `.gitignore`
- [x] Conflict view; resolve with ours / theirs / mark resolved / external merge tool
- [x] Merge / rebase / cherry-pick / revert banner with Continue / Skip / Abort
- [x] Commit graph with lanes, ref badges, HEAD marker; commit details, files, diff
- [x] Find commit by message / author / SHA
- [x] Checkout commit, new branch / tag here, cherry-pick, revert, reset (soft/mixed/hard), merge (ff/no-ff/ff-only/squash), rebase, save as patch, copy SHA / info
- [x] Sidebar: branches (folders, ahead/behind, upstream gone), remotes, tags, stashes
- [x] Branches: checkout (local + remote with tracking), create, rename, delete (+ upstream), fast-forward without checkout, set upstream
- [x] Fetch all / per remote, pull (merge or rebase, autostash), push (upstream, tags, force-with-lease)
- [x] Remotes: add, edit URL, remove; delete remote branches
- [x] Tags: create (lightweight / annotated), push, delete (local + remote), checkout
- [x] Stash (untracked, keep index), apply, pop, drop, save as patch
- [x] Credential prompts via `aslgc-askpass`; system credential helpers still apply
- [x] Follows the system light/dark palette; bundled MIT icon set (Tabler Icons) tinted to match
- [x] GitHub / GitHub Enterprise / GitLab / self-managed GitLab accounts (personal access token, kept in the system keyring)
- [x] HTTPS fetch / pull / push authenticate with the account token automatically
- [x] GitHub browser sign-in via OAuth device flow (enabled when built with a Client ID)
- [x] Clone dialog: pick a repository from a signed-in account
- [x] Create a pull request (GitHub) or merge request (GitLab) from a branch, prefilled from its commits; browser fallback without an account
- [x] Open repository / branch / commit on GitHub or GitLab

## 2. History power tools
- [x] Interactive rebase editor: drag or Alt+↑/↓ to reorder; pick / reword / edit / squash / fixup / drop (keys P R E S F D); from any commit or onto any branch; warns about pushed commits and flattened merges
- [x] Edit Commit Message for any commit (amend for the latest, rebase for older ones)
- [ ] Interactive rebase: edit the combined message of a squash group in the dialog; keep merges (`--rebase-merges`) — M
- [x] File history (follows renames), also for folders: own window per path, breadcrumb to parent folders, View → File History… (Ctrl+Shift+H) or a file's context menu
- [x] Blame view: age-coloured annotations, click through to the commit, blame the previous revision, back navigation, uncommitted lines marked
- [x] Blame options: ignore whitespace, detect lines moved within the file or copied from other files (`-w -M -C`), remembered
- [x] File history: uncommitted changes (staged, unstaged, untracked) as the first row; follow the current branch, any local or remote branch, or all branches
- [x] History of selected lines (`git log -L`): from the blame view or any commit diff; follows renames; working tree lines are matched to HEAD's
- [x] Compare commits: select two to see their differences, three or more (or "include the oldest") for their combined changes; compare any commit with the working tree; save either as a patch
- [ ] File tree at a revision: browse, open, save a file as of that commit — M
- [ ] Reflog view, plus an option to show reflog entries in the graph — S
- [ ] Graph filters: current branch only, chosen branches, hide tags, show/hide stashes, collapse merges — M
- [ ] Pin branches so they stay at the top and in the filter — S
- [ ] Layout options: details pane bottom or right, hide details pane, open a commit in its own window — S
- [ ] Search by file path, by content (`-S` / `-G`), by date range, with a results list — M
- [x] Save selected commits as numbered patch files (`git format-patch`, ready for `git am`)
- [ ] Author avatars (opt-in, cached) and per-author colours — S
- [ ] Turn issue IDs in commit messages into links (bug-tracker rules) — S
- [ ] Load more than 20 000 commits on demand — M

## 3. Local changes & conflicts
- [ ] Side-by-side 3-way merge resolver (ours / base / theirs, pick per chunk, editable result) — L
- [ ] Recreate a conflict (reset a file to its unmerged state) — S
- [ ] Dry-run conflict check before merge / rebase / cherry-pick, with a warning — M
- [ ] Tree view toggle for file lists — S
- [ ] Amend mode lists the last commit's files so they can be taken out of it — M
- [ ] Commit box: recent messages, commit template, subject-length guide — S
- [ ] Commit options: sign-off, skip hooks, signing on/off, commit & push — S
- [ ] Show hook output when a hook prints or fails — S
- [ ] Partial stash (selected files or hunks) — M
- [ ] Copy as patch; apply a patch file with a dry-run check — S
- [ ] Untrack a file (`rm --cached`); add `.gitignore` templates — S
- [ ] Diff options: ignore whitespace, context lines, word diff, show invisible characters — M
- [ ] Syntax highlighting in diffs — M
- [ ] Image diff (side by side, swipe) — M
- [ ] Submodule diff shows the commits between old and new — S
- [ ] Keyboard staging (S / U / Del on lines and hunks), go to line — S
- [ ] Open a file in the configured editor or external diff tool — S

## 4. Branches, remotes, stashes
- [ ] Fetch dialog: pick remote, prune, tags; custom refspecs — S
- [ ] Quick fetch / pull / push without a dialog (dialog with a modifier key) — S
- [ ] Background auto-fetch on an interval — S
- [ ] Push several branches or tags at once — S
- [ ] Rename a remote; rename a remote branch — S
- [ ] Checkout & sync (checkout, then fast-forward to upstream) — S
- [ ] Warning icon for a branch whose upstream is missing — S
- [ ] Annotated tag details — S
- [ ] Stash: rename, apply with index restore, proper diff incl. untracked part — S
- [ ] Per-remote "don't fetch automatically" — S
- [ ] Drag a branch onto another in the sidebar → merge / rebase menu — M

## 5. Repository structure
- [ ] Submodules: sidebar list, add, init/update (recursive), open in a tab, move, delete, change URL, discard changes, warn about unpushed submodule commits — L
- [ ] Worktrees: sidebar list, create, check out a local or remote branch as a worktree, open, delete, copy path — M
- [ ] Git LFS: install/uninstall, track patterns, fetch, pull, prune, status, lock/unlock + lock list, LFS previews in diffs — L
- [ ] Short-lived-branch helpers: start / sync / finish — S
- [ ] Bisect: start, mark good / bad / skip from the graph, progress, reset — M

## 6. Integrations
- [ ] Accounts for Bitbucket, Bitbucket Server and Gitea — M
- [ ] Register the aslgc GitHub OAuth App and set its Client ID as the default `ASLGC_GITHUB_CLIENT_ID` — S
- [ ] Browser sign-in for GitLab (OAuth device grant or PKCE with a loopback redirect) — M
- [ ] Publish: create the remote repository on an account and push the local repo in one step — S
- [ ] Account avatars; author avatars from the hosting service — S
- [ ] List open pull / merge requests for the repository; check out a request's branch — M
- [ ] Link issue references in commit messages to the hosting service — S
- [ ] SSH keys: list, generate, add to agent, test connection, copy public key — M
- [ ] Custom commands: user-defined git/shell commands with a small form (text, checkbox, branch picker); shared per-repo commands with confirmation — L
- [ ] AI helpers (opt-in, user's own API key): commit message from staged diff, review of changes — M

## 7. App shell & preferences
- [ ] Preferences: theme (system / light / dark), UI scale, UI + diff fonts, diff/merge tool, editor, terminal, git executable, default clone folder, auto-fetch interval, refresh-on-focus — L
- [ ] Keyboard-shortcut editor — M
- [ ] Command palette (Ctrl+P): any action, quick branch checkout, open repository — M
- [ ] Repository manager: groups, scan folders for repositories, rename / remove entries — M
- [ ] Workspaces: named sets of tabs — M
- [ ] Tabs: reopen closed tab; tab menu (terminal, file manager, close others) — S
- [ ] Repository settings: local name / email, commit template, bug-tracker rules — M
- [ ] Repository statistics: commits over time, per author, charts — M
- [ ] Desktop notifications when background operations finish — S
- [ ] Single instance: `aslgc <path>` opens a tab in the running window — S
- [ ] File-manager "Open in aslgc" action (Dolphin service menu, Nautilus) — S
- [ ] Accessibility pass: accessible names, full keyboard navigation — M

## Needs verification
- [x] First CI run on all four distros: Arch, Fedora, Debian 13 and Ubuntu 24.04 with Qt 6.4 all pass
- [ ] `packaging/arch/PKGBUILD` builds once the code is pushed
- [ ] Real GitHub and GitLab sign-in: repository picker, pull/merge request creation, token-based HTTPS push (so far only checked against the live APIs with an invalid token)
- [ ] GitHub browser sign-in end to end (needs the registered OAuth App Client ID)
- [ ] Interactive rebase dialog by hand: drag-and-drop reordering, keyboard shortcuts (engine is covered by tests)

## Tech debt & known gaps
- [ ] Graph lanes are never compacted, so very branchy histories get wide — M
- [ ] `.git` watcher only covers top-level `refs/heads`; nested branch folders rely on focus refresh — S
- [ ] Line-level staging of untracked files stages the whole file — S
- [ ] `RepoView.cpp` (~1400 lines) should move its actions into a separate `RepoActions` class — M
- [ ] Unit tests for UI-free logic still missing: sidebar tree building, badge mapping, operation detection — S

## Not planned
- License / registration screens
- Bundling a private copy of git (aslgc always uses the system `git`)
