// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    public class DeviceTwin
    {
        public JsonObject? Desired { get; set; }

        public JsonObject? Reported { get; set; }

        public UInt64? DesiredVersion { get; set; }

        public UInt64? ReportedVersion { get; set; }
    }
}
