import asyncio
import json
import time

from config import SETTINGS
from game import GameWorld
from models import ClientSession, PlayerState
from security import sanitize_name, safe_text
from storage import Storage


class Server:
    def __init__(self):
        self.storage = Storage()
        self.world = GameWorld(self.storage)
        self.next_id = 1

    async def send(self, session, payload):
        raw = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(raw) > SETTINGS.max_packet:
            return
        session.writer.write(raw)
        await session.writer.drain()

    async def broadcast(self, payload, skip=0):
        dead = []
        for cid, session in list(self.world.sessions.items()):
            if cid == skip:
                continue
            try:
                await self.send(session, payload)
            except Exception:
                dead.append(cid)
        for cid in dead:
            self.world.remove_session(cid)

    async def disconnect(self, cid, reason="disconnect"):
        session = self.world.remove_session(cid)
        if not session:
            return
        try:
            await self.send(session, {"type": "disconnect", "reason": reason})
        except Exception:
            pass
        try:
            session.writer.close()
            await session.writer.wait_closed()
        except Exception:
            pass
        await self.broadcast({"type": "leave", "id": cid, "reason": reason})

    async def handle(self, cid, msg):
        session = self.world.sessions.get(cid)
        if not session or not isinstance(msg, dict):
            return
        session.last_seen = time.monotonic()
        kind = safe_text(msg.get("type"), 24)
        if kind == "hello":
            name = sanitize_name(msg.get("name"))
            ok, reason = self.world.authenticate(cid, name)
            if not ok:
                await self.disconnect(cid, "banned:" + reason)
                return
            if SETTINGS.admin_token and msg.get("admin_token") == SETTINGS.admin_token:
                session.admin = True
            await self.send(session, {"type": "welcome", "id": cid, "server": SETTINGS.server_name, "tick": SETTINGS.tick_rate, "token": session.session_token, "protocol": 2, "admin": session.admin, "achievements": sorted(self.world.achievements.get(cid, set()))})
            await self.broadcast({"type": "presence", "id": cid, "name": session.state.name, "status": "online"}, skip=cid)
            return
        if not self.world.verify(cid, msg):
            if session.violations >= 12:
                await self.disconnect(cid, "protocol_violation")
            return
        if kind == "ping":
            await self.send(session, {"type": "pong", "time": time.time_ns() // 1_000_000})
            return
        event = None
        if kind == "join":
            event = self.world.join(cid, safe_text(msg.get("team"), 16))
        elif kind == "move":
            event = self.world.move(cid, msg)
        elif kind == "buy":
            event = self.world.buy(cid, safe_text(msg.get("item"), 32))
            if event:
                await self.send(session, event)
                return
        elif kind == "shoot":
            event = self.world.shoot(cid, msg)
        elif kind == "plant":
            event = self.world.plant(cid, msg.get("site", "A"))
        elif kind == "defuse":
            event = self.world.defuse(cid)
        elif kind == "grenade":
            event = self.world.grenade(cid, msg)
        elif kind == "vote":
            event = self.world.vote(cid, msg.get("choice"))
        elif kind == "admin":
            event = self.world.admin_command(cid, msg)
        elif kind == "chat":
            if time.monotonic() >= session.mute_until and self.world.limiter.allow((cid, "chat"), 5, 4.0):
                text = safe_text(msg.get("text"), 160)
                if text:
                    event = {"type": "chat", "id": cid, "name": session.state.name, "text": text}
        if event:
            if event.get("type") == "admin_kick":
                await self.disconnect(int(event["target"]), safe_text(event.get("reason"), 80))
                await self.broadcast({"type": "admin", "action": "kick", "target": event["target"], "by": cid})
            else:
                await self.broadcast(event)

    async def client_loop(self, reader, writer):
        if len(self.world.sessions) >= SETTINGS.max_clients:
            writer.write(b'{"type":"disconnect","reason":"server_full"}\n')
            await writer.drain()
            writer.close()
            return
        cid = self.next_id
        self.next_id += 1
        session = ClientSession(writer=writer, state=PlayerState(player_id=cid))
        self.world.add_session(cid, session)
        try:
            while cid in self.world.sessions:
                raw = await reader.readline()
                if not raw or len(raw) > SETTINGS.max_packet:
                    break
                try:
                    msg = json.loads(raw.decode("utf-8"))
                except Exception:
                    session.violations += 1
                    continue
                await self.handle(cid, msg)
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            if cid in self.world.sessions:
                await self.disconnect(cid)

    async def ticker(self):
        interval = 1.0 / SETTINGS.tick_rate
        snapshot_every = max(1, SETTINGS.tick_rate // 10)
        tick = 0
        while True:
            start = time.monotonic()
            event = self.world.tick()
            if event:
                await self.broadcast(event)
            stale = [cid for cid, s in self.world.sessions.items() if start - s.last_seen > 20.0]
            for cid in stale:
                await self.disconnect(cid, "timeout")
            if tick % snapshot_every == 0:
                await self.broadcast(self.world.snapshot())
            tick += 1
            spent = time.monotonic() - start
            await asyncio.sleep(max(0.0, interval - spent))

    async def run(self):
        server = await asyncio.start_server(self.client_loop, SETTINGS.host, SETTINGS.port, limit=SETTINGS.max_packet, backlog=SETTINGS.max_clients * 2)
        addresses = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
        print(f"{SETTINGS.server_name} listening on {addresses}")
        async with server:
            await asyncio.gather(server.serve_forever(), self.ticker())


def run():
    asyncio.run(Server().run())


if __name__ == "__main__":
    run()
