// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    public class NoRetry : IRetryPolicy
    {
        public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
        {
            retryDelay = TimeSpan.Zero;

            // never retry
            return RetryGuidance.AbandonRetry;
        }
    }
}
