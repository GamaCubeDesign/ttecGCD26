#!/usr/bin/env python3
"""
LoRa time-on-air, data budget and link budget for the CubeDesign 2026 mission.

Every number quoted in docs/adr/0003, docs/adr/0004 and docs/budgets/ comes
from this script. It takes no arguments and needs no dependencies beyond the
standard library, so a reviewer can reproduce the figures that justify the
architecture:

    python3 tools/analysis/lora_budget.py

Time-on-air follows the SX1276/77/78/79 datasheet (Semtech, rev. 7), section
4.1.1.7. Receiver sensitivity figures are the datasheet's typical values for
the 433 MHz band at 125 kHz bandwidth.
"""

import argparse
import math

# --- mission constants, from the CubeDesign 2026 rules -------------------

MISSION_S = 600          # HLR-ADS-05: 10 continuous minutes
MAX_AIRCRAFT = 20        # HLR-ADS-05: up to 20 aircraft per test

# --- radio constants, from the hardware we have -------------------------

FREQ_MHZ = 433.0         # Ai-Thinker RA-02 / SX1278, band is fixed
TX_DBM = 20.0            # PA_BOOST maximum
CABLE_LOSS_DB = 2.0      # connectors and feedline, both ends combined
ANT_GAIN_DBI = 2.0       # quarter-wave whip, each end

# Datasheet typical sensitivity at BW = 125 kHz, per spreading factor.
SENSITIVITY_DBM = {7: -123.0, 8: -126.0, 9: -129.0,
                   10: -132.0, 11: -134.5, 12: -137.0}

# --- protocol constants, from common/ -----------------------------------

FRAME_OVERHEAD = 7       # GAMA_FRAME_OVERHEAD
TRACK_WIRE_LEN = 20      # GAMA_TRACK_WIRE_LEN
TRACKS_PER_FRAME = 12    # GAMA_TRACKS_PER_FRAME


def ndjson_line_bytes():
    """Average size of one line of the onboard ADS-B log, in bytes.

    Reproduces the printf format of adsb_capture.c:message_to_ndjson() for a
    representative aircraft, weighted by the ADS-B broadcast mix: airborne
    position and airborne velocity at 2 Hz each, identification every 5 s.
    Computed rather than typed, because the typed value (110 B) understated
    the real line by 31% and every volume figure in the budget inherited the
    error.
    """
    rx_epoch_ns = 1789900000123456789          # 19 digits: a 2026 epoch

    def line(tt, callsign="", fields=""):
        return (f'{{"icao":"E48DF5","rx_epoch_ns":{rx_epoch_ns},'
                f'"transmission_type":{tt},"callsign":"{callsign}"'
                f'{fields},"on_ground":0}}\n')

    position = line(3, fields=',"altitude_ft":37000'
                              ',"lat":-23.559616,"lon":-46.658908')
    velocity = line(4, fields=',"ground_speed_kt":451.7,"track_deg":128.4'
                              ',"vertical_rate_fpm":-1216')
    ident = line(1, callsign="TAM3054")

    mix = ((position, 2.0), (velocity, 2.0), (ident, 0.2))
    return (sum(len(text) * hz for text, hz in mix)
            / sum(hz for _, hz in mix))


NDJSON_LINE = ndjson_line_bytes()

# Half the mission is spent transmitting. The other half is not idle time we
# could reclaim: LoRa is half-duplex, so every second spent transmitting is a
# second in which a telecommand cannot be heard (HLR-COMM-01).
TX_DUTY = 0.50

# The snapshot period we actually fly. The air-time budget would permit 4.2 s,
# but that consumes 299 s of the 300 s allocation and leaves nothing for
# housekeeping, statistics or acknowledgements. See docs/budgets/data-budget.md.
STREAM_PERIOD_S = 5.0


def time_on_air(payload_bytes, sf, bw_hz, cr, preamble=8, crc=True,
                implicit_header=False):
    """Time on air in seconds for one LoRa packet.

    cr is the denominator offset: 1 for 4/5 through 4/8 for 4/8.
    """
    symbol_s = (2 ** sf) / bw_hz

    # Low data rate optimisation is mandatory when a symbol exceeds 16 ms; it
    # costs two bits of spreading factor in the payload calculation.
    low_rate_opt = 1 if symbol_s > 0.016 else 0

    preamble_s = (preamble + 4.25) * symbol_s

    numerator = (8 * payload_bytes - 4 * sf + 28
                 + (16 if crc else 0) - (20 if implicit_header else 0))
    denominator = 4 * (sf - 2 * low_rate_opt)
    payload_symbols = 8 + max(math.ceil(numerator / denominator) * (cr + 4), 0)

    return preamble_s + payload_symbols * symbol_s


def goodput_bps(payload_bytes, sf, bw_hz, cr):
    """Useful bits per second, counting only payload against total air time."""
    return payload_bytes * 8 / time_on_air(payload_bytes, sf, bw_hz, cr)


