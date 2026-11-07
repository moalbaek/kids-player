#!/bin/sh
# Build both boards and publish them on the GitHub Pages site (../docs):
#   docs/flash/kids-player-<board>.bin    full image for the browser installer (offset 0)
#   docs/flash/manifest-<board>.json      ESP Web Tools manifest
#   docs/fw/<board>/kids-player.bin       app image for over-the-air updates
#   docs/fw/<board>/version.json          players update when this is newer than what they run
# Bump PROJECT_VER in CMakeLists.txt first, then commit docs/ and push.
# Needs the ESP-IDF environment (. $HOME/esp/esp-idf/export.sh).
set -e
cd "$(dirname "$0")"
VER=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "PROJECT_VER not found in CMakeLists.txt"; exit 1; }
mkdir -p ../docs/flash

for b in cube strip; do
    case $b in
        cube) title='Kids Player for the 2.16" cube' ;;
        strip) title='Kids Player for the 3.49" strip' ;;
    esac
    echo "== $b $VER"
    # Note: an existing build_$b/sdkconfig wins over changed sdkconfig.defaults*; delete it to pick changes up.
    idf.py -B "build_$b" -D SDKCONFIG="build_$b/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$b" build
    (cd "build_$b" && esptool.py --chip esp32s3 merge_bin -o "../../docs/flash/kids-player-$b.bin" @flash_args)
    mkdir -p "../docs/fw/$b"
    cp "build_$b/kids-player.bin" "../docs/fw/$b/kids-player.bin"
    printf '{"version": "%s"}\n' "$VER" > "../docs/fw/$b/version.json"
    cat > "../docs/flash/manifest-$b.json" <<EOF
{
  "name": "$(printf '%s' "$title" | sed 's/"/\\"/g')",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [{ "path": "kids-player-$b.bin", "offset": 0 }]
    }
  ]
}
EOF
done
echo "Published $VER to ../docs. Commit and push to update the install page and the players."
