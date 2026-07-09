using Microsoft.Azure.Devices.Client.IotHub;
using System.Net.Http.Headers;
using System.Runtime.ConstrainedExecution;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private readonly HttpClient _httpClient;
        private readonly ConnectionContext _connectionContext;

        public FileUploadClient(ConnectionClient connection)
        {
            throw new NotImplementedException("Using File upload APIs over MQTT requires AEG Hub support which does not exist yet.");   
        }

        public FileUploadClient(ConnectionContext connectionContext, X509AuthenticationProvider authenticationProvider)
        {
            if (connectionContext.IsAzureEventGrid)
            {
                throw new NotSupportedException("File upload APIs are done over MQTT when connected to AEG Hub");
            }

            var handler = new HttpClientHandler();
            handler.ClientCertificates.Add(authenticationProvider.ClientCertificate); // TODO what about when this gets rotated by cert management APIs?
            handler.ServerCertificateCustomValidationCallback = (message, cert2, chain, errors) => true;
            _httpClient = new(handler);

            _connectionContext = connectionContext;
            _httpClient.BaseAddress = new Uri("https://" + connectionContext.IotHubHostName);
        }

        public FileUploadClient(ConnectionContext connectionContext, HttpClient httpClient)
        {
            if (connectionContext.IsAzureEventGrid)
            {
                throw new NotSupportedException("File upload APIs are done over MQTT when connected to AEG Hub");
            }

            _httpClient = httpClient;

            _connectionContext = connectionContext;
            _httpClient.BaseAddress = new Uri("https://" + connectionContext.IotHubHostName);
        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public async Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            string requestUri = $"devices/{_connectionContext.DeviceId}/files?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync())!;
            }
            else
            {
                string errorContent = await httpResponse.Content.ReadAsStringAsync();
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        var exception = new IotHubServiceException($"Failed to get the file upload Sas Uri: {nestedErrorPayload.Message}.")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                        throw exception;
                    }
                }

                throw new IotHubServiceException($"Received an error message from IoT hub with an unexpected format: {errorContent}");
            }
        }

        /// <summary>
        /// Signal to IoT hub that the SAS URI retrieved with <see cref="GetFileUploadSasUri(FileUploadSasUriRequest, CancellationToken)"/> is no longer needed 
        /// and should be released.
        /// </summary>
        /// <param name="completion">The notification that includes the SAS URI from <see cref="FileUploadSasUriResponse"/>.</param>
        /// <param name="cancellationToken">the cancellation token</param>
        public async Task CompleteFileUploadSasUriAsync(FileUploadCompletionNotification completion, CancellationToken cancellationToken = default)
        {
            string requestUri = $"devices/{_connectionContext.DeviceId}/files/notifications?api-version={IotHubConnection.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                string errorContent = await httpResponse.Content.ReadAsStringAsync();
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        var exception = new IotHubServiceException($"Failed to complete the file upload Sas Uri: {nestedErrorPayload.Message}")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                        throw exception;
                    }
                }

                throw new IotHubServiceException($"Received an error message from IoT hub with an unexpected format: {errorContent}");
            }
        }

        public void Dispose()
        {
            if (_httpClient != null)
            {
                _httpClient.Dispose();
            }
        }
    }
}
