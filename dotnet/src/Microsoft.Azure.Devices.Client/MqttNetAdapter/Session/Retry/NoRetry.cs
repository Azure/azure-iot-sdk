using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.MqttNetAdapter.Session.Retry
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
