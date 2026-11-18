# Install with Codex or Claude Code

[По-русски](#по-русски)

## Start here

Clone this repository, open it in Codex or Claude Code, and send this request:

> Help me install this ESP32 controller. Follow INSTALL_WITH_AGENT.md. Start with observation mode and verify HTTPS and NAT through both WireGuard exits. Ask me for my router model and firmware version if you cannot determine them. Have me enter any passwords in the local serial console.

Have the ESP32, a USB data cable and two working WireGuard profiles on the router ready. The agent can inspect the prerequisites, prepare local configuration, run the build and monitor the board. Some steps, such as plugging in USB and entering local credentials, require the operator.

## По-русски

Скачайте репозиторий, откройте его в Codex или Claude Code и отправьте запрос:

> Помоги установить этот контроллер на ESP32. Следуй INSTALL_WITH_AGENT.md. Начни с режима наблюдения и проверь HTTPS и NAT через оба туннеля WireGuard. Если не можешь определить модель и версию прошивки роутера, спроси меня. Пароли я введу локально в USB-консоли.

Понадобятся ESP32, USB-кабель с передачей данных и два работающих профиля WireGuard на роутере. Агент может проверить окружение, подготовить локальную конфигурацию, собрать прошивку и прочитать журнал платы. Подключение кабеля и ввод паролей выполняет пользователь.

## Installation procedure for the agent

1. Read the [English README](README.md) or [Russian README](README.ru.md). Identify the host OS, router model and firmware, ESP32 chip and flash size, USB port, and installed Python and ESP-IDF versions. If the ESP-IDF environment is missing, follow the official [ESP-IDF 5.5 setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/) for that OS. Use the classic `esp32` target and check for at least 4 MB of flash.
2. Use the router UI and read-only CLI diagnostics to verify its LAN address and the exact IDs of both WireGuard interfaces and their dedicated probe policies. Each policy should permit only its matching WireGuard exit. Confirm the ESP32 uses the segment's default policy between rounds. If prerequisites are missing, report the exact gap and guide the operator through the router setup in the README. Keep existing WAN, Wi-Fi and working routes intact during observation.
3. Configure the router's LAN IPv4 and the two pairs of interface and policy IDs through `idf.py menuconfig` or local `sdkconfig`. For observation, leave the post-switch hostname blank and keep the managed binding lists empty. Keep all device-specific settings out of tracked files.
4. If the operator wants Wi-Fi updates, run `python3 provision-ota-key.py` once before the first USB flash. Keep `ota.key`, `main/ota_bootstrap.h` and the resulting bootstrap image local. Run `idf.py -p PORT flash monitor` with the discovered port. Have the operator enter Wi-Fi and router login details at the firmware prompts, without pasting credentials into chat or shell arguments.
5. Watch a full probe round and verify YouTube 204, ChatGPT 200 and Codex 200 on both exits, with router-confirmed NAT for each and restoration of the board's client policy. If a check fails, report the failing exit and whether it was HTTPS, NAT or policy restoration. Leave automatic failover disarmed while diagnosing.
6. To verify Wi-Fi updates after provisioning, remove the local bootstrap header, clean the build and build an ordinary application image as described in the README. Send it with `update-wifi.py` using the board's LAN address. Verify the reported image identity, new slot, `validating=0` and a complete healthy round after reboot.
7. If the operator also requests live route switching, configure only the agreed bindings in the ignored `main/site_config.h` and a managed HTTPS post-check. Verify the starting bindings, run `test-failover`, check HTTPS/NAT on the target and read back restoration of the original bindings. Run `arm-failover` only when a live exercise passes and the operator has requested unattended switching.
