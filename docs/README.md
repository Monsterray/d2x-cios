# d2x developer docs

Notes for people working on the cIOS. For setup and the build, start with [CONTRIBUTING.md](../CONTRIBUTING.md). For what a release changes, see [data/Changelog.txt](../data/Changelog.txt).

| Page | Read it when |
| --- | --- |
| [usb-storage.md](usb-storage.md) | you change how the USB driver retries, recovers or gives up: the layers, error codes, deadlines, the recovery ladder, USBS |
| [sd-high-speed.md](sd-high-speed.md) | you touch the SD card driver: how 50 MHz is switched on, the fallback, the `-DSDIO_NO_HIGH_SPEED` opt-out |
| [debug-build.md](debug-build.md) | you chase a hang inside IOS: the `+dbg` build, its trace, watchdog and fault injection |
| [mload.md](mload.md) | you touch MLOAD or load modules from the SD card: how mload works, the SD loader (files, limits, report), security, the Wii test steps, BootMii-reborn notes |
| [module-size.md](module-size.md) | a module grows: each module's fixed size and how much room is left |
| [bench.md](bench.md) | you compare a measurement from a Wii (USB, SD, Wi-Fi) |
| [wii-mini.md](wii-mini.md) | you touch the Wii Mini bases (57 v31775/v31776) or the IOP module they add: what came from d2xl, what is unchecked |
| [roadmap.md](roadmap.md) | you have an idea that is not a defect yet: ideas with a trigger |
| [../REVIEW.md](../REVIEW.md) | you look for a verified finding, its fix and its check |

Where things go: a verified defect in `REVIEW.md`, a measurement in `bench.md`, an idea in `roadmap.md`. Say what each result was checked with: a host test, a build, or a run on a Wii.
