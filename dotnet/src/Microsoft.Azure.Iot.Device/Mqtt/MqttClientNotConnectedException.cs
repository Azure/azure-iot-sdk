// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttClientNotConnectedException : Exception
    {
        public MqttClientNotConnectedException(string message, Exception e) : base(message, e)
        {

        }

        public MqttClientNotConnectedException(string message) : base(message)
        {

        }
    }
}
