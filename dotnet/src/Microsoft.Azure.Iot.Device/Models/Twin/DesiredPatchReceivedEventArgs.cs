// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    public class DesiredPatchReceivedEventArgs : EventArgs
    {
        public required ulong DesiredPropertiesVersion { get; set; }

        public required JsonObject DesiredProperties { get; set; }
    }
}
