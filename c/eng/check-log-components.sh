#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Fails when an SDK log call in c/src or c/adapters does not pass an SDK
# component (an AZ_IOT_LOG_COMPONENT_* macro other than _APP, defined in
# inc/azure/iot/az_iot_log.h), or when its message still starts with
# "<component>: ", which the component argument replaces. See c/docs/logging.md.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "${root_dir}" <<'PY'
import glob, os, re, sys

root = sys.argv[1]
header = open(os.path.join(root, "inc", "azure", "iot", "az_iot_log.h"), encoding="utf-8").read()
components = dict(re.findall(r'#define\s+(AZ_IOT_LOG_COMPONENT_\w+)\s+"([^"]+)"', header))
sdk = {k: v for k, v in components.items() if k != "AZ_IOT_LOG_COMPONENT_APP"}

COMMENT = r'(?:/\*.*?\*/\s*)*'
ARG = r'((?:[^,()]|\([^()]*\))*)'
# Component, then message: arguments 1-2 of AZ_IOT_LOG_*, 2 and 5 of az_iot_log_emit*().
macro = re.compile(
    r'\bAZ_IOT_LOG_(?:TRACE|DEBUG|INFO|WARN|ERROR)F?\s*\(\s*' + COMMENT + ARG + r',\s*' + COMMENT + r'(.)',
    re.S)
direct = re.compile(
    r'\baz_iot_log_emitf?\s*\(' + ARG + r',\s*' + COMMENT + ARG + r',' + ARG + r',' + ARG + r',\s*'
    + COMMENT + r'(.)',
    re.S)
literal = re.compile(r'"((?:[^"\\]|\\.)*)"')

files = []
for sub in ("src", "adapters"):
    for ext in ("c", "h"):
        files += glob.glob(os.path.join(root, sub, "**", "*." + ext), recursive=True)

bad = []
checked = 0
for path in sorted(files):
    rel = os.path.relpath(path, root)
    # The facade forwards caller-supplied components and messages.
    if rel in (os.path.join("src", "core", "log.c"), os.path.join("src", "core", "log_file.c")):
        continue
    text = open(path, encoding="utf-8").read()
    calls = [(m, m.group(1), m.start(2)) for m in macro.finditer(text)]
    calls += [(m, m.group(2), m.start(5)) for m in direct.finditer(text)]
    for m, comp, msg_start in sorted(calls, key=lambda c: c[0].start()):
        checked += 1
        line = text.count("\n", 0, m.start()) + 1
        comp = comp.strip()
        if comp not in sdk:
            bad.append(f"  {rel}:{line}: component '{comp[:40]}' is not an SDK AZ_IOT_LOG_COMPONENT_* macro")
            continue
        lit = literal.match(text, msg_start)
        if lit and lit.group(1).startswith(sdk[comp] + ": "):
            bad.append(f"  {rel}:{line}: message repeats its component: \"{lit.group(1)[:50]}\"")

if bad:
    print("Log calls with a missing, unknown or repeated component:\n")
    print("\n".join(bad))
    print(f"\n{len(bad)} call(s). SDK components: {', '.join(sorted(sdk.values()))}.")
    print("See c/docs/logging.md.")
    sys.exit(1)
if checked == 0:
    print("No log calls found; the scanner is broken.")
    sys.exit(1)
print(f"Every SDK log call ({checked}) passes an SDK component.")
PY
