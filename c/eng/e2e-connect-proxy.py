#!/usr/bin/env python3
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

"""HTTP CONNECT proxy for the e2e proxy tests on runners without tinyproxy (Windows).

Listens on 127.0.0.1, tunnels CONNECT requests to the allowed ports and appends each request
line ("CONNECT host:port HTTP/1.1") to the log, the line the tests look for in a tinyproxy log.

usage: e2e-connect-proxy.py --port 3128 --log proxy.log [--allow-port 443 --allow-port 8883]
"""

import argparse
import asyncio
import datetime

_HEADER_TIMEOUT_S = 30
_CONNECT_TIMEOUT_S = 30


async def _pump(reader, writer):
    """Copies @p reader to @p writer until either side ends, then closes @p writer."""
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except OSError:
        pass
    finally:
        writer.close()


async def _handle(client_reader, client_writer, allowed_ports, log):
    try:
        head = await asyncio.wait_for(client_reader.readuntil(b"\r\n\r\n"), _HEADER_TIMEOUT_S)
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, OSError):
        client_writer.close()
        return
    request_line = head.split(b"\r\n", 1)[0].decode("latin-1")
    log.write(f"{datetime.datetime.now(datetime.timezone.utc).isoformat()} {request_line}\n")
    log.flush()

    parts = request_line.split()
    host, port = "", ""
    if len(parts) == 3 and parts[0] == "CONNECT":
        host, _, port = parts[1].rpartition(":")
    if not host or not port.isdigit() or int(port) not in allowed_ports:
        client_writer.write(b"HTTP/1.1 403 Forbidden\r\n\r\n")
        client_writer.close()
        return
    try:
        upstream_reader, upstream_writer = await asyncio.wait_for(
            asyncio.open_connection(host, int(port)), _CONNECT_TIMEOUT_S)
    except (asyncio.TimeoutError, OSError):
        client_writer.write(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        client_writer.close()
        return
    client_writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
    await asyncio.gather(
        _pump(client_reader, upstream_writer), _pump(upstream_reader, client_writer))


async def _serve(args):
    with open(args.log, "a", encoding="utf-8") as log:
        server = await asyncio.start_server(
            lambda r, w: _handle(r, w, set(args.allow_port or [443, 8883]), log),
            "127.0.0.1", args.port)
        async with server:
            await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, required=True, help="port to listen on (127.0.0.1)")
    parser.add_argument("--log", required=True, help="access log the request lines are appended to")
    parser.add_argument("--allow-port", type=int, action="append",
                        help="destination port to allow (repeatable; default 443 and 8883)")
    asyncio.run(_serve(parser.parse_args()))


if __name__ == "__main__":
    main()
