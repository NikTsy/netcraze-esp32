# Netcraze failover controller for ESP32

[Русский](README.ru.md)

A classic ESP32 monitors two WireGuard exits on a Netcraze router. It verifies HTTPS and router NAT per probe, moves the configured routes and DNS bindings after three consecutive measured failures, and restores the original bindings if verification fails. After USB provisioning, the ESP32 can run from a USB power adapter and accept authenticated firmware updates over the local Wi-Fi network.

## Hardware and scope

- Classic ESP32 with at least 4 MB flash, a USB data cable for first setup, USB power and 2.4 GHz Wi-Fi.
- Netcraze Racer with two working WireGuard connections, a local Telnet CLI, access to the router DNS resolver and the TCP NAT table. Other models and firmware builds need compatibility testing.
- ESP-IDF 5.5.x and Python 3. See the [Espressif setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/).
- The firmware checks YouTube (204), ChatGPT (200) and Codex (200). Both exits must support these probes. The router uses existing WireGuard profiles supplied by the operator.

## 1. Configure the router

1. Set up two working WireGuard exits and create two probe policies in the router's connection priorities. Each probe policy must permit exactly one global interface, the corresponding WireGuard exit, with no ISP fallback.
2. Register the ESP32 as a device on the local network. It must use the segment's default policy (`conform`) between rounds and reach the router's DNS and CLI under both probe policies. The controller temporarily changes only its own policy during probes. Use a DHCP reservation if you want a stable address for OTA.
3. Enable Telnet on the trusted LAN and use an account with the required CLI permissions. Telnet sends credentials in cleartext, and this build uses unencrypted NVS. Keep Telnet and ESP32 port 8266 on the trusted LAN and avoid exposing them to the internet.
4. Use `show running-config` to find the exact interface IDs, policy IDs and existing route bindings. To enable route writes, start with all managed bindings on one of the two exits. Supported formats are static IPv4 routes with `auto`, DNS object-group routes with `auto reject`, and per-domain `ip name-server <resolver> <hostname> on <interface>` rules. Tunnel-wide resolvers and all other entries stay with their current configuration.

## 2. Build and provision

```sh
idf.py set-target esp32
idf.py menuconfig
```

Under **Netcraze failover**, set the router IPv4 address and the two interface and policy IDs. For observation, leave the post-switch hostname blank and skip the site configuration file. To exercise failover:

```sh
cp main/site_config.example.h main/site_config.h
```

Replace the documentation-only entries with your router's actual bindings. Assign a hostname, URL path and expected HTTPS status in **Netcraze failover**. Choose a public host covered by your managed routes that reliably returns that status. The ESP32 verifies this post-switch request through the new exit under its normal client policy.

If you want Wi-Fi updates, generate a per-device key before the first USB flash:

```sh
python3 provision-ota-key.py
idf.py -p <serial-port> flash monitor
```

For a USB-only setup, run the second command by itself. Enter the Wi-Fi SSID and password, then the router username and password, on separate lines when prompted. The ESP32 saves them after checking connectivity and login. Find the ESP32's IP in the router client list or the serial log. Leave the board in observation mode until you see a full successful round and restoration of its own policy.

The key and generated header are private. The first-install binary embeds that key. After successful provisioning, remove `main/ota_bootstrap.h` and run `idf.py fullclean` before building ordinary updates. Keep the local `ota.key` for uploading, and keep local configurations and generated images out of commits.

## 3. Exercise and enable failover

Send `test-failover` with a newline over USB serial at 115200 baud. Both exits must be healthy. The board moves the managed bindings once, checks the new route tables and the configured HTTPS/NAT post-check, then restores the original bindings. Test during a window when a brief change to managed traffic is acceptable.

After a successful exercise in the same boot, send `arm-failover`. The armed state persists across reboots. Send `disarm-failover` to clear it. Keep a single writer for managed bindings. Disarm before changing the site configuration and verify the router has no pending recovery before updating the firmware.

The normal interval is 60 seconds. An automatic move requires a restored probe policy, three consecutive measured failures on the active exit, current HTTPS/NAT success on the reserve, and working router UDP DNS. The controller saves the new bindings only after readback and the post-switch check. The NVS journal restores the full managed set after an error or interrupted write. Each direction uses the same checks. The active exit stays in use until another eligible failure or an operator change.

## 4. Run from wall power and update over Wi-Fi

```sh
python3 update-wifi.py --host <esp32-ip>
idf.py -D PROJECT_VER=my-version build
python3 update-wifi.py --host <esp32-ip> build/netcraze_esp32_observer.bin
```

The updater signs a single-use challenge, image hash and image length with the local key. It checks the new image identity after boot. The inactive OTA slot becomes valid after a successful probe round through at least one exit. A three-minute validation deadline triggers rollback. Use USB for bootloader or partition changes.

The status response shows the last round since boot. A mask of `7` means all three probes passed for that exit and check type. `probe_policy_restored=1` confirms restoration of the board's default policy for that round.

## Validation

```sh
cc -std=c11 -Wall -Wextra -Werror -Imain tests/observation.c -o /tmp/netcraze-observation-test
/tmp/netcraze-observation-test
```

The original controller was exercised on a classic ESP32 and Netcraze Racer, including a live route and split-DNS move, rollback, wall power and Wi-Fi OTA. The public build was compiled with ESP-IDF 5.5.4 and passed the host decision test. Its parameterized configuration still needs an independent live run.
