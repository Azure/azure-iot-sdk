# Paho patches

Applied in name order to the fetched Eclipse Paho MQTT C v1.3.16 (commit `4a939ddb`) by
[patch_paho.cmake](../../../cmake/patch_paho.cmake). Any other Paho source is built unpatched.
Why: [Races on the command queue and client state](../../../docs/dependencies.md#races-on-the-command-queue-and-client-state).

| Patch | Upstream | Fixes |
| --- | --- | --- |
| `0001-guard-command-queue-in-freeCommands.patch` | [eclipse-paho/paho.mqtt.c#1711](https://github.com/eclipse-paho/paho.mqtt.c/pull/1711), commit `2156f71d` | [#1709](https://github.com/eclipse-paho/paho.mqtt.c/issues/1709) |
| `0002-hold-mutex-in-connect-until-queued.patch` | [eclipse-paho/paho.mqtt.c#1716](https://github.com/eclipse-paho/paho.mqtt.c/pull/1716), commit `19e24106` | [#1714](https://github.com/eclipse-paho/paho.mqtt.c/issues/1714) |

Each is the upstream commit, rebased onto v1.3.16 without conflicts. Delete them when a `PAHO_C_TAG`
contains the fixes.
