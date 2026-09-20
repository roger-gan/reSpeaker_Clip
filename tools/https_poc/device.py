"""USB AT test client for macOS/Linux. Does not flash firmware or print secrets."""
import argparse
import json
import os
from pathlib import Path
import select
import termios
import time
import tty


def exchange(fd, command):
    payload = (command + "\r\n").encode()
    if len(payload) > 255:
        raise ValueError("command exceeds firmware limit")
    offset = 0
    while offset < len(payload):
        offset += os.write(fd, payload[offset:])
    end = time.monotonic() + 10
    response = b""
    while time.monotonic() < end:
        if not select.select([fd], [], [], 0.2)[0]:
            continue
        data = os.read(fd, 1024)
        if not data:
            raise RuntimeError("USB disconnected")
        response += data
        if len(response) > 4096:
            raise RuntimeError("response too large")
        while b"\n" in response:
            line, response = response.split(b"\n", 1)
            try:
                result = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if not result.get("ok"):
                raise RuntimeError(f"device rejected command: {result}")
            return result
    raise TimeoutError("AT response timeout")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--config", type=Path, help="private JSON: ssid,password,host,port,device,token")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--upload", help="session_id:positive_segment_number")
    mode.add_argument("--cancel", action="store_true")
    args = parser.parse_args()
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY)
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        options = termios.tcgetattr(fd)
        options[4] = options[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, options)
        termios.tcflush(fd, termios.TCIFLUSH)
        if args.config:
            if args.config.stat().st_mode & 0o077:
                parser.error("credential file must have mode 0600")
            cfg = json.loads(args.config.read_text())
            for key in ("ssid", "password", "host", "device", "token"):
                if not isinstance(cfg[key], str) or any(c in cfg[key] for c in "\r\n\0"):
                    raise ValueError("invalid configuration value")
            if "," in cfg["ssid"]:
                raise ValueError("SSID with comma not supported by this test interface")
            exchange(fd, f"AT+NETCFG={cfg['ssid']},{cfg['password']}")
            exchange(fd, f"AT+UPLOADCFG={cfg['host']},{int(cfg['port'])},{cfg['device']},{cfg['token']}")
            exchange(fd, f"AT+UPTIME={int(time.time())}")
        command = "AT+UPLOAD?"
        if args.upload:
            if any(c in args.upload for c in "\r\n\0"):
                raise ValueError("invalid upload selector")
            command = "AT+UPLOAD=" + args.upload
        elif args.cancel:
            command = "AT+UPLOADCANCEL"
        print(json.dumps(exchange(fd, command), ensure_ascii=False))
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, old)
        os.close(fd)


if __name__ == "__main__":
    main()
