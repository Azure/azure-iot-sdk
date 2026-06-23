using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    internal class IotHubWirePropertyNames
    {
        internal const string AbsoluteExpiryTime = "$.exp";
        internal const string ConnectionDeviceId = "$.cdid";
        internal const string ConnectionModuleId = "$.cmid";
        internal const string ContentType = "$.ct";
        internal const string ContentEncoding = "$.ce";
        internal const string ComponentName = "$.sub";
        internal const string CreationTimeUtc = "$.ctime";
        internal const string CorrelationId = "$.cid";
        internal const string InterfaceId = "$.ifid";
        internal const string MessageId = "$.mid";
        internal const string MessageSchema = "$.schema";
        internal const string MqttDiagIdKey = "$.diagid";
        internal const string MqttDiagCorrelationContextKey = "$.diagctx";
        internal const string OutputName = "$.on";
        internal const string To = "$.to";
        internal const string UserId = "$.uid";
    }
}
