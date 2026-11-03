#!/usr/bin/env python3
"""Does the flight configuration match the boards?

Reads the KiCad layouts in hardware/ — the copper the fabricator builds, so
the boards' real netlist, with or without a schematic — and one board set's
flight configuration (flight/conf/obc-v2/), and follows every wire the
firmware counts on, from the Pi's GPIO to the pin of the chip on the board at
the other end of the 40-pin bus:

  ttcd.conf   spi_dev, reset_line, dio0_line   -> the RA-02 on the TT&C board
  obcd.conf   i2c_dev                          -> the INA219, and the AOCS
                                                  ESP32 and BMI088 on the same bus
              burnwire_line, heater_line       -> the mechanism's connector, the
                                                  TIP120 bases on both batteries
  config.txt  w1-gpio gpiopin                  -> the DS18B20's DQ, with a pull-up
  the INA219's input and output                -> the EPS's 8 V Bruto and 8 V Liq

  board_check.py [--conf DIR]     check the stack; exit status 1 on a mismatch
  board_check.py bus              the 40-pin bus of every board, side by side
  board_check.py diff OLD NEW     what changed in a board's connections

Run from the repository root.
"""
import argparse
import pathlib
import re
import sys
from collections import defaultdict

# The boards that stack, and the reference of each one's bus connector.
STACK = {
    "OBC": ("hardware/obc-v2", "J1"),
    "TT&C": ("hardware/ttec", "J3"),
    "EPS": ("hardware/eps-potencia-v2", "J1"),
    "bat. cima": ("hardware/bateria-cima", "J1"),
    "bat. baixo": ("hardware/bateria-baixo", "J1"),
    "AOCS": ("hardware/aocs-v2", "Barramento1"),
}
CONF = "flight/conf/obc-v2"

# The Pi's fixed functions: SPI0 and the two I2C buses.
SPI0 = {"SCK": 11, "MOSI": 10, "MISO": 9}
SPI_CS = {"/dev/spidev0.0": 8, "/dev/spidev0.1": 7}
I2C = {"/dev/i2c-1": (2, 3), "/dev/i2c-0": (0, 1)}


# ---- the layout -----------------------------------------------------------

def parse(text):
    stack = [[]]
    for t in re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text):
        if t == "(":
            stack.append([])
        elif t == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]


