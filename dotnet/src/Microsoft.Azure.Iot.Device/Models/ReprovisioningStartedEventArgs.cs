// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using System;

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// Raised, for informational purposes only, when this client starts re-provisioning the device through Device
    /// Provisioning Service on its own.
    /// </summary>
    /// <remarks>
    /// A device provisioned through Device Provisioning Service re-provisions on its own when IoT hub rejects its identity
    /// or stays unreachable for the number of attempts the retry policy allows. This event is raised once when that
    /// recovery begins, not for each registration attempt within it. The application does not need to act on it.
    /// </remarks>
    public class ReprovisioningStartedEventArgs : EventArgs
    {
        /// <summary>
        /// The connection error that made this client re-provision.
        /// </summary>
        public required DeviceException Exception { get; init; }
    }
}
