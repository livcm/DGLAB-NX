# stb

`stb_truetype.h` (v1.26), vendored as a single header.

- Upstream: <https://github.com/nothings/stb>
- Licence: public domain / MIT (dual, see the header itself)
- Why: the NRO needs a TTF rasteriser for the system shared font, because the
  Chinese UI cannot be drawn with libnx's 256 glyph bitmap font. FreeType would
  mean adding the `switch-freetype` portlib, which this project avoids; one
  header with no build system of its own is the smaller dependency.
- How it is used: `nro/source/ui/text_ttf.c` defines
  `STB_TRUETYPE_IMPLEMENTATION` in exactly that one translation unit and wraps it
  in a `DglabGlyphSource`; nothing else includes the implementation.
- Updating: replace the file, keep the version in the first line, and re-check the
  rasterisation notes in `docs/nro-ui.md` (the glyphs are thresholded to one bit).
