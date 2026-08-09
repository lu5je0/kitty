#!/bin/bash
set -euo pipefail

deploy_macos() {
    local app_src="kitty/launcher/kitty.app"
    local app_dst="/Applications/kitty.app"

    if [[ ! -d "$app_src" ]]; then
        echo "Error: $app_src not found, run ./dev.sh build first" >&2
        exit 1
    fi

    if [[ -d "$app_dst" ]]; then
        echo "Removing old $app_dst ..."
        rm -rf "$app_dst"
    fi

    echo "Copying $app_src to $app_dst ..."
    cp -R "$app_src" "$app_dst"

    echo "Deploy done. Run: open $app_dst"
}

deploy_linux() {
    local pkg_src="linux-package"
    local app_dst="$HOME/.local/share/kitty.app"
    local desktop_dir="$HOME/.local/share/applications"
    local desktop_dst="$desktop_dir/kitty.desktop"

    if ! command -v sphinx-build >/dev/null 2>&1; then
        echo "Warning: sphinx-build not found, packaging without docs/man pages ..."
        mkdir -p docs/_build/html docs/_build/man
    fi

    # upstream's shader pipeline needs slangc; the repo bundles it
    if ! command -v slangc >/dev/null 2>&1 && [[ -x dependencies/linux-amd64/bin/slangc ]]; then
        export PATH="$PWD/dependencies/linux-amd64/bin:$PATH"
    fi

    echo "Building $pkg_src ..."
    python3 setup.py linux-package

    if [[ -d "$app_dst" ]]; then
        echo "Removing old $app_dst ..."
        rm -rf "$app_dst"
    fi

    echo "Copying $pkg_src to $app_dst ..."
    mkdir -p "$(dirname "$app_dst")"
    cp -R "$pkg_src" "$app_dst"

    echo "Creating $desktop_dst ..."
    mkdir -p "$desktop_dir"
    sed \
        -e "s|^Icon=kitty$|Icon=$app_dst/share/icons/hicolor/256x256/apps/kitty.png|" \
        -e "s|^Exec=kitty$|Exec=$app_dst/bin/kitty|" \
        -e "s|^TryExec=kitty$|TryExec=$app_dst/bin/kitty|" \
        "$app_dst/share/applications/kitty.desktop" > "$desktop_dst"

    echo "Deploy done. Run: $app_dst/bin/kitty"
}

case "$(uname)" in
    Darwin) deploy_macos ;;
    Linux) deploy_linux ;;
    *)
        echo "Error: unsupported platform $(uname)" >&2
        exit 1
        ;;
esac
