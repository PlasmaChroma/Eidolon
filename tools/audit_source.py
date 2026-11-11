#!/usr/bin/env python3
"""Static firmware-free distribution checks. No network, downloads, or writes.

This catches accidental payloads and patch-serialization regressions, not arbitrary
malicious code or rights provenance. Run against the source tree before packaging.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP = {".git", "build", "dist", "target", "__pycache__", ".pytest_cache"}
FORBIDDEN = {".fwsc", ".fm1save", ".flash", ".syx", ".sysex", ".rom", ".hex", ".uf2"}
# Written keys. flashDelta carries the firmware's own flash writes (sectors that differ from
# the package image), never package bytes; stateId is only read, from schema-1 patches.
PATCH_KEYS = {"schema", "firmwareSha256", "flashDelta", "clockMhz", "leadFrames"}
errors = []

def check(condition, message):
    if not condition:
        errors.append(message)

def source_without_comments(text):
    # Retain strings while stripping comments so comments about forbidden state
    # are allowed, but calls that write it are still inspected.
    token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')
    return token.sub(lambda m: "" if m.group().startswith(("//", "/*")) else m.group(), text)

files = [p for p in ROOT.rglob("*") if p.is_file() and not any(x in SKIP for x in p.relative_to(ROOT).parts)]
for path in files:
    rel = path.relative_to(ROOT)
    check(path.suffix.lower() not in FORBIDDEN, f"Firmware/state payload in source: {rel}")
    check(path.name not in {"libfm1_engine.a", "libcompiler_rt.a", "libc.a", "uboot.boot", "ota.bin", "boot-config.bin"},
          f"Vendor SDK/boot binary in source: {rel}")
    if "vendor" in rel.parts:
        check(path.suffix.lower() not in {".a", ".so", ".dll", ".dylib", ".bin", ".zip"}, f"Binary in vendored source: {rel}")

frontend = ROOT / "src" / "FM1Host.cpp"
check(frontend.is_file(), "Missing Rack frontend src/FM1Host.cpp")
if frontend.is_file():
    code = source_without_comments(frontend.read_text())
    literal_keys = re.findall(r'json_object_set_new\s*\([^,]+,\s*"([^"]+)"', code)
    keys = set(literal_keys)
    check(keys == PATCH_KEYS, f"Custom patch keys differ from whitelist: {sorted(keys)}")
    check(len(re.findall(r'json_object_set_new\s*\(', code)) == len(literal_keys),
          "Dynamic patch keys are not permitted; keep serialization explicit")
    check(not re.search(r'\bjson_(?:object_set(?!_new)|object_update\w*|pack|array\w*)\s*\(', code),
          "Unexpected JSON construction bypasses the explicit patch-key whitelist")
    check("getPatchStorageDirectory" not in code, "Patch storage must not contain emulated flash/firmware")
    check(len(re.findall(r"toBase64\s*\(", code)) == 1 and "toBase64(delta)" in code,
          "Base64 serialization other than the flash delta in Rack frontend")
    for key in ("firmwareSha256", "stateId", "flashDelta"):
        direct = re.search(r'json_object_get\s*\([^,]+,\s*"' + key + r'"', code)
        helper = re.search(r'getString\s*\(\s*"' + key + r'"', code)
        check(direct or helper, f"Missing identity restore key: {key}")

manifest = ROOT / "plugin.json"
check(manifest.is_file(), "Missing plugin manifest")
check((ROOT / "vendor" / "fm1-emu" / "LICENSE").is_file(), "Missing upstream MIT notice")

if errors:
    print("Firmware-free source audit: FAIL", file=sys.stderr)
    for error in errors:
        print(" - " + error, file=sys.stderr)
    sys.exit(1)
print(f"Firmware-free source audit: PASS ({len(files)} files; patches: identity, options, flash delta)")
