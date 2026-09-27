from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Config:
    reference_db: Path = ROOT.parent / 'banco-aeronaves' / 'referencias.db'
    telemetry_db: Path = ROOT / 'data' / 'telemetria.db'
    output: Path = ROOT / 'data' / 'estado.json'
    radius_km: float = 500.0
    top: int = 5
    queue_size: int = 256
    field_ttl_s: float = 60.0
    window_s: float = 900.0
    max_observations: int = 2000
    # Inclui instalações sem serviço comercial; fechadas ficam fora do ranking.
    airport_types: tuple[str, ...] = ('small_airport', 'medium_airport', 'large_airport')
