using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class FaultInjectionTestConstants
    {
        public const string disconnectFaultName = "fault:disconnect";
        public const string rejectConnectFaultName = "fault:rejectconnect";
        public const string disconnectFaultDelayName = "fault:delay";
        public const string faultRequestIdName = "fault:requestid";
    }
}
