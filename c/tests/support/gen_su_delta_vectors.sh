#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Generate tests/unit/data/su_delta/{source.swu,target.swu,delta.bin,SHA256SUMS}, the software
# updates microsoft/delta:1 test vectors: two small SWUpdate archives and the delta from one to
# the other, made by DiffGenTool 3.0.0 (github.com/Azure/iot-hub-device-update-delta).
#
#   tests/support/gen_su_delta_vectors.sh <work-directory> [output-directory]
#
# The output is committed; tests never run this. It is byte-reproducible: rerunning it must
# leave the committed files unchanged. Then check the delta with the real delta processor:
# configure with -DAZ_IOT_WITH_SU_DELTA=ON -DAZ_IOT_SU_DELTA_TEST_LIBRARY=<path printed by
# eng/install-su-delta-processor.sh> and run ctest -R su_delta_interop.
#
# Needs x86_64 Linux, bash, curl, sha256sum, tar and python3; no .NET install (DiffGenTool is
# self-contained) and no root.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: ${0##*/} <work-directory> [output-directory]" 1>&2
    exit 1
fi
work="$(mkdir -p "$1" && cd "$1" && pwd)"
output="$(mkdir -p "${2:-${script_dir}/../unit/data/su_delta}" && cd "${2:-${script_dir}/../unit/data/su_delta}" && pwd)"

commit="f31e98446d2527531e1222e8ebd43b6ccd782605" # "Preview 3.0.0" (Git LFS objects)
archive="DiffGenTool.3.0.0.x64-linux.Release.tar.gz"
sha256="f6543afd9b67d20939976a082f08dd707b8d5cdd1c4df81a641026b4d2c78d9d"
url="https://media.githubusercontent.com/media/Azure/iot-hub-device-update-delta/${commit}/preview/3.0.0/${archive}"

if ! echo "${sha256}  ${work}/${archive}" | sha256sum -c --status 2>/dev/null; then
    curl -sSfL --retry 3 -o "${work}/${archive}" "${url}"
fi
echo "${sha256}  ${work}/${archive}" | sha256sum -c -

rm -rf "${work}/diffgentool" "${work}/gen"
mkdir -p "${work}/diffgentool" "${work}/gen"
tar -xzf "${work}/${archive}" -C "${work}/diffgentool"
tool="${work}/diffgentool/linux-x64"
chmod +x "${tool}"/{DiffGenTool,dumpextfs,applydiff,dumpdiff,bsdiff,bspatch,zstd_compress_file,recompress,makecpio,extract}

# Two SWUpdate archives (cpio "newc", sw-description first) of a 2048-byte payload: a SHA-256
# chain, of which the target changes 8 bytes at offset 100 and flips byte 1500. 2048 bytes is
# about the smallest payload whose delta DiffGenTool accepts (it fails when delta >= target).
# Every header field that could vary (mtime, uid, gid, inode, device) is fixed.
cd "${work}/gen"
python3 - <<'PY'
import hashlib

def newc_entry(inode, name, data, mode=0o100644):
    header = "070701" + "".join("%08X" % v for v in (
        inode, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0))
    out = header.encode() + name.encode() + b"\0"
    out += b"\0" * (-len(out) % 4)
    out += data
    return out + b"\0" * (-len(out) % 4)

def write_swu(path, version, payload):
    description = (
        'software = {\n'
        '  version = "%s";\n'
        '  images: ( { filename = "payload.bin"; type = "raw"; device = "/dev/null"; } );\n'
        '};\n' % version).encode()
    blob = newc_entry(1, "sw-description", description)
    blob += newc_entry(2, "payload.bin", payload)
    blob += newc_entry(0, "TRAILER!!!", b"", mode=0)
    open(path, "wb").write(blob + b"\0" * (-len(blob) % 512))

data, chain = b"", b"azure-iot-sdk-c-su-delta-vector"
while len(data) < 2048:
    chain = hashlib.sha256(chain).digest()
    data += chain
source = bytearray(data[:2048])
target = bytearray(source)
target[100:108] = b"TARGETv2"
target[1500] ^= 0xFF
write_swu("source.swu", "1.0", bytes(source))
write_swu("target.swu", "2.0", bytes(target))
PY

# No ICU on minimal hosts: run .NET in invariant globalization mode.
DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 LD_LIBRARY_PATH="${tool}" \
    "${tool}/DiffGenTool" source.swu target.swu delta.bin diffgentool-work > diffgentool.log 2>&1 \
    || { tail -20 diffgentool.log; exit 1; }

cp source.swu target.swu delta.bin "${output}/"
(cd "${output}" && sha256sum source.swu target.swu delta.bin | tee SHA256SUMS)
