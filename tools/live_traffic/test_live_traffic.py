"""live_traffic.py without the network: a recorded adsb.lol response and two
recorded OpenSky tracks, from Buenos Aires on 2026-11-17 (fixtures/)."""

import copy
import gzip
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import live_traffic as lt  # noqa: E402

FIX = HERE / 'fixtures'
POINT = json.loads((FIX / 'adsblol_point_unsam.json').read_text())
TRACK_JES3104 = json.loads((FIX / 'opensky_track_e0b110.json').read_text())   # leaving Aeroparque
TRACK_ARG1669 = json.loads((FIX / 'opensky_track_e0b255.json').read_text())   # seen from cruise

# Aeroparque and the heliports around it, as referencias.db has them, and an
# airport far away.
AIRPORTS = [
    ('SABE', 'large_airport', -34.559419, -58.415536, 18.0),
    ('AR-0126', 'heliport', -34.57257, -58.40045, 19.0),
    ('AR-0347', 'heliport', -34.5808, -58.4294, 193.0),
    ('SAEZ', 'large_airport', -34.8222, -58.5358, 67.0),
]


def by_icao(records):
    return {r['icao']: r for r in records}


class TestRecords(unittest.TestCase):
    def test_every_aircraft_with_a_recent_message_becomes_a_record(self):
        recs = lt.records_from_response(POINT)
        self.assertEqual(len(recs), len(POINT['ac']))
        self.assertTrue(all(r['source'] == 'adsb.lol' for r in recs))

    def test_fields_and_time(self):
        r = by_icao(lt.records_from_response(POINT))['E0B110']   # JES3104 climbing out of SABE
        self.assertEqual(r['callsign'], 'JES3104')
        self.assertEqual(r['category'], 'A3')
        self.assertEqual(r['altitude_ft'], 1425)
        self.assertEqual(r['on_ground'], 0)
        self.assertEqual(r['vertical_rate_fpm'], 3136)
        self.assertEqual(r['aircraft_type'], 'A320')
        # now minus seen_pos: 1794926970.500 - 0.171 s
        self.assertEqual(r['rx_epoch_ns'], 1794926970329000000)

    def test_on_ground_has_no_altitude(self):
        r = by_icao(lt.records_from_response(POINT))['E80333']   # SKU538 parked at SABE
        self.assertEqual(r['on_ground'], 1)
        self.assertNotIn('altitude_ft', r)
        self.assertIn('lat', r)

    def test_stale_position_is_left_out_like_adsbd(self):
        # LVHQO's position is 60.7 s old: adsbd's position_max_age is 10 s.
        r = by_icao(lt.records_from_response(POINT))['E0844F']
        self.assertNotIn('lat', r)
        self.assertEqual(r['altitude_ft'], 1700)
        # Its time is then that of its last message.
        self.assertEqual(r['rx_epoch_ns'], POINT['now'] * 1_000_000 - 60_700_000_000)

    def test_mlat_and_non_icao_are_not_what_our_receiver_sees(self):
        resp = copy.deepcopy(POINT)
        jes = next(a for a in resp['ac'] if a['hex'] == 'e0b110')   # a fresh position
        jes['mlat'] = ['lat', 'lon']
        resp['ac'].append(dict(jes, hex='~2a01ff', mlat=[]))
        recs = by_icao(lt.records_from_response(resp))
        self.assertNotIn('lat', recs['E0B110'])
        self.assertNotIn('~2A01FF', recs)
        self.assertIn('lat', by_icao(lt.records_from_response(resp, keep_mlat=True))['E0B110'])

    def test_deduper_keeps_only_newer_updates(self):
        keep = lt.Deduper()
        t = 1794926970_000_000_000
        r = {'icao': 'E0B110', 'rx_epoch_ns': t}
        self.assertTrue(keep(r))
        self.assertFalse(keep(dict(r)))                          # the same update, next poll
        self.assertFalse(keep(dict(r, rx_epoch_ns=t + 1_000_000)))   # the same, rounded apart
        self.assertFalse(keep(dict(r, rx_epoch_ns=t - 1)))
        self.assertTrue(keep(dict(r, rx_epoch_ns=t + 500_000_000)))


