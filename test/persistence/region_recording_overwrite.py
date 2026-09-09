"""Exercise enabled-to-disabled overwrite through the shipping recorder/player."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="region-recording-overwrite-") as directory:
        root = Path(directory)
        shutil.copytree(args.source / "data/common", root / "data/common")
        shutil.copytree(args.source / "worlds/atlas/presets", root / "worlds/atlas/presets")
        systems_path = root / "data/common/systems.json"
        systems = json.loads(systems_path.read_text(encoding="utf-8-sig"))
        recording = root / "overwrite.lrec"
        companion = Path(str(recording) + ".regions.json")

        def run(*options, failure=False):
            # Use the authored flat preset so this filesystem/replay regression
            # also finishes under ASan; both roundtrips still verify tick 30.
            command = [str(args.server), "--root", str(root), "--radius", "1",
                       "--collision-radius", "1", "--preset", "flat_lands", *map(str, options)]
            result = subprocess.run(command, cwd=root, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=180)
            print(result.stdout, flush=True)
            if failure:
                assert result.returncode != 0, "recording reported success after removal failed"
                assert "cannot remove stale region schedule trace" in result.stdout
            else:
                assert result.returncode == 0, command

        systems["sim"]["active_regions"] = {"enabled": True}
        systems_path.write_text(json.dumps(systems))
        # Playback requires a final checkpoint (one every 30 ticks).
        run("--record", recording, "--ticks", "30")
        assert companion.is_file()
        enabled_bytes = recording.read_bytes()
        run("--replay", recording)

        systems["sim"]["active_regions"] = {"enabled": False}
        systems_path.write_text(json.dumps(systems))
        run("--record", recording, "--ticks", "30")
        assert recording.read_bytes() != enabled_bytes
        assert not companion.exists(), "disabled recording retained enabled companion"
        run("--replay", recording)

        # A nonempty directory fails removal even when run as root; no chmod or
        # filesystem timing assumptions are needed to exercise the error path.
        companion.mkdir()
        (companion / "keep").write_text("unrelated file")
        run("--record", recording, "--ticks", "1", failure=True)
        assert (companion / "keep").read_text() == "unrelated file"
    print("Enabled/disabled replay overwrite and failed removal: PASS")


if __name__ == "__main__":
    main()
