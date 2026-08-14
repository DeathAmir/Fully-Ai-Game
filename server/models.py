import secrets
import time
from dataclasses import dataclass, field


@dataclass
class PlayerState:
    player_id: int
    name: str = "Player"
    team: str = "spectator"
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    yaw: float = 0.0
    pitch: float = 0.0
    hp: int = 100
    armor: int = 0
    money: int = 800
    kills: int = 0
    deaths: int = 0
    assists: int = 0
    weapon: str = "pistol"
    alive: bool = True
    has_bomb: bool = False
    last_move: float = field(default_factory=time.monotonic)
    last_shot: float = 0.0


@dataclass
class ClientSession:
    writer: object
    state: PlayerState
    session_token: str = field(default_factory=lambda: secrets.token_urlsafe(24))
    connected_at: float = field(default_factory=time.monotonic)
    last_seen: float = field(default_factory=time.monotonic)
    authenticated: bool = False
    admin: bool = False
    violations: int = 0
    mute_until: float = 0.0


@dataclass
class BombState:
    state: str = "idle"
    site: str = ""
    owner: int = 0
    planter: int = 0
    planted_at: float = 0.0
    ends: float = 0.0


@dataclass
class RoundState:
    number: int = 0
    phase: str = "warmup"
    ends: float = 0.0
    terrorist_score: int = 0
    counter_score: int = 0
    winner: str = ""
    reason: str = ""
