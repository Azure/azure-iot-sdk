// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods
{
    public class DirectMethodRequestProbeReceivedEventArgs : EventArgs
    {
        public required string MethodName { get; init; }

        public required uint ResponseTimeoutSeconds { get; init; }
    }
}
