import json
import sqlite3
from contextlib import contextmanager

from config import SETTINGS


class Storage:
    def __init__(self, path: str = SETTINGS.db_path):
        self.path = path
        self._init_db()

    @contextmanager
    def connection(self):
        conn = sqlite3.connect(self.path, timeout=10)
        try:
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA synchronous=NORMAL")
            yield conn
            conn.commit()
        finally:
            conn.close()

    def _init_db(self):
        with self.connection() as conn:
            conn.execute("CREATE TABLE IF NOT EXISTS players(name TEXT PRIMARY KEY, kills INTEGER NOT NULL DEFAULT 0, deaths INTEGER NOT NULL DEFAULT 0, assists INTEGER NOT NULL DEFAULT 0, money INTEGER NOT NULL DEFAULT 800, achievements TEXT NOT NULL DEFAULT '[]', last_seen INTEGER NOT NULL DEFAULT 0)")
            conn.execute("CREATE TABLE IF NOT EXISTS bans(name TEXT PRIMARY KEY, reason TEXT NOT NULL, expires INTEGER NOT NULL DEFAULT 0)")

    def load_player(self, player):
        with self.connection() as conn:
            row = conn.execute("SELECT kills,deaths,assists,money,achievements FROM players WHERE name=?", (player.name,)).fetchone()
        if not row:
            return []
        player.kills = int(row[0])
        player.deaths = int(row[1])
        player.assists = int(row[2])
        player.money = max(0, int(row[3]))
        try:
            return list(json.loads(row[4]))
        except Exception:
            return []

    def save_player(self, player, achievements):
        payload = json.dumps(sorted(set(achievements)), separators=(",", ":"), ensure_ascii=False)
        with self.connection() as conn:
            conn.execute("INSERT INTO players(name,kills,deaths,assists,money,achievements,last_seen) VALUES(?,?,?,?,?,?,strftime('%s','now')) ON CONFLICT(name) DO UPDATE SET kills=excluded.kills,deaths=excluded.deaths,assists=excluded.assists,money=excluded.money,achievements=excluded.achievements,last_seen=excluded.last_seen", (player.name, player.kills, player.deaths, player.assists, player.money, payload))

    def is_banned(self, name: str):
        with self.connection() as conn:
            row = conn.execute("SELECT reason,expires FROM bans WHERE name=?", (name,)).fetchone()
            if not row:
                return None
            expires = int(row[1])
            if expires and expires <= int(__import__('time').time()):
                conn.execute("DELETE FROM bans WHERE name=?", (name,))
                return None
            return str(row[0])

    def ban(self, name: str, reason: str, expires: int = 0):
        with self.connection() as conn:
            conn.execute("INSERT INTO bans(name,reason,expires) VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET reason=excluded.reason,expires=excluded.expires", (name, reason[:160], int(expires)))
