using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    /// <summary>
    /// The request parameters when getting a file upload sas uri from IoT hub.
    /// </summary>
    public class FileUploadSasUriRequest
    {
        /// <summary>
        /// The name of the file for which a SAS URI will be generated. This field is mandatory.
        /// </summary>
        [JsonPropertyName("blobName")]
        public required string BlobName { get; set; }
    }
}
