import random
import time
from dataclasses import asdict

from config import SETTINGS
from models import BombState, RoundState
from security import RateLimiter, clamp_pitch, finite_number, safe_text, valid_move

WEAPONS = {
    "pistol": {"price": 0, "damage": 24, "rpm": 360, "range": 45.0},
    "deagle": {"price": 700, "damage": 54, "rpm": 220, "range": 58.0},
    "smg": {"price": 1250, "damage": 22, "rpm": 780, "range": 38.0},
    "shotgun": {"price": 1800, "damage": 66, "rpm": 95, "range": 18.0},
    "rifle": {"price": 2700, "damage": 34, "rpm": 620, "range": 70.0},
    "ak": {"price": 2900, "damage": 36, "rpm": 600, "range": 72.0},
    "sniper": {"price": 4750, "damage": 115, "rpm": 42, "range": 160.0},
    "mythic_ember": {"price": 0, "damage": 36, "rpm": 600, "range": 72.0, "effect": "ember"},
    "mythic_void": {"price": 0, "damage": 34, "rpm": 640, "range": 70.0, "effect": "void"},
}

GRENADE_PRICES = {"flash": 200, "smoke": 300, "he": 300}


class GameWorld:
    def __init__(self, storage):
        self.storage = storage
        self.sessions = {}
        self.achievements = {}
        self.bomb = BombState()
        self.round = RoundState(number=0, phase="warmup", ends=time.monotonic() + SETTINGS.warmup_seconds)
        self.limiter = RateLimiter()
        self.votes = {}
        self.next_bomb_owner = 0

    def add_session(self, cid, session):
        self.sessions[cid] = session
        self.achievements[cid] = set()

    def remove_session(self, cid):
        session = self.sessions.pop(cid, None)
        achievements = self.achievements.pop(cid, set())
        self.votes.pop(cid, None)
        self.limiter.forget(cid)
        if session and session.authenticated:
            self.storage.save_player(session.state, achievements)
        return session

    def snapshot(self):
        now = time.monotonic()
        players = []
        for cid, session in self.sessions.items():
            p = session.state
            players.append({"id": cid, "name": p.name, "team": p.team, "x": round(p.x, 3), "y": round(p.y, 3), "z": round(p.z, 3), "yaw": round(p.yaw, 3), "pitch": round(p.pitch, 3), "hp": p.hp, "armor": p.armor, "money": p.money, "kills": p.kills, "deaths": p.deaths, "assists": p.assists, "weapon": p.weapon, "alive": p.alive, "bomb": p.has_bomb})
        round_payload = asdict(self.round)
        round_payload["remaining"] = max(0.0, self.round.ends - now)
        bomb_payload = asdict(self.bomb)
        bomb_payload["remaining"] = max(0.0, self.bomb.ends - now) if self.bomb.state == "planted" else 0.0
        return {"type": "snapshot", "round": round_payload, "bomb": bomb_payload, "players": players}

    def authenticate(self, cid, name):
        session = self.sessions[cid]
        session.state.name = name
        reason = self.storage.is_banned(name)
        if reason:
            return False, reason
        loaded = self.storage.load_player(session.state)
        self.achievements[cid] = set(loaded)
        session.authenticated = True
        return True, ""

    def verify(self, cid, msg):
        session = self.sessions.get(cid)
        if not session or not session.authenticated:
            return False
        if msg.get("token") != session.session_token:
            session.violations += 1
            return False
        if not self.limiter.allow((cid, "packet"), 120, 1.0):
            session.violations += 1
            return False
        return True

    def join(self, cid, team):
        if team not in {"terrorist", "counter", "spectator"}:
            return None
        p = self.sessions[cid].state
        p.team = team
        p.hp = 100
        p.armor = 0
        p.alive = team != "spectator"
        p.has_bomb = False
        return {"type": "join", "id": cid, "name": p.name, "team": team}

    def move(self, cid, msg):
        session = self.sessions[cid]
        p = session.state
        if not p.alive or p.team == "spectator":
            return None
        now = time.monotonic()
        nx = finite_number(msg.get("x"), p.x)
        ny = finite_number(msg.get("y"), p.y)
        nz = finite_number(msg.get("z"), p.z)
        if not valid_move(p, nx, ny, nz, now):
            session.violations += 1
            return None
        p.x, p.y, p.z = nx, ny, nz
        p.yaw = finite_number(msg.get("yaw"), p.yaw) % 360.0
        p.pitch = clamp_pitch(finite_number(msg.get("pitch"), p.pitch))
        p.last_move = now
        return None

    def buy(self, cid, item):
        p = self.sessions[cid].state
        if self.round.phase not in {"freeze", "live"}:
            return None
        if time.monotonic() > self.round.ends - SETTINGS.round_seconds + SETTINGS.buy_seconds and self.round.phase == "live":
            return None
        if item in WEAPONS:
            price = int(WEAPONS[item]["price"])
            if p.money < price:
                return {"type": "buy", "ok": False, "item": item, "reason": "money"}
            p.money -= price
            p.weapon = item
            return {"type": "buy", "ok": True, "item": item, "money": p.money}
        if item == "armor":
            if p.money < 650:
                return {"type": "buy", "ok": False, "item": item, "reason": "money"}
            p.money -= 650
            p.armor = 100
            return {"type": "buy", "ok": True, "item": item, "money": p.money}
        if item in GRENADE_PRICES:
            price = GRENADE_PRICES[item]
            if p.money < price:
                return {"type": "buy", "ok": False, "item": item, "reason": "money"}
            p.money -= price
            return {"type": "buy", "ok": True, "item": item, "money": p.money}
        return None

    def shoot(self, cid, msg):
        attacker_session = self.sessions[cid]
        attacker = attacker_session.state
        if self.round.phase != "live" or not attacker.alive:
            return None
        weapon_name = safe_text(msg.get("weapon", attacker.weapon), 32)
        spec = WEAPONS.get(weapon_name)
        if not spec:
            attacker_session.violations += 1
            return None
        now = time.monotonic()
        min_delay = 60.0 / float(spec["rpm"])
        if now - attacker.last_shot + 0.01 < min_delay:
            attacker_session.violations += 1
            return None
        attacker.last_shot = now
        try:
            target_id = int(msg.get("target", 0))
        except Exception:
            return None
        victim_session = self.sessions.get(target_id)
        if not victim_session:
            return None
        victim = victim_session.state
        if not victim.alive or victim.team == attacker.team or victim.team == "spectator":
            return None
        dx = victim.x - attacker.x
        dy = victim.y - attacker.y
        dz = victim.z - attacker.z
        if dx * dx + dy * dy + dz * dz > float(spec["range"]) ** 2:
            attacker_session.violations += 1
            return None
        base = int(spec["damage"])
        hit = safe_text(msg.get("hit", "body"), 16)
        damage = base * (2 if hit == "head" else 1)
        absorbed = min(victim.armor, max(0, damage // 2))
        victim.armor -= absorbed
        victim.hp -= damage - absorbed
        event = {"type": "hit", "attacker": cid, "victim": target_id, "damage": damage - absorbed, "hp": max(victim.hp, 0), "hit": hit}
        if victim.hp <= 0:
            victim.hp = 0
            victim.alive = False
            victim.deaths += 1
            attacker.kills += 1
            attacker.money += 300
            effect = spec.get("effect", "default")
            event = {"type": "kill", "killer": cid, "victim": target_id, "weapon": weapon_name, "effect": effect, "headshot": hit == "head"}
            self._unlock(cid, "first_blood") if attacker.kills == 1 else None
            self._unlock(cid, "headhunter") if hit == "head" else None
            self.storage.save_player(victim, self.achievements.get(target_id, set()))
            self.storage.save_player(attacker, self.achievements.get(cid, set()))
        return event

    def plant(self, cid, site):
        p = self.sessions[cid].state
        if self.round.phase != "live" or p.team != "terrorist" or not p.alive or not p.has_bomb or self.bomb.state != "idle":
            return None
        site = safe_text(site, 1).upper()
        if site not in {"A", "B"}:
            return None
        now = time.monotonic()
        self.bomb = BombState(state="planted", site=site, owner=cid, planter=cid, planted_at=now, ends=now + SETTINGS.bomb_seconds)
        p.has_bomb = False
        p.money += 300
        return {"type": "bomb", **asdict(self.bomb)}

    def defuse(self, cid):
        p = self.sessions[cid].state
        if self.round.phase != "live" or p.team != "counter" or not p.alive or self.bomb.state != "planted":
            return None
        self.bomb.state = "defused"
        self.bomb.ends = time.monotonic()
        p.money += 300
        self.finish_round("counter", "defused")
        return {"type": "bomb", **asdict(self.bomb)}

    def grenade(self, cid, msg):
        p = self.sessions[cid].state
        if self.round.phase != "live" or not p.alive:
            return None
        kind = safe_text(msg.get("kind"), 16)
        if kind not in GRENADE_PRICES:
            return None
        if not self.limiter.allow((cid, "grenade"), 3, 5.0):
            return None
        return {"type": "grenade", "id": cid, "kind": kind, "x": p.x, "y": p.y, "z": p.z, "vx": finite_number(msg.get("vx")), "vy": finite_number(msg.get("vy")), "vz": finite_number(msg.get("vz"))}

    def vote(self, cid, choice):
        choice = safe_text(choice, 24)
        if choice not in {"restart", "dust", "industrial", "suburban", "station"}:
            return None
        self.votes[cid] = choice
        counts = {}
        for value in self.votes.values():
            counts[value] = counts.get(value, 0) + 1
        needed = max(2, (len(self.sessions) // 2) + 1)
        passed = counts.get(choice, 0) >= needed
        return {"type": "vote", "choice": choice, "votes": counts.get(choice, 0), "needed": needed, "passed": passed}

    def admin_command(self, cid, msg):
        session = self.sessions[cid]
        if not session.admin:
            return None
        action = safe_text(msg.get("action"), 24)
        if action == "restart":
            self.start_round()
            return {"type": "admin", "action": action, "by": cid}
        if action == "kick":
            try:
                target = int(msg.get("target", 0))
            except Exception:
                return None
            if target in self.sessions and target != cid:
                return {"type": "admin_kick", "target": target, "reason": safe_text(msg.get("reason", "admin"), 80)}
        return None

    def tick(self):
        now = time.monotonic()
        if self.round.phase == "warmup" and now >= self.round.ends:
            self.start_round()
            return {"type": "round", **asdict(self.round)}
        if self.round.phase == "freeze" and now >= self.round.ends:
            self.round.phase = "live"
            self.round.ends = now + SETTINGS.round_seconds
            return {"type": "round", **asdict(self.round)}
        if self.round.phase == "live":
            if self.bomb.state == "planted" and now >= self.bomb.ends:
                self.bomb.state = "exploded"
                self.finish_round("terrorist", "exploded")
                return {"type": "bomb", **asdict(self.bomb)}
            alive_t = sum(1 for s in self.sessions.values() if s.state.team == "terrorist" and s.state.alive)
            alive_ct = sum(1 for s in self.sessions.values() if s.state.team == "counter" and s.state.alive)
            if alive_t == 0 and any(s.state.team == "terrorist" for s in self.sessions.values()) and self.bomb.state != "planted":
                self.finish_round("counter", "elimination")
                return {"type": "round", **asdict(self.round)}
            if alive_ct == 0 and any(s.state.team == "counter" for s in self.sessions.values()):
                self.finish_round("terrorist", "elimination")
                return {"type": "round", **asdict(self.round)}
            if now >= self.round.ends and self.bomb.state != "planted":
                self.finish_round("counter", "timeout")
                return {"type": "round", **asdict(self.round)}
        if self.round.phase == "ended" and now >= self.round.ends:
            self.start_round()
            return {"type": "round", **asdict(self.round)}
        return None

    def start_round(self):
        now = time.monotonic()
        self.round.number += 1
        self.round.phase = "freeze"
        self.round.ends = now + SETTINGS.freeze_seconds
        self.round.winner = ""
        self.round.reason = ""
        self.bomb = BombState()
        self.votes.clear()
        terrorists = []
        for session in self.sessions.values():
            p = session.state
            if p.team in {"terrorist", "counter"}:
                p.hp = 100
                p.armor = 0
                p.alive = True
                p.has_bomb = False
                p.weapon = "pistol"
                if p.team == "terrorist":
                    terrorists.append(p)
        if terrorists:
            random.choice(terrorists).has_bomb = True

    def finish_round(self, winner, reason):
        self.round.phase = "ended"
        self.round.ends = time.monotonic() + 6.0
        self.round.winner = winner
        self.round.reason = reason
        if winner == "terrorist":
            self.round.terrorist_score += 1
        elif winner == "counter":
            self.round.counter_score += 1
        for session in self.sessions.values():
            p = session.state
            if p.team == winner:
                p.money = min(16000, p.money + 3250)
            elif p.team in {"terrorist", "counter"}:
                p.money = min(16000, p.money + 1900)

    def _unlock(self, cid, achievement):
        unlocked = self.achievements.setdefault(cid, set())
        unlocked.add(achievement)
