#!/bin/sh
# Builds and runs the host unit tests. Needs gcc and an ESP-IDF checkout for cJSON (IDF_PATH).
set -e
cd "$(dirname "$0")"
CJSON="${IDF_PATH:?run . $HOME/esp/esp-idf/export.sh first}/components/json/cJSON"
gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../main -I"$CJSON" \
    test_host.c ../main/kp_util.c ../main/sonos_proto.c ../main/spotify_parse.c "$CJSON/cJSON.c" -o /tmp/kp_test -lm
/tmp/kp_test
