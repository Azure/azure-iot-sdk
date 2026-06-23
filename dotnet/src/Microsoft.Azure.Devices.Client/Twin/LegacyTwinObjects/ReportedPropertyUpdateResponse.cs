using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Twin.LegacyTwinObjects
{
    public class ReportedPropertyUpdateResponse
    {
        /// <summary>
        /// The status the service responded with.
        /// </summary>
        /// <remarks>
        /// 204 indicates a successful patch twin request.
        /// </remarks>
        public int Status { get; set; }

        /// <summary>
        /// The new version of the twin after the patch.
        /// </summary>
        public ulong Version { get; set; }

        /// <summary>
        /// The error message if the request failed.
        /// </summary>
        public string? ErrorResponseMessage { get; set; }
    }
}
