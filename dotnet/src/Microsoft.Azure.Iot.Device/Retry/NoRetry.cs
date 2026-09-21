// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    public class NoRetry : IRetryPolicy
    {
        public bool ShouldRetry(uint currentRetryCount, Exception? lastException, out TimeSpan retryDelay)
        {
            retryDelay = TimeSpan.Zero;

            // never retry
            return false;
        }
    }
}