def kids(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def kid(node, key):
    k = kids(node, key)
    return k[0] if k else None


class Board:
    """Footprints, their pads' nets, and who is on each net."""

    def __init__(self, path):
        self.path = path
        root = parse(path.read_text(encoding="utf-8"))
        names = {n[1]: n[2] for n in kids(root, "net") if len(n) >= 3}
        self.fp = {}                       # ref -> {"value", "pads": {pad: (function, net)}}
        self.members = defaultdict(list)   # net -> [(ref, pad, function)]
        for f in kids(root, "footprint"):
            ref = value = None
            for p in kids(f, "property"):
                if len(p) > 2 and p[1] == "Reference":
                    ref = p[2]
                elif len(p) > 2 and p[1] == "Value":
                    value = p[2]
            if not ref or ref.startswith(("G***", "REF**")) or ref == "logo":
                continue
            pads = {}
            for pad in kids(f, "pad"):
                n = kid(pad, "net")
                net = None
                if n is not None:
                    net = n[2] if len(n) >= 3 else names.get(n[1], n[1])
                if not net or net.startswith("unconnected"):
                    continue
                fn = kid(pad, "pinfunction")
                pads[pad[1]] = (fn[1] if fn else "", net)
                self.members[net].append((ref, pad[1], fn[1] if fn else ""))
            self.fp[ref] = {"value": value or "", "pads": pads}

    def find(self, pattern):
        """The footprint whose value matches."""
        hits = [r for r, f in self.fp.items() if re.search(pattern, f["value"], re.I)]
        if len(hits) != 1:
            raise LookupError(f"{self.path}: {len(hits)} footprints match /{pattern}/")
        return hits[0]


def load(folder):
    pcbs = sorted(pathlib.Path(folder).glob("*.kicad_pcb"))
    if len(pcbs) != 1:
        raise LookupError(f"{folder}: expected one .kicad_pcb, found {len(pcbs)}")
    return Board(pcbs[0])


def bus_of(board, ref):
    return {int(p): net for p, (_, net) in board.fp[ref]["pads"].items() if p.isdigit()}


# ---- the configuration ----------------------------------------------------

def read_conf(path):
    out = {}
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def w1_pin(path):
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*dtoverlay=w1-gpio,gpiopin=(\d+)\s*$", line)
        if m:
            return int(m.group(1))
    return None


# ---- the checks -----------------------------------------------------------

class Checker:
    def __init__(self, conf):
        self.conf = conf
        self.boards = {name: load(folder) for name, (folder, _) in STACK.items()}
        self.bus = {name: bus_of(self.boards[name], ref) for name, (_, ref) in STACK.items()}
        obc = self.boards["OBC"]
        pi = obc.find(r"RASPBERRY")
        self.gpio = {}                               # BCM GPIO -> net on the OBC board
        for fn, net in obc.fp[pi]["pads"].values():
            m = re.match(r"GPIO(\d+)/", fn)
            if m:
                self.gpio[int(m.group(1))] = net
        self.failed = 0

    def report(self, ok, what, chain):
        print(f"{'ok  ' if ok else 'FAIL'}  {what:<22s} {chain}")
        self.failed += 0 if ok else 1

    def to_bus(self, gpio):
        """GPIO -> the OBC bus pin it reaches, directly or through one
        series resistor. Returns (pin, resistor, value) or None."""
        obc = self.boards["OBC"]
        net = self.gpio.get(gpio)
        if net is None:
            return None
        pins = [p for p, n in self.bus["OBC"].items() if n == net]
        if pins:
            return pins[0], None, None
        for ref, pad, _ in obc.members[net]:
            f = obc.fp[ref]
            if ref.startswith("R") and len(f["pads"]) == 2:
                other = [n for p, (_, n) in f["pads"].items() if p != pad][0]
                pins = [p for p, n in self.bus["OBC"].items() if n == other]
                if pins:
                    return pins[0], ref, f["value"]
        return None

    def reaches(self, board, pin, ref, function=None, pad=None):
        """Does the net on `board`'s bus pin reach footprint `ref`, at a pad
        whose function starts with `function` (or numbered `pad`)?"""
        net = self.bus[board].get(pin)
        if net is None:
            return False, None
        for r, p, fn in self.boards[board].members[net]:
            if r == ref and (function is None or fn.startswith(function)) and (pad is None or p == pad):
                return True, net
        return False, net

    def follow(self, what, origin, gpio, board, ref, function=None, pad=None, label=None,
               series_ok=False):
        """GPIO -> bus pin -> the pad on the other board. A series resistor
        is allowed only where it belongs (series_ok: a transistor's base);
        on a signal line it is a fault, as the DS18B20's was (finding A7)."""
        hop = self.to_bus(gpio)
        if hop is None:
            self.report(False, what, f"{origin} -> GPIO{gpio}: does not reach the OBC's bus connector")
            return None
        pin, res, val = hop
        ok, net = self.reaches(board, pin, ref, function, pad)
        ok = ok and (series_ok or res is None)
        via = f" -> {res} {val}{'' if series_ok or res is None else ' IN SERIES'}" if res else ""
        end = label or f"{self.boards[board].fp[ref]['value']} {function or pad}"
        self.report(ok, what, f"{origin} -> GPIO{gpio}{via} -> OBC J1.{pin} -> {board} pin {pin} "
                              f"({net or 'nothing'}) -> {end}")
        return pin, res, val

    def run(self):
        ttcd = read_conf(f"{self.conf}/ttcd.conf")
        obcd = read_conf(f"{self.conf}/obcd.conf")
        tt = self.boards["TT&C"]
        radio = tt.find(r"Ra-?02")

        # the radio (ADR-0012)
        cs = SPI_CS.get(ttcd.get("spi_dev", ""))
        if cs is None:
            self.report(False, "LoRa", f"ttcd spi_dev {ttcd.get('spi_dev')!r} is not SPI0")
        else:
            for sig, gpio in (("NSS", cs), ("SCK", SPI0["SCK"]), ("MOSI", SPI0["MOSI"]), ("MISO", SPI0["MISO"])):
                self.follow(f"LoRa {sig}", f"ttcd spi_dev {ttcd['spi_dev']}", gpio, "TT&C", radio, sig)
        self.follow("LoRa RESET", "ttcd reset_line", int(ttcd["reset_line"]), "TT&C", radio, "RST")
        self.follow("LoRa DIO0", "ttcd dio0_line", int(ttcd["dio0_line"]), "TT&C", radio, "DIO0")

        # the I2C bus: the INA219 on the OBC, the ESP32 and BMI088 on the AOCS (ADR-0017)
        obc = self.boards["OBC"]
        ina = obc.find(r"INA219")
        sda, scl = I2C.get(obcd.get("i2c_dev", ""), (None, None))
        if sda is None:
            self.report(False, "I2C", f"obcd i2c_dev {obcd.get('i2c_dev')!r} is not a Pi I2C bus")
        else:
            aocs = self.boards["AOCS"]
            esp, bmi = aocs.find(r"^ESP32"), aocs.find(r"BMI0?88|BMI008")
            for sig, gpio in (("SDA", sda), ("SCL", scl)):
                net = self.gpio.get(gpio)
                on_ina = any(fn.startswith(sig) for _, (fn, n) in obc.fp[ina]["pads"].items() if n == net)
                self.report(on_ina, f"I2C {sig}, INA219", f"obcd i2c_dev {obcd['i2c_dev']} -> GPIO{gpio} -> INA219 {sig}")
                hop = self.to_bus(gpio)
                if hop is None:
                    self.report(False, f"I2C {sig}, AOCS", f"GPIO{gpio} does not reach the bus")
                    continue
                pin = hop[0]
                anet = self.bus["AOCS"].get(pin, "")
                refs = {r for r, _, _ in aocs.members.get(anet, [])}
                ok = sig in anet.upper() and esp in refs and bmi in refs
                esp_pad = [p for r, p, _ in aocs.members.get(anet, []) if r == esp]
                self.report(ok, f"I2C {sig}, AOCS", f"GPIO{gpio} -> OBC J1.{pin} -> AOCS pin {pin} ({anet or 'nothing'})"
                                                    f" -> ESP32 pad {','.join(esp_pad) or '-'} and BMI088")

        # power: the INA219 in series in the 8 V path (finding A1)
        eps = self.bus["EPS"]
        for fn_name, want in (("VIN+", "BRUTO"), ("VIN-", "LIQ")):
            nets = {n for _, (fn, n) in obc.fp[ina]["pads"].items() if fn.startswith(fn_name)}
            pins = [p for p, n in self.bus["OBC"].items() if n in nets]
            ok = len(pins) == 1 and want in eps.get(pins[0], "").upper()
            where = f"OBC J1.{pins[0]} -> EPS pin {pins[0]} ({eps.get(pins[0], 'nothing')})" if pins else "not on the bus"
            self.report(ok, f"INA219 {fn_name}", f"{where}")

        # the battery temperature (finding A7)
        pin = w1_pin(f"{self.conf}/config.txt")
        if pin is None:
            self.report(False, "DS18B20", "config.txt has no dtoverlay=w1-gpio,gpiopin=N")
        else:
            top = self.boards["bat. cima"]
            self.follow("DS18B20", f"config.txt gpiopin={pin}", pin, "bat. cima", top.find(r"DS18B20"), "DQ")
            net = self.gpio.get(pin)
            pullups = [f"{r} {obc.fp[r]['value']}" for r, p, _ in obc.members.get(net, [])
                       if r.startswith("R") and any("3V3" in n.upper() for q, (_, n) in obc.fp[r]["pads"].items() if q != p)]
            self.report(bool(pullups), "DS18B20 pull-up", f"GPIO{pin} -> {', '.join(pullups) or 'no resistor'} -> 3V3")

        # the outputs (findings A5, A6): the base resistor values are reported, not judged
        mech = tt.find(r"Conn_01x03")
        hop = self.follow("burn-wire", f"obcd burnwire_line {obcd['burnwire_line']}", int(obcd["burnwire_line"]),
                          "TT&C", mech, pad="3", label=f"TT&C {mech} pin 3, the mechanism", series_ok=True)
        for name in ("bat. cima", "bat. baixo"):
            b = self.boards[name]
            hop = self.follow(f"heaters, {name}", f"obcd heater_line {obcd['heater_line']}", int(obcd["heater_line"]),
                              name, b.find(r"TIP120"), "B", series_ok=True)
        return self.failed


def bus_table():
    boards = {name: bus_of(load(folder), ref) for name, (folder, ref) in STACK.items()}
    pins = sorted({p for b in boards.values() for p in b})
    w = 22
    print("pin  " + " | ".join(f"{n:{w}s}" for n in boards))
    for p in pins:
        cells = [re.sub(r"^/", "", b.get(p, "")).replace("Net-(", "(")[:w] for b in boards.values()]
        print(f"{p:3d}  " + " | ".join(f"{c:{w}s}" for c in cells))


def diff(old, new):
    def peers(b):
        out = {}
        for net, m in b.members.items():
            for r, p, _ in m:
                out[f"{r}.{p}"] = (net, frozenset(f"{a}.{c}" for a, c, _ in m) - {f"{r}.{p}"})
        return out
    a, b = peers(Board(pathlib.Path(old))), peers(Board(pathlib.Path(new)))
    changes = []
    for pad in sorted(set(a) | set(b)):
        if pad not in b:
            changes.append(f"- {pad} ({a[pad][0]})")
        elif pad not in a:
            changes.append(f"+ {pad} ({b[pad][0]})")
        elif a[pad][1] != b[pad][1]:
            changes.append(f"~ {pad}: {a[pad][0]} -> {b[pad][0]}")
    print(f"{len(changes)} connection changes")
    for c in changes:
        print(c)
    return 1 if changes else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", nargs="?", default="check", choices=["check", "bus", "diff"])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--conf", default=CONF, help=f"the flight configuration to check (default {CONF})")
    args = ap.parse_args()
    if args.what == "bus":
        bus_table()
        return 0
    if args.what == "diff":
        if len(args.files) != 2:
            ap.error("diff takes OLD.kicad_pcb NEW.kicad_pcb")
        return diff(*args.files)
    failed = Checker(args.conf).run()
    print(f"{'the flight configuration matches the boards' if not failed else f'{failed} mismatches'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
