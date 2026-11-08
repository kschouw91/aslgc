# Third-party notices

aslgc's source code is released under the MIT License (see `LICENSE`). The
following bundled assets come from third parties and keep their own licenses.

## Tabler Icons — action icons

- Files: `resources/icons/actions/*.svg`
- Source: Tabler Icons 3.49.0, https://tabler.io/icons
- Copyright (c) 2020-2026 Paweł Kuna
- License: MIT — full text in `resources/icons/actions/LICENSE`

## Application icon

Files: `resources/icons/aslgc-source.png` and the sizes generated from it in
`resources/icons/hicolor/`. The icon is a composition that contains:

- **The Git logo** by Jason Long, licensed under the
  [Creative Commons Attribution 3.0 Unported License](https://creativecommons.org/licenses/by/3.0/).
  Source: https://git-scm.com/downloads/logos
- **Tux** by Larry Ewing (lewing@isc.tamu.edu), created with The GIMP.
  Permission to use and/or modify the image is granted provided the author and
  The GIMP are acknowledged.

"Git" and the Git logo are trademarks of the Software Freedom Conservancy;
"Linux" is a registered trademark of Linus Torvalds. aslgc is an independent
project and is not affiliated with or endorsed by either.

## Runtime libraries (not bundled)

aslgc links against these at build time; they are provided by your
distribution:

- Qt 6 (Core, Gui, Widgets, Concurrent, Network, Svg, Test) — LGPL-3.0 / GPL
- QtKeychain — BSD-3-Clause
