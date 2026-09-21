// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The service endpoint that a connection client is currently connecting to, or connected to.
    /// </summary>
    /// <remarks>
    /// A single client connects to Device Provisioning Service first and to the IoT hub it was assigned to afterwards.
    /// Each endpoint has its own flow to run upon connecting, so the client tracks which endpoint the current connection
    /// targets in order to run the right one.
    /// </remarks>
    internal enum ConnectionEndpoint
    {
        /// <summary>
        /// The client is not connecting to, or connected to, any endpoint.
        /// </summary>
        None,

        /// <summary>
        /// The client is connecting to, or connected to, Device Provisioning Service. Each established connection starts
        /// the provisioning flow.
        /// </summary>
        DeviceProvisioningService,

        /// <summary>
        /// The client is connecting to, or connected to, an IoT hub. Each established connection starts the device
        /// presence flow.
        /// </summary>
        IotHub,
    }
}
