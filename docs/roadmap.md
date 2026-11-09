# Roadmap

Ideas that are not defects yet, each with a trigger. Verified defects go in [REVIEW.md](../REVIEW.md).

## Next

- **USB hangs: let the next boot through.** Tried on 2026-11-08 and it does not work: the Wii's EHCI has no port power switching, and a bus reset or a disabled port before the watchdog's reset still leaves IOS58 wedged on a stuck drive (REVIEW.md, the 2026-11-08 update to "A USB drive left mid-transfer"). What is left to try: a drive other than the SanDisk, and what HBC's run-time PORTSC enable-bit clear does that IOS58's own start-up does not. Until then, a USB hang test needs someone at the Wii to unplug the drive. Then `usbs-big` on 252 base 58 (install the current build there first) and `di-error` after a reinstall of 250.

- **Faster networking (decision, 2026-11-08; evidence in [bench.md](bench.md#network-over-wi-fi-devwii-2026-11-06-and-2026-11-08)).** Over Wi-Fi the time goes to the radio link, not to IOS. The PowerPC-to-IOS round trip costs 65-200 us a call; the ARM stays idle during transfers; the Wi-Fi chip's SDIO bus runs 4-bit at 24 MHz with about 96% of it unused. UDP, with no TCP in the way, is still capped near 0.5 MB/s each way. Each datagram costs 1.5-2.6 ms, which fits an 802.11b-class rate (5.5-11 Mbit/s) or retries.
  - (a) Patching Nintendo's modules through the module patcher: **not now.** Bigger TCP buffers (which IOS refuses through the API) or fewer IPC calls would remove about 0.2 ms of a 1.5-2.6 ms packet. High speed on the Wi-Fi SDIO bus would double a bus that is 4% used.
  - (b) A new IOS module for a USB LAN adapter through d2x's EHCI, with its own TCP/IP stack: the only path that leaves the radio behind. But IOS already drives Nintendo's USB LAN adapter (ASIX AX88772) through its own ETH module, so first measure that.
  - Steps, cheapest first:
    1. Read the router's view of the Wii (link rate, signal) and run netbench once with the Wii near the access point. HBC's own transfers reached 1.35 MB/s received on a better day ([hbc-reborn docs/devnet.md](https://github.com/Monsterray/hbc-reborn/blob/develop/docs/devnet.md)), so the link varies.
    2. With an AX88772-based adapter, run netbench wired on a stock IOS and on a `+next` slot.
    3. Build (b) only if wired through Nintendo's own stack is still far below what EHCI can move (21.8 MB/s for storage). It is a large module (TCP/IP in a few hundred KB of ARM memory), so the trigger is a wired result that shows the stack, not the radio, as the limit.
  - Run long benches on lrwii (`--wii lrwii`, its 252/253/247 or stock IOSes, never 249).

- **Bench test program.** A DOL, run from HBC through the bench queue, that reports which slots hold which cIOS (read only), then opens the cIOS's USB device and times sector reads and writes to a scratch file area, printing results to the HBC log. Trigger: before the first install of a fork build.
- **ARM emulator harness.** Run the real module ELFs (big-endian ARM926) under an emulator such as Unicorn, with IOS syscalls and EHCI registers simulated in Python. This tests the compiled binary instead of a host rebuild. Trigger: a bug that the host tests cannot show (timing, endianness, the stripped ELF layout) or a second module that needs hardware-level simulation.