def fspl_db(distance_m, freq_mhz=FREQ_MHZ):
    """Free-space path loss. Undefined at zero distance."""
    return (20 * math.log10(distance_m / 1000.0)
            + 20 * math.log10(freq_mhz) + 32.44)


def link_margin_db(distance_m, sf):
    """Margin above the receiver's sensitivity, in dB."""
    received = (TX_DBM - CABLE_LOSS_DB + 2 * ANT_GAIN_DBI
                - fspl_db(distance_m))
    return received - SENSITIVITY_DBM[sf]


def snapshot_frames(aircraft=MAX_AIRCRAFT):
    """Frames needed to carry one snapshot of every tracked aircraft."""
    return math.ceil(aircraft / TRACKS_PER_FRAME)


def snapshot_bytes(aircraft=MAX_AIRCRAFT):
    """Bytes on the wire for one full snapshot, framing included.

    The last frame is partial: 20 aircraft is 12 records plus 8, not 12 plus
    12. Rounding that up overstates the mission total by 17%.
    """
    full, remainder = divmod(aircraft, TRACKS_PER_FRAME)
    total = full * (FRAME_OVERHEAD + TRACKS_PER_FRAME * TRACK_WIRE_LEN)
    if remainder:
        total += FRAME_OVERHEAD + remainder * TRACK_WIRE_LEN
    return total


def snapshot_air_time(sf, bw_hz, cr, aircraft=MAX_AIRCRAFT):
    """Total air time for one full snapshot, across however many frames."""
    full, remainder = divmod(aircraft, TRACKS_PER_FRAME)
    total = full * time_on_air(
        FRAME_OVERHEAD + TRACKS_PER_FRAME * TRACK_WIRE_LEN, sf, bw_hz, cr)
    if remainder:
        total += time_on_air(
            FRAME_OVERHEAD + remainder * TRACK_WIRE_LEN, sf, bw_hz, cr)
    return total


def print_rate_comparison():
    print("=" * 78)
    print("TIME ON AIR AND DOWNLINK CAPACITY")
    print("=" * 78)
    print(f"{'configuration':<26}{'ToA 208B':>10}{'goodput':>10}"
          f"{'600s @50%':>12}{'vs legacy':>11}")
    print("-" * 78)

    configs = [
        ("LEGACY SF12/BW62.5/CR4:8", 12, 62_500, 4),
        ("SAFE    SF12/BW125/CR4:8", 12, 125_000, 4),
        ("        SF11/BW125/CR4:5", 11, 125_000, 1),
        ("        SF10/BW125/CR4:5", 10, 125_000, 1),
        ("NOMINAL SF9 /BW125/CR4:5", 9, 125_000, 1),
        ("        SF8 /BW125/CR4:5", 8, 125_000, 1),
        ("FAST    SF7 /BW125/CR4:5", 7, 125_000, 1),
    ]

    baseline = None
    for name, sf, bw, cr in configs:
        toa = time_on_air(208, sf, bw, cr)
        gp = goodput_bps(208, sf, bw, cr)
        capacity = gp * MISSION_S * TX_DUTY / 8
        if baseline is None:
            baseline = capacity
        print(f"{name:<26}{toa:>9.3f}s{gp:>9.0f}b{capacity/1024:>11.1f}K"
              f"{capacity/baseline:>10.0f}x")


def print_data_volume():
    print()
    print("=" * 78)
    print("ADS-B DATA VOLUME vs DOWNLINK CAPACITY")
    print("=" * 78)
    print(f"20 aircraft, {MISSION_S} s. ADS-B airborne position and velocity")
    print("are each nominally 2 Hz, identification every 5 s.")
    print()

    capacity = goodput_bps(208, 9, 125_000, 1) * MISSION_S * TX_DUTY / 8

    for rate in (2, 4, 6):
        msgs = MAX_AIRCRAFT * rate * MISSION_S
        raw = msgs * NDJSON_LINE  # one line of the onboard log
        print(f"  {rate} msg/s/aircraft: {msgs:>6} messages"
              f" = {raw/1024/1024:>5.2f} MiB raw"
              f"  ->  {raw/capacity:>6.0f}x the NOMINAL downlink budget")

    print()
    print(f"  Aggregated to {TRACK_WIRE_LEN}-byte track state:")
    snap = snapshot_air_time(9, 125_000, 1)
    print(f"    one snapshot of 20 aircraft = "
          f"{snapshot_frames()} frames, {snapshot_bytes()} B, "
          f"{snap:.2f} s of air time")

    fastest = snap / TX_DUTY
    print(f"    fastest cadence the air time allows: {fastest:.1f} s"
          f"  (leaves {MISSION_S*TX_DUTY - (MISSION_S/fastest)*snap:.0f} s spare"
          f" -- not enough for housekeeping)")

    count = MISSION_S / STREAM_PERIOD_S
    used = count * snap
    downlinked = count * snapshot_bytes()
    print(f"    flown cadence: {STREAM_PERIOD_S:.0f} s"
          f"  ->  {count:.0f} snapshots, {count:.0f} trajectory points"
          f" per aircraft")
    print(f"    track downlink: {downlinked/1024:.1f} KB in {used:.0f} s,"
          f" leaving {MISSION_S*TX_DUTY - used:.0f} s of the"
          f" {MISSION_S*TX_DUTY:.0f} s allocation for HK, stats and acks")
    print()
    print(f"  Reduction achieved onboard: "
          f"{MAX_AIRCRAFT * 4 * MISSION_S * NDJSON_LINE / downlinked:.0f}:1"
          f"   ({MAX_AIRCRAFT * 4 * MISSION_S * NDJSON_LINE / 1024 / 1024:.2f} MiB"
          f" -> {downlinked/1024:.1f} KB)")


