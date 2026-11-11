# Contributing to aslgc

Thanks for helping! Bug reports, fixes and features are all welcome. For larger changes,
please open an issue first so the approach can be agreed before you write the code.

## Getting started

Install the dependencies listed in the [README](README.md#building), then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/aslgc /path/to/some/repository
```

## Code layout

```
src/git/        Qt-Core-only layer, safe to use from worker threads
  Git             process wrapper (fixed environment: no prompts, no pager, no optional locks)
  Parsers         log / for-each-ref / status --porcelain=v2 / name-status / stash
  Diff            unified diff parser and hunk / line patch builder for `git apply`
  Graph           lane layout for the commit graph
  Repository      read-only queries and loadSnapshot() (one refresh, off the GUI thread)
src/hosting/    GitHub / GitLab
  Account, RemoteUrl   account model, remote URL parsing, web links (core library)
  AccountStore         accounts + tokens in the system keyring (QtKeychain)
  HostingClient        REST client (QNetworkAccessManager), OAuth device flow
src/ui/         Qt Widgets
  MainWindow      tabs, menus, toolbar, session
  RepoView        one repository: sidebar + views, every git action, conflict banner
  ChangesView     staging and commit
  HistoryView     commit graph, details and diff
  DiffView        diff renderer with gutters and clickable hunk / line actions
src/askpass/    aslgc-askpass: GIT_ASKPASS / SSH_ASKPASS prompt and git credential helper
i18n/           translations (Qt Linguist .ts)
tests/          integration tests that run real git in temporary repositories
packaging/      desktop entry, AppStream metadata, man page, Arch PKGBUILD, Flatpak manifest
resources/      icons (see below)
tools/          maintenance scripts
```

Principles:

- **Every repository change goes through the `git` CLI.** Don't reimplement git behaviour.
- **Never block the GUI thread** on anything that can be slow: use `runAsync()` for reads and
  `RepoView::runCommands()` / `runNetwork()` for changes.
- Keep `src/git/` and the core part of `src/hosting/` free of widget code so they stay testable.

## Style

- C++20, Qt 6. Format with the repository's `.clang-format` (`clang-format -i <files>`).
- Builds must be warning-free with `-Wall -Wextra -Wpedantic -Wshadow` (on by default).
- New source files start with `// SPDX-License-Identifier: MIT`.
- User-visible strings go through `tr()`.

## Tests

`tests/tst_core.cpp` runs real `git` in temporary repositories. It isolates itself from your
own git configuration through `GIT_CONFIG_GLOBAL=/dev/null`. Please add a test for parser,
diff / patch, graph, URL or credential changes.

## Screenshots

The app can capture itself for documentation or visual checks without a display:

```sh
QT_QPA_PLATFORM=offscreen ASLGC_SCREENSHOT=/tmp/shot.png ASLGC_VIEW=history ./build/aslgc /path/to/repo
```

`ASLGC_VIEW` can be `history`, `changes`, `accounts`, `add-github`, `rebase` (range from
`ASLGC_REBASE_BASE`, default `HEAD~3`), `file-history` or `blame` (path from `ASLGC_FILE`; a
trailing `/` or an empty path shows a folder's history), and `ASLGC_SCREENSHOT_DELAY` sets the
wait in milliseconds. Set `XDG_CONFIG_HOME` to a temporary folder to keep your own
settings untouched. Optimise new screenshots with `tools/optimize-images.sh --palette`.

`tools/make-screenshots.sh` regenerates the README screenshots in `docs/screenshots/` from a
demo repository with fictional authors, then optimises them.

## Translations

User-visible strings use `tr()`. Use Qt's plural form for counts: `tr("%n file(s)", nullptr, count)`.

- `i18n/aslgc_en.ts` only holds the English singular/plural forms. Refresh it after adding or
  changing a plural string:
  `lupdate src -ts i18n/aslgc_en.ts -pluralonly -no-obsolete -locations none`, then fill in
  the two forms (Qt Linguist or a text editor).
- New language, e.g. German: `lupdate src -ts i18n/aslgc_de.ts -no-obsolete -locations relative`,
  translate it in Qt Linguist and commit the `.ts` file. CMake picks up every `i18n/*.ts`
  automatically. They are compiled with `lrelease` and embedded in the binary.
- `lupdate` / `lrelease` come with Qt's Linguist tools. Without them the build skips translations.

## Icons

- **Action icons** are [Tabler Icons](https://tabler.io/icons) (MIT) in `resources/icons/actions/`.
  To add one, copy the outline SVG from the same Tabler release and use
  `theme::actionIcon("name")`. The icon is tinted to the palette automatically.
- **The app icon** sizes in `resources/icons/hicolor/` are generated from
  `resources/icons/aslgc-source.png` by `tools/make-icons.sh` (requires ImageMagick 7), which
  also optimises them.
- **Optimise PNGs** before committing them: `tools/optimize-images.sh <files>` recompresses
  losslessly and keeps a result only when it is smaller and pixel-identical. For documentation
  screenshots use `tools/optimize-images.sh --palette docs/screenshots/*.png` (256 colours,
  roughly half the size; never use it on icons).

## Releases

Releases follow git-flow: `develop` → `release/<version>` → `main`, with the version tag on
`main` (no prefix, e.g. `0.2.0`).

1. Start the release from `develop` (Git Flow → Start Release).
2. On the release branch, make sure `CHANGELOG.md` lists the changes under `## [Unreleased]`
   (or `## [<version>] — unreleased`), then run `tools/set-version.sh <version>`. It sets the
   version in `CMakeLists.txt`, dates the CHANGELOG entry and adds the release to the AppStream
   metadata. Commit that.
3. Push the release branch and wait for the release workflow's dry run to pass.
4. Finish the release (merge into `main` and back into `develop`, tag), then push `main`,
   `develop` and the tag.

Pushing the tag runs `.github/workflows/release.yml`: it checks the version with
`tools/set-version.sh <version> --check`, builds the `.deb` (Ubuntu 24.04, Debian 13), `.rpm`
(Fedora), AppImage and Flatpak bundle, installs or starts each one, and publishes them with the
CHANGELOG notes as a GitHub release. Pushing the `release/<version>` (or `hotfix/<version>`) branch
before finishing runs the same checks and builds as a dry run; its packages are only kept as
workflow artifacts.

Afterwards, start a new `## [Unreleased]` section at the top of `CHANGELOG.md` on `develop`.

## Pull requests

- Keep each pull request focused on one change and describe what it changes and why.
- Make sure `ctest` passes and the build has no warnings.
- Update `CHANGELOG.md` for user-visible changes.
