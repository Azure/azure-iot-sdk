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
        public bool IsAcknowledged => AcknowledgeCount > 0;

        public int AcknowledgeCount { get; private set; }

        /// <summary>The task returned by <see cref="AcknowledgeAsync"/>, so tests can hold an acknowledgment in flight.</summary>
        public Task AcknowledgeResult { get; set; } = Task.CompletedTask;

        public override Task AcknowledgeAsync(CancellationToken cancellationToken)
        {
            AcknowledgeCount++;
            return AcknowledgeResult;
        }
    }
}
