// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public enum MqttProtocolVersion
    {
        V311, // These are the only MQTT versions that IoT hub + DPS may communicate over
        V500
    }
}
