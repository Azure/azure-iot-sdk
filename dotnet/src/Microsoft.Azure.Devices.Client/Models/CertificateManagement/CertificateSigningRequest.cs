using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.Models.CertificateManagement
{
    /// <summary>
    /// Represents a certificate signing request to be sent to IoT Hub.
    /// </summary>
    public class CertificateSigningRequest //TODO "IotHubCertificateSigningRequest"?
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="CertificateSigningRequest"/> class.
        /// </summary>
        /// <param name="deviceId">The device ID the certificate will be issued for. Must match the device ID of the currently authenticated device.</param>
        /// <param name="csrData">The Base64-encoded PKCS#10 CSR without PEM headers/footers or newlines.</param>
        /// <param name="requestId">Optional. The request ID to associate with this certificate signing request.
        /// Must be 4 to 36 characters, ASCII alphanumerics and dashes only, and must not begin or end with a dash.
        /// This value should be unique from any ongoing certificate signing request (for example, a GUID).
        /// If null or empty, a new GUID will be generated.
        /// The use case for providing a specific value here is for re-submitting a certificate signing request,
        /// which should be done if the client loses connection at any point during the certificate signing process.</param>
        /// <param name="replace">Optional. The request ID to replace, or "*" to replace any active request.
        /// To not replace any pending certificate signing operation, this value should be null (the default).</param>
        public CertificateSigningRequest(string deviceId, string csrData, string? requestId = null, string? replace = null)
        {
            DeviceId = deviceId;
            CertificateSigningRequestData = csrData;
            RequestId = string.IsNullOrEmpty(requestId) ? Guid.NewGuid().ToString() : requestId!;
            Replace = replace;
        }

        /// <summary>
        /// The request ID associated with this certificate signing request.
        /// Must be 4 to 36 characters, ASCII alphanumerics and dashes only, and must not begin or end with a dash.
        /// Users may assign this value via <see cref="CertificateSigningRequest(string, string, string?, string?)"/>;
        /// if not provided, a random GUID is generated.
        /// The use case for providing a specific value is for re-submitting a certificate signing request,
        /// which should be done if the client loses connection at any point during the certificate signing process.
        /// May be used as a value for <see cref="Replace"/> property to replace an existing pending request.
        /// </summary>
        [JsonIgnore]
        public readonly string RequestId;

        /// <summary>
        /// Required. The device ID the certificate will be issued for.
        /// Must match the device ID of the currently authenticated device.
        /// </summary>
        [JsonPropertyName("id")]
        public string DeviceId { get; set; }

        /// <summary>
        /// Required. The Base64-encoded PKCS#10 CSR without PEM headers/footers or newlines.
        /// </summary>
        [JsonPropertyName("csr")]
        public string CertificateSigningRequestData { get; set; }

        /// <summary>
        /// Optional. Request ID to replace, or "*" to replace any active request.
        /// Use when:
        /// - The CSR is known to be different from a previous incomplete request
        /// - Client received 409005 and doesn't know if CSR has changed (e.g., storage failure)
        /// To not replace any pending certificate signing operation, this value should be null (the default).
        /// </summary>
        [JsonPropertyName("replace")]
        public string? Replace { get; set; }
    }
}
