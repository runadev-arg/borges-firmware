# Borges

Your library, on your reader too.

**[Download Borges for Xteink X4](https://github.com/runadev-arg/borges-firmware/releases/latest/download/borges-x4.bin)** · [Installation guide](https://github.com/totokatz/borges.koplugin/blob/main/docs/FIRMWARE.md) · [Open Borges](https://borges.runadev.com)

Borges is e-reader firmware with a connected library, reading progress, highlights, notes and offline synchronization. The boot screen, menus, device interface and updates all use the Borges name.

## Install

Download `borges-x4.bin` from the latest release. Back up the microSD card and verify the published SHA-256 before installing. Follow the linked installation guide for your device; the KOReader plugin is a separate package.

After installation, connect Wi-Fi and open **Settings → System → Borges → Pair device**. Approve the code in your Borges account and choose **Sync now**.

## Build

Install PlatformIO, then run:

```sh
pio run -e borges_release
```

The firmware is written to `.pio/build/borges_release/firmware.bin`.
The SDK source is vendored at revision `d4dee40130c8de53b0e1bb2df5cd56f5d2afe1fd` so the release can be rebuilt without a private or unavailable submodule commit.

## Attribution and license

This firmware is a fork of the [upstream reader project](https://github.com/crosspoint-reader/crosspoint-reader), using [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk). Original copyright notices and licenses are retained. See [LICENSE](LICENSE).
