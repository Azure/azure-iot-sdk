using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    /// <summary>
    /// The request payload to send to IoT hub to notify it when a file upload is completed, whether successful or not.
    /// </summary>
    public class FileUploadCompletionNotification
    {
        /// <summary>
        /// The correlation id that maps this completion notification to the file upload.
        /// The value should equal the <see cref="FileUploadSasUriResponse.CorrelationId">correlation id </see>
        /// returned from IoT hub when first getting the SAS Uri for this file upload 
        /// </summary>
        [JsonPropertyName("correlationId")]
        public string CorrelationId { get; set; }

        /// <summary>
        /// Whether the file upload was successful or not. This field is mandatory.
        /// </summary>
        [JsonPropertyName("isSuccess")]
        public bool IsSuccess { get; set; }

        /// <summary>
        /// The status code for the file upload. This is user defined and will be presented to the service client listening
        /// for file upload notifications. This field is optional.
        /// </summary>
        [JsonPropertyName("statusCode")]
        public int StatusCode { get; set; }

        /// <summary>
        /// A brief description of the file upload status. This is user defined and will be presented to the service client listening
        /// for file upload notifications. This field is optional.
        /// </summary>
        [JsonPropertyName("statusDescription")]
        public string StatusDescription { get; set; }
    }
}
