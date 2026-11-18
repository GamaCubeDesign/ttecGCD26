# ADR-0022: The ground infers a large aircraft from its kinematics, and the take-off distance from its climb

- **Status:** Proposed
- **Date:** 2026-11-18
- **Requirements:** competition rules 4.4.1 (estimate origin and destination, a
  primary objective), HLR-ADS-04 (processing at the ground)
- **Deciders:** TT&C team; the estimator's author to review

## Context

The ground's origin and destination estimator (`ground/host/adsb/estimator`,
ADR-0019) filters airports by the ADS-B emitter category: a jet (A3) cannot
come from a heliport and rarely from a small strip. That category never
reaches the ground. The satellite's `adsbd` reads `dump1090-fa`'s SBS feed,
which has no category field, and the downlinked track record carries none
(ADR-0021 already said so). `build_snapshot` deliberately never infers it
("never infer it from ICAO/speed").

Measured against a truth (`tools/live_traffic`, `docs/vv/trafego-real/`):

| Set | Origin decisions, without the category | with it |
|---|---|---|
| 1 h of real traffic around UNSAM, 17/11 (27 origins known) | **0** | 5 of 6 right |
| 16/11 simulation around SBBR, 6 arrivals | the arrivals pointed at small airfields next to SBBR | SBBR, 6 of 6 |

Without the category the candidates around Aeroparque — the large airport, three
heliports within 3 km, small strips — are too close to each other, and the
decision fusion abstains.

Origin has a second gap. It is estimated only while climbing, during the first
180 s an aircraft is seen (`aircraft_manager.py`). The take-off anchor exists
only when a position **on the ground** was received; an aircraft first seen
already airborne has only the reverse of its track and its distance growing.

The competition's ADS-B is simulated by the organisers with an SDR (rules
4.4.4). Its aircraft may be replays, straight flights between real airports,
or invented. Whatever we infer must not rest on real-world databases (ICAO
address to type, callsign to route), which an invented scenario would defeat.

## Decision

1. **A large aircraft from its own kinematics.** When no category is received
   (missing, invalid or A0), the ground marks an aircraft "A3" once its
   observations show, at least twice:
   - a ground speed above 250 kt, **or**
   - an altitude above 18 000 ft, **or**
   - a climb above 1 800 ft/min at more than 160 kt.

   The class holds for the tracking episode, so a jet slowing down on approach
   stays large. The source is recorded as `kinematics`; a received category
   always wins. Only "large" is inferred. A helicopter at 120 kt and 2 000 ft
   moves like a light aircraft, and calling it fixed wing would rule out the
   heliports it flies to, so "light" and "rotorcraft" stay unknown.
   (`aircraft_category.infer_class`, `AircraftManager._kinematic_category`;
   `Config.kinematic_category`, `--no-kinematic-category`.)
2. **The take-off distance from the climb.** For the origin, with no take-off
   anchor, each candidate airport gets a component `climb_back`. An aircraft
   h feet above the airport, climbing at v ft/min at s kt, left it about h/v
   minutes ago, at a distance d = s·h/v. The score is exp(−((distance − d)/σ)²),
   with σ = max(3 km, 0.3·d). Its weight is 0.30, and it applies only with a
   climb of at least 300 ft/min and a known class, received or inferred. (`airport_estimator.climb_back`;
   `Config.climb_back_weight`, `--climb-back-weight`.)
3. **The route-pair method is off by default** in the ground's page and in
   `make sim` (`gs_web.py --pairs` turns it on). The estimator's own CLI is
   unchanged.

Nothing crosses the radio; the satellite is unchanged.

## Rationale

Measured on four sets, the estimator's local method, before and after, without
the category. "Decided / right" counts cases with a known truth; scored by the
estimator's `adsb/validation/avaliar_estimativas.py`, and the combined conclusion
by `tools/live_traffic/avaliar_combinada.py`, with the same rule.

| Set | Origin before | Origin after | Destination before → after |
|---|---|---|---|
| Real traffic, UNSAM 17/11 (27 / 10 known; thresholds set here) | 0 | **7 / 7** | 9 of 10 → 9 of 10 |
| Real traffic, UNSAM 18/11 (26 / 12 known; **validation**, not used to set anything) | 4 / 4 | **6 / 6** | 10 of 12 → 10 of 12 |
| Synthetic straight flights, UNSAM (23 / 23) | 3 / 3 | **10 / 10** | 8 / 8 → 8 / 8 |
| Synthetic invented aircraft (no airport is right) | 0 false positives | 0 false positives | 4 false positives → 4 |

Without the category, the change reaches what the estimator did **with** it
before (17/11: 5 of 6; 18/11: 6 of 6). With the category, the climb-back
distance lifts the origins further (18/11: 6 → 10, 17/11: 5 of 6 → 7 of 7). The
combined conclusion (ADR-0019) goes from 0 to 4 origins and 3 destinations on
17/11, from 4 to 6 origins and 1 to 3 destinations on 18/11, and from 3 to 10
origins on the straight flights; no wrong decision anywhere.

The two parts must go together: the climb-back distance alone, without
knowing the aircraft is a jet, lifted heliports that sat at the right distance
(17/11: 2 of 4 origins wrong). It therefore applies only with a known class,
received or inferred.

The classifier against the true category of the 47 aircraft of 17/11: of 31 A3,
29 inferred large and 2 left unknown; of 7 A1, **none inferred large**. One A2, a
fast business jet, was inferred large; the only effect is a lighter weight on
small airports for it. A0 and unreported aircraft with jet kinematics were
inferred large.

The pair method, on the same sets: 3 of 12 origins right on real traffic, 12 of 21
on the straight flights (its best case, 57 %), and 11 false positives on the invented
aircraft. Whenever it was on, the combined conclusion abstained on every flight.

## Alternatives considered

### The category on board (ADR-0023)

It fixes the cause, but changes the flight software six days before the
competition, and helps only if the organisers' simulator sends the
identification messages with a category. It goes ahead behind a
configuration switch; this inference stays as its fallback either way.

### The type from the ICAO address, or the route from the callsign

Both rest on real-world databases. An invented scenario would make them wrong
with full confidence, and the callsign arrives only at the downlink.

### Inferring "light" and "rotorcraft" too

The kinematics of a light aeroplane and a helicopter overlap. A wrong "fixed
wing" excludes heliports, the worst error for a helicopter; a wrong "unknown"
costs nothing over today.

## Consequences

### Positive

- Origins decided without the category, with no wrong decision in the sets measured.
- No radio bytes, no flight software change.
- Both parts can be turned off for comparison
  (`--no-kinematic-category`, `--climb-back-weight 0`).

### Negative

- The thresholds were set by hand on 42 real flights and validated on 38 more,
  plus two synthetic scenarios: a small sample, a few hours of one airspace.
- A fast small aircraft (A2) can be marked large and see small airports
  weighted down.
- `climb_back` assumes a straight climb at today's rate. A climb that changes
  rate or turns makes the expected distance wrong; the wide σ is the hedge.
- The page's default departs from ADR-0019's intent of running both methods.

### Follow-up required

- The estimator's author reviews the inference against his note in `build_snapshot`.
- Revisit when the category arrives on board (ADR-0023): a received category
  makes the inference unnecessary.

## References

- `docs/vv/trafego-real/README.md`: the captures, the truth, the evaluations
- `tools/live_traffic/synthetic.py`, `avaliar.sh`, `avaliar_combinada.py`
- `ground/host/tests/estimator/test_kinematic_category.py`, `test_climb_back.py`
- `docs/relatorios/2026-11-18-proposta-categoria-adsb.html`
