// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Unified.Connection.Retry;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    /// <summary>
    /// The optional parameters that can be specified when creating a session client.
    /// </summary>
    public class MqttSessionClientOptions
    {
        /// <summary>
        /// The retry policy that the session client will consult each time it attempts to reconnect and/or each time it attempts the initial connect.
        /// </summary>
        /// <remarks>
        /// By default, this is an <see cref="ExponentialBackoffRetryPolicy"/> that runs for around 4 minutes. Users may implement custom retry policies
        /// instead if they prefer to use a different retry algorithm.
        /// 
        /// This value cannot be null.
        /// </remarks>
        public IRetryPolicy ConnectionRetryPolicy { get; set; } = new ExponentialBackoffRetryPolicy();

        /// <summary>
        /// True if you want the session client to enable MQTT-level logs. False if you do not want these logs.
        /// </summary>
        public bool EnableMqttLogging { get; set; }

        /// <summary>
        /// How long to wait for a single connection attempt to finish before abandoning it.
        /// </summary>
        /// <remarks>
        /// This value allows for you to configure the connection attempt timeout for both initial
        /// connection and reconnection scenarios. Note that this value is ignored for the initial 
        /// connect attempt if <see cref="RetryOnFirstConnect"/> is false.
        /// </remarks>
        public TimeSpan ConnectionAttemptTimeout { get; set; } = TimeSpan.FromSeconds(10);

        internal void Validate()
        {
            ArgumentNullException.ThrowIfNull(ConnectionRetryPolicy, "Connection retry policy must not be null.");
        }
    }
}
