// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Client.Provisioning.Models;

namespace Microsoft.Azure.Devices.Client.Provisioning
{
    /// <summary>
    /// The result of a single provisioning flow. This is the provisioning analog of
    /// <see cref="Gen2.Connection.DevicePresenceFlowCompletedArgs"/>: it is raised once the flow that runs upon
    /// connecting to Device Provisioning Service has either produced a registration result or failed.
    /// </summary>
    internal sealed class ProvisioningFlowCompletedArgs : EventArgs
    {
        /// <summary>
        /// Whether the provisioning flow ran to completion.
        /// </summary>
        public bool IsSuccess { get; }

        /// <summary>
        /// The registration result reported by Device Provisioning Service. Only set when <see cref="IsSuccess"/> is true.
        /// </summary>
        /// <remarks>
        /// A registration result is produced for every terminal registration state, so this being set does not
        /// necessarily mean the device was assigned to an IoT hub.
        /// </remarks>
        public DeviceRegistrationResult? RegistrationResult { get; }

        /// <summary>
        /// The error that ended the provisioning flow. Only set when <see cref="IsSuccess"/> is false.
        /// </summary>
        public Exception? Exception { get; }

        public ProvisioningFlowCompletedArgs(DeviceRegistrationResult registrationResult)
        {
            IsSuccess = true;
            RegistrationResult = registrationResult;
        }

        public ProvisioningFlowCompletedArgs(Exception exception)
        {
            IsSuccess = false;
            Exception = exception;
        }
    }
}
