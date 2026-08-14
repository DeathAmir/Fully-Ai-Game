import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


def recv_line(sock):
    data = bytearray()
    while not data.endswith(b"\n"):
        part = sock.recv(4096)
        if not part:
            raise RuntimeError("server closed connection")
        data.extend(part)
    return json.loads(data.decode("utf-8"))


def main():
    server_dir = Path(__file__).resolve().parent
    env = os.environ.copy()
    env["IRX_HOST"] = "127.0.0.1"
    env["IRX_PORT"] = "19832"
    env["IRX_DB"] = str(server_dir / "smoke_test.db")
    process = subprocess.Popen([sys.executable, "irx_server.py"], cwd=server_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.time() + 10
        sock = None
        while time.time() < deadline:
            if process.poll() is not None:
                output = process.stdout.read() if process.stdout else ""
                raise RuntimeError("server exited early: " + output)
            try:
                sock = socket.create_connection(("127.0.0.1", 19832), timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            raise RuntimeError("server did not accept connections")
        with sock:
            sock.sendall(b'{"type":"hello","name":"BuildTest","protocol":2}\n')
            welcome = recv_line(sock)
            if welcome.get("type") != "welcome" or welcome.get("protocol") != 2 or not welcome.get("token"):
                raise RuntimeError("invalid welcome packet")
            token = welcome["token"]
            join = json.dumps({"type": "join", "token": token, "team": "terrorist"}, separators=(",", ":")).encode() + b"\n"
            sock.sendall(join)
            deadline = time.time() + 3
            joined = False
            while time.time() < deadline:
                packet = recv_line(sock)
                if packet.get("type") == "join" and packet.get("team") == "terrorist":
                    joined = True
                    break
            if not joined:
                raise RuntimeError("join packet not observed")
            ping = json.dumps({"type": "ping", "token": token, "time": 1}, separators=(",", ":")).encode() + b"\n"
            sock.sendall(ping)
            deadline = time.time() + 3
            pong = False
            while time.time() < deadline:
                packet = recv_line(sock)
                if packet.get("type") == "pong":
                    pong = True
                    break
            if not pong:
                raise RuntimeError("pong packet not observed")
        print("iRx server smoke test passed")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
        db = server_dir / "smoke_test.db"
        wal = server_dir / "smoke_test.db-wal"
        shm = server_dir / "smoke_test.db-shm"
        for path in (db, wal, shm):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
