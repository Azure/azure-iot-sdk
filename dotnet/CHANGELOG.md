# Release History

## Unreleased

- MQTTv5: births, direct method responses and twin requests are now published on `ih/srv/{deviceId}` instead of `ih/{deviceId}/srv/presence`, `ih/{deviceId}/srv/methods` and `ih/{deviceId}/srv/twin`; telemetry is unchanged. This requires an IoT hub that supports the `ih/srv` topic.