class TestConvert(unittest.TestCase):
    def test_two_polls_are_deduplicated_and_time_ordered(self):
        later = copy.deepcopy(POINT)
        later['now'] += 5000
        for a in later['ac']:
            if a['hex'] == 'e0b110':
                a['seen_pos'] = a['seen'] = 0.2            # a new position
            else:
                a['seen_pos'] = (a.get('seen_pos') or 0) + 5   # nothing new
                a['seen'] = (a.get('seen') or 0) + 5
        with tempfile.TemporaryDirectory() as d:
            raw = Path(d) / 'raw.jsonl.gz'
            with gzip.open(raw, 'wt', encoding='utf-8') as f:
                f.write(json.dumps(POINT) + '\n' + json.dumps(later) + '\n')
            recs = lt.convert(raw)
            plain = lt.convert(raw, no_category=True)
        self.assertEqual(len(recs), len(POINT['ac']) + 1)
        stamps = [r['rx_epoch_ns'] for r in recs]
        self.assertEqual(stamps, sorted(stamps))
        self.assertEqual(sum(r['icao'] == 'E0B110' for r in recs), 2)
        self.assertFalse(any('category' in r for r in plain))

    def test_the_estimator_accepts_every_record(self):
        sys.path.insert(0, str(lt.ROOT / 'ground/host/ground-aeronaves'))
        from ground.parser import parse_line
        for r in lt.records_from_response(POINT):
            parse_line(json.dumps(r), r['rx_epoch_ns'])


class TestTruth(unittest.TestCase):
    def test_snap_prefers_the_airport_to_the_closer_heliport(self):
        # A point between SABE and the Dines heliport, nearer the heliport.
        self.assertEqual(lt.snap_airport(-34.5700, -58.4050, None, AIRPORTS), 'SABE')
        self.assertEqual(lt.snap_airport(-34.5700, -58.4050, None, AIRPORTS, 'A3'), 'SABE')
        self.assertEqual(lt.snap_airport(-34.5700, -58.4050, None, AIRPORTS, 'A7'), 'AR-0126')
        self.assertIsNone(lt.snap_airport(-34.5700, -58.4050, 5000, AIRPORTS))     # too high
        self.assertIsNone(lt.snap_airport(-34.70, -58.47, None, AIRPORTS))          # too far

    def test_a_jet_low_after_take_off_snaps_to_the_airport_not_the_heliport(self):
        # GLO7640's first OpenSky report: 914 m, 6.6 km from Guarulhos, 1.7 km
        # from a heliport (17/11).
        gru = [('SBGR', 'large_airport', -23.435556, -46.473056, 2459.0),
               ('BR-2467', 'heliport', -23.428675, -46.408766, 2540.0)]
        self.assertEqual(lt.snap_airport(-23.414, -46.412, 914 * lt.FT_PER_M, gru, 'A3'), 'SBGR')

    def test_entering_the_capture_flying_low_is_not_a_take_off(self):
        # SAETTA92 on 17/11: first seen at 1350 ft over a heliport, level.
        heli = [('AR-0809', 'heliport', -34.376348, -58.665353, 3.0)]
        recs = [{'icao': 'E200D7', 'rx_epoch_ns': k, 'lat': -34.3587 - k * 1e-3, 'lon': -58.6778,
                 'altitude_ft': alt, 'on_ground': 0} for k, alt in enumerate((1350, 1375, 1350, 1350))]
        self.assertIsNone(lt.truth(recs, {}, heli)['E200D7']['origin'])

    def test_a_departure_track_gives_the_origin_and_no_destination(self):
        # JES3104's track starts at 0 m, 0.4 km from SABE, and ends climbing.
        t = lt.truth([{'icao': 'E0B110', 'rx_epoch_ns': 1, 'lat': -34.54, 'lon': -58.44,
                       'altitude_ft': 1425, 'on_ground': 0}],
                     {'E0B110': TRACK_JES3104}, AIRPORTS)['E0B110']
        self.assertEqual(t['origin'], 'SABE')
        self.assertEqual(t['evidence']['origin'], 'opensky_track')
        self.assertIsNone(t['destination'])

    def test_a_track_seen_from_cruise_gives_no_origin(self):
        t = lt.truth([{'icao': 'E0B255', 'rx_epoch_ns': 1, 'lat': -34.87, 'lon': -58.97,
                       'altitude_ft': 12875, 'on_ground': 0}],
                     {'E0B255': TRACK_ARG1669}, AIRPORTS)['E0B255']
        self.assertIsNone(t['origin'])
        self.assertIsNone(t['destination'])

    def test_a_landing_seen_by_the_capture_gives_the_destination(self):
        recs = [{'icao': 'E4953D', 'rx_epoch_ns': 1, 'lat': -34.70, 'lon': -58.30, 'altitude_ft': 6000, 'on_ground': 0},
                {'icao': 'E4953D', 'rx_epoch_ns': 2, 'lat': -34.60, 'lon': -58.38, 'altitude_ft': 1500, 'on_ground': 0},
                {'icao': 'E4953D', 'rx_epoch_ns': 3, 'lat': -34.5590, 'lon': -58.4160, 'on_ground': 1}]
        t = lt.truth(recs, {}, AIRPORTS)['E4953D']
        self.assertEqual(t['destination'], 'SABE')
        self.assertEqual(t['evidence']['destination'], 'capture')
        self.assertIsNone(t['origin'])                     # first seen at 6000 ft

    def test_low_at_the_end_without_descending_is_not_a_landing(self):
        recs = [{'icao': 'E0B110', 'rx_epoch_ns': 1, 'lat': -34.5590, 'lon': -58.4160, 'on_ground': 1},
                {'icao': 'E0B110', 'rx_epoch_ns': 2, 'lat': -34.5560, 'lon': -58.4200, 'altitude_ft': 300, 'on_ground': 0},
                {'icao': 'E0B110', 'rx_epoch_ns': 3, 'lat': -34.5480, 'lon': -58.4360, 'altitude_ft': 1425, 'on_ground': 0}]
        t = lt.truth(recs, {}, AIRPORTS)['E0B110']
        self.assertEqual(t['origin'], 'SABE')
        self.assertEqual(t['evidence']['origin'], 'capture')
        self.assertIsNone(t['destination'])

    def test_parked_aircraft_are_not_flights(self):
        recs = [{'icao': 'E80333', 'rx_epoch_ns': 1, 'lat': -34.5573, 'lon': -58.4149, 'on_ground': 1}]
        self.assertEqual(lt.truth(recs, {}, AIRPORTS), {})


