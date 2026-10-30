// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.MQTTv5.Models
{
    /// <summary>
    /// Configures the MQTTv5-only direct method client.
    /// </summary>
    public class DirectMethodClientOptions
    {
        /// <summary>
        /// The default value of <see cref="MaxConcurrentDirectMethods"/>. It matches IoT hub's per-device limit on in-flight
        /// direct method invocations, so by default this device never turns away an invocation that the service would send it.
        /// </summary>
        public const int DefaultMaxConcurrentDirectMethods = 16;

        /// <summary>
        /// The most direct method invocations this device holds at once, across all method names. Must be at least 1.
        /// Defaults to <see cref="DefaultMaxConcurrentDirectMethods"/>.
        /// </summary>
        /// <remarks>
        /// <para>
        /// An invocation counts against this limit from the moment this client accepts its probe until it is fully cleaned up:
        /// while it waits for the service's exec message, and while the direct method handler runs. It stops counting when the
        /// exec's ready wait expires, or when the direct method handler returns. A handler that ignores its cancellation token keeps
        /// its invocation counted until it returns, even after the response timeout has passed.
        /// </para>
        /// <para>
        /// A probe that arrives while this many invocations are held is rejected with <see cref="Device.Models.DirectMethods.RejectedReason.DeviceBusy"/>
        /// after the probe handler accepts it, so the caller gets an immediate rejection instead of a timeout.
        /// </para>
        /// <para>
        /// This value is read once when the direct method client is constructed.
        /// </para>
        /// </remarks>
        public int MaxConcurrentDirectMethods { get; set; } = DefaultMaxConcurrentDirectMethods;
    }
}
