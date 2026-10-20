// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    internal class MockMqttPublishReceivedEventArgs : MqttPublishReceivedEventArgs
    {
        public bool IsAcknowledged { get; private set; } = false;

        public override Task AcknowledgeAsync(CancellationToken cancellationToken)
        {
            IsAcknowledged = true;
            return Task.CompletedTask;
        }
    }
}
