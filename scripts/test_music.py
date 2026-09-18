"""Validate playlist packing and limits with synthetic Opus packets."""
from pathlib import Path
from argparse import Namespace
import json
import runpy
import tempfile
import unittest

prepare = runpy.run_path(str(Path(__file__).with_name("prepare_music.py")))
song = prepare["pack_song"]
catalog = prepare["pack_catalog"]


class MusicTests(unittest.TestCase):
    def playlist_inputs(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "playlist.json"
            path.write_text(json.dumps(entries))
            args = Namespace(playlist=path, files=[], title=None, artist=None)
            return prepare["inputs"](args)

    def test_playlist_bitrates_preserve_defaults_and_per_song_overrides(self):
        entries = self.playlist_inputs([
            "first.flac",
            {"file": "second.flac", "title": "Second", "bitrateKbps": 88},
            {"file": "third.flac", "bitrateKbps": 64},
        ])
        self.assertEqual([entry["bitrateKbps"] for entry in entries], [96, 88, 64])
        self.assertEqual(entries[1]["title"], "Second")

    def test_invalid_playlist_bitrates_are_rejected_before_encoding(self):
        for value in [None, True, False, "88", "88k", 88.5, 0, -1, 513]:
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "bitrateKbps"):
                self.playlist_inputs([{"file": "missing.flac", "bitrateKbps": value}])

    def test_catalog_matches_the_independent_rust_fixture(self):
        packets = [b"\xf8\xff\xfe"]
        packed = catalog([song(packets*2, "First café", "Artist"), song(packets*3, "第二首", "")])
        fixture = Path(__file__).resolve().parents[1] / "firmware/crates/radio-core/tests/fixtures/playlist-v4.pack"
        self.assertEqual(packed, fixture.read_bytes())

    def test_bounds_are_applied_before_writing(self):
        packet = b"\xf8\xff\xfe"
        for packets in [[], [b""], [b"x"*1276]]:
            with self.assertRaises(ValueError): song(packets,"Title","")
        packed = song([packet], "Title", "")
        for songs in [[], [packed]*33]:
            with self.assertRaises(ValueError): catalog(songs)
        large = song([packet]*30000, "Title", "")
        with self.assertRaises(ValueError): catalog([large]*7)


if __name__ == "__main__":
    unittest.main()
