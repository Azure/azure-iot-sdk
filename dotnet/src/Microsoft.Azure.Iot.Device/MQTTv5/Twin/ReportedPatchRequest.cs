// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    public class ReportedPatchRequest
    {
        public required JsonObject ReportedProperties { get; set; }

        /// <summary>
        /// The version of the reported properties as the device currently understands it. If out-of-date, this patch request will be rejected.
        /// </summary>
        /// <remarks>
        /// If this value is set to 0, then the if-match field will not be included in the serialized payload and the request will be treated 
        /// as unconditional by IoT Hub.
        /// </remarks>
        public UInt64 IfMatch { get; set; }
    }
}
