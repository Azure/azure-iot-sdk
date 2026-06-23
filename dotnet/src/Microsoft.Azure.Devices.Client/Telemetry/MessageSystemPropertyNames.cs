using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    internal class MessageSystemPropertyNames
    {
        internal const string MessageId = "message-id";

        internal const string LockToken = "iothub-messagelocktoken";

        internal const string SequenceNumber = "sequence-number";

        internal const string To = "to";

        internal const string EnqueuedTime = "iothub-enqueuedtime";

        internal const string ExpiryTimeUtc = "absolute-expiry-time";

        internal const string CorrelationId = "correlation-id";

        internal const string UserId = "user-id";

        internal const string OutputName = "iothub-outputname";

        internal const string InputName = "iothub-inputname";

        internal const string MessageSchema = "iothub-message-schema";

        internal const string CreationTimeUtc = "iothub-creation-time-utc";

        internal const string CreationTimeBatchUtc = "iothub-app-iothub-creation-time-utc";

        internal const string ContentEncoding = "iothub-content-encoding";

        internal const string ContentType = "iothub-content-type";

        internal const string ConnectionDeviceId = "iothub-connection-device-id";

        internal const string ConnectionModuleId = "iothub-connection-module-id";

        internal const string DiagId = "iothub-diag-id";

        internal const string DiagCorrelationContext = "diag-correlation-context";

        internal const string InterfaceId = "iothub-interface-id";

        internal const string ComponentName = "dt-subject";
    }
}
