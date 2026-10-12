# Run HelixScreen in a Window on a Linux Desktop

HelixScreen can run as an ordinary app window on an x86_64 Linux desktop or laptop. It connects to your printer over the network, so you get the same screen you would have on the printer, at your desk, without installing anything on the printer itself.

This is a different package from the x86 kiosk install, which takes over a mini PC's whole screen at boot. The desktop package never touches your login screen or desktop session, needs no `sudo`, and installs only into your own home directory.

---

## Requirements

- **An x86_64 (64-bit Intel/AMD) Linux PC** with a graphical desktop: GNOME, KDE Plasma, Xfce, COSMIC, and others all work, on X11 or Wayland.
- **Debian 11, Ubuntu 20.04, Fedora 34, or anything newer.** The app needs glibc 2.31 or later. Check yours with `ldd --version | head -1`.
- **Network access to your printer's Moonraker** (port `7125` by default). Your printer needs to be running Klipper and Moonraker; the Mainsail or Fluidd web interface is not required.

Everything else the app needs is already on a normal desktop install. The 3D G-code preview uses your graphics driver's OpenGL ES support when it has it; without it, the preview draws in 2D and everything else works the same.

---

## Install

Open a terminal **on your desktop PC** (not on the printer) and run:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --desktop
```

Do not use `sudo`. The installer puts HelixScreen in `~/.local/share/helixscreen` and adds **HelixScreen** to your applications menu.

> **Without `--desktop`:** on a PC with a desktop session, the installer asks whether you want HelixScreen in a window (the default) or as a full-screen kiosk. The kiosk option is meant for a dedicated printer screen; it replaces your login screen, so pick the window unless that is what you want. Passing `--desktop` skips the question.

---

## First Launch

Start **HelixScreen** from your applications menu, or from a terminal:

```bash
~/.local/share/helixscreen/bin/helix-screen
```

The setup wizard opens in the window. At the **Moonraker Connection** step, enter your **printer's IP address** (for example `192.168.1.50`) and leave the port at `7125`. The wizard then discovers your printer's fans, heaters, sensors and filament system as usual. See [Remote Screen Setup](../INSTALL.md#remote-screen-setup-run-on-a-separate-device) for more on connecting to a printer over the network.

To skip the address entry, or to point a second window at another printer, pass the address on the command line:

```bash
~/.local/share/helixscreen/bin/helix-screen --moonraker ws://192.168.1.50:7125
```

### Window size

The window opens at 800x480, the size of a typical printer touchscreen. Pick another size with `-s`, either a size name or exact pixels:

```bash
~/.local/share/helixscreen/bin/helix-screen -s 1024x600
~/.local/share/helixscreen/bin/helix-screen -s large
```

Size names are `micro`, `tiny`, `small`, `medium`, `large` and `xlarge`. The layout adapts to the size you choose, the same way it does on a printer's screen.

### Keyboard and mouse

Click and drag work like taps and swipes, and the mouse wheel scrolls lists. A few keys are handy on a desktop (they do nothing while you are typing in a text field):

| Key | Action |
|-----|--------|
| **S** | Save a screenshot (to `/tmp`) |
| **D** | Toggle dark and light mode |
| **M** | Show memory statistics |

Close the window to quit.

---

## Updating

When a new version is out, HelixScreen tells you in the app. **Install Update** opens the release page in your web browser. To install it, close HelixScreen and run:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --update
```

The installer finds your desktop install by itself and keeps your settings.

---

## Uninstalling

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall
```

This removes `~/.local/share/helixscreen` and the applications-menu entry, and nothing else.

---

## Running Without Installing

You can also download the desktop package and run it from any folder. Grab `helixscreen-x86-desktop-<version>.tar.gz` from the [releases page](https://github.com/prestonbrown/helixscreen/releases/latest), then:

```bash
tar -xzf helixscreen-x86-desktop-*.tar.gz
./helixscreen/bin/helix-screen --moonraker ws://192.168.1.50:7125
```

Settings are stored in `helixscreen/config/` next to the app, so keep the folder together.

---

## Troubleshooting

**The window does not open, and the terminal shows `GLIBC_2.xx not found`.** Your Linux release is older than the requirement above. Upgrade the OS, or run HelixScreen on a Raspberry Pi instead.

**The G-code preview is flat (2D) instead of 3D.** Your graphics driver does not offer OpenGL ES to HelixScreen. Updating your graphics drivers (Mesa on most systems) usually brings it back. Everything else works the same in 2D.

**It cannot reach the printer.** From the same PC, open `http://PRINTER_IP:7125/server/info` in a browser. If that fails too, the problem is the network or Moonraker, not HelixScreen.

For anything else, see [Troubleshooting](../TROUBLESHOOTING.md) or ask on [Discord](https://discord.gg/RZCT2StKhr).

---

**Next:** [Getting Started](getting-started.md) | [Back to User Guide](../USER_GUIDE.md)
