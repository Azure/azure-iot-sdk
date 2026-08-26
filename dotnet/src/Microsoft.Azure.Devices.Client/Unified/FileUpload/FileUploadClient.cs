using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.FileUpload;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Diagnostics;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.FileUpload
{
    public class FileUploadClient : IDisposable
    {
        private bool _isDisposed = false;
        private bool _isInitialized = false;
        private bool _isUserSuppliedHttpClient;
        private HttpClient? _httpClient;
        private readonly IConnectionClient _connection;
        private Gen2.FileUpload.FileUploadClient _aegFileUploadClient;

        public FileUploadClient(IConnectionClient connection)
        {
            _connection = connection;
            _httpClient = null;
            _isUserSuppliedHttpClient = false;
            _aegFileUploadClient = new(new Stub(_connection));
        }

        /// <summary>
        /// Create a file upload client that uses a user-provided http client when applicable.
        /// </summary>
        /// <param name="connection"></param>
        /// <param name="httpClient">The user-supplied HTTP client for this client to use when applicable</param>
        /// <remarks>
        /// <para>
        /// File upload operations are done over HTTP only when connected to a Gen 1 IoT hub. File upload operations use MQTT when connected to Gen 2 IoT Hubs. Since devices using this "unified" flavor of the file upload 
        /// client may be provisioned to either a Gen 1 IoT Hub or a Gen 2 IoT Hub, the user-supplied HTTP client may not always be used.
        /// </para>
        /// <para>
        /// When providing a custom HTTP client, you must configure it to present the same client certificates that the provided <see cref="IConnectionClient"/> uses. However, this client will set the base address of the http client
        /// to target the IoT Hub endpoint correctly.
        /// </para>
        /// </remarks>
        public FileUploadClient(IConnectionClient connection, HttpClient httpClient)
        {
            _connection = connection;
            _httpClient = httpClient;
            _isUserSuppliedHttpClient = true;
            _aegFileUploadClient = new(new Stub(_connection));
        }

        private void InitializeIfUninitialized()
        {
            if (_isInitialized)
            {
                return;
            }

            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (_httpClient == null)
            {
                // If no user-supplied HTTP client, create one for them
                var handler = new HttpClientHandler();
                handler.ClientCertificates.Add(currentConnectionContext.AuthenticationProvider.ClientCertificate); // TODO what about when this gets rotated by cert management APIs?
                _httpClient = new(handler)
                {
                    BaseAddress = new Uri("https://" + currentConnectionContext.IotHubHostName)
                };
            }
            else
            {
                // If user supplied an HTTP client, just target its base address appropriately. We cannot load the client certificates into this client due to how the .NET HTTP client works, 
                // so it is the user's responsibility to set that up when creating the HTTP client that they passed in.
                _httpClient.BaseAddress = new Uri("https://" + currentConnectionContext.IotHubHostName);
            }

            _isInitialized = true;
        }

        /// <summary>
        /// Request a SAS URI that can be used to upload a file to a configured Azure Storage account.
        /// </summary>
        /// <param name="request">The request for the SAS URI.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The SAS URI.</returns>
        public async Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            InitializeIfUninitialized();

            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (currentConnectionContext.IsGen2Hub)
            {
                return await _aegFileUploadClient.GetFileUploadSasUriAsync(request, cancellationToken);
            }

            string requestUri = $"devices/{currentConnectionContext.DeviceId}/files?api-version={Unified.Connection.ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await _httpClient!.SendAsync(requestMessage, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync())!;
            }
            else
            {
                throw BuildServiceException(
                    "Failed to get the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
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
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            InitializeIfUninitialized();

            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must connect device prior to using this method");
            }

            if (currentConnectionContext.IsGen2Hub)
            {
                await _aegFileUploadClient.CompleteFileUploadSasUriAsync(completion, cancellationToken);
                return;
            }

            string requestUri = $"devices/{currentConnectionContext.DeviceId}/files/notifications?api-version={ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            Debug.Assert(_httpClient != null);
            var httpResponse = await _httpClient.SendAsync(requestMessage, cancellationToken);
            if (httpResponse.StatusCode != System.Net.HttpStatusCode.NoContent)
            {
                throw BuildServiceException(
                    "Failed to complete the file upload Sas Uri", await httpResponse.Content.ReadAsStringAsync(cancellationToken));
            }
        }

        private static IotHubServiceException BuildServiceException(string errorMessage, string errorContent)
        {
            try
            {
                var errorPayload = JsonSerializer.Deserialize<IotHubServiceError>(errorContent);
                if (errorPayload != null)
                {
                    var nestedErrorPayload = JsonSerializer.Deserialize<IotHubNestedServiceException>(errorPayload.ErrorDetails);
                    if (nestedErrorPayload != null)
                    {
                        return new IotHubServiceException($"{errorMessage}: {nestedErrorPayload.Message}.")
                        {
                            ErrorMessage = errorPayload.ExceptionMessage,
                            ErrorDetails = nestedErrorPayload,
                        };
                    }
                }
            }
            catch (JsonException)
            {
                // Not an error shape this SDK recognizes; fall through to the generic report.
            }

            return new IotHubServiceException(
                $"Received an error message from IoT hub with an unexpected format: {errorContent}");
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            // Dispose the underlying HTTP client depending on if it was user-supplied and if the disposing flag is set
            if (disposing)
            {
                _httpClient?.Dispose();
            }
            else if (!_isUserSuppliedHttpClient)
            {
                _httpClient?.Dispose();
            }

            if (disposing)
            {
                _connection.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _httpClient?.Dispose();

            _connection.Dispose();

            _isDisposed = true;
        }
    }
}
