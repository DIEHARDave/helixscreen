# Settings: Connection

**Settings > Connection** is about how HelixScreen reaches your network and your printers. Come here to join a Wi-Fi network, check your Ethernet connection, add or switch printers, point HelixScreen at a different Moonraker address, or share the screen with Mainsail and Fluidd.

On the Settings screen, the **Connection** row shows how the screen is connected right now: the Wi-Fi network name (for example *Wi-Fi HomeNet*), *Ethernet*, or *Not connected*.

![Connection settings](../../../images/user/settings-connection.png)

---

## Network Settings

> Hidden on Android, where the phone or tablet manages its own network.

Opens the network screen. It has two columns.

![Network Settings](../../../images/user/settings-network.png)

**Left: your connection**

- **WiFi**: turn Wi-Fi on or off and see the network name, IP address, MAC address and signal strength. A **2.4GHz** tag appears if your hardware only supports that band.
- **Country**, under the Wi-Fi switch, sets the Wi-Fi country. Until one is set, the Wi-Fi radio stays in a restricted worldwide mode and usually can't find 5 GHz networks until it has joined a 2.4 GHz one; the Country box then reads **2.4 GHz only** when your hardware supports 5 GHz. If your printer's system already set a country, it is shown here and left alone. Choose **Not set** to go back to the system's own setting. Hidden on printers where the printer's own network service manages Wi-Fi.
- **Ethernet**: the IP and MAC address of the wired connection. Read-only.
- **Test Network**: checks that the internet is reachable. Greyed out when nothing is connected.

**Right: available networks**

- The Wi-Fi networks in range, with their signal strength.
- Tap a network to join it. Enter the password if it asks.
- **Add Hidden Network** joins a network that doesn't broadcast its name.
- **Refresh** scans again.

Joining Wi-Fi while Ethernet is connected shows a warning first that the wired connection will drop, because some boards have one network chip and can't use both at once (#1542). If joining fails, the screen tells you why. If an administrator has blocked the Wi-Fi radio (for example with `rfkill`), HelixScreen leaves it blocked unless you set up Wi-Fi in HelixScreen on that printer (#1697).

---

## Printers

Opens **Manage Printers**, the list of every printer this screen knows about.

![Manage Printers](../../../images/user/settings-printers.png)

- **Switch printers**: tap a printer in the list. HelixScreen disconnects from the current printer, connects to the new one, confirms with a message and takes you to the Home screen.
- **Add a printer**: tap **+ Add Printer**. The setup wizard runs for the new printer, skipping the Wi-Fi and language steps you already did. You can cancel at any time and go back to your current printer.
- **Delete a printer**: tap the trash icon next to a printer you aren't using and confirm. You can't delete the last printer.
- **Show Printer Switcher**: adds a printer icon to the navigation bar, so you can switch printers from any screen.

See [Getting Started](../getting-started.md) for adding a second printer.

---

## Host

Shows the Moonraker address HelixScreen is connected to, such as `192.168.1.50:7125`.

To connect to a different address:

1. Tap **Host**.
2. Enter the printer's IP address or host name, and the port (usually 7125).
3. Tap **Test Connection**. **Save** unlocks once the test succeeds.
4. Tap **Save**. HelixScreen disconnects and reconnects at the new address.

Host names are looked up again every time HelixScreen reconnects, so if your router gives the printer a new IP address, HelixScreen finds it again without a restart.

---

## Screen Sharing

> Hidden on printers whose own firmware already shares the screen (the Snapmaker U1 with its stock screen web page).

HelixScreen can show its own screen as a webcam in Mainsail and Fluidd, and let you tap and drag on it from a browser. Both are off until you turn them on.

**Share Screen** adds a webcam called **HelixScreen Screen** to Mainsail and Fluidd. Open the webcam panel there to watch the printer's screen. Turning Share Screen off removes the webcam again.

**Allow Remote Control** lets a browser use the screen as if it were touching it: tap, drag, scroll and long-press all work. With control on, the HelixScreen Screen webcam opens the screen as a page you can click on. A small icon appears in the top-right corner of the printer's screen while someone is using it remotely, so a person standing at the printer can see that someone else is.

Who may use it depends on how your printer is set up:

- **Printers where Mainsail or Fluidd is served by nginx** (Creality K1 and K2, Raspberry Pi with MainsailOS or KIAUH, most others): the installer adds the screen to nginx, and anyone Moonraker lets in may watch and control it. If you have Moonraker logins turned on, only browsers on Moonraker's trusted list see it, because a webcam picture carries no login.
- **Printers with nothing in front of Moonraker** (FlashForge Adventurer 5M, Elegoo Centauri Carbon): anyone on your network can watch, but controlling the screen needs pairing:
  1. Turn on **Allow Remote Control**. A **Pairing Code** row appears with six digits.
  2. Open the HelixScreen Screen webcam in your browser and type the code when it asks.
  3. That browser stays paired. Each new browser needs the code once.

  **Forget Paired Browsers** signs every browser out and makes a new code. Use it if you shared the code with someone who should no longer have it.

Screen sharing costs nothing while nobody is watching. While someone watches, HelixScreen sends at most 5 pictures a second, and fewer during a print (1 or 2 a second, depending on the printer) to keep the extra work on the printer small.

---

[Back to Settings](../settings.md) | [Prev: Safety & Alerts](safety.md) | [Next: Language & Time](language-time.md)
