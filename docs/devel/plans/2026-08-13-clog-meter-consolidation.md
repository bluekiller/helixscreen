# Clog meter: remaining work

**Status:** one cosmetic item open. The rest of the consolidation (shared
`ClogMeterModel`, safe-state predicate, the bar in the Buffer Status modal, filament
pressure as a meter source) has shipped; `docs/devel/FILAMENT_MANAGEMENT.md` describes it.

## Danger band legibility when warning is set (minor)

When `warning == 1` the fill is drawn in `danger` and the danger band is also `danger` at
30% opacity, so "how far into the danger zone" is hardest to read exactly when it matters
most. Visible in `ctl scenario flowguard_clog`. Options: outline the band instead of
filling it, or shift the fill to a lighter tint when it overlaps. Cosmetic.

---

## Verification recipe

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
    --remote-socket "$HELIX_SOCK" --printer happy_hare > /tmp/helix-$TREE.log 2>&1 &

# clog_detection is default_enabled=false — place it before driving states.
./build/bin/helix-screen ctl -s "$HELIX_SOCK" scenario flowguard_tangle
./build/bin/helix-screen ctl -s "$HELIX_SOCK" screenshot /tmp/x.png --target clog_detection
```

`ctl geom clog_bar_fill` / `clog_bar_peak` / `clog_bar_danger_{lo,hi}` read the exact
placement — prefer them over eyeballing a screenshot.
