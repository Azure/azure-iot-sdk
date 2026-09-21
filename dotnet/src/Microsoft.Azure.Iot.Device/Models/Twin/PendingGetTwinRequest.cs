using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    internal class PendingGetTwinRequest
    {
        /// <summary>
        /// Constructor for get twin operations.
        /// </summary>
        public PendingGetTwinRequest()
        {
            TwinResponseTask = new();
        }

        /// <summary>
        /// The pending task for getting a twin to be signaled when complete.
        /// </summary>
        /// <remarks>
        /// Will be null if this if this class is not being used for get twin.
        /// </remarks>
        public TaskCompletionSource<DeviceTwin> TwinResponseTask { get; }

        /// <summary>
        /// When the request was sent so we know when to time out older operations
        /// </summary>
        public DateTimeOffset RequestSentOnUtc { get; set; } = DateTimeOffset.UtcNow;

        public bool GetReported { get; set; } 
        
        public bool GetDesired { get; set; }
        
        public ulong IfNotMatchReported { get; set; }

        public ulong IfNotMatchDesired { get; set; }
    }
}
