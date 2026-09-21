// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    public class FaultInjectionTestConstants
    {
        public const string disconnectFaultName = "fault:disconnect";
        public const string rejectConnectFaultName = "fault:rejectconnect";
        public const string disconnectFaultDelayName = "fault:delay";
        public const string faultRequestIdName = "fault:requestid";
    }
}
