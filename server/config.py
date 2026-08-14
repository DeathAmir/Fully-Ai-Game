import os
from dataclasses import dataclass


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except ValueError:
        return default


@dataclass(frozen=True)
class Settings:
    host: str = os.getenv("IRX_HOST", "0.0.0.0")
    port: int = _env_int("IRX_PORT", 9832)
    tick_rate: int = max(10, min(_env_int("IRX_TICK_RATE", 20), 60))
    max_packet: int = max(4096, min(_env_int("IRX_MAX_PACKET", 65536), 262144))
    db_path: str = os.getenv("IRX_DB", "irx_server.db")
    admin_token: str = os.getenv("IRX_ADMIN_TOKEN", "")
    server_name: str = os.getenv("IRX_SERVER_NAME", "iRx Official")
    max_clients: int = max(2, min(_env_int("IRX_MAX_CLIENTS", 32), 128))
    warmup_seconds: int = max(3, _env_int("IRX_WARMUP_SECONDS", 12))
    round_seconds: int = max(45, _env_int("IRX_ROUND_SECONDS", 115))
    freeze_seconds: int = max(1, _env_int("IRX_FREEZE_SECONDS", 5))
    bomb_seconds: int = max(20, _env_int("IRX_BOMB_SECONDS", 40))
    buy_seconds: int = max(10, _env_int("IRX_BUY_SECONDS", 25))


SETTINGS = Settings()
