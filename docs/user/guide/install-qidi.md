# Installing HelixScreen on QIDI Printers

How to put HelixScreen on the built-in screen of a QIDI Q2, Max 4 or Plus 5, with the [HelixScreen easy installer](https://helixscreen.org/installer/) or from the command line. HelixScreen replaces QIDI's own screen program on these printers.

> **Which QIDI models?** On-device install works on the **Q2**, **Max 4** and **Plus 5**, which have a Linux-driven screen. The Plus 4 and the 3-series (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro) use a serial TJC display HelixScreen can't drive; they can still be [controlled remotely](easy-installer-unsupported.md#qidi-with-a-tjc-screen).

## Read This First: QIDI Studio and the Cloud

While HelixScreen runs, the printer's QIDI screen program is off, and some QIDI features depend on it:

- QIDI Studio can't see the QIDI Box or send prints to this printer
- QIDI cloud is unavailable
- Filament changes made for the Box don't stick

You can still send prints through Moonraker: Fluidd, Mainsail, or a slicer's Klipper connection all work. Removing HelixScreen brings the QIDI features back. The easy installer shows this notice before it changes anything.

## With the Easy Installer

1. Switch the printer on and make sure it is on the same network as your computer.
2. Open the installer and pick your printer from the list. If it is not listed, choose **My printer isn't listed**, pick the QIDI model and type the printer's address (on the printer's screen, usually under Settings, then Network).
3. The installer recognizes a QIDI by its 64-bit ARM board plus the chamber thermal protection sensor in its Klipper setup, or the hostname `linaro-alip`.
4. It signs in over SSH. QIDI printers ship with the login `mks` and password `makerbase`; the installer tries the QIDI logins it knows and asks you if none works.
5. It checks everything first and changes nothing until that passes, then installs HelixScreen and waits for it to start on the printer's screen.

### The sudo password

Those logins are not root, so the install needs `sudo`. If the printer lets the account use sudo without a password, nothing is asked. If sudo wants a password, the installer gives it the one you signed in with. If you signed in with an SSH key, the installer has no password to give and asks for the account's password in a screen titled "Your printer needs your password". It checks that password with sudo before it uploads anything, and asks again if sudo turns it down (three tries in all). The password is used for this install only and never saved.

## Updating

Run the installer again. When HelixScreen is already on the printer it offers an update instead, follows the channel you are on, and never installs an older version over a newer one.

## Removing HelixScreen

The installer app does not remove HelixScreen yet. From an SSH session on the printer, use the uninstall command from the [Installation guide](../INSTALL.md#uninstalling). It removes HelixScreen and brings QIDI's screen program back, so Studio box sync and the cloud work again.

## From the Command Line

The one-line installer in the [Installation guide](../INSTALL.md#quick-start) works on these printers, run over SSH as the `mks` user.

## If HelixScreen Doesn't Appear

Wait a few minutes: the installer waits up to five minutes for the service to start. If the screen stays on QIDI's interface, collect the log with `journalctl -u helixscreen -n 100` over SSH and ask in [Discord](https://discord.gg/RZCT2StKhr) or on [GitHub](https://github.com/prestonbrown/helixscreen/issues).

## See Also

- [Supported Printers: QIDI](supported-printers.md#qidi-plus-4--q2--plus-5--max-4): what HelixScreen does with the QIDI Box
- [Remote screen setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device): for models that can't run it on-device
