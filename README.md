# ESP-01S/ESP8266 relay controller

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

## Scheduled reboots

Sign in and select **Schedules**. In **Time settings**, enter an NTP server
(default `pool.ntp.org`), a whole-number sync interval from **1 to 6 hours**, and
your time zone, then select **Save**. The local time above Save comes from the
ESP8266 and refreshes every second. The default zone is `Etc/UTC`; select your
local zone before adding schedules. Daylight saving adjustments follow that
zone's rules.

The ESP8266 has no battery-backed real-time clock (RTC), and its time can drift
significantly between NTP syncs. After power loss or a firmware restart,
schedules wait for a successful NTP sync. A reachable NTP server and working
Wi-Fi are required for synchronization. After syncing, schedules continue using
the device clock if Wi-Fi drops.

The first schedule form appears automatically. Choose an hour from **00–23**,
a minute from **00–55** in five-minute steps, and a power on delay of **5, 10,
15, 30, or 60 seconds**. Select the days of week using the checkboxes, ordered
**Sunday through Saturday**, then select **Save**. Selecting only Monday runs
the reboot once a week on Monday at the saved local time.

**Note: Selecting ALL or NONE reboots every day.**

The saved schedule shows its days and time, with
**Edit** and **Delete** buttons. Select **New schedule** to add another, up to
32 schedules. Entries may share a time when their selected days do not overlap.
Settings and schedules are retained after reboot or power loss, alongside
existing device and Wi-Fi settings.

Each schedule power cycles the relay's connected equipment: the relay
turns OFF on its selected days at the saved local time and turns ON after its
delay. The controller stays running. An active startup/manual/scheduled power on countdown takes
priority; schedules due during that countdown are skipped. Manual relay controls
retain their existing behavior and can cancel a countdown.

Missed times are skipped, including during boot, outages, and forward clock or
daylight saving changes. A schedule runs at most once per local calendar day
while the controller stays running, including when the clock moves backward.
Editing a schedule retains that day's execution guard.
Daily schedules saved by the earlier firmware remain daily after upgrading;
their day checkboxes start unchecked.

The dropdown includes 461 named zones, with POSIX rules bundled from
[posix_tz_db](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv).
Time zone rule changes require a firmware update. To refresh the bundled list,
replace `tools/zones.csv` from that source and run
`python tools/generate_timezones.py`, then rebuild.

## Build

Run `pio run` to build, or `pio run --target upload` to upload using the port
configured in `platformio.ini` (currently COM4).

The scheduler has compile-time behavioral tests, runnable without hardware:

```powershell
& "$env:USERPROFILE/.platformio/packages/toolchain-xtensa/bin/xtensa-lx106-elf-g++.exe" -std=c++14 -Iinclude -fsyntax-only test/test_reboot_schedule.cpp test/test_schedule_storage.cpp
```
