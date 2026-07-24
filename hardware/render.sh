#!/usr/bin/env bash
# Regenerate the preview renders in renders/ from the OpenSCAD source.
# Run after changing esp32_max485_case.scad so the images do not go stale.
#
#   ./render.sh
#
# Needs the OpenSCAD CLI on PATH (macOS: brew install --cask openscad, then
# /Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD, or the openscad wrapper).
set -euo pipefail
cd "$(dirname "$0")"

SCAD=esp32_max485_case.scad
OUT=renders
# --render=cgal is not optional: the default OpenCSG preview drops thin walls
# inside a difference(), which makes the lid skirt look like it has holes in it.
# Same trap as hitting F5 instead of F6 in the GUI.
COMMON=(--imgsize=1400,1050 --colorscheme=Tomorrow --autocenter --viewall --render=cgal)

mkdir -p "$OUT"

# --camera=tx,ty,tz,rot_x,rot_y,rot_z,dist  (dist ignored under --viewall)
render() {  # render <outfile> <part> <rot_x> <rot_z>
  openscad -o "$OUT/$1.png" "${COMMON[@]}" \
           --camera=0,0,0,"$3",0,"$4",0 -D "part=\"$2\"" "$SCAD"
}

render exploded             both  60 35   # lid floating above, flipped as printed
render base-interior        base  60 35   # glue pads and snap catches
render base-connector-wall  base  78 200  # USB window + RS-485 cable slot
render base-pry-end         base  78 20   # blank wall, pry notch in the rim
render lid-underside        lid   60 240  # skirt, catch ridges, vent slots

echo "wrote $(ls -1 "$OUT"/*.png | wc -l | tr -d ' ') renders to $OUT/"
