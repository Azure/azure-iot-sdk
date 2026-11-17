// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// The Retry-After that Device Provisioning Service most recently asked for during a registration, over either
    /// transport. It floors the retry policy's backoff, so a registration that fails after the service asked to be left
    /// alone waits at least that long before registering again. Mirrors the C client's dps_pending_retry_after_secs
    /// (connection_client.c:2149-2166).
    /// </summary>
    /// <remarks>
    /// Stored as ticks so that it can be written from a response handler and read from a retry loop without tearing.
    /// </remarks>
    internal sealed class ProvisioningServiceRetryAfter
    {
        private long _ticks;

        /// <summary>
        /// The Retry-After the service most recently asked for, or <see cref="TimeSpan.Zero"/> when it asked for none.
        /// </summary>
        public TimeSpan Value
        {
            get
            {
                long ticks = Volatile.Read(ref _ticks);
                return ticks > 0 ? TimeSpan.FromTicks(ticks) : TimeSpan.Zero;
            }
        }

        /// <summary>
        /// Remember the Retry-After of a provisioning response. A response that carries none clears the stored value, so
        /// the response that ends the registration is the one that counts.
        /// </summary>
        public void Capture(TimeSpan? retryAfter)
        {
            Volatile.Write(ref _ticks, retryAfter is { } value && value > TimeSpan.Zero ? value.Ticks : 0);
        }

        /// <summary>
        /// Forget any Retry-After the service asked for, so that a fresh attempt starts with no standing service guidance.
        /// </summary>
        public void Reset()
        {
            Volatile.Write(ref _ticks, 0);
        }

        /// <summary>
        /// The longer of the retry policy's delay and the Retry-After the service asked for. The policy's maximum delay
        /// deliberately does not cap the service's value: it bounds how long the SDK waits of its own accord, not how long
        /// the service asked to be left alone.
        /// </summary>
        public TimeSpan Floor(TimeSpan retryDelay)
        {
            TimeSpan serviceRetryAfter = Value;
            return serviceRetryAfter > retryDelay ? serviceRetryAfter : retryDelay;
        }
    }
}
