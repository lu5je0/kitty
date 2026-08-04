#!/bin/bash
set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "Error: deploy script is only for macOS" >&2
    exit 1
fi

APP_SRC="kitty/launcher/kitty.app"
APP_DST="/Applications/kitty.app"

if [[ ! -d "$APP_SRC" ]]; then
    echo "Error: $APP_SRC not found, run ./dev.sh build first" >&2
    exit 1
fi

if [[ -d "$APP_DST" ]]; then
    echo "Removing old $APP_DST ..."
    rm -rf "$APP_DST"
fi

echo "Copying $APP_SRC to $APP_DST ..."
cp -R "$APP_SRC" "$APP_DST"

echo "Deploy done. Run: open $APP_DST"