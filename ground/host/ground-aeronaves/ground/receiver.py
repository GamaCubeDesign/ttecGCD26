import time
from queue import Full


def receive(stream, queue, interval, stop):
    """Arquivo ou stdin; EOF encerra. O receptor físico pode escrever no stdin."""

    def put(item):
        while not stop.is_set():
            try:
                queue.put(item, timeout=0.1)
                return
            except Full:
                pass

    try:
        while not stop.is_set():
            raw = stream.readline()
            if not raw:
                break
            if not raw.strip():
                continue
            put((raw, time.time_ns()))
            if interval:
                stop.wait(interval)
    except Exception as exc:
        put(exc)
    finally:
        put(None)
