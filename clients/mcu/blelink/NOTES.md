# Notes: BleLink MCU client

## Verified

- Host tests (`ctest`, 4 targets) pass: codec and session vectors from `docs/protocol-test-vectors.json`,
  session behaviour, three sizing/CRC configurations, and the demo app against a scripted fake server.
- The same tests pass with UBSan (`-fsanitize=undefined`, no recover) on macOS/clang. The CI job also
  runs them with ASan + UBSan on Linux.
- The library compiles for Cortex-M4 with `-Wall -Wextra -Wpedantic -Werror`. Flash is 6.1-7.1 KB
  depending on the CRC implementation (README).

## Not verified (needs hardware or a toolchain this machine does not have)

- **The ports were never compiled here.** ESP-IDF and Zephyr SDKs are not installed. CI jobs
  `mcu-esp32` (espressif/idf:v5.2.2 container) and `mcu-zephyr` (Zephyr v4.1.0, nRF52840 DK) are their
  first build. Expect possible API-version fixes there.
- No run on an ESP32 or nRF52840. To check by hand against `ble_test_server`, once it speaks BleLink v1:
  - connection, MTU 247 -> packetSize 244; also force ATT_MTU 23 (packetSize 20);
  - HELLO with response, then the greeting and echo of a 1 KB and a 100 KB message;
  - flow control: NimBLE `BLE_HS_ENOMEM` retry, Zephyr `bt_gatt_write_without_response_cb` credits;
  - a full event queue (dropped notification -> seq gap -> RESUME/MSG_RESEND);
  - disconnect mid-transfer, then reconnect and a new session;
  - streaming receive with a slow flash write (raise `cfg.timeout`).
- The ESP32 port retries after `BLELINK_TX_BUSY` every 5 ms because NimBLE has no "buffer free" event
  for write without response. On hardware, check that this does not starve the host task.

## Spec issues (numbered as in the iOS and Android notes; also posted on the spec task)

- **S1** resume vector: `senderResendPacketsHex` uses seq 7, 8, 9 but should use 6, 7, 8. The receiver test
  runs the vector both as published (one extra RESUME) and with renumbered seq. The sender test expects
  6, 7, 8.
- **S2** gap while already in resync: this client follows 8.1 literally and sends RESUME again, like
  Android. iOS skips such messages.
- **S3** reset initiator: the client goes to OPENING when it sends a reset HELLO.
- **S4** duplicate memory: only status 0 is remembered as completed. 2/3/4 are remembered so that the
  trailing MSG_DATA is ignored. Status 1 is not remembered, so a CRC retry with the same msgId is accepted.
- **S5** `window = 0` in the peer HELLO is treated as 1.
- **S6** HELLO with version 0 is ignored, like iOS. Android accepts it.
- **S7** after an ACK-timeout give-up and after more than 8 restarts, the client sends CANCEL(reason 1).
- **S8** PROGRESS is sent when the **peer's** HELLO has flag bit 0 set, like Android. iOS requires the
  bit on both sides.
- **S9** a failed HELLO write-with-response is not handled. The HELLO timer re-sends it.
- **S10 (new)** HELLO re-sent after a HELLO timeout: if the server's late answer to the first HELLO and
  then its answer to the second both arrive, the second one looks like a server-initiated reset while
  READY, and answering it starts a HELLO ping-pong. The client counts its outstanding HELLOs. An extra
  answer clears the session like a reset but is not answered. The spec should say how a HELLO retry
  and its late answer are told apart.
