// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public enum MqttPublishAckReasonCode
    {
        Success = 0,

        NoMatchingSubscribers = 16,
        UnspecifiedError = 128,
        ImplementationSpecificError = 131,
        NotAuthorized = 135,
        TopicNameInvalid = 144,
        PacketIdentifierInUse = 145,
        QuotaExceeded = 151,
        PayloadFormatInvalid = 153
    }
}
