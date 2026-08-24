#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the OpenDeck SPA (webui UI mode) and inject the Tauri-compat shim so it
# talks to our OpenDeckBridge over a QWebChannel. Output: a static site in
# src/app/webui/opendeck/build/ that CMake bundles into the :/opendeck Qt
# resource when -DAJAZZ_BUILD_WEBUI=ON.
#
# OpenDeck is vendored as a git submodule at src/app/webui/opendeck (pristine
# upstream nekename/OpenDeck). Update it with:
#   git submodule update --remote src/app/webui/opendeck
# Our customizations live OUTSIDE the submodule (the shim below + the C++
# OpenDeckBridge/scheme-handler), so the submodule stays pristine and bumpable.
#
# Requires Node.js >= 22.18 + npm. Idempotent. See docs/opendeck-ui/.
#
# Why 22.18 and not the 20 this used to claim: the pinned submodule ships its
# SvelteKit config as `svelte.config.ts`, and loading a TypeScript config needs
# Node's type stripping, which is only on by default from 22.18.0 (and 23.6.0).
# On anything older the build dies deep inside vite with
# `ERR_UNKNOWN_FILE_EXTENSION: Unknown file extension ".ts"`, which says nothing
# about Node versions -- hence the explicit gate below.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
webui_dir="$repo_root/src/app/webui/opendeck"
shim="$repo_root/resources/opendeck-shim/tauri-shim.js"
out="$webui_dir/build"

node_min="22.18.0"

if ! command -v npm >/dev/null 2>&1 || ! command -v node >/dev/null 2>&1; then
    echo "build-webui: node/npm not found — install Node.js >= $node_min to build the webui SPA." >&2
    exit 2
fi

node_have="$(node --version | sed 's/^v//')"
if [[ "$(printf '%s\n%s\n' "$node_min" "$node_have" | sort -V | head -n1)" != "$node_min" ]]; then
    cat >&2 <<EOF
build-webui: Node.js $node_have is too old — need >= $node_min.

The vendored OpenDeck submodule configures SvelteKit through svelte.config.ts.
Loading a TypeScript config requires Node's type stripping, on by default only
from 22.18.0. Older versions fail inside vite with:
    ERR_UNKNOWN_FILE_EXTENSION: Unknown file extension ".ts"

Fixes:
  * install a current Node (nvm install --lts, or nodesource);
  * if a conda/venv environment is active, it may be shadowing the system
    node -- check with 'which -a node';
  * or configure with -DAJAZZ_BUILD_WEBUI=OFF to skip the SPA entirely (the
    app then falls back to the native qml UI).
EOF
    exit 2
fi
if [[ ! -f $shim ]]; then
    echo "build-webui: missing shim at $shim" >&2
    exit 1
fi

cd "$webui_dir"

# Local UI patches (patches/opendeck/*.patch): device-layout fidelity fixes we
# carry until upstreamed (keys -> touch strip -> dials row order matching the
# physical hardware; rectangular touch-strip segments). Idempotent: an
# already-applied patch (dev working tree) is detected via --reverse --check
# and skipped; CI's pristine submodule checkout gets it applied fresh.
for p in "$repo_root"/patches/opendeck/*.patch; do
    [[ -e $p ]] || continue
    if git apply --check "$p" 2>/dev/null; then
        git apply "$p"
        echo "build-webui: applied $(basename "$p")"
    elif git apply --reverse --check "$p" 2>/dev/null; then
        echo "build-webui: $(basename "$p") already applied"
    else
        echo "build-webui: ERROR — $(basename "$p") does not apply (submodule bumped?)" >&2
        exit 1
    fi
done

echo "build-webui: installing deps in $webui_dir"
if [[ -f package-lock.json ]]; then npm ci; else npm install; fi
# esbuild's native binary postinstall is sometimes skipped under restrictive npm
# configs; ensure it's present so vite/esbuild can run.
[[ -f node_modules/esbuild/install.js ]] && node node_modules/esbuild/install.js || true

echo "build-webui: vite build"
npm run build

# Stage the shim beside the SPA and inject it as the FIRST <head> script so it
# runs before the deferred SvelteKit module entry.
cp "$shim" "$out/tauri-shim.js"
python3 - "$out/index.html" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
tag = '<script src="./tauri-shim.js"></script>'
if "tauri-shim.js" not in s:
    s = s.replace("<head>", "<head>\n\t\t" + tag, 1)
    open(p, "w", encoding="utf-8").write(s)
    print("build-webui: injected tauri-shim.js into index.html")
else:
    print("build-webui: shim already injected")
PY

echo "build-webui: done -> $out"
