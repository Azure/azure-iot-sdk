// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public enum MqttQualityOfServiceLevel
    {
        AtMostOnce = 0x00,
        AtLeastOnce = 0x01,
        ExactlyOnce = 0x02
    }
}
