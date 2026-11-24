# Heroicons (artwork only)

- Version: 2.2.0, the 20 px solid ("mini") set
- Source: https://github.com/tailwindlabs/heroicons
- License: MIT, see LICENSE

No Heroicons files are vendored here. The Space / pin icon picker's picture
icons are Heroicons artwork: the Mac generates its imagesets from this
release (`scripts/generate-phi-icons.py` in phibrowser-mac), and
`tools/gen-icon-masks.py` rasterizes those imagesets into the 48 px alpha
masks in `client/phi_icon_masks.cc`. The catalog's letters, digits and
punctuation are Phi's own drawings, not Heroicons.
