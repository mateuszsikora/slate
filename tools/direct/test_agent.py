#!/usr/bin/env python3
"""Unit tests for the direct provider's in-memory reference models."""

import unittest

if __package__:
    from .agent import Cover
else:
    from agent import Cover


class CoverTest(unittest.TestCase):
    def test_toggle_starts_motion_from_an_idle_position(self) -> None:
        for position, expected in ((0, "opening"), (43, "closing")):
            with self.subTest(position=position):
                cover = Cover()
                cover.position = position

                self.assertTrue(cover.apply("toggle", {}))
                self.assertEqual(cover.motion, expected)

    def test_toggle_stops_motion_in_either_direction(self) -> None:
        for motion in ("opening", "closing"):
            with self.subTest(motion=motion):
                cover = Cover()
                cover.motion = motion

                self.assertTrue(cover.apply("toggle", {}))
                self.assertEqual(cover.motion, "idle")


if __name__ == "__main__":
    unittest.main()