def print_link_budget():
    print()
    print("=" * 78)
    print(f"LINK BUDGET  ({TX_DBM:.0f} dBm, {FREQ_MHZ:.0f} MHz, "
          f"{ANT_GAIN_DBI:.0f} dBi each end, {CABLE_LOSS_DB:.0f} dB losses)")
    print("=" * 78)
    print(f"{'distance':>10}{'FSPL':>9}{'Rx power':>11}"
          f"{'margin SF7':>12}{'margin SF9':>12}{'margin SF12':>13}")
    print("-" * 78)
    for d in (10, 50, 100, 500, 1000, 5000):
        print(f"{d:>8} m{fspl_db(d):>8.1f}d"
              f"{TX_DBM - CABLE_LOSS_DB + 2*ANT_GAIN_DBI - fspl_db(d):>10.1f}d"
              f"{link_margin_db(d, 7):>11.1f}d"
              f"{link_margin_db(d, 9):>11.1f}d"
              f"{link_margin_db(d, 12):>12.1f}d")
    print()
    print("The competition ground station operates within tens of metres of")
    print("the CubeSat. Even at 1 km, SF9 retains more than 60 dB of margin.")


def check():
    """Assert the figures quoted in the ADRs. Exits non-zero on drift."""
    failures = []

    def expect(label, actual, want, tol):
        if abs(actual - want) > tol:
            failures.append(f"{label}: got {actual:.4f}, expected {want} +/- {tol}")

    legacy = goodput_bps(208, 12, 62_500, 4) * MISSION_S * TX_DUTY / 8
    expect("ADR-0004 legacy capacity (bytes)", legacy, 2674, 40)

    nominal = goodput_bps(208, 9, 125_000, 1) * MISSION_S * TX_DUTY / 8
    expect("ADR-0004 NOMINAL capacity (bytes)", nominal, 59700, 700)

    expect("ADR-0004 legacy ToA of a 208 B frame (s)",
           time_on_air(208, 12, 62_500, 4), 23.347, 0.01)
    expect("ADR-0004 NOMINAL ToA of a 208 B frame (s)",
           time_on_air(208, 9, 125_000, 1), 1.046, 0.01)

    expect("ADR-0004 margin at 100 m, SF9 (dB)", link_margin_db(100, 9), 85.8, 0.5)
    expect("ADR-0004 margin at 10 m, SF9 (dB)", link_margin_db(10, 9), 105.8, 0.5)

    expect("ADR-0004 fastest cadence the air time allows (s)",
           snapshot_air_time(9, 125_000, 1) / TX_DUTY, 4.2, 0.3)

    downlinked = (MISSION_S / STREAM_PERIOD_S) * snapshot_bytes()
    expect("data-budget track downlink (KB)", downlinked / 1024, 48.5, 0.5)
    expect("data-budget onboard reduction ratio",
           MAX_AIRCRAFT * 4 * MISSION_S * NDJSON_LINE / downlinked, 155, 2)

    expect("data-budget NDJSON line size (bytes)", NDJSON_LINE, 160.4, 0.5)

    expect("ADR-0003 erratum: raw volume at 4 msg/s (MiB)",
           MAX_AIRCRAFT * 4 * MISSION_S * NDJSON_LINE / 1024 / 1024, 7.34, 0.02)

    expect("ADR-0003 erratum: raw volume over NOMINAL capacity",
           MAX_AIRCRAFT * 4 * MISSION_S * NDJSON_LINE / nominal, 129, 1)

    if failures:
        print("BUDGET CHECK FAILED — the documentation no longer matches:")
        for f in failures:
            print("  " + f)
        return 1

    print("budget check: all figures quoted in the ADRs reproduce")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify the figures quoted in the ADRs and exit")
    args = parser.parse_args()

    if args.check:
        raise SystemExit(check())

    print_rate_comparison()
    print_data_volume()
    print_link_budget()


if __name__ == "__main__":
    main()
