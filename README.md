# ESP-01S relay controller

## Wi-Fi setup

1. Build and upload the firmware using PlatformIO, then power on the board.
2. On the first boot, connect your phone to `ESP-Relay-Setup-<chip ID>`.
   The hotspot password is `RelaySetup!`.
3. Stay connected if your phone warns that the hotspot has no internet.
   Open **http://192.168.4.1** if the setup page does not appear automatically.
4. Enter your **2.4 GHz** Wi-Fi SSID and password, then select **Connect and save**.
   Enter the network name exactly, including spaces and capitalization.
   Leave the password blank only for an open network.
5. Allow up to 30 seconds. Use **Refresh connection status** to see the result.
   After success, the page shows the device's IP address. Rejoin your normal
   Wi-Fi and open that address, or **http://esp-relay.local** (unless you have
   configured another hostname).

Credentials are saved in flash only after a successful connection and reused
after power loss. The setup hotspot closes about 30 seconds after connecting.
Existing relay, hostname, startup delay, and admin password settings are preserved.

If the saved network cannot be reached for 30 seconds, the setup hotspot opens
again while the device continues trying to reconnect. Incorrect new credentials
leave the hotspot available for another attempt and do not replace saved ones.
You can also sign in and select **Config → Configure Wi-Fi** to change networks.
Changing networks may disconnect your browser; join the setup hotspot to check
the result at **http://192.168.4.1**.

The relay timer continues to operate during Wi-Fi setup and connection attempts.
The hotspot password is separate from both your home Wi-Fi password and the
web admin password. The default web login is `admin` / `ChangeMe!`; change it
through the configuration page.

## Build

Run `pio run` to build, or `pio run --target upload` to upload using the port
configured in `platformio.ini` (currently COM4).
