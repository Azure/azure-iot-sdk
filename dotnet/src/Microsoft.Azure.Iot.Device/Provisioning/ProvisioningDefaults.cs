// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Reflection;
using System.Runtime.InteropServices;

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// Values that every registration with Device Provisioning Service uses, whichever transport carries it.
    /// </summary>
    internal static class ProvisioningDefaults
    {
        // Used for every DPS session. Required for the "connectionProfile" registration result field and supports
        // certificate signing requests.
        public const string ApiVersion = "2026-11-02-preview";

        public static readonly TimeSpan OperationPollingInterval = TimeSpan.FromSeconds(2);

        public static readonly string UserAgent = BuildUserAgent();

        private static string BuildUserAgent()
        {
            const string name = "Microsoft.Azure.Devices.Provisioning.Client";

            string version = typeof(ProvisioningDefaults).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            return $"{name}/{version} ({runtime}; {operatingSystem}; {processorArchitecture})";
        }
    }
}
