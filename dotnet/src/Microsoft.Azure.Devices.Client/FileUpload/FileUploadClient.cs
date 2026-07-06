using Microsoft.Azure.Devices.Client.IotHub;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private readonly HttpClient _httpClient;
        private readonly string _deviceId;

        public FileUploadClient(ConnectionClient connection)
        {
            throw new NotImplementedException("Using File upload APIs over MQTT requires AEG Hub support which does not exist yet.");   
        }

        public FileUploadClient(ConnectionContext connectionContext, HttpClient? httpClient = null)
        {
            if (connectionContext.IsAzureEventGrid)
            {
                throw new NotSupportedException("File upload APIs are done over MQTT when connected to AEG Hub");
            }

            _httpClient = httpClient;
            _deviceId = connectionContext.DeviceId;
            _httpClient.BaseAddress = new Uri(connectionContext.IotHubHostName);
        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public async Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            string requestUri = $"devices/{_deviceId}/files?api-version={IotHubConnection.ClassicHubApiVersion}";
            StringContent httpContent = new(JsonSerializer.Serialize(request));
            var httpResponse = await _httpClient.PostAsync(requestUri, httpContent, cancellationToken);
            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync());
            }
            else
            {
                throw new Exception("TODO error mapping: " + httpResponse.StatusCode);
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
            string requestUri = $"devices/{_deviceId}/files/notifications?api-version={IotHubConnection.ClassicHubApiVersion}";
            StringContent httpContent = new(JsonSerializer.Serialize(completion));
            var httpResponse = await _httpClient.PostAsync(requestUri, httpContent, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                throw new Exception("TODO error mapping" + httpResponse.StatusCode);
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
