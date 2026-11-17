// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// Creates the handlers that send this SDK's HTTPS requests, such as to Device Provisioning Service and to IoT hub for
    /// file uploads.
    /// </summary>
    internal static class HttpMessageHandlerFactory
    {
        /// <summary>
        /// Creates a handler that applies the TLS settings of the given credentials: its client certificate and its
        /// certificate validation and selection callbacks.
        /// </summary>
        internal static HttpMessageHandler Create(AuthenticationCredentials credentials)
        {
            // SocketsHttpHandler is used rather than HttpClientHandler because only it exposes a local certificate selection callback.
            var handler = new SocketsHttpHandler();
            handler.SslOptions.RemoteCertificateValidationCallback = credentials.RemoteCertificateValidationCallback;

            X509Certificate2? clientCertificate = credentials.ClientCertificate;
            if (clientCertificate != null)
            {
                handler.SslOptions.ClientCertificates = new X509CertificateCollection { clientCertificate };
            }

            // Per the AuthenticationCredentials contract, present the client certificate when there is no selection callback rather than leaving
            // the choice to the TLS stack, which may present nothing if the certificate's issuer isn't one the server asked for.
            handler.SslOptions.LocalCertificateSelectionCallback = credentials.LocalCertificateSelectionCallback
                ?? (clientCertificate == null ? null : (_, _, _, _, _) => clientCertificate);

            return handler;
        }
    }
}