class FakeNet:
    """adsb.lol answers with the fixture; OpenSky with a track or a 429."""

    def __init__(self, opensky_status=200):
        self.opensky_status = opensky_status
        self.urls = []

    def __call__(self, url, timeout=20.0):
        self.urls.append(url)
        if 'adsb.lol' in url:
            return 200, POINT
        if self.opensky_status != 200:
            return self.opensky_status, None
        return 200, TRACK_JES3104


class TestCapture(unittest.TestCase):
    def run_capture(self, net, max_tracks=200):
        t = [0.0]
        with tempfile.TemporaryDirectory() as d:
            meta = lt.capture(d, lt.UNSAM, 150, 5.0, 12.0, max_tracks, fetch=net,
                              clock=lambda: t[0], sleep=lambda s: t.__setitem__(0, t[0] + s),
                              log=lambda *a: None)
            files = sorted(p.name for p in (Path(d) / 'tracks').iterdir())
            raw_lines = len(gzip.open(Path(d) / 'raw.jsonl.gz', 'rt').read().splitlines())
            on_disk = json.loads((Path(d) / 'meta.json').read_text())
        return meta, files, raw_lines, on_disk

    def test_polls_on_the_period_and_asks_tracks_at_the_end(self):
        net = FakeNet()
        meta, files, raw_lines, on_disk = self.run_capture(net)
        self.assertEqual(meta['polls'], 3)                 # at 0, 5 and 10 s of 12
        self.assertEqual(raw_lines, 3)
        self.assertEqual(meta['aircraft'], len(POINT['ac']))
        self.assertEqual(len(files), len(POINT['ac']))
        self.assertEqual(on_disk['tracks'], len(POINT['ac']))
        lol = [u for u in net.urls if 'adsb.lol' in u]
        self.assertEqual(lol[0], 'https://api.adsb.lol/v2/point/-34.5772/-58.5225/150')

    def test_a_429_stops_the_track_requests(self):
        net = FakeNet(opensky_status=429)
        meta, files, _, _ = self.run_capture(net)
        self.assertEqual(files, [])
        self.assertEqual(sum('opensky' in u for u in net.urls), 1)
        self.assertIn('429', meta['track_stop'])

    def test_a_429_backs_off_and_a_200_resets(self):
        # adsb.lol refused 30% of the polls at 5 s on 17/11 (HTTP 429).
        answers = iter([429, 429, 429, 200, 200] + [200] * 50)
        polls = []
        t = [0.0]

        def net(url, timeout=20.0):
            if 'adsb.lol' not in url:
                return 200, TRACK_JES3104
            polls.append(t[0])
            status = next(answers)
            return (status, POINT) if status == 200 else (status, None)
        with tempfile.TemporaryDirectory() as d:
            meta = lt.capture(d, lt.UNSAM, 150, 5.0, 80.0, 0, fetch=net, clock=lambda: t[0],
                              sleep=lambda s: t.__setitem__(0, t[0] + s), log=lambda *a: None)
        gaps = [b - a for a, b in zip(polls, polls[1:])]
        self.assertEqual(gaps[:5], [10.0, 15.0, 25.0, 5.0, 5.0])   # 5 + 5, 5 + 10, 5 + 20, then the period
        self.assertEqual(meta['rate_limited'], 3)

    def test_max_tracks_is_respected(self):
        meta, files, _, _ = self.run_capture(FakeNet(), max_tracks=3)
        self.assertEqual(len(files), 3)


