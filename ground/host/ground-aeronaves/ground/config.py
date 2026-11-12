from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Config:
    reference_db: Path = ROOT.parent / 'banco-aeronaves' / 'referencias.db'
    telemetry_db: Path = ROOT / 'data' / 'telemetria.db'
    output: Path = ROOT / 'data' / 'estado.json'
    pairs_db: Path = ROOT.parent / 'pares' / 'pares_aeroportos.db'
    # CLI habilita comparação por padrão; API Python opta explicitamente.
    pairs_enabled: bool = False
    pair_interval_s: float = 1.0
    pair_workers: int = 1
    pair_max_pending: int = 128
    pair_corridor_km: float = 80.0
    pair_shutdown_timeout_s: float = 30.0
    radius_km: float = 500.0
    top: int = 5
    queue_size: int = 256
    field_ttl_s: float = 60.0
    category_ttl_s: float = 300.0
    window_s: float = 900.0
    max_observations: int = 2000
    # Inclui instalações sem serviço comercial; fechadas ficam fora do ranking.
    airport_types: tuple[str, ...] = ('small_airport', 'medium_airport', 'large_airport', 'heliport')


def add_pair_arguments(parser):
    defaults=Config()
    parser.add_argument('--pairs-db',type=Path,default=defaults.pairs_db)
    parser.add_argument('--no-pairs',action='store_false',dest='pairs_enabled',default=True,
                        help='Desativa a comparação por pares; mantém somente o método atual')
    parser.add_argument('--pair-workers',type=int,choices=range(1,5),default=defaults.pair_workers)
    parser.add_argument('--pair-interval',type=float,default=defaults.pair_interval_s,help='Intervalo mínimo entre análises por aeronave, em segundos')
    parser.add_argument('--pair-corridor-km',type=float,default=defaults.pair_corridor_km)


def pair_options(parser,args):
    import math
    if not math.isfinite(args.pair_interval) or not .1<=args.pair_interval<=3600:
        parser.error('Intervalo de pares deve estar entre 0,1 e 3600 s')
    if not math.isfinite(args.pair_corridor_km) or not 1<=args.pair_corridor_km<=500:
        parser.error('Corredor dos pares deve estar entre 1 e 500 km')
    return dict(pairs_db=args.pairs_db,pairs_enabled=args.pairs_enabled,pair_workers=args.pair_workers,
                pair_interval_s=args.pair_interval,pair_corridor_km=args.pair_corridor_km)
