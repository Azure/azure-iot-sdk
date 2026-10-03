// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    /// <summary>
    /// Implements Binary Exponential Backoff (BEB) retry policy as follows:
    /// CurrentExponent = min(MaxExponent, baseExponent + currentRetryCount)
    /// RetryDelay = min(pow(2, CurrentExponent), maxWait) milliseconds
    /// </summary>
    public class ExponentialBackoffRetryPolicy : IRetryPolicy
    {
        // Default base exponent set equal 6, in this case first retry starts at 2^(6+1)=128 milliseconds, and exceed 1 second delay on retry #4.
        private readonly uint _baseExponent = 6u;

        // Avoid integer overflow (max of 32) and clamp max delay.
        private const uint MaxExponent = 32u;

        /// <summary>
        /// The maximum number of retries
        /// </summary>
        private readonly uint _maxRetries;

        private readonly TimeSpan _maxDelay;
        private readonly bool _useJitter;

        /// <summary>
        /// The number of consecutive failed IoT hub connection attempts, during an automatic reconnection, after which
        /// this policy tells a device provisioned through Device Provisioning Service to re-provision rather than
        /// continuing to retry an unreachable hub forever. Zero disables the fallback.
        /// </summary>
        private readonly uint _maxHubConnectAttemptsBeforeReprovision;

        /// <summary>
        /// The default number of consecutive failed IoT hub connection attempts after which this policy advises
        /// re-provisioning.
        /// </summary>
        public const uint DefaultMaxHubConnectAttemptsBeforeReprovision = 50u;

        public ExponentialBackoffRetryPolicy()
        {
            //TODO magic number defaults
            _maxRetries = uint.MaxValue;
            _maxDelay = TimeSpan.FromMinutes(30);
            _useJitter = true;
            _maxHubConnectAttemptsBeforeReprovision = DefaultMaxHubConnectAttemptsBeforeReprovision;
        }

        /// <summary>
        /// Creates an instance of this class with a default base exponent equals 6.
        /// </summary>
        /// <param name="maxRetries">The maximum number of retry attempts.</param>
        /// <param name="maxWait">The maximum amount of time to wait between retries.</param>
        /// <param name="useJitter">Whether to add a small, random adjustment to the retry delay to avoid synchronicity in clients retrying.</param>
        /// <param name="maxHubConnectAttemptsBeforeReprovision">
        /// The number of consecutive failed IoT hub connection attempts, during an automatic reconnection, after which
        /// a device provisioned through Device Provisioning Service re-provisions rather than continuing to retry an
        /// unreachable hub forever. A hub that was vacated service-side may stop answering rather than rejecting the
        /// device's identity, in which case nothing else would ever send the device back to DPS. This only applies to
        /// a device that was provisioned through DPS, since there is otherwise no registration to renew. Set to 0 to
        /// disable this fallback and retry the hub indefinitely. Defaults to 50.
        /// </param>
        public ExponentialBackoffRetryPolicy(uint maxRetries, TimeSpan maxWait, bool useJitter = true, uint maxHubConnectAttemptsBeforeReprovision = DefaultMaxHubConnectAttemptsBeforeReprovision)
        {
            _maxRetries = maxRetries;
            _maxDelay = maxWait;
            _useJitter = useJitter;
            _maxHubConnectAttemptsBeforeReprovision = maxHubConnectAttemptsBeforeReprovision;
        }

        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <param name="maxRetries">The maximum number of retry attempts.</param>
        /// <param name="baseExponent">The base exponent to start the backoff calculation (CurrentExponent(currentRetryCount) = baseExponent + currentRetryCount).</param>
        /// <param name="maxWait">The maximum amount of time to wait between retries.</param>
        /// <param name="useJitter">Whether to add a small, random adjustment to the retry delay to avoid synchronicity in clients retrying.</param>
        /// <param name="maxHubConnectAttemptsBeforeReprovision">
        /// The number of consecutive failed IoT hub connection attempts, during an automatic reconnection, after which
        /// a device provisioned through Device Provisioning Service re-provisions rather than continuing to retry an
        /// unreachable hub forever. Set to 0 to disable this fallback and retry the hub indefinitely. Defaults to 50.
        /// </param>
        public ExponentialBackoffRetryPolicy(uint maxRetries, uint baseExponent, TimeSpan maxWait, bool useJitter = true, uint maxHubConnectAttemptsBeforeReprovision = DefaultMaxHubConnectAttemptsBeforeReprovision) :
        this(maxRetries, maxWait, useJitter, maxHubConnectAttemptsBeforeReprovision)
        {
            _baseExponent = baseExponent;
        }

        /// <inheritdoc/>
        public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
        {
            retryDelay = TimeSpan.Zero;

            // A hub that keeps failing to connect may have been vacated service-side rather than rejecting the device's
            // identity. After this many consecutive hub connect attempts, advise a device provisioned through DPS to
            // re-provision for a fresh assignment instead of retrying the unreachable hub forever. This only applies to
            // hub endpoints; a Device Provisioning Service endpoint keeps retrying (and the client ignores this guidance
            // for a device that holds no registration to renew).
            if (connectionEndpoint == ConnectionEndpoint.IotHub
                && _maxHubConnectAttemptsBeforeReprovision > 0
                && currentRetryCount > _maxHubConnectAttemptsBeforeReprovision)
            {
                return RetryGuidance.Reprovision;
            }

            if (_maxRetries == 0 || currentRetryCount > _maxRetries)
            {
                return RetryGuidance.AbandonRetry;
            }

            // Avoid integer overflow and clamp max delay.
            // if currentRetryCount is very high, adding MinExponent would just wrap around safely
            // and decrease the value suddenly, so exponent gets capped before addition
            // Result: The delay stays at maximum instead of suddenly dropping.
            uint exponent;
            if (currentRetryCount > uint.MaxValue - _baseExponent)
            {
                exponent = MaxExponent;
            }
            else
            {
                exponent = currentRetryCount + _baseExponent;
                exponent = Math.Min(MaxExponent, exponent);
            }

            // 2 to the power of the retry count gives us exponential back-off.
            double exponentialIntervalMs = Math.Pow(2.0, exponent);

            double clampedWaitMs = Math.Min(exponentialIntervalMs, _maxDelay.TotalMilliseconds);

            retryDelay = _useJitter
                ? UpdateWithJitter(clampedWaitMs)
                : TimeSpan.FromMilliseconds(clampedWaitMs);

            return RetryGuidance.Retry;
        }

        /// <summary>
        /// Gets jitter between 95% and 105% of the base time.
        /// </summary>
        private static TimeSpan UpdateWithJitter(double baseTimeMs)
        {
            // Don't calculate jitter if the value is very small
            if (baseTimeMs < 50)
            {
                return TimeSpan.FromMilliseconds(baseTimeMs);
            }

            double jitterMs = Random.Shared.Next(95, 106) * baseTimeMs / 100.0;

            return TimeSpan.FromMilliseconds(jitterMs);
        }
    }
}
