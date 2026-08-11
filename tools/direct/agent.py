#!/usr/bin/env python3
"""Answer a Slate panel's semantic actions through the direct provider.

design.md §4.2 (the WebSocket), §5.2 (normalized state), §5.3 (semantic actions
and optimistic updates), §5.4 (the direct provider).

    SLATE_TOKEN=... tools/direct/agent.py <host> [--fail]

This is the reference consumer and the other half of `publish.sh`: it attaches
to the direct provider, receives the `action` frames a tap produces, applies
them to a small in-memory model of one light, answers with `action_result`, and
publishes the resulting §5.2 snapshot back over HTTP. That last step is the one
worth watching, because §5.3 makes it load-bearing — an `action_result` with
`success: true` acknowledges delivery and nothing more, and it is the matching
state snapshot that confirms the tile. A consumer that answered and never
published would leave every tap reverting after three seconds.

`--fail` answers every action with `success: false` instead, which is the other
documented outcome: §5.3 reverts immediately rather than waiting out the
timeout, and the tile shows a brief error.

Standard library only, deliberately. A dependency on a WebSocket package would
make the reference client for a public contract (ADR-4) something a reader has
to install before they can see the contract work, and RFC 6455's client side is
about eighty lines for frames this small.

What this is not: a broker, a bridge or a daemon to run in production. §5.4 is
explicit that the direct provider is the smallest useful interoperability path —
a script, a Node-RED flow or a test fixture. This file is the shape such a thing
takes, not a component of the system.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import socket
import struct
import sys
import urllib.error
import urllib.request

TEXT, CLOSE, PING, PONG = 0x1, 0x8, 0x9, 0xA


class WebSocket:
    """The client half of RFC 6455, for one text frame at a time."""

    def __init__(self, host: str, port: int, path: str, timeout: float = 30.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=10.0)
        self._sock.settimeout(timeout)
        self._buffer = b""

        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self._sock.sendall(
            (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                f"Sec-WebSocket-Key: {key}\r\n\r\n"
            ).encode()
        )

        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self._sock.recv(1)
            if not chunk:
                raise ConnectionError("the panel closed the connection during the upgrade")
            header += chunk
        status_line = header.split(b"\r\n", 1)[0]
        if b" 101 " not in status_line:
            raise ConnectionError("upgrade refused: %r" % status_line)

    def send(self, text: str, opcode: int = TEXT, payload: bytes | None = None) -> None:
        payload = text.encode() if payload is None else payload
        if len(payload) > 0xFFFF:
            raise ValueError("frame too large for this client")
        # Clients must mask; servers must not (RFC 6455 §5.3).
        mask = secrets.token_bytes(4)
        if len(payload) < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | len(payload))
        else:
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, len(payload))
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self._sock.sendall(header + mask + masked)

    def pong(self, payload: bytes) -> None:
        """RFC 6455 §5.5.3: a pong carries the ping's payload back, unchanged."""
        self.send("", opcode=PONG, payload=payload)

    def _read(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("the panel closed the connection")
            self._buffer += chunk
        taken, self._buffer = self._buffer[:count], self._buffer[count:]
        return taken

    def receive(self) -> tuple[int, bytes]:
        first, second = struct.unpack("!BB", self._read(2))
        # Checked before the payload is read, not after: a masked server frame
        # means the stream is not what this client thinks it is, and reading a
        # length from it first is trusting the same bytes twice.
        if second & 0x80:  # A server frame is never masked (RFC 6455 §5.1).
            raise ConnectionError("the panel sent a masked frame")
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._read(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._read(8))
        return opcode, self._read(length)

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


class Light:
    """One resource, in §5.2's vocabulary rather than in any upstream system's."""

    def __init__(self, resource: str = "living-room") -> None:
        self.resource = resource
        self.on = True
        self.brightness = 62
        self.color_temperature = 3200

    def apply(self, action: str, params: dict) -> bool:
        """Carry out a semantic action. False is a refusal this client owns."""
        if action == "toggle":
            self.on = not self.on
        elif action == "set_power":
            value = params.get("value")
            if not isinstance(value, bool):
                return False
            self.on = value
        elif action == "set_brightness":
            value = params.get("value")
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not 0 <= value <= 100
            ):
                return False
            self.brightness = int(value)
            self.on = self.brightness > 0
        elif action == "set_color_temperature":
            value = params.get("value")
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not 2200 <= value <= 6500
            ):
                return False
            self.color_temperature = int(value)
        else:
            return False
        return True

    def snapshot(self) -> dict:
        return {
            "resource": self.resource,
            "kind": "light",
            "name": "Living room",
            "available": True,
            "state": {
                "power": "on" if self.on else "off",
                "brightness": self.brightness,
                "color_temperature": self.color_temperature,
            },
            "capabilities": {
                "toggle": True,
                "set_power": True,
                "set_brightness": {"min": 0, "max": 100},
                "set_color_temperature": {"min": 2200, "max": 6500},
            },
        }


