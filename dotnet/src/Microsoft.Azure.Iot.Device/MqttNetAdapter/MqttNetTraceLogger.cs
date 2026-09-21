using MQTTnet.Diagnostics.Logger;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;

namespace Microsoft.Azure.Iot.Device.MQTTnetAdapter
{
    public class MqttNetTraceLogger
    {
        [DebuggerStepThrough()]
        public static MqttNetEventLogger CreateTraceLogger()
        {
            MqttNetEventLogger logger = new();
            logger.LogMessagePublished += (s, e) =>
            {
                string trace = $">> [{e.LogMessage.Timestamp:O}] [{e.LogMessage.ThreadId}]: {e.LogMessage.Message}";
                if (e.LogMessage.Exception != null)
                {
                    trace += Environment.NewLine + e.LogMessage.Exception.ToString();
                }
                Trace.TraceInformation(trace);
            };
            return logger;
        }
    }
}
