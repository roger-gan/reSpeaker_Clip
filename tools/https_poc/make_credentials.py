"""Generate isolated short-lived test credentials, never overwrite existing files."""
import argparse
from pathlib import Path
import secrets
import subprocess


def generate(directory, hostname="localhost"):
    directory = Path(directory)
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    # DNS only; prevent injection into openssl extension syntax.
    if not hostname or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-" for c in hostname):
        raise ValueError("invalid DNS name")
    key, cert = directory / "server.key", directory / "ca.pem"
    key.touch(mode=0o600)
    subprocess.run([
        "openssl", "req", "-x509", "-sha256", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
        "-pkeyopt", "ec_param_enc:named_curve",
        "-nodes", "-days", "7", "-keyout", str(key), "-out", str(cert),
        "-subj", "/CN=" + hostname, "-addext", "subjectAltName=DNS:" + hostname,
        "-addext", "basicConstraints=critical,CA:TRUE",
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    key.chmod(0o600)
    token = directory / "token"
    token.touch(mode=0o600)
    token.write_text(secrets.token_urlsafe(32) + "\n")
    return cert, key


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("--hostname", default="localhost")
    args = parser.parse_args()
    generate(args.directory, args.hostname)
