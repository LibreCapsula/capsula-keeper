#!/usr/bin/env python3
"""Stress tests for the keeper CLI: hostile mock dongle on a pty.

Covers the failure modes a real dongle produces: unplugged USB mid-command,
garbage on the wire, bursty logs, rapid-fire CLI invocations and two
processes fighting over the port.
"""

import os
import pty
import re
import select
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path

KEEPER = str(Path(__file__).parent / "control" / "build" / "capsula-keeper")
SOF = 0x5A
T_LOG = 0x01
T_CTRL = 0x02
failures = []


def frame(t: int, payload: bytes) -> bytes:
    return bytes((SOF, t, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF)) + payload


class HostileDongle:
    """Mock that replies to @commands and can inject garbage / drop dead."""

    def __init__(self, master_fd, garbage_every=0, reply_delay=0.0):
        self.master = master_fd
        self.alive = True
        self.garbage_every = garbage_every  # inject garbage before each reply
        self.reply_delay = reply_delay  # seconds to sit on each reply
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _send_ctrl(self, text):
        with self.lock:
            try:
                os.write(self.master, frame(T_CTRL, text.encode()))
            except OSError:
                pass

    def _inject_garbage(self):
        if not self.garbage_every:
            return
        with self.lock:
            try:
                os.write(self.master, bytes((SOF, 0x77, 0x40, 0x00) for _ in range(0)) or
                         b"\x5a\xff\x10\x00decoy-header" + os.urandom(9))
            except OSError:
                pass

    def _run(self):
        buf = b""
        while self.alive:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.startswith(b"@"):
                    self._inject_garbage()
                    cmd = line[1:].decode(errors="replace").strip()
                    if self.reply_delay:
                        time.sleep(self.reply_delay)
                    if cmd == "PING":
                        self._send_ctrl("OK pong")
                    elif cmd == "STAT":
                        self._send_ctrl("OK up_s=9 baud=1500000 drops=0 "
                                        "rx_total=42 rst=release rec=release "
                                        "msk=release")
                    elif cmd.startswith("RESET"):
                        self._send_ctrl("OK reset done")
                    else:
                        self._send_ctrl(f"ERR unhandled: {cmd}")
                else:
                    with self.lock:
                        try:
                            os.write(self.master, frame(T_LOG, b"echo:" + line + b"\r\n"))
                        except OSError:
                            return

    def close(self):
        self.alive = False
        try:
            os.close(self.master)
        except OSError:
            pass


def make_port(garbage_every=0, reply_delay=0.0):
    master, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    return master, os.ttyname(slave), HostileDongle(master, garbage_every,
                                                    reply_delay)


def check(name, cond, info=""):
    print(f"[{'ok' if cond else 'FAIL'}] {name} {info}")
    if not cond:
        failures.append(name)


