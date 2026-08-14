import math
import re
import time
from collections import defaultdict, deque

_NAME_RE = re.compile(r"[^0-9A-Za-z_\-\u0600-\u06FF ]+")


class RateLimiter:
    def __init__(self):
        self._events = defaultdict(deque)

    def allow(self, key, limit: int, window: float) -> bool:
        now = time.monotonic()
        q = self._events[key]
        cutoff = now - window
        while q and q[0] < cutoff:
            q.popleft()
        if len(q) >= limit:
            return False
        q.append(now)
        return True

    def forget(self, prefix):
        for key in list(self._events):
            if isinstance(key, tuple) and key and key[0] == prefix:
                self._events.pop(key, None)


def sanitize_name(value) -> str:
    name = _NAME_RE.sub("", str(value or "Player")).strip()
    return (name or "Player")[:24]


def finite_number(value, default=0.0):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return number if math.isfinite(number) else default


def valid_move(player, nx: float, ny: float, nz: float, now: float, max_speed: float = 10.0) -> bool:
    dt = max(0.016, min(now - player.last_move, 0.5))
    dx = nx - player.x
    dy = ny - player.y
    dz = nz - player.z
    distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    return distance <= max_speed * dt + 1.5 and abs(ny) < 500.0 and abs(nx) < 5000.0 and abs(nz) < 5000.0


def clamp_pitch(value: float) -> float:
    return max(-89.0, min(89.0, value))


def safe_text(value, limit: int) -> str:
    return str(value or "").replace("\x00", "").strip()[:limit]
