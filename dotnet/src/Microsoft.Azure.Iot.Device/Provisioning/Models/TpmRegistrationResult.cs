// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    /// <summary>
    /// TPM registration result.
    /// </summary>
    public class TpmRegistrationResult
    {
        /// <summary>
        /// The Base64-encoded key, encrypted for the device's TPM, that the device authenticates with IoT Hub with.
        /// </summary>
        [JsonPropertyName("authenticationKey")]
        public string? AuthenticationKey { get; set; }
    }
}