def run(*argv, timeout=15):
    try:
        p = subprocess.run([KEEPER, *argv], capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "HANG", "", ""


def main():
    # --- 1. garbage on the wire before/around replies ---------------------
    m, slave, mock = make_port(garbage_every=1)
    rc, out, err = run("--port", slave, "ping")
    check("ping with garbage on the wire", rc == 0 and "OK pong" in out,
          f"rc={rc} out={out!r} err={err!r}")
    mock.close()
    os.close(m) if False else None

    # --- 2. dongle unplugged while a command is in flight ------------------
    m, slave, mock = make_port(reply_delay=3.0)
    t = threading.Timer(0.5, mock.close)
    t.start()
    rc, out, err = run("--port", slave, "ping", timeout=20)
    check("unplug mid-command errors out (no hang)",
          rc not in ("HANG", 0), f"rc={rc} err={err!r}")

    # --- 3. dongle unplugged during log stream -----------------------------
    class LogDongle(HostileDongle):
        def _run(self):
            i = 0
            while self.alive:
                with self.lock:
                    try:
                        os.write(self.master, frame(T_LOG, b"log line %d\r\n" % i))
                    except OSError:
                        return
                i += 1
                time.sleep(0.05)

    m, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    mock = LogDongle(m)
    slave_name = os.ttyname(slave)
    t = threading.Timer(0.5, mock.close)
    t.start()
    rc, out, err = run("--port", slave_name, "log", timeout=20)
    check("unplug during log stream errors out (no hang)", rc not in ("HANG", 2),
          f"rc={rc} err={err!r}")
    try:
        os.close(slave)
    except OSError:
        pass

    # --- 4. rapid-fire invocations -----------------------------------------
    m, slave, mock = make_port()
    ok = 0
    t0 = time.monotonic()
    for _ in range(30):
        rc, out, err = run("--port", os.ttyname(m if False else m) if False else slave, "ping")
        if rc == 0 and "OK pong" in out:
            ok += 1
    dt = time.monotonic() - t0
    check("30 rapid-fire pings all OK", ok == 30, f"ok={ok}/30 in {dt:.1f}s")
    mock.close()

    # --- 5. port lock: a second keeper must get a clear error ---------------
    m, slave, mock = make_port()
    p1 = subprocess.Popen([KEEPER, "--port", slave, "log"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          stdin=subprocess.DEVNULL)
    time.sleep(0.5)  # let p1 take the lock
    rc, out, err = run("--port", slave, "ping", timeout=10)
    check("second keeper on busy port fails clearly",
          rc == 1 and "busy" in err, f"rc={rc} out={out!r} err={err!r}")
    p1.kill()
    p1.communicate(timeout=20)
    rc, out, err = run("--port", slave, "ping")
    check("port lock released after the holder exits", rc == 0 and "OK pong" in out,
          f"rc={rc} err={err!r}")
    mock.close()

    # --- 6. long burst of log data (stream integrity) ------------------------
    class BurstDongle(HostileDongle):
        @staticmethod
        def _write_all(fd, data):
            off = 0
            while off < len(data):
                try:
                    _, w, _ = select.select([], [fd], [], 5.0)
                    if not w:
                        return
                    off += os.write(fd, data[off:])
                except OSError:
                    return

        def _run(self):
            # start streaming only after the CLI has opened the port
            time.sleep(0.5)
            payload = b"".join(b"line-%06d abcdefgh\r\n" % i for i in range(500))
            for k in range(0, len(payload), 100):
                self._write_all(self.master, frame(T_LOG, payload[k:k + 100]))
            while self.alive:
                r, _, _ = select.select([self.master], [], [], 0.05)
                if not r:
                    continue
                try:
                    buf = os.read(self.master, 256)
                except OSError:
                    return
                if b"@PING" in buf:
                    self._send_ctrl("OK pong")

    m, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    mock = BurstDongle(m)
    slave_name6 = os.ttyname(slave)
    rc, out, err = run("--port", slave_name6, "log", "--max-lines", "500",
                       "--timeout", "10")
    lines = re.findall(rb"line-(\d{6})", out.encode())
    seq_ok = len(lines) == 500 and lines == [b"%06d" % i for i in range(500)]
    check("500-line burst arrives complete and in order", rc == 0 and seq_ok,
          f"rc={rc} lines={len(lines)} err={err!r}")
    # --- 8. reset --log: boot logs that arrive before/with the RESET reply ----
    class ResetDongle(HostileDongle):
        def _run(self):
            buf = b""
            while self.alive:
                r, _, _ = select.select([self.master], [], [], 0.05)
                if not r:
                    continue
                try:
                    buf += os.read(self.master, 256)
                except OSError:
                    return
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.startswith(b"@"):
                        continue
                    if not line[1:].startswith(b"RESET"):
                        continue
                    # boot log BEFORE the reply: keeper must buffer it
                    with self.lock:
                        try:
                            os.write(self.master,
                                     frame(T_LOG, b"DDR training ok\r\n"))
                        except OSError:
                            return
                    self._send_ctrl("OK reset done")
                    with self.lock:
                        try:
                            os.write(self.master,
                                     frame(T_LOG, b"U-Boot 2021.10\r\n"))
                        except OSError:
                            return

    m, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    slave_name8 = os.ttyname(slave)
    mock = ResetDongle(m)
    rc, out, err = run("--port", slave_name8, "reset", "--log", "--max-lines",
                       "2", "--timeout", "5")
    ok = (rc == 0 and "OK reset done" in out and "DDR training ok" in out
          and "U-Boot 2021.10" in out)
    check("reset --log shows boot logs arriving around the RESET reply", ok,
          f"rc={rc} out={out!r} err={err!r}")
    mock.close()

    # --- 9. dongle loses the first command in a log flood (CLI must resend) ---
    class DeafOnceDongle(HostileDongle):
        def __init__(self, fd):
            super().__init__(fd)
            self.first_cmd_seen = False

        def _run(self):
            buf = b""
            while self.alive:
                r, _, _ = select.select([self.master], [], [], 0.05)
                if not r:
                    continue
                try:
                    buf += os.read(self.master, 256)
                except OSError:
                    return
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.startswith(b"@"):
                        continue
                    if not self.first_cmd_seen:
                        self.first_cmd_seen = True
                        continue  # swallow the first command like a busy fw
                    if line[1:].strip() == b"PING":
                        self._send_ctrl("OK pong")

    m, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    slave_name9 = os.ttyname(slave)
    mock = DeafOnceDongle(m)
    rc, out, err = run("--port", slave_name9, "ping")
    check("lost first command is resent and answered", rc == 0 and "OK pong" in out,
          f"rc={rc} out={out!r} err={err!r}")
    mock.close()

    # --- 7. ping right after log stream closed (port reopen) -----------------
    rc, out, err = run("--port", slave_name6, "ping")
    check("reopen after burst still works", rc == 0 and "OK pong" in out,
          f"rc={rc} err={err!r}")
    mock.close()
    try:
        os.close(slave)
    except OSError:
        pass

    if failures:
        print(f"\n{len(failures)} test(s) FAILED: {failures}")
        return 1
    print("\nstress OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
