// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Retry;
using System;
using System.Collections.Generic;
using System.Net;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Unified.Models
{
    /// <summary>
    /// Configures the unified connection client using settings common to both MQTT versions.
    /// </summary>
    public class ConnectionClientOptions
    {
        /// <summary>
        /// The retry policy that the connection client will consult each time it attempts to reconnect and/or each time it attempts the initial connect.
        /// </summary>
        /// By default, this is an <see cref="ExponentialBackoffRetryPolicy"/> with jitter. To prevent any reconnection, you may set this to an instance of <see cref="NoRetry"/>.
        /// </remarks>
        public IRetryPolicy ConnectionRetryPolicy { get; set; } = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(60));

        /// <summary>
        /// How long to wait for a single connection attempt to finish before abandoning it.
        /// </summary>
        /// <remarks>
        /// This value allows for you to configure the connection attempt timeout for both initial
        /// connection and reconnection scenarios. Note that this value is ignored for the initial 
        /// connect attempt if <see cref="RetryOnFirstConnect"/> is false.
        /// </remarks>
        public TimeSpan ConnectionAttemptTimeout { get; set; } = TimeSpan.FromSeconds(10);

        /// <summary>
        /// True if you want to enable MQTT-level logs. False if you do not want these logs.
        /// </summary>
        public bool EnableMqttLogging { get; set; }

        /// <summary>
        /// The MQTT client to use. If null, a default MQTT client will be created for you.
        /// </summary>
        /// <remarks>
        /// This MQTT client will be managed for you such that this package will re-connect as necessary. If no MQTT client is provided, an instance of
        /// <see cref="MqttNetClient"/> will be created for you.
        /// 
        /// Note that, if you provide a custom MQTT client, you are also responsible for configuring the proxy + websocket settings via <see cref="MqttNetClientOptions"/> 
        /// since, by default, a custom MQTT client created without those options set will ignore any prxoy + websocket settings defined in this class.
        /// </remarks>
        public IMqttClient? MqttClient { get; set; }

        /// <summary>
        /// True to make MQTT connections over websockets on port 443 rather than over TCP on port 8883. False (the default) to use TCP,
        /// unless <see cref="Proxy"/> is set.
        /// </summary>
        /// <remarks>
        /// This configures the <see cref="MqttNetClient"/> that is created for you when <see cref="MqttClient"/> is null. A supplied
        /// <see cref="MqttClient"/> keeps its own websocket setting, such as <see cref="MqttNetAdapter.MqttNetClientOptions.UseWebsocket"/>.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool UseWebsocket { get; set; }

        /// <summary>
        /// The HTTP proxy that this client and the feature clients that use it send their network traffic through.
        /// Null (the default) to connect directly.
        /// </summary>
        /// <remarks>
        /// <para>
        /// A single device may reach the cloud over more than one protocol (for example, it may provision over HTTPS and then connect
        /// to IoT hub over MQTT), and this proxy applies to all of them:
        /// </para>
        /// <list type="bullet">
        /// <item><description>MQTT connections to Device Provisioning Service and IoT hub, which are tunnelled through the proxy over websockets.
        /// When <see cref="MqttClient"/> is null, the <see cref="MqttNetClient"/> created for you uses websockets whenever a proxy is set,
        /// regardless of <see cref="UseWebsocket"/>.</description></item>
        /// <item><description>HTTPS requests to Device Provisioning Service, such as provisioning with a <see cref="Authentication.TpmAuthenticationProvider"/>.</description></item>
        /// <item><description>HTTPS requests to IoT hub made by a <see cref="FileUpload.FileUploadClient"/> that creates its own HTTP client.</description></item>
        /// </list>
        /// <para>
        /// A supplied <see cref="MqttClient"/> keeps its own proxy and websocket settings for MQTT connections, such as
        /// <see cref="MqttNetAdapter.MqttNetClientOptions.Proxy"/>, which take precedence over this proxy. HTTPS requests still use this proxy.
        /// Likewise, an <see cref="HttpClient"/> that you supply to a <see cref="FileUpload.FileUploadClient"/> must be configured to use a proxy itself.
        /// </para>
        /// <para>
        /// When null, HTTPS requests use the system's default proxy settings (such as the HTTPS_PROXY environment variable), while the
        /// <see cref="MqttNetClient"/> created for you connects directly.
        /// </para>
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public IWebProxy? Proxy { get; set; }

        /// <summary>
        /// True (the default) if this device uses twins. False to opt out of twins.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the twin response or desired property update
        /// topics, and constructing a <see cref="Twin.TwinClient"/> with
        /// this connection client throws an <see cref="InvalidOperationException"/>.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableTwin { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives direct methods. False to opt out of direct methods.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the direct method request topic, and constructing a
        /// <see cref="DirectMethods.DirectMethodClient"/>
        /// with this connection client throws an <see cref="InvalidOperationException"/>.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableDirectMethods { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives cloud-to-device messages. False to opt out of cloud-to-device messages.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the cloud-to-device message topic, and adding a handler
        /// to <see cref="Telemetry.TelemetryClient.CloudToDeviceTelemetryReceivedAsync"/> on a telemetry client that
        /// uses this connection client throws an <see cref="InvalidOperationException"/>. Sending device-to-cloud telemetry
        /// is unaffected.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableCloudToDeviceMessages { get; set; } = true;

        /// <summary>
        /// False (the default) to ask IoT hub to resume the previous MQTT session when the connection is re-established.
        /// True to start a new session each time this client connects to IoT hub.
        /// </summary>
        /// <remarks>
        /// Resuming a session preserves IoT hub's redelivery of unacknowledged QoS 1 messages across transient disconnections.
        /// The device presence flow skips re-sending its SUBSCRIBE only when IoT hub reports that it resumed the session and
        /// this process already received a SUBACK for those subscriptions on that session, with no failed connection attempt
        /// since. Otherwise it subscribes again, so the first connection made by each process always
        /// subscribes. This only applies to IoT hub connections because Device Provisioning Service does not support
        /// persistent sessions. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool UseCleanSession { get; set; }
    }
}
