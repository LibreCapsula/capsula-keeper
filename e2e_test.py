#!/usr/bin/env python3
"""End-to-end test of the keeper CLI against a mock dongle on a pty.

No hardware and no extra packages needed:
    make -C control check && make -C control && python3 e2e_test.py
"""

import os
import pty
import select
import subprocess
import sys
import threading
import time
from pathlib import Path

KEEPER = str(Path(__file__).parent / "control" / "build" / "capsula-keeper")

SOF = 0x5A
T_LOG = 0x01
T_CTRL = 0x02


def frame(t: int, payload: bytes) -> bytes:
    return bytes((SOF, t, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF)) + payload


class MockDongle:
    """Minimal firmware emulation: '@' commands + a burst of log lines."""

    def __init__(self, master_fd):
        self.master = master_fd
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _send_ctrl(self, text):
        try:
            os.write(self.master, frame(T_CTRL, text.encode()))
        except OSError:
            pass

    def _push_logs(self):
        regular = b"U-Boot 2021.10 ( keeper mock )\r\n"
        autoboot = b"Hit any key to stop autoboot:  3\r\n"
        i = 0
        while not self.stop:
            payload = autoboot if i % 3 == 0 else regular
            for k in range(0, len(payload), 20):
                try:
                    os.write(self.master, frame(T_LOG, payload[k:k + 20]))
                except OSError:
                    return
            i += 1
            time.sleep(0.2)

    def _run(self):
        threading.Thread(target=self._push_logs, daemon=True).start()
        buf = b""
        while not self.stop:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            if self.stop:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.startswith(b"@"):
                    cmd = line[1:].decode().strip()
                    if cmd == "PING":
                        self._send_ctrl("OK pong")
                    elif cmd == "STAT":
                        self._send_ctrl("OK up_s=1 baud=1500000 drops=0 "
                                        "rx_total=0 rst=release rec=release")
                    elif cmd.startswith("RESET"):
                        self._send_ctrl("OK reset done")
                    elif cmd.startswith("LOADER"):
                        self._send_ctrl("OK loader done")
                    elif cmd.startswith("MASKROM"):
                        self._send_ctrl("OK maskrom done")
                    else:
                        self._send_ctrl(f"ERR unhandled in mock: {cmd}")
                else:
                    # console passthrough: echo it back as a LOG frame
                    try:
                        os.write(self.master, frame(T_LOG, b"echo:" + line + b"\r\n"))
                    except OSError:
                        return


def run(*argv):
    p = subprocess.run([KEEPER, *argv], capture_output=True, text=True,
                       timeout=30)
    return p.returncode, p.stdout, p.stderr


def main():
    master, slave = pty.openpty()
    # The pty echoes master writes back to master by default; with the mock's
    # console-echo branch that becomes a feedback loop. Silence the echo.
    import termios
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    slave_name = os.ttyname(slave)
    mock = MockDongle(master)
    failures = []

    def check(name, cond, info=""):
        status = "ok" if cond else "FAIL"
        print(f"[{status}] {name} {info}")
        if not cond:
            failures.append(name)

    time.sleep(0.1)  # let log burst finish

    rc, out, err = run("--port", slave_name, "ping")
    check("ping", rc == 0 and "OK pong" in out, f"rc={rc} out={out!r} err={err!r}")

    rc, out, err = run("--port", slave_name, "status")
    check("status", rc == 0 and "baud=1500000" in out, f"rc={rc} out={out!r}")

    rc, out, err = run("--port", slave_name, "reset")
    check("reset", rc == 0 and "OK reset done" in out, f"rc={rc} out={out!r}")

    rc, out, err = run("--port", slave_name, "loader", "--no-usb-check")
    check("loader", rc == 0 and "OK loader done" in out, f"rc={rc} out={out!r}")

    rc, out, err = run("--port", slave_name, "maskrom", "--no-usb-check")
    check("maskrom", rc == 0 and "OK maskrom done" in out, f"rc={rc} out={out!r}")

    rc, out, err = run("--port", slave_name, "reset", "--log", "--max-lines",
                       "1", "--timeout", "5")
    check("reset --log streams", rc == 0 and "OK reset done" in out
          and out.count("\n") >= 2, f"rc={rc} out={out[:120]!r} err={err!r}")

    rc, out, err = run("--port", slave_name, "log", "--until", "autoboot",
                       "--timeout", "5")
    check("log --until hits", rc == 0 and "autoboot" in out,
          f"rc={rc} out={out!r} err={err!r}")

    rc, out, err = run("--port", slave_name, "log", "--max-lines", "2",
                       "--timeout", "5")
    check("log --max-lines stops after 2", rc == 0 and out.count("\n") == 2,
          f"rc={rc} lines={out.count(chr(10))} err={err!r}")

    rc, out, err = run("--port", slave_name, "log", "--until", "no-such-line",
                       "--timeout", "1")
    check("log --until timeout -> rc 2", rc == 2, f"rc={rc} err={err!r}")

    rc, out, err = run("--port", slave_name, "send", "version")
    check("send", rc == 0, f"rc={rc} err={err!r}")

    rc, out, err = run("--port", slave_name, "send", "@bad")
    check("send rejects leading @", rc == 1, f"rc={rc}")

    p = subprocess.run([KEEPER, "--port", slave_name, "telnet"],
                       input="version\n", capture_output=True, text=True,
                       timeout=30)
    check("telnet pipes stdin to console", p.returncode == 0
          and "echo:version" in p.stdout,
          f"rc={p.returncode} out={p.stdout[:80]!r} err={p.stderr[-120:]!r}")

    os.close(master)
    os.close(slave)
    mock.stop = True

    if failures:
        print(f"\n{len(failures)} test(s) FAILED: {failures}")
        return 1
    print("\ne2e OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
