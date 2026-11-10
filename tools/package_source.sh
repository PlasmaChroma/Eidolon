#!/usr/bin/env bash
# Package a fixed source-only allowlist, never the developer's local caches.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/audit_source.py
mkdir -p dist
output="${1:-dist/FirmwareHost-2.0.0-source.zip}"
if [[ -e "$output" ]]; then
    echo "Refusing to append to an existing archive: $output" >&2
    exit 1
fi
zip -q -r -X "$output" \
    Makefile LICENSE THIRD_PARTY.md UPSTREAM.md README.md .gitignore plugin.json \
    include src runtime/Cargo.toml runtime/Cargo.lock runtime/src \
    tests tools docs licenses \
    vendor/fm1-emu/Cargo.toml vendor/fm1-emu/Cargo.lock \
    vendor/fm1-emu/LICENSE vendor/fm1-emu/crates vendor/fm1-emu/tests \
    -x '*__pycache__*' '*.pyc'
echo "$output"
