import argparse
import json
import sys
from pathlib import Path
from queue import Queue
from threading import Event, Thread

from .airport_estimator import AirportEstimator
from .aircraft_manager import AircraftManager
from .config import Config
from .database import Database
from .parser import parse_line
from .receiver import receive
from .reporting import table_row


def run(config, stream, interval, verbose=False):
    estimator = AirportEstimator(config)
    database = Database(config.telemetry_db)
    manager = AircraftManager(database, estimator, config)
    queue = Queue(maxsize=config.queue_size)
    stop = Event()
    counts = {'accepted': 0, 'rejected': 0}

    def consume():
        while True:
            item = queue.get()
            if item is None:
                return
            if isinstance(item, Exception):
                raise item
            raw, received = item
            try:
                observation = parse_line(raw, received)
            except (ValueError, TypeError, OverflowError, RecursionError) as exc:
                database.reject(raw, received, str(exc))
                counts['rejected'] += 1
                print(f'Mensagem em quarentena: {exc}', file=sys.stderr)
                continue
            result = manager.process(observation)
            counts['accepted'] += 1
            if verbose:
                print(json.dumps({'icao':result['icao'],'flight_state':result['flight_state'],
                    'airports':{k:result[k] for k in ('reason','airport_method','destination_candidates','origin_candidates')},
                    'quality':result['data_quality'],'vertical':result['vertical_estimate']}, ensure_ascii=False), flush=True)

    receiver = Thread(target=receive, args=(stream, queue, interval, stop), daemon=True)
    receiver.start()
    try:
        consume()
    finally:
        stop.set()
        receiver.join(timeout=1)
    print('\nICAO | MÉTODO | DECOLAGEM | POUSO | CONFIABILIDADE', flush=True)
    for result in manager.states.values():
        print(table_row(result, config.field_ttl_s, include_method=True), flush=True)
    print('Confiabilidade qualitativa: hipóteses geométricas não calibradas; não é porcentagem de acerto.', flush=True)
    print(f"Mensagens aceitas: {counts['accepted']} | rejeitadas: {counts['rejected']}", file=sys.stderr)
    return counts


def main():
    defaults = Config()
    parser = argparse.ArgumentParser(description='Ground ADS-B: NDJSON → SQLite → estimativas por aeroportos')
    parser.add_argument('--input', default='-', help='Arquivo NDJSON; - recebe do stdin')
    parser.add_argument('--reference-db', type=Path, default=defaults.reference_db)
    parser.add_argument('--telemetry-db', type=Path, default=defaults.telemetry_db)
    parser.add_argument('--output', type=Path, default=defaults.output)
    parser.add_argument('--radius-km', type=float, default=defaults.radius_km)
    parser.add_argument('--top', type=int, default=defaults.top)
    parser.add_argument('--interval', type=float, default=0, help='Pausa entre linhas no replay, em segundos')
    parser.add_argument('--verbose', action='store_true', help='Mostra o resultado de cada mensagem, além do resumo final')
    parser.add_argument('--airport-types', nargs='+', default=list(defaults.airport_types),
                        choices=['small_airport', 'medium_airport', 'large_airport', 'heliport', 'seaplane_base', 'balloonport'])
    args = parser.parse_args()
    if not 0 < args.radius_km <= 20020 or not 1 <= args.top <= 100 or not 0 <= args.interval <= 3600:
        parser.error('Raio deve estar em (0,20020], top em [1,100] e intervalo em [0,3600]')
    config = Config(reference_db=args.reference_db, telemetry_db=args.telemetry_db, output=args.output,
                    radius_km=args.radius_km, top=args.top, airport_types=tuple(args.airport_types))
    paths = [config.reference_db.resolve(), config.telemetry_db.resolve(), config.output.resolve()]
    if len(set(paths)) != 3:
        parser.error('Referências, telemetria e saída devem ser arquivos diferentes')
    if args.input != '-' and Path(args.input).resolve() in paths:
        parser.error('A entrada NDJSON deve ser diferente dos arquivos de banco e saída')
    stream = sys.stdin if args.input == '-' else open(args.input, encoding='utf-8')
    try:
        run(config, stream, args.interval, args.verbose)
    except KeyboardInterrupt:
        print('Execução interrompida; observações já confirmadas permanecem no SQLite.', file=sys.stderr)
    finally:
        if stream is not sys.stdin:
            stream.close()


if __name__ == '__main__':
    main()
