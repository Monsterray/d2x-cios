## What and why

<!-- What the change does, and the problem it solves. -->

## How it was checked

<!-- Say which level of evidence each is: a host test, a build, or a run on a Wii (which console, which slot and base, which drive). -->

- [ ] `tools/build.sh test` passes
- [ ] `EXTRA_CFLAGS=-Werror tools/build.sh` builds with no warnings
- [ ] A new failure mode has a host test (a fault in `tests/host/ums/sim.c` for USB)
- [ ] If it changes a release's behaviour: `data/Changelog.txt` says so
- [ ] No keys or console data in the commits

See [CONTRIBUTING.md](../CONTRIBUTING.md).
