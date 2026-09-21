// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models.Twin
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
