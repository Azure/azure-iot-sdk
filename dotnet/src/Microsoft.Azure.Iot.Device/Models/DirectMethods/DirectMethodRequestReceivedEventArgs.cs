// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.DirectMethods
{
    public class DirectMethodRequestReceivedEventArgs : EventArgs
    {
        public byte[]? Payload { get; init; }

        public required string MethodName { get; init; }
    }
}
