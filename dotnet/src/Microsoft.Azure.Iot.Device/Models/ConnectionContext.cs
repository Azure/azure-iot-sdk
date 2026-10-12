// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.Models
{
    public class ConnectionContext
    {
        public required string DeviceId { get; init; }

        public required string IotHubHostName { get; init; }

        public required ConnectionProfile ConnectionProfile { get; init; }

        public IReadOnlyList<string>? IssuedClientCertificates { get; init; }

        public required AuthenticationProvider AuthenticationProvider { get; set; }
    }
}
