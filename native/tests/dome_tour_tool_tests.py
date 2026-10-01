#!/usr/bin/env python3
"""Route geometry and evidence acceptance regressions for dome tours."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from observatory_routes import graph, StaticWalls
from dome_tour import session_passed, menu_errors

class DomeToolTests(unittest.TestCase):
    def test_drop_cannot_pass_through_upper_floor(self):
        nodes = [(25,420,25,255,(0,0)), (75,392,25,255,(1,0)), (75,-26,25,255,(1,0))]
        edges, _ = graph(nodes, {(0,0):[0], (1,0):[1,2]})
        self.assertIn((1, None), edges[0])
        self.assertNotIn((2, "drop"), edges[0])
        edges, _ = graph([nodes[0], nodes[2]], {(0,0):[0], (1,0):[1]})
        self.assertIn((1, "drop"), edges[0])

    def test_diagonal_ray_reaches_neighbor_cell(self):
        try:
            import numpy as np  # observatory_routes.py's static collision needs numpy
        except ImportError:
            self.skipTest("numpy is not installed (pip install numpy); static-wall rays not checked")
        wall = np.array([[[65., 0., 20.], [65., 200., 20.], [65., 0., 100.]],
                         [[65., 200., 20.], [65., 200., 100.], [65., 0., 100.]]])
        clear = StaticWalls(wall).clear(25., 0., 25., (0, 0))
        self.assertFalse(clear & (1 << 1))
        self.assertTrue(clear & (1 << 2))

    def test_missing_or_incorrect_mission_menus_fail(self):
        menus = {"StarDustGalaxy": list(range(1, 7)), "BattleShipGalaxy": list(range(1, 8)),
                 "TamakoroExLv1Galaxy": [1], "BreakDownPlanetGalaxy": [1], "KoopaBattleVs1Galaxy": [1]}
        self.assertEqual(menu_errors(2, menus), {})
        menus["StarDustGalaxy"] = [1, 2, 3]
        self.assertIn("StarDustGalaxy", menu_errors(2, menus))
        del menus["BattleShipGalaxy"]
        self.assertIn("BattleShipGalaxy", menu_errors(2, menus))
        self.assertEqual(menu_errors(7, {"PeachCastleFinalGalaxy": [1]}), {})

    def test_failed_sessions_are_not_successful_commands(self):
        good = dict(status=0, result="PASS (tour)", visits=[dict(smoke="PASS", missing=[])],
                    in_progress=None, unattributed_missing=[], hang_samples=[], crash_reports=[])
        self.assertTrue(session_passed(good))
        for key, value in [("menu_errors", {"StarDustGalaxy": {}}), ("missing_galaxies", ["StarDustGalaxy"]), ("status", 1), ("status", "timeout"), ("result", "FAIL (route)"),
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
