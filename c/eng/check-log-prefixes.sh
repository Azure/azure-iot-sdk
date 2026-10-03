#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Fails when an SDK log message in c/src or c/adapters does not start with a
# known "<component>: " prefix. The prefix is how a log reader filters by
# component and tells SDK lines from the application's ("app: " is reserved
# for applications). The list is documented in c/docs/logging.md; keep both in
# sync.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "${root_dir}" <<'PY'
import glob, os, re, sys

root = sys.argv[1]
allowed = {
    "connection", "dps", "cert", "cert_pem", "su", "paho", "c2d",
    "mqttv3_telemetry", "mqttv3_twin", "mqttv3_direct_method", "mqttv3_file_upload",
    "mqttv5_telemetry", "mqttv5_twin", "mqttv5_direct_method",
}
COMMENT = r'(?:/\*.*?\*/\s*)*'
# Message or format argument: the first argument of an AZ_IOT_LOG_* macro, the
# fourth of az_iot_log_emit()/az_iot_log_emitf(). It may sit on a later line or
# follow a comment.
macro = re.compile(r'AZ_IOT_LOG_(?:TRACE|DEBUG|INFO|WARN|ERROR)F?\s*\(\s*' + COMMENT + r'(.)', re.S)
direct = re.compile(r'\baz_iot_log_emitf?\s*\(((?:[^,()]|\([^()]*\))*,){3}\s*' + COMMENT + r'(.)', re.S)
literal = re.compile(r'"((?:[^"\\]|\\.)*)"')
prefix = re.compile(r'([a-z0-9_]+): ')

files = []
for sub in ("src", "adapters"):
    files += glob.glob(os.path.join(root, sub, "**", "*.c"), recursive=True)

bad = []
for path in sorted(files):
    rel = os.path.relpath(path, root)
    # The facade itself forwards caller-supplied messages.
    if rel == os.path.join("src", "core", "log.c"):
        continue
    text = open(path, encoding="utf-8").read()
    for m in sorted(list(macro.finditer(text)) + list(direct.finditer(text)), key=lambda x: x.start()):
        line = text.count("\n", 0, m.start()) + 1
        start = m.start(m.lastindex)
        lit = literal.match(text, start)
        if lit is None:
            bad.append(f"  {rel}:{line}: message is not a string literal, so its prefix cannot be checked")
            continue
        p = prefix.match(lit.group(1))
        if p is None or p.group(1) not in allowed:
            bad.append(f"  {rel}:{line}: \"{lit.group(1)[:60]}\"")

if bad:
    print("Log messages without a known '<component>: ' prefix:\n")
    print("\n".join(bad))
    print(f"\n{len(bad)} message(s). Allowed: {', '.join(sorted(allowed))}.")
    print("See c/docs/logging.md.")
    sys.exit(1)
print("Every SDK log message carries a known component prefix.")
PY
