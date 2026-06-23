<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# API A vs API B

`azure-iot-sdk` ships two layered public API surfaces. Both live in the same
package and link against the same underlying `mqtt_iface` + `protocol_profile`
pipeline. Pick whichever fits your app; you can mix them in the same process.

---

## Overview

```
  +---------------------------+        +---------------------------+
  |      Your application     |        |      Your application     |
  +---------------------------+        +---------------------------+
              |                                    |
              v                                    v
  +---------------------------+        +---------------------------+
  |       API A  (core)       |
  |  connection_client        |
  |  twin / direct_method /   |
  |  telemetry / dps clients  |
  +---------------------------+        |  do_work(timeout_ms) pump |
              |                        +---------------------------+
              v                                    |
  +-------------------------------------------------+
  |                       API A                     |
  +-------------------------------------------------+
```



## Threading model

### API A
- **Single-threaded by contract.** Every `az_iot_*` call (other than
  `_destroy` after a clean close) must come from the same thread.
- All callbacks fire from inside `az_iot_connection_client_do_work()`,
  on the thread that invoked it.
- The pump returns whenever a fixed time budget elapses or new traffic is
  delivered; it is the application's responsibility to call it often enough.



## Memory & failure model



## Surface coverage (P0)





## See also

- [docs/design.md](design.md) — overall architecture and protocol_profile dispatch.
- [docs/how_to_byo_mqtt_client.md](how_to_byo_mqtt_client.md) — write your own MQTT adapter.
- [samples/telemetry_quickstart/main.c](../samples/telemetry_quickstart/main.c) — API B end-to-end.
- [samples/twin_get_patch/main.c](../samples/twin_get_patch/main.c) — API A pump pattern.
