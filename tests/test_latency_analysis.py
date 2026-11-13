import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).resolve().parents[1]/'scripts/analyze_latency.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class Analysis(unittest.TestCase):
    def test_percentiles(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'samples.csv'
            path.write_text('timestamp_ns,type,workflow_id,device_id,command_id,detail\n'
                            '0,command_submitted,1,0,1,\n1000000,response_received,1,0,1,\n'
                            '0,command_submitted,2,1,2,\n2000000,response_received,2,1,2,\n'
                            '0,command_submitted,3,1,3,\n4000000,response_received,3,1,3,\n')
            result, series = analysis.analyze(path)
            self.assertEqual(result['summary']['count'], 3)
            self.assertAlmostEqual(result['summary']['mean'], 7/3)
            self.assertEqual(result['summary']['p50'], 2)
            self.assertAlmostEqual(result['summary']['p95'], 3.8)
            self.assertAlmostEqual(result['summary']['p99'], 3.96)
            self.assertEqual(result['summary']['max'], 4)
            self.assertAlmostEqual(result['summary']['jitter'], (14/9)**.5)
            self.assertEqual(result['per_device']['1']['count'], 2)
            self.assertEqual(len(series), 3)

    def test_dropped_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'dropped.csv'
            path.write_text('# dropped_events=9\n'
                            'timestamp_ns,type,workflow_id,device_id,command_id,detail\n'
                            '100,response_received,1,0,1,\n')
            result, series = analysis.analyze(path)
            self.assertEqual(result['dropped_events'], 9)
            self.assertEqual(result['unmatched_outcomes'], 1)
            self.assertIn('WARNING', analysis.report(result, series))
            self.assertEqual(result['summary']['count'], 0)


if __name__ == '__main__':
    unittest.main()