class TestCombinedScore(unittest.TestCase):
    def test_the_same_rule_as_the_estimators_validation(self):
        import avaliar_combinada as ac
        truth = {'A': {'origin': 'SABE', 'destination': None}, 'B': {'origin': 'SAEZ', 'destination': 'SABE'},
                 'C': {'origin': 'SADF', 'destination': None}}
        states = {
            'A': {'combined_analysis': {'origin': {'ident': 'SABE', 'status': 'ESTIMATED'},
                                        'destination': {'ident': 'SBGR', 'status': 'TENTATIVE'}},
                  'pair_analysis': {'candidates': [{'origin': {'ident': 'SABE'}, 'destination': {'ident': 'SBGR'}}]}},
            'B': {'combined_analysis': {'origin': {'ident': 'SABE', 'status': 'ESTIMATED'},
                                        'destination': {'ident': None, 'status': 'UNKNOWN'}}},
            'C': {'combined_analysis': {'origin': {'ident': 'SADF', 'status': 'UNKNOWN'}}},
        }
        r = ac.score(states, truth, 'combined')
        self.assertEqual((r['origin']['acertos'], r['origin']['erros'], r['origin']['unknown']), (1, 1, 1))
        self.assertEqual((r['destination']['unknown'], r['destination']['falsos_positivos']), (1, 1))
        self.assertAlmostEqual(r['origin']['acerto_quando_decidiu'], 0.5)
        p = ac.score(states, truth, 'pairs')
        self.assertEqual((p['origin']['acertos'], p['origin']['unknown']), (1, 2))


class TestReferenceDb(unittest.TestCase):
    @unittest.skipUnless(lt.REFERENCE_DB.exists(), 'referencias.db not present')
    def test_aeroparque_from_the_reference_database(self):
        airports = lt.load_airports(lt.REFERENCE_DB)
        self.assertEqual(lt.snap_airport(-34.5569, -58.4198, 0, airports), 'SABE')
        self.assertEqual(lt.snap_airport(-34.5569, -58.4198, 0, airports, 'A3'), 'SABE')
        self.assertEqual(lt.parse_center('SABE', airports), (-34.559419, -58.415536))
        self.assertEqual(lt.parse_center('-34.5,-58.5'), (-34.5, -58.5))
        self.assertEqual(lt.parse_center(None), lt.UNSAM)


if __name__ == '__main__':
    os.chdir(HERE)
    unittest.main()
