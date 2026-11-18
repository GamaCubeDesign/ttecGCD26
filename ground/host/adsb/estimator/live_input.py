"""Acompanha o NDJSON enquanto o servidor web está aberto."""
import fcntl
import os
import time
from threading import Event, Thread
from queue import Queue, Empty, Full

from .database import Database
from .airport_estimator import AirportEstimator
from .aircraft_manager import AircraftManager
from .parser import parse_line


def follow_lines(path, stop):
    stream=None
    try:
        while not stop.is_set():
            try:
                stat=path.stat()
                if stream and (os.fstat(stream.fileno()).st_ino!=stat.st_ino or stat.st_size<stream.tell()):
                    stream.close();stream=None
                if stream is None:
                    stream=path.open('rb')
                offset=stream.tell()
                line=stream.readline()
                if line.endswith(b'\n'):
                    yield line.decode('utf-8')
                    continue
                stream.seek(offset)  # Wait for the producer to finish the line.
            except FileNotFoundError:
                pass
            stop.wait(.2)
    finally:
        if stream:
            stream.close()


class LiveInput:
    def __init__(self,path,config):
        self.path,self.config=path,config
        self.stop=Event()
        self.lock=None
        self.thread=None
        self.session=None

    def start(self):
        self.config.telemetry_db.parent.mkdir(parents=True,exist_ok=True)
        self.lock=self.config.telemetry_db.with_suffix('.viewer.lock').open('a')
        try:
            fcntl.flock(self.lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close();self.lock=None
            print('Outro painel já acompanha este banco; usando seu processamento.',flush=True)
            return
        database=Database(self.config.telemetry_db)
        manager=AircraftManager(database,AirportEstimator(self.config),self.config)
        self.session=database.session_id
        queue=Queue(maxsize=self.config.queue_size)
        def receive_live():
            try:
                for raw in follow_lines(self.path,self.stop):
                    while not self.stop.is_set():
                        try:
                            queue.put((raw,time.time_ns()),timeout=.1)
                            break
                        except Full: pass
            except Exception as exc:
                while not self.stop.is_set():
                    try:
                        queue.put(exc,timeout=.1);break
                    except Full: pass
        reader=Thread(target=receive_live,daemon=True)
        reader.start()
        def consume():
            try:
                while not self.stop.is_set() or not queue.empty():
                    try: item=queue.get(timeout=.1)
                    except Empty:
                        manager.poll_pairs();continue
                    if isinstance(item,Exception): raise item
                    raw,received=item
                    try:
                        obs=parse_line(raw,received)
                    except (ValueError,TypeError,OverflowError,RecursionError) as exc:
                        database.reject(raw,received,str(exc));continue
                    if obs.event_ns<received-int(self.config.window_s*1e9):
                        continue
                    manager.process(obs)
            except Exception as exc:
                print(f'Leitura automática interrompida: {exc}',flush=True)
            finally:
                self.stop.set()
                reader.join(timeout=1)
                manager.close()
        self.thread=Thread(target=consume,daemon=True)
        self.thread.start()
        print(f'Processamento automático: {self.path}',flush=True)

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join()
        if self.lock:
            self.lock.close()
