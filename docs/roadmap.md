# Roadmap

Ideas that are not defects yet, each with a trigger. Verified defects go in [REVIEW.md](../REVIEW.md).

## Next

- **A way out of an IOS hang.** Every recovery that goes back to HBC needs IOS. When IOS itself hangs (the 32 KB transfer, the DI gap read), only a power cycle helps, so these tests need someone at the Wii.
  - Two ideas to test:
    - an HW_RESETS fallback in HBC's agent;
    - a debug-only ARM-side reset timer in a cIOS test build: an mload thread that resets the console when the PowerPC hasn't checked in for N seconds. Never in 249 or a release.
  - First check whether a Starlet thread still runs when the DI module or `/dev/usb/ven` hangs, and whether BootMii as boot2 lets the reset through.
  - Trigger: the next test that can hang IOS.

- **Bench test program.** A DOL, run from HBC through the bench queue, that reports which slots hold which cIOS (read only), then opens the cIOS's USB device and times sector reads and writes to a scratch file area, printing results to the HBC log. Trigger: before the first install of a fork build.
- **ARM emulator harness.** Run the real module ELFs (big-endian ARM926) under an emulator such as Unicorn, with IOS syscalls and EHCI registers simulated in Python. This tests the compiled binary instead of a host rebuild. Trigger: a bug that the host tests cannot show (timing, endianness, the stripped ELF layout) or a second module that needs hardware-level simulation.
