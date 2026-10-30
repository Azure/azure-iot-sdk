// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    public class TwinPushSection
    {
        public required JsonObject Properties { get; set; }

        public required ulong PropertiesVersion { get; set; }
    }
}
