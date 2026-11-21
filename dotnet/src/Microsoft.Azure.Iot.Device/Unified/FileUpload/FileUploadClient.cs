// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Net.Http.Headers;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Iot.Device.Unified.FileUpload
{
    /// <summary>
    /// Retrieve and complete SAS URIs that allow you to upload files to Azure Storage accounts associated with your IoT Hub.
    /// </summary>
    /// <remarks>
    /// This client does not directly support uploading files to Azure Storage. See the file upload sample for how to use this client
    /// in conjunction with the Azure Storage SDK to do the actual file upload step.
    /// 
    /// Each request targets the IoT hub in the device's current <see cref="ConnectionContext"/>. If the device is re-provisioned to a
    /// different IoT hub after <see cref="GetFileUploadSasUriAsync(FileUploadSasUriRequest, CancellationToken)"/> but before
    /// <see cref="CompleteFileUploadSasUriAsync(FileUploadCompletionNotification, CancellationToken)"/>, the completion notification is sent
    /// to the new IoT hub, which did not issue that SAS URI and will reject it. The SAS URI is then left outstanding on the previous IoT hub
    /// and the upload is not reported there. These lingering SAS URIs are not released early, but they expire on their own once the
    /// SAS token lifetime configured for file uploads on the previous IoT hub elapses.
    /// </remarks>
    public class FileUploadClient : IDisposable
    {
        private bool _isDisposed = false;
        private readonly object _httpClientLock = new();
        private HttpClientLease? _httpClient;
        private readonly IConnectionClient _connection;

        /// <summary>
        /// Create a file upload client.
        /// </summary>
        /// <param name="connection">The connection client whose device this client uploads files for.</param>
        /// <remarks>
        /// File upload operations are done over HTTPS only when connected to an MQTTv3 IoT hub. These requests are sent with HTTP clients
        /// created by the connection client's <see cref="Unified.Models.ConnectionClientOptions.HttpClientFactory"/>, if one was supplied, or
        /// otherwise by this SDK using <see cref="Unified.Models.ConnectionClientOptions.Proxy"/>. A new HTTP client is created whenever the
        /// device's TLS credentials change or the device is re-provisioned to a different IoT hub, and this client disposes each HTTP client
        /// once it is replaced or this client is disposed.
        /// </remarks>
        public FileUploadClient(IConnectionClient connection)
        {
            _connection = connection;
        }

        private async Task<HttpResponseMessage> SendAsync(HttpRequestMessage requestMessage, ConnectionContext currentConnectionContext, CancellationToken cancellationToken)
        {
            // Fetched per request so SAS tokens are renewed before they expire and newly issued client certificates are
            // picked up. Uses the same primary or secondary credentials as the device's current connection.
            AuthenticationCredentials credentials = await currentConnectionContext.GetOperationalAuthenticationCredentialsAsync(false, cancellationToken).ConfigureAwait(false);

            // Providers that authenticate with a SAS token return it as the password; otherwise the client certificate authenticates the request.
            if (credentials.Password.Length > 0)
            {
                requestMessage.Headers.TryAddWithoutValidation("Authorization", Encoding.UTF8.GetString(credentials.Password));
            }

            HttpClientLease lease = AcquireHttpClient(currentConnectionContext, credentials);
            try
            {
                // The default completion option buffers the response content, so it stays readable after the lease is released.
                return await lease.Client.SendAsync(requestMessage, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                lock (_httpClientLock)
                {
                    lease.Release();
                }
            }
        }

        private HttpClientLease AcquireHttpClient(ConnectionContext currentConnectionContext, AuthenticationCredentials credentials)
        {
            lock (_httpClientLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, this);

                if (_httpClient == null)
                {
                    _httpClient = CreateHttpClient(currentConnectionContext, credentials);
                }
                else if (!HasSameTlsSettings(_httpClient.Credentials, credentials)
                    || !string.Equals(_httpClient.IotHubHostName, currentConnectionContext.IotHubHostName, StringComparison.OrdinalIgnoreCase))
                {
                    // The client certificate and TLS callbacks are bound to the handler when the HTTP client is created, so apply changed ones
                    // (for example, a certificate issued by certificate management) through a new HTTP client. Likewise, a device re-provisioned
                    // to a different IoT hub gets a new HTTP client so that connections to the previous IoT hub are not kept open.
                    // The old one is disposed once its in-flight requests complete since disposing an HTTP client cancels its pending requests.
                    _httpClient.Release();
                    _httpClient = CreateHttpClient(currentConnectionContext, credentials);
                }

                _httpClient.AddReference();
                return _httpClient;
            }
        }

        private HttpClientLease CreateHttpClient(ConnectionContext currentConnectionContext, AuthenticationCredentials credentials)
        {
            // Requests use absolute URIs, so the new HTTP client is used as the connection client created it.
            return new HttpClientLease(_connection.CreateHttpClient(credentials), credentials, currentConnectionContext.IotHubHostName);
        }

        private static bool HasSameTlsSettings(AuthenticationCredentials first, AuthenticationCredentials second)
        {
            return IsSameCertificate(first.ClientCertificate, second.ClientCertificate)
                && Equals(first.RemoteCertificateValidationCallback, second.RemoteCertificateValidationCallback)
                && Equals(first.LocalCertificateSelectionCallback, second.LocalCertificateSelectionCallback);
        }

        private static bool IsSameCertificate(X509Certificate2? first, X509Certificate2? second)
        {
            if (ReferenceEquals(first, second))
            {
                return true;
            }

            if (first == null || second == null)
            {
                return false;
            }

            return first.RawDataMemory.Span.SequenceEqual(second.RawDataMemory.Span);
        }

        /// <summary>
        /// Reference counts an HTTP client so that a replaced client is only disposed after its in-flight requests complete.
        /// Callers must hold <see cref="_httpClientLock"/>.
        /// </summary>
        private sealed class HttpClientLease
        {
            // Starts with the reference held by the file upload client while this is its current HTTP client.
            private int _references = 1;

            public HttpClientLease(HttpClient client, AuthenticationCredentials credentials, string iotHubHostName)
            {
                Client = client;
                Credentials = credentials;
                IotHubHostName = iotHubHostName;
            }

            public HttpClient Client { get; }

            /// <summary>
            /// The credentials whose TLS settings the HTTP client was created with.
            /// </summary>
            public AuthenticationCredentials Credentials { get; }

            /// <summary>
            /// The IoT hub that the device was assigned to when the HTTP client was created.
            /// </summary>
            public string IotHubHostName { get; }

            public void AddReference() => _references++;

            public void Release()
            {
                _references--;
                if (_references == 0)
                {
                    Client.Dispose();
                }
            }
        }

        private ConnectionContext GetRequiredConnectionContext()
        {
            return _connection.GetCurrentConnectionContext()
                ?? throw new NotSupportedException("Must connect device prior to using this method");
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

            ConnectionContext currentConnectionContext = GetRequiredConnectionContext();

            string requestUri = $"https://{currentConnectionContext.IotHubHostName}/devices/{Uri.EscapeDataString(currentConnectionContext.DeviceId)}/files?api-version={Unified.Connection.ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await SendAsync(requestMessage, currentConnectionContext, cancellationToken);

            if (httpResponse.StatusCode == System.Net.HttpStatusCode.OK)
            {
                return JsonSerializer.Deserialize<FileUploadSasUriResponse>(await httpResponse.Content.ReadAsStringAsync(cancellationToken))!;
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
        /// <remarks>
        /// If the device was re-provisioned to a different IoT hub since the SAS URI was retrieved, this notification is sent to the new IoT hub
        /// and fails, leaving the SAS URI outstanding on the previous IoT hub until it expires. See the remarks on <see cref="FileUploadClient"/>.
        /// </remarks>
        public async Task CompleteFileUploadSasUriAsync(FileUploadCompletionNotification completion, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            ConnectionContext currentConnectionContext = GetRequiredConnectionContext();

            string requestUri = $"https://{currentConnectionContext.IotHubHostName}/devices/{Uri.EscapeDataString(currentConnectionContext.DeviceId)}/files/notifications?api-version={ConnectionClient.ClassicHubApiVersion}";

            HttpRequestMessage requestMessage = new(HttpMethod.Post, requestUri)
            {
                Content = new StringContent(JsonSerializer.Serialize(completion), Encoding.UTF8, "application/json")
            };
            requestMessage.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            var httpResponse = await SendAsync(requestMessage, currentConnectionContext, cancellationToken);
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
                if (errorPayload?.ErrorDetails != null)
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
            DisposeHttpClient();

            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            GC.SuppressFinalize(this);
            DisposeHttpClient();
            _connection.Dispose();
        }

        private void DisposeHttpClient()
        {
            lock (_httpClientLock)
            {
                // Disposed once any in-flight requests complete.
                if (_httpClient != null && !_isDisposed)
                {
                    _httpClient.Release();
                }

                _isDisposed = true;
            }
        }
    }
}
