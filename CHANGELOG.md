# Changelog

All notable changes to aslgc are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

## [0.1.0] — unreleased

First public release.

### Added
- Repository tabs with session restore, recent repositories, open / clone / init.
- Commit graph with lanes, ref labels, commit details, per-file diffs and commit search.
- Staging of files, hunks and selected lines; discard; commit and amend; author identity prompt.
- Branch, tag, remote and stash management from the sidebar and commit context menus.
- Merge, rebase, cherry-pick, revert and reset, with a conflict view and Continue / Skip / Abort banner.
- Fetch, pull and push with progress; GUI prompts for passwords, passphrases and SSH host keys.
- GitHub and GitLab accounts (incl. Enterprise / self-managed) with tokens in the system keyring:
  repository picker for cloning, pull / merge request creation, open in browser, HTTPS authentication.
- Optional GitHub browser sign-in through the OAuth device flow.
- Interactive rebase editor (reorder, reword, edit, squash, fixup, drop) and Edit Commit Message for any commit.
- git-flow: set up, start and finish features, bugfixes, releases and hotfixes, with version suggestions,
  tagging and optional push. Support branches keep older release lines alive, with hotfixes of their own.
  Finishing can fetch first and fast-forwards branches that are behind the remote. Uses the same
  settings as the git-flow command line tool.
- File history for any file (following renames) or folder, and a blame view that clicks through to
  each commit and steps back through earlier revisions (View → File History…, or a file's context menu).
  The history can follow another branch or all branches and lists uncommitted changes first; blame
  can ignore whitespace and detect lines moved within the file or copied from other files.
- Commit comparison: select two commits for their differences or a range for its combined changes, compare
  any commit with the working tree, and save a comparison or the selected commits as patches.
- History of selected lines: select lines in the blame view or a commit diff to list the commits that
  changed them, each with the diff of just those lines.
- Reflog window for HEAD or any branch (View → Reflog): every commit, checkout, reset, merge and rebase,
  with commits that are no longer on any branch marked. All Commits can show these lost commits too,
  faded and named after their reflog entry, so an amend, reset or rebase can be undone with a branch.
- Theme-aware Tabler action icons; desktop entry, AppStream metadata and man page.
