#!/bin/sh
# render.sh - the launcher icon's PNGs for Android 6-7 (no adaptive icons
# there) from ic_launcher.svg: the 72-unit middle of the 108 grid (what an
# adaptive icon shows), as a rounded square (ic_launcher.png) and a circle
# (ic_launcher_round.png), per density. Needs inkscape and ImageMagick 7.
# Run from android/ after changing the SVG; the PNGs are committed.
set -eu
cd "$(dirname "$0")/.."
RES=app/src/main/res
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
inkscape icon/ic_launcher.svg -w 1296 -h 1296 -o "$TMP/full.png" 2>/dev/null
magick "$TMP/full.png" -crop 864x864+216+216 +repage "$TMP/mid.png"
magick "$TMP/mid.png" \( -size 864x864 xc:none -fill white -draw "roundrectangle 0,0 863,863 150,150" \) \
    -compose DstIn -composite "$TMP/square.png"
magick "$TMP/mid.png" \( -size 864x864 xc:none -fill white -draw "circle 432,432 432,0" \) \
    -compose DstIn -composite "$TMP/round.png"
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    dir=$RES/mipmap-${d%%:*}
    px=${d##*:}
    mkdir -p "$dir"
    magick "$TMP/square.png" -filter Lanczos -resize "${px}x${px}" -strip "$dir/ic_launcher.png"
    magick "$TMP/round.png" -filter Lanczos -resize "${px}x${px}" -strip "$dir/ic_launcher_round.png"
done
echo "icons: $RES/mipmap-*dpi/ic_launcher{,_round}.png"
