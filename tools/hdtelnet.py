#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hdtelnet.py -- minimal Telnet command runner for the HD900B (BusyBox root shell).

Python 3.13 no longer ships telnetlib, so this speaks just enough Telnet itself (it refuses all options).
The stock firmware logs in as 'root' with an empty password.

    python3 tools/hdtelnet.py <player-ip> 'uname -a'
"""
import re
import socket
import sys
import time
import uuid


class Device:
    def __init__(self, host, port=23, user="root", password="", timeout=15):
        self.host, self.port, self.user, self.password = host, port, user, password
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(1.0)
        self.buf = b""
        self._login()

    # -- low level -----------------------------------------------------------------------------
    def _recv(self, until=None, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                if until and until in self.buf:
                    return
                continue
            if not data:
                raise ConnectionError("connection closed by the player")
            out, i = bytearray(), 0
            while i < len(data):
                if data[i] == 255 and i + 2 < len(data) and data[i + 1] in (251, 252, 253, 254):   # IAC WILL/WONT/DO/DONT
                    if data[i + 1] in (251, 253):                                                   # refuse everything
                        self.sock.sendall(bytes([255, 254 if data[i + 1] == 251 else 252, data[i + 2]]))
                    i += 3
                elif data[i] == 255 and i + 1 < len(data) and data[i + 1] == 255:
                    out.append(255); i += 2
                elif data[i] == 255:
                    i += 2
                else:
                    out.append(data[i]); i += 1
            self.buf += bytes(out)
            if until and until in self.buf:
                return

    def _login(self):
        self._recv(b"login:", 6)
        if b"login:" in self.buf:
            self.sock.sendall(self.user.encode() + b"\r\n")
            self.buf = b""
            self._recv(b"assword:", 5)
            if b"assword:" in self.buf:
                self.sock.sendall(self.password.encode() + b"\r\n")
        self.buf = b""
        self._recv(b"#", 6)
        self.buf = b""

    # -- API -------------------------------------------------------------------------------------
    def run(self, cmd, timeout=60):
        """Run a shell command; returns (exit_status, output)."""
        tag = uuid.uuid4().hex[:8]
        self.buf = b""
        self.sock.sendall(("(%s) 2>&1; echo __END%s_$?__\r\n" % (cmd, tag)).encode())
        pat = re.compile(rb"__END%s_(\d+)__" % tag.encode())
        end = time.time() + timeout
        m = None
        while time.time() < end:
            self._recv(b"__END" + tag.encode() + b"_", 2)
            m = pat.search(self.buf)
            if m:
                break
        if not m:
            raise TimeoutError("command timed out: %s" % cmd[:60])
        text = self.buf[:m.start()].decode("utf-8", "replace").replace("\r", "")
        # The shell echoes our command line first (the pty may wrap it); the literal "$?__" occurs only in that echo.
        idx = text.find("$?__")
        if idx != -1:
            text = text[text.find("\n", idx) + 1:]
        return int(m.group(1)), text.strip("\n")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    d = Device(sys.argv[1])
    rc, out = d.run(" ".join(sys.argv[2:]))
    print(out)
    sys.exit(rc)
