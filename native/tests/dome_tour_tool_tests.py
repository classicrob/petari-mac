#!/usr/bin/env python3
"""Route geometry and evidence acceptance regressions for dome tours."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from observatory_routes import graph
from dome_tour import session_passed

class DomeToolTests(unittest.TestCase):
    def test_drop_cannot_pass_through_upper_floor(self):
        nodes = [(25,420,25,255,(0,0)), (75,392,25,255,(1,0)), (75,-26,25,255,(1,0))]
        edges, _ = graph(nodes, {(0,0):[0], (1,0):[1,2]})
        self.assertIn((1, None), edges[0])
        self.assertNotIn((2, "drop"), edges[0])
        edges, _ = graph([nodes[0], nodes[2]], {(0,0):[0], (1,0):[1]})
        self.assertIn((1, "drop"), edges[0])

    def test_failed_sessions_are_not_successful_commands(self):
        good = dict(status=0, result="PASS (tour)", visits=[dict(smoke="PASS", missing=[])],
                    in_progress=None, unattributed_missing=[], hang_samples=[], crash_reports=[])
        self.assertTrue(session_passed(good))
        for key, value in [("missing_galaxies", ["StarDustGalaxy"]), ("status", 1), ("status", "timeout"), ("result", "FAIL (route)"),
                           ("result", "ASSISTED (tour)"), ("visits", []),
                           ("in_progress", "EggStarGalaxy"), ("unattributed_missing", ["missing"]),
                           ("hang_samples", ["sample"]), ("crash_reports", ["crash"])]:
            bad = copy.deepcopy(good)
            bad[key] = value
            self.assertFalse(session_passed(bad), (key, value))
        bad = copy.deepcopy(good)
        bad["visits"][0]["missing"] = ["sound"]
        self.assertFalse(session_passed(bad))

if __name__ == "__main__":
    unittest.main()
