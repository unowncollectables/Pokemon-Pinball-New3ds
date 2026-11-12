#!/usr/bin/env python3
"""Run the two table replays against the ARM headless build."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def check_ppm(path, width, height):
    header = f"P6\n{width} {height}\n255\n".encode()
    data = path.read_bytes()
    if not data.startswith(header) or len(data) != len(header) + width * height * 3:
        raise ValueError(f"{path.name}: expected a complete {width}x{height} image")


def check_table(command, output, field, view):
    selection = "640:RIGHT," if field == 1 else ""
    keys = (
        f"100:START,250:START,400:START,550:A,{selection}650:A,750:A,"
        "950:A+R,1050:A+R,1150:A+R,1250:A+R"
    )
    env = os.environ.copy()
    env.pop("PORT_NOTALL", None)
    env.update(PORT_TEST_BONUS="1", PORT_TEST_TRACE="1", PORT_LAYOUTDUMP="1",
               PORT_VIEW=str(view), PORT_DEPTH="0")
    run = subprocess.run(
        [*command, "--frames", "1900", "--dump", "550,1200,1450,1800",
         "--keys", keys, "--out", str(output)],
        env=env, capture_output=True, text=True, timeout=90,
    )
    if run.returncode:
        tail = "\n".join(run.stderr.splitlines()[-15:])
        raise ValueError(f"headless exited with status {run.returncode}\n{tail}")

    transitions = re.findall(r"^check: (enter bonus|leave bonus)$", run.stderr, re.M)
    if transitions != ["enter bonus", "leave bonus"]:
        raise ValueError(f"unexpected bonus transitions {transitions}")

    frames = {
        int(frame): (int(board), int(ball), int(row), int(moving))
        for frame, board, ball, row, moving in re.findall(
            r"^check: tall (\d+) (\d+) (-?\d+) (\d+) ([01])$", run.stderr, re.M
        )
    }
    if frames.get(1200, (None,))[0] != field or frames.get(1800, (None,))[0] != field:
        raise ValueError("the replay did not enter and return to the expected table")
    if 1450 in frames:
        raise ValueError("the bonus stage rendered in tall mode")

    for frame, height in [(550, 160), (1200, 480), (1450, 160), (1800, 480)]:
        check_ppm(output / f"f{frame:05d}.ppm", 240, height)
        check_ppm(output / f"f{frame:05d}_top.ppm", 400, 240)
        check_ppm(output / f"f{frame:05d}_bot.ppm", 320, 240)

    if view:
        # With the default eight-row gap the camera can scroll through 112 source rows.
        if any(row > 112 * 16 for _, _, row, _ in frames.values()):
            raise ValueError("the follow camera exceeded the table bounds")
        if not any(moving for _, _, _, moving in frames.values()):
            raise ValueError("the follow camera never moved")
        if field == 0:
            for frame in range(1201, 1300):
                _, previous_ball, previous_row, _ = frames[frame - 1]
                _, ball, row, _ = frames[frame]
                if previous_ball >= 0 and ball - previous_ball >= 4 and row > previous_row:
                    break
            else:
                raise ValueError("the follow camera did not track a fast descent on Ruby")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("build/pinball_headless"))
    parser.add_argument("--qemu", default="qemu-arm", help="ARM emulator executable")
    args = parser.parse_args()

    for field, table in enumerate(("Ruby", "Sapphire")):
        for view, layout in enumerate(("whole table", "follow ball")):
            try:
                with tempfile.TemporaryDirectory(prefix="pinball-check-") as output:
                    check_table([args.qemu, str(args.binary.resolve())], Path(output), field, view)
            except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                parser.exit(1, f"{table}, {layout}: {error}\n")
            print(f"PASS {table}, {layout}")


if __name__ == "__main__":
    main()
