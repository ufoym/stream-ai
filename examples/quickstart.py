import time

import streamcam


def main() -> int:
    devices = streamcam.list_devices()
    if not devices:
        print("no camera available")
        return 1

    device = devices[0]
    print(f"streamcam {streamcam.version()}")
    print(f"opening: {device['name']} [{device['id']}]")

    try:
        with streamcam.open(device["id"]) as reader:
            reader.start()
            try:
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    frame = reader.get_latest_frame()
                    if frame is not None:
                        print(
                            "frame: "
                            f"{frame['width']}x{frame['height']}, "
                            f"stride={frame['stride_bytes']}, "
                            f"bytes={frame['size_bytes']}, "
                            f"sequence={frame['sequence']}, "
                            f"format={frame['pixel_format']}"
                        )
                        return 0
                    time.sleep(0.005)
            finally:
                reader.stop()

        print("timed out waiting for the first frame")
        return 1
    except streamcam.Error as exc:
        print(exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
