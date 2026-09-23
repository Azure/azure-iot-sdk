// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class MockMqttOutgoingTraffic
    {
        public MqttPublish? Publish;

        public MqttSubscribe? Subscribe;

        public MqttUnsubscribe? Unsubscribe;

        public MockMqttOutgoingTraffic(MqttPublish publish)
        {
            Publish = publish;
        }

        public MockMqttOutgoingTraffic(MqttSubscribe subscribe)
        {
            Subscribe = subscribe;
        }

        public MockMqttOutgoingTraffic(MqttUnsubscribe unsubscribe)
        {
            Unsubscribe = unsubscribe;
        }
    }
}
