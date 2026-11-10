# Release History

## 2.0.0-preview5-alpha1 (Unreleased)

### Breaking Changes

- When connected to an IoT hub over MQTTv5, the device now publishes its births, direct method responses (probe acks,
  results and abandons) and twin requests (gets and reported property patches) on its own topic, `ih/srv/{deviceId}`,
  instead of `ih/{deviceId}/srv/presence`, `ih/{deviceId}/srv/methods` and `ih/{deviceId}/srv/twin`. The `type` user
  property, correlation data, payloads and QoS are unchanged. This requires an IoT hub that supports the per-device
  topic.

### Features Added

- `MQTTv5.Models.ServiceBoundTopicFormat` (`PerDevice`, the default, or `Legacy`) and a `ServiceBoundTopicFormat`
  property on both `MQTTv5.Models.ConnectionClientOptions` and `Unified.Models.ConnectionClientOptions`. Set it to
  `ServiceBoundTopicFormat.Legacy` to keep publishing on the previous topics, for IoT hubs that don't support the
  per-device topic.

### Other Changes

- Telemetry (`ih/{deviceId}/srv/telemetry`), the `ih/{deviceId}/dev/#` subscription and all device bound traffic are
  unchanged in either format.
