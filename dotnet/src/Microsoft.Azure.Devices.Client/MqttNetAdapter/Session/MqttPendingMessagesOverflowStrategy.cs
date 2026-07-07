using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.MqttNetAdapter.Session
{
    public enum MqttPendingMessagesOverflowStrategy
    {
        DropOldestQueuedMessage,

        DropNewMessage
    }
}
