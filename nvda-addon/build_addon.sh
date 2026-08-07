#!/usr/bin/env bash
# Packages the EchoTalk NVDA add-on.
#
# An .nvda-addon is just a zip with manifest.ini at the root.
#
# The default build is PUBLIC-SAFE: the Textalker images are NOT included.
# They are proprietary to Street Electronics (3.1.3 also carries an
# American Printing House for the Blind copyright), so a user supplies
# their own and the driver stays unavailable until they do. Pass
# --with-images for a personal build that bundles them; that package must
# never be distributed.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

WITH_IMAGES=0
[[ "${1:-}" == "--with-images" ]] && WITH_IMAGES=1

DRV=synthDrivers/echotalk

for f in "$DRV/echotalk64.dll" "$DRV/echotalk32.dll"; do
    [[ -f "$f" ]] || { echo "missing $f -- see README.md" >&2; exit 1; }
done

# The third-party notices have to travel with the binaries: the TMS5220
# port is BSD-3-Clause, which requires the notice in any binary
# redistribution, and Fake6502 asks for credit.
cp ../THIRD_PARTY_LICENSES.md "$DRV/THIRD_PARTY_LICENSES.txt"

rm -f echotalk.nvda-addon
if [[ $WITH_IMAGES == 1 ]]; then
    shopt -s nullglob
    images=("$DRV"/*.obj.bin)
    [[ ${#images[@]} -gt 0 ]] || { echo "no Textalker images to bundle" >&2; exit 1; }
    zip -r -q echotalk.nvda-addon manifest.ini synthDrivers
    echo "wrote echotalk.nvda-addon (PRIVATE build: bundles proprietary Textalker images, do not distribute)"
else
    zip -r -q echotalk.nvda-addon manifest.ini synthDrivers \
        -x "$DRV/*.bin"
    echo "wrote echotalk.nvda-addon (public-safe, no Textalker images)"
fi
