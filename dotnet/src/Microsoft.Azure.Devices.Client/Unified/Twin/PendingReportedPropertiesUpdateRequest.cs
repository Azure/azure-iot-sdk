using Microsoft.Azure.Devices.Client.Twin;

namespace Microsoft.Azure.Devices.Client.Unified.Twin
{
    internal class PendingReportedPropertiesUpdateRequest
    {
        /// <summary>
        /// Constructor for patch twin operations.
        /// </summary>
        public PendingReportedPropertiesUpdateRequest()
        {
            ReportedPropertyUpdateResponse = new();
        }

        /// <summary>
        /// The pending task for patching a twin to be signaled when complete.
        /// </summary>
        /// <remarks>
        /// Will be null if this if this class is not being used for patch twin.
        /// </remarks>
        public TaskCompletionSource<ReportedPatchResponse> ReportedPropertyUpdateResponse { get; }

        /// <summary>
        /// When the request was sent so we know when to time out older operations
        /// </summary>
        public DateTimeOffset RequestSentOnUtc { get; set; } = DateTimeOffset.UtcNow;
    }
}
