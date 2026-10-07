"""Acompanha o NDJSON enquanto o servidor web está aberto."""

import fcntl
import os
import time
from threading import Event, Thread

from .database import Database
from .airport_estimator import AirportEstimator
from .aircraft_manager import AircraftManager
from .parser import parse_line


def follow_lines(path, stop):
    stream = None
    try:
        while not stop.is_set():
            try:
                stat = path.stat()
                if stream and (
                    os.fstat(stream.fileno()).st_ino != stat.st_ino
                    or stat.st_size < stream.tell()
                ):
                    stream.close()
                    stream = None
                if stream is None:
                    stream = path.open("rb")
                offset = stream.tell()
                line = stream.readline()
                if line.endswith(b"\n"):
                    yield line.decode("utf-8")
                    continue
                stream.seek(offset)  # Wait for the producer to finish the line.
            except FileNotFoundError:
                pass
            stop.wait(0.2)
    finally:
        if stream:
            stream.close()


class LiveInput:
    def __init__(self, path, config):
        self.path, self.config = path, config
        self.stop = Event()
        self.lock = None
        self.thread = None
        self.session = None

    def start(self):
        self.config.telemetry_db.parent.mkdir(parents=True, exist_ok=True)
        self.lock = self.config.telemetry_db.with_suffix(".viewer.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            self.lock = None
            print(
                "Outro painel já acompanha este banco; usando seu processamento.",
                flush=True,
            )
            return
        database = Database(self.config.telemetry_db)
        manager = AircraftManager(database, AirportEstimator(self.config), self.config)
        self.session = database.session_id

        def consume():
            try:
                for raw in follow_lines(self.path, self.stop):
                    received = time.time_ns()
                    try:
                        obs = parse_line(raw, received)
                    except (ValueError, TypeError, OverflowError) as exc:
                        database.reject(raw, received, str(exc))
                        continue
                    # Catch up with a simulator already started without replaying
                    # old captures as if they were live. Existing history remains.
                    if obs.event_ns < received - int(self.config.window_s * 1e9):
                        continue
                    manager.process(obs)
            except Exception as exc:
                print(f"Leitura automática interrompida: {exc}", flush=True)

        self.thread = Thread(target=consume, daemon=True)
        self.thread.start()
        print(f"Processamento automático: {self.path}", flush=True)

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join()
        if self.lock:
            self.lock.close()