def publish(base: str, token: str, snapshot: dict) -> None:
    """§5.3's confirmation: the state update, not the acknowledgement."""
    request = urllib.request.Request(
        f"{base}/api/v1/direct/state",
        data=json.dumps(snapshot).encode(),
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            print(f"  published: HTTP {response.status} {response.read().decode().strip()}")
    except urllib.error.HTTPError as error:
        print(f"  publish refused: HTTP {error.code} {error.read().decode().strip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host", help="192.168.1.42, slate-a1b2c3.local or http://…")
    parser.add_argument(
        "--fail",
        action="store_true",
        help="answer every action with success:false — §5.3's immediate revert",
    )
    parser.add_argument("--resource", default="living-room")
    args = parser.parse_args()

    token = os.environ.get("SLATE_TOKEN")
    if not token:
        print("set SLATE_TOKEN to the device token (design.md §4.3)", file=sys.stderr)
        return 2

    host = args.host.removeprefix("http://").removeprefix("https://").rstrip("/")
    hostname, _, port = host.partition(":")
    base = f"http://{host}"
    light = Light(args.resource)

    ws = WebSocket(hostname, int(port or 80), "/api/v1/ws")
    ws.send(json.dumps({"type": "auth", "token": token}))

    try:
        while True:
            opcode, payload = ws.receive()
            if opcode == PING:
                # §4.2 keeps control ping/pong as a transport mechanism with no
                # application meaning — which is exactly why it is answered here
                # rather than ignored. The JSON `ping` this client would send is
                # the separate, application-level one.
                ws.pong(payload)
                continue
            if opcode == CLOSE:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
                print(f"panel closed the connection ({code})")
                return 1
            if opcode != TEXT:
                continue

            frame = json.loads(payload)
            kind = frame.get("type")

            if kind == "auth_invalid":
                print("the panel rejected the token", file=sys.stderr)
                return 1
            if kind == "auth_ok":
                # §5.4: one consumer at a time, so two processes cannot both
                # operate the same light. A second attachment gets provider_busy.
                ws.send(json.dumps({"type": "provider_attach", "provider": "direct"}))
                print("authenticated; attaching to the direct provider")
                continue
            if kind == "error":
                print(f"panel refused: {frame.get('error')}", file=sys.stderr)
                if frame.get("error") == "provider_busy":
                    return 1
                continue
            if kind == "status":
                # There is no `attached` frame in §4.2, and this is why one is
                # not needed: attaching moves the provider from `degraded` to
                # `online`, which is the fact a consumer was asking about.
                print(f"status: providers={frame.get('providers')} heap={frame.get('heap_free')}")
                continue
            if kind == "log":
                print(f"  [{frame.get('level')}] {frame.get('msg')}")
                continue
            if kind != "action":
                continue

            action = frame.get("action", "")
            params = frame.get("params") or {}
            identifier = frame.get("id")
            print(f"action {identifier}: {action} {params} on {frame.get('resource')}")

            if args.fail:
                ws.send(
                    json.dumps(
                        {
                            "type": "action_result",
                            "id": identifier,
                            "success": False,
                            "error": "refused_by_agent",
                        }
                    )
                )
                print("  answered success:false — the tile reverts immediately (§5.3)")
                continue

            applied = light.apply(action, params)
            ws.send(
                json.dumps(
                    {
                        "type": "action_result",
                        "id": identifier,
                        "success": applied,
                        **({} if applied else {"error": "unsupported_action"}),
                    }
                )
            )
            if applied:
                # The acknowledgement above says only that the request was
                # taken. This is what confirms the tile (§5.3).
                publish(base, token, light.snapshot())
    except KeyboardInterrupt:
        return 0
    except ConnectionError as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        ws.close()


if __name__ == "__main__":
    sys.exit(main())
