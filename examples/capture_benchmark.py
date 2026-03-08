import time

import streamcam


def main() -> int:
    devices = streamcam.list_devices()
    if not devices:
        print("no camera available")
        return 1

    try:
        with streamcam.open(devices[0]["id"]) as reader:
            reader.start()

            deadline = time.monotonic() + 3.0
            last_sequence = 0
            frames = 0

            while time.monotonic() < deadline:
                frame = reader.get_latest_frame()
                if frame is not None and frame["sequence"] != last_sequence:
                    last_sequence = frame["sequence"]
                    frames += 1
                time.sleep(0.001)

            reader.stop()

        print(f"frames in 3s: {frames}")
        print(f"approx fps: {frames / 3.0:.2f}")
        return 0
    except streamcam.Error as exc:
        print(exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
