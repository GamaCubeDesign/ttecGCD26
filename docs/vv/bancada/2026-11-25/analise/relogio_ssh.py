"""How far behind this PC the ground's clock starts on the Pi, over ssh
(./gama ground --ssh): gs_web's own SshSource, N times; per run, the arrival
here of gs_cli's first line minus that line's wall_ms. That is the lag
gs_cli hands the satellite in SET_TIME, plus one Wi-Fi hop back.

    python3 relogio_ssh.py pi@gamapi.local [N] [--stray]

--stray leaves a gs_cli on the Pi first, as a dropped ssh does, so the
page's pkill and its 0.5 s sleep run. Run it with the page closed: the
remote side stops any gs_cli on the Pi, the page's too. gs_cli starts with
--auto-time on, as from the page, so the satellite gets SET_TIME.

25/11: before the fix (the clock read when ssh started) 0.56 s to 1.87 s,
1.07 s to 1.25 s with --stray; notas.md has the runs after it."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "..", "ground", "host"))
import gs_web  # noqa: E402


class Hub:
    def __init__(self):
        self.lines = []

    def feed(self, obj):
        self.lines.append((time.time(), obj))


def main(argv):
    host = argv[1]
    n = int(argv[2]) if len(argv) > 2 and argv[2].isdigit() else 5
    stray = "--stray" in argv
    for run in range(n):
        if stray:
            subprocess.run(["ssh", "-o", "BatchMode=yes", host,
                            "cd ttec && (nohup sh -c 'sleep 60 | build/tools/gs_cli/gs_cli --radio udp"
                            " > /dev/null' > /dev/null 2>&1 &); sleep 0.5"],
                           check=True, capture_output=True, timeout=20)
        hub = Hub()
        source = gs_web.SshSource(host, "ttec", retry_s=60)
        source.start(hub)
        deadline = time.time() + 30
        while not any("wall_ms" in o for _, o in hub.lines) and time.time() < deadline:
            time.sleep(0.01)
        first = next(((t, o) for t, o in hub.lines if "wall_ms" in o), None)
        source.stop()
        if first is None:
            print("run %d: no line from gs_cli in 30 s" % (run + 1))
            continue
        t, o = first
        print("run %d%s: gs_cli %d ms behind this PC" % (run + 1, " (stray)" if stray else "",
                                                       round(t * 1000 - o["wall_ms"])))
        time.sleep(1)


if __name__ == "__main__":
    main(sys.argv)
