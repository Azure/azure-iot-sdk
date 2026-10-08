// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    /// <summary>
    /// This defines the ways that a device may handle a retry scenario.
    /// </summary>
    /// <remarks>
    /// For example, it allows for a device to abandon reconnecting to IoT Hub and revert to re-provisioning the 
    /// device if it encounters a particular exception or passes as retry count threshold.
    /// </remarks>
    public enum RetryGuidance
    {
        /// <summary>
        /// If the retry policy was consulted for a <see cref="ConnectionEndpoint.IotHub"/> endpoint retry,
        /// allow the device to continue reconnecting to IoT Hub.
        /// 
        /// If the retry policy was consulted for a <see cref="ConnectionEndpoint.DeviceProvisioningService"/> endpoint retry,
        /// allow the device to continue re-provisioning/reconnecting to DPS.
        /// </summary>
        Retry,

        /// <summary>
        /// If the retry policy was consulted for a <see cref="ConnectionEndpoint.IotHub"/> endpoint retry, then this guidance
        /// will make the device abandon reconnecting to IoT Hub and instead will resort to re-provisioning the device.
        /// 
        /// If the retry policy was consulted for a <see cref="ConnectionEndpoint.DeviceProvisioningService"/> endpoint retry,
        /// then this value is treated the same as <see cref="Retry"/>
        /// </summary>
        Reprovision,

        /// <summary>
        /// Regardless of <see cref="ConnectionEndpoint"/> type provided in the retry consultation, stop trying to 
        /// reconnect/retry. This puts the device in a terminal state.
        /// </summary>
        AbandonRetry,
    }
}
