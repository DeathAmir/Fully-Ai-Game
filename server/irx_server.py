import asyncio
import json
import sqlite3
import time
from dataclasses import dataclass, field

HOST = "0.0.0.0"
PORT = 9832
TICK_RATE = 20
DB_PATH = "irx_server.db"
MAX_PACKET = 65536

@dataclass
class Client:
    writer: asyncio.StreamWriter
    name: str = "Player"
    team: str = "spectator"
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    yaw: float = 0.0
    pitch: float = 0.0
    hp: int = 100
    armor: int = 0
    kills: int = 0
    deaths: int = 0
    last_seen: float = field(default_factory=time.monotonic)
    authed: bool = False

clients: dict[int, Client] = {}
next_id = 1
bomb = {"state": "idle", "site": "", "owner": 0, "ends": 0.0}
round_state = {"number": 1, "phase": "warmup", "ends": time.monotonic() + 15.0}


def db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("CREATE TABLE IF NOT EXISTS players(name TEXT PRIMARY KEY, kills INTEGER NOT NULL DEFAULT 0, deaths INTEGER NOT NULL DEFAULT 0, achievements TEXT NOT NULL DEFAULT '[]')")
    return conn


def save_player(c: Client):
    with db() as conn:
        conn.execute("INSERT INTO players(name,kills,deaths,achievements) VALUES(?,?,?,?) ON CONFLICT(name) DO UPDATE SET kills=excluded.kills,deaths=excluded.deaths", (c.name, c.kills, c.deaths, "[]"))


def load_player(c: Client):
    with db() as conn:
        row = conn.execute("SELECT kills,deaths FROM players WHERE name=?", (c.name,)).fetchone()
    if row:
        c.kills, c.deaths = int(row[0]), int(row[1])


async def send(c: Client, payload: dict):
    raw = (json.dumps(payload, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
    if len(raw) > MAX_PACKET:
        return
    c.writer.write(raw)
    await c.writer.drain()


async def broadcast(payload: dict, skip: int = 0):
    dead = []
    for cid, c in list(clients.items()):
        if cid == skip:
            continue
        try:
            await send(c, payload)
        except Exception:
            dead.append(cid)
    for cid in dead:
        clients.pop(cid, None)


def snapshot():
    return {
        "type": "snapshot",
        "round": round_state,
        "bomb": bomb,
        "players": [
            {"id": cid, "name": c.name, "team": c.team, "x": round(c.x, 3), "y": round(c.y, 3), "z": round(c.z, 3), "yaw": round(c.yaw, 3), "pitch": round(c.pitch, 3), "hp": c.hp, "armor": c.armor, "kills": c.kills, "deaths": c.deaths}
            for cid, c in clients.items()
        ],
    }


def valid_team(team: str):
    return team in {"terrorist", "counter", "spectator"}


async def handle_message(cid: int, msg: dict):
    c = clients.get(cid)
    if not c:
        return
    c.last_seen = time.monotonic()
    t = str(msg.get("type", ""))
    if t == "hello":
        name = str(msg.get("name", "Player")).strip()[:24]
        c.name = name or "Player"
        c.authed = True
        load_player(c)
        await send(c, {"type": "welcome", "id": cid, "server": "iRx", "port": PORT, "tick": TICK_RATE})
        return
    if not c.authed:
        return
    if t == "join":
        team = str(msg.get("team", "spectator"))
        if valid_team(team):
            c.team = team
            c.hp = 100
            c.armor = 0
            await broadcast({"type": "join", "id": cid, "team": team, "name": c.name})
    elif t == "move":
        try:
            nx, ny, nz = float(msg["x"]), float(msg["y"]), float(msg["z"])
            yaw, pitch = float(msg.get("yaw", 0.0)), float(msg.get("pitch", 0.0))
        except Exception:
            return
        dx, dy, dz = nx - c.x, ny - c.y, nz - c.z
        if dx * dx + dy * dy + dz * dz > 64.0:
            return
        c.x, c.y, c.z, c.yaw, c.pitch = nx, ny, nz, yaw, max(-89.0, min(89.0, pitch))
    elif t == "shoot":
        target = int(msg.get("target", 0))
        damage = max(0, min(int(msg.get("damage", 0)), 120))
        v = clients.get(target)
        if not v or v.team == c.team or v.team == "spectator" or damage <= 0:
            return
        absorbed = min(v.armor, damage // 2)
        v.armor -= absorbed
        v.hp -= damage - absorbed
        if v.hp <= 0:
            v.hp = 0
            v.deaths += 1
            c.kills += 1
            save_player(v)
            save_player(c)
            await broadcast({"type": "kill", "killer": cid, "victim": target, "weapon": str(msg.get("weapon", "unknown"))[:32], "effect": str(msg.get("effect", "default"))[:32]})
    elif t == "plant":
        if c.team == "terrorist" and bomb["state"] == "idle":
            bomb.update(state="planted", site=str(msg.get("site", "A"))[:1], owner=cid, ends=time.monotonic() + 40.0)
            await broadcast({"type": "bomb", **bomb})
    elif t == "defuse":
        if c.team == "counter" and bomb["state"] == "planted":
            bomb.update(state="defused", ends=time.monotonic())
            await broadcast({"type": "bomb", **bomb})
    elif t == "chat":
        text = str(msg.get("text", "")).strip()[:160]
        if text:
            await broadcast({"type": "chat", "id": cid, "name": c.name, "text": text})


async def client_loop(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    global next_id
    cid = next_id
    next_id += 1
    clients[cid] = Client(writer=writer)
    try:
        while True:
            raw = await reader.readline()
            if not raw or len(raw) > MAX_PACKET:
                break
            try:
                msg = json.loads(raw.decode("utf-8"))
            except Exception:
                continue
            if isinstance(msg, dict):
                await handle_message(cid, msg)
    finally:
        c = clients.pop(cid, None)
        if c:
            save_player(c)
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass
        await broadcast({"type": "leave", "id": cid})


async def ticker():
    while True:
        now = time.monotonic()
        if bomb["state"] == "planted" and now >= bomb["ends"]:
            bomb.update(state="exploded", ends=now)
            await broadcast({"type": "bomb", **bomb})
        stale = [cid for cid, c in clients.items() if now - c.last_seen > 15.0]
        for cid in stale:
            c = clients.pop(cid, None)
            if c:
                c.writer.close()
        await broadcast(snapshot())
        await asyncio.sleep(1.0 / TICK_RATE)


async def main():
    server = await asyncio.start_server(client_loop, HOST, PORT, limit=MAX_PACKET)
    print(f"iRx server listening on {HOST}:{PORT}")
    async with server:
        await asyncio.gather(server.serve_forever(), ticker())


if __name__ == "__main__":
    asyncio.run(main())
