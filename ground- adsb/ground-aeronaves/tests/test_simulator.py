import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ground.parser import parse_line
from ground.geo import distance_km
from simular_trajetorias import Flight, generate


class SimulatorTests(unittest.TestCase):
    def setUp(self):
        self.flight=Flight('F00001','SIM001','arrival',10,480,-15.87,-47.92,3500,110,60)

    def test_messages_parse_and_aircraft_has_start_and_end(self):
        self.assertIsNone(self.flight.message(9,1700000000000000000))
        self.assertIsNone(self.flight.message(491,1700000000000000000))
        for t in (10,120,300,490):
            m=self.flight.message(t,1700000000000000000)
            parse_line(json.dumps(m),1)
        last=self.flight.message(490,1700000000000000000)
        self.assertLess(distance_km(last['lat'],last['lon'],self.flight.lat,self.flight.lon),.001)
        self.assertEqual(last['altitude_ft'],3500)
        self.assertEqual(last['on_ground'],1)

    def test_velocity_and_altitude_change_agree(self):
        for mode in ('arrival','departure','cruise'):
            flight=Flight('F00001','SIM001',mode,0,480,-15.87,-47.92,3500,110,60)
            a,b=[flight.message(t,1700000000000000000) for t in (120,121)]
            speed=distance_km(a['lat'],a['lon'],b['lat'],b['lon'])*3600/1.852
            self.assertAlmostEqual(speed,a['ground_speed_kt'],delta=1)
            self.assertAlmostEqual((b['altitude_ft']-a['altitude_ft'])*60,a['vertical_rate_fpm'],delta=60)

    def test_append_preserves_file_and_shared_timestamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            output=Path(tmp)/'eventos.ndjson'
            original='{"icao":"ABC123"}'
            output.write_text(original)
            flights=[Flight(icao,'SIM','departure',0,480,0,0,100,90,60) for icao in ('F00001','F00002')]
            with patch('simular_trajetorias.time.sleep'):
                self.assertEqual(generate(output,flights,duration=1,interval=1),4)
            lines=output.read_text().splitlines()
            self.assertEqual(lines[0],original)
            rows=[json.loads(line) for line in lines[1:]]
            self.assertEqual(rows[0]['rx_epoch_ns'],rows[1]['rx_epoch_ns'])
            self.assertEqual(rows[2]['rx_epoch_ns']-rows[0]['rx_epoch_ns'],1000000000)
