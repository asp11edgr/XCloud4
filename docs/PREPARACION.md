# Environment preparation — October 8, 2026

This records the observed setup at preparation time. Later builds and console results are in [JORNADA.md](JORNADA.md); space figures are historical snapshots.

## PC and virtual machine

- Existing VM: `Lubuntu 26.04`, VirtualBox 7.2.18.
- Observed guest OS: Ubuntu 26.04.1 LTS, x86_64; user `edgarg`.
- Resources: 4096 MB RAM, two processors, 16 GB virtual disk.
- Free space after installation: 5.7 GB in Lubuntu; approximately 6.9 GiB on C:.
- NAT networking; SSH from this PC through `127.0.0.1:2224`.
- Display configured to 1280 × 720, VirtualBox scale 85%, automatic resize disabled. The complete desktop was checked after reboot.

## Password reset

At the owner's request, the `edgarg` password was reset from recovery mode. Lubuntu confirmed `passwd: password updated successfully`. The password is not included in the project or this report.

## Installed tools

Tools were installed from the configured Ubuntu repositories. Observed versions:

| Tool | Version |
|---|---|
| Clang | 21.1.8 |
| LLD | 21.1.8 |
| Make | 4.4.1 |
| CMake | 4.2.3 |
| Ninja | 1.13.2 |
| Git | 2.53.0 |
| Python | 3.14.4 |

Also installed: build-essential, pkg-config, curl and OpenSSH Server. Clang/LLD 18 were unavailable in the configured repositories, so the distribution's version was used. Subsequent XCloud4 builds succeeded with Clang/LLD 21; this does not establish compatibility of every external library.

## SDK

OpenOrbis v0.5.4, official `toolchain-llvm-18.tar.gz`, 158688666 bytes. Its SHA-256 matches GitHub's published value and was checked again inside Lubuntu:

```text
3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526
```

Headers, libraries, Linux tools, linker script and licenses were extracted. `PkgTool.Core` dynamic dependencies were present; `create-fself` is a static executable.

## Guest locations

- Project: `/home/edgarg/Projects/XCloud4`.
- SDK: `/home/edgarg/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.
- Environment: `/home/edgarg/.config/xcloud4/env.sh`, loaded from `.bashrc`.
- SDK log: `/home/edgarg/.local/share/xcloud4/sdk-*.log`.
- Tool installation log: `/tmp/xcloud4-herramientas.log`.

Preparation initialized Git on `main`. The owner initially authorized private GitHub publication, then authorized public publication with the latest update and error report on 2026-10-09. Repository documentation is English; the PS4 UI stays Spanish.

## Access from Windows

OpenSSH is enabled and a public-key connection as `edgarg` was confirmed. VirtualBox forwarding listens only on `127.0.0.1:2224` on this PC. The private key stays outside the repository; the authorized key restricts interactive terminal use and SSH forwarding.

Initial transfer used a temporary server on `127.0.0.1:8765`, accessible from Lubuntu as `10.0.2.2:8765`. The project archive passed SHA-256 comparison. The temporary server was stopped afterward.

## Build progression

Initial preparation did not compile the application. Version 0.1.0 later produced UI/controller ELF, eboot and PKG artifacts. Packaging uses the isolated OpenSSL 1.1 libraries documented in [third-party notices](../THIRD_PARTY_NOTICES.md).

Since then, console-confirmed milestones reached 0.6.2: local media, Microsoft authorization, catalog, remote preparation and connection authorization. Actual Xbox game video/audio remains under development. See [the current status](../README.md).

## Sources

- [OpenOrbis v0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4).
- [Official OpenOrbis instructions](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- [VirtualBox NAT documentation](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/networkingdetails.html).
