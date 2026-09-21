# Third-Party Notices

This SDK includes third-party assets under their own licenses. The SDK's own
code is MIT-licensed (see `LICENSE`); the items below are exceptions and carry
the licenses noted.

## Fonts

### BigBlue Terminal 437 TT — large 8x12 glyph table

- **Used by:** `op::sim::kMockFont8x12` in `sim/src/mock_font.cpp` (the
  simulator's large font, shipped so the sim renders text identically to the
  device).
- **Author:** VileR
- **Source:** Oldschool PC Font Pack — https://int10h.org/oldschool-pc-fonts/
- **License:** Creative Commons Attribution-ShareAlike 4.0 International
  (CC BY-SA 4.0) — see `licenses/CC-BY-SA-4.0.txt`.
- **Changes made:** rasterized from the original to an 8x12-cell column-major
  bitmap (page-addressed encoding) for the SSD1306 framebuffer mock. No glyph
  shapes were altered.

### Small 5x7 pixel font — `op::sim::kMockFont5x7`

- The small font is the Operator project's own work, dedicated to the public
  domain under **CC0 1.0** (see `licenses/CC0-1.0.txt`). No attribution is
  required, but credit to the Operator project is appreciated.

## CSS / JS theme

### doxygen-awesome-css — API-docs site theme

- **Used by:** the SDK API-docs site generated from `Doxyfile` (the vendored
  CSS styles the HTML output and the two JavaScript extensions add a dark-mode
  toggle and a copy-to-clipboard button on code fragments).
- **Author:** jothepro
- **Source:** https://github.com/jothepro/doxygen-awesome-css (pinned v2.4.2)
- **License:** MIT — see `licenses/MIT-doxygen-awesome.txt`.
- **Location:** vendored under `docs/doxygen/`.
- **Changes made:** vendored a subset of the release files
  (`doxygen-awesome.css`, `doxygen-awesome-sidebar-only.css`,
  `doxygen-awesome-darkmode-toggle.js`,
  `doxygen-awesome-fragment-copy-button.js`) plus an SDK-authored
  `header.html` that loads the two JavaScript extensions and initializes them.
  The two `.js` files were **patched to remove their jQuery dependency**
  (ported the `$(function(){...})` / `$(window).resize()` init to vanilla
  `DOMContentLoaded` + `addEventListener`), because Doxygen 1.14+ no longer
  ships jQuery and the original init threw `$ is not defined`; each change is
  marked with an `OPERATOR PATCH` comment. An SDK-authored
  `operator-overrides.css` (loaded last) keeps the dark-mode toggle inside the
  sidebar in the sidebar-only layout. The CSS theme files are otherwise
  unmodified.
