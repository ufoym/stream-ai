import streamcam


def main() -> int:
    devices = streamcam.list_devices()

    print(f"streamcam {streamcam.version()}")
    print(f"devices: {len(devices)}")
    for index, device in enumerate(devices):
        print(f"{index}: {device['name']} [{device['id']}]")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
