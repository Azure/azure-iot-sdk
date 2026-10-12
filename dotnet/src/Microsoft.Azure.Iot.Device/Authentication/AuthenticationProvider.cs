// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    //TODO document exactly which fields are expected to be provided here. For instance, deviceId is fill in for the user since it should always be the same
    // TODO this folds nicely into making this interface generic enough to handle HTTP connections as well for file upload/provisioning over HTTP
    public abstract class AuthenticationProvider
    {
        //TODO track signed certificates in here somehow? SecureString stuff probably
        // Kill the onCsrCompleted callback thing since now users who want to customly handle signed certificates just implement this interface instead

        public virtual MqttConnect AddAuthentication(MqttConnect mqttConnect, ConnectionEndpoint endpointKind)
        {
            mqttConnect.LocalCertificateSelectionCallback = LocalCertificateSelectionCallback;
            mqttConnect.RemoteCertificateValidationCallback = RemoteCertificateValidationCallback;
            return mqttConnect;
        }

        public abstract string GetRegistrationId();

        /// <summary>
        /// An optional callback for custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; }

        /// <summary>
        /// An optional callback for selecting which client certificate to present during the TLS handshake.
        /// When null, <see cref="ClientCertificate"/> is presented.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; }

        public AuthenticationProvider(
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }
    }
}
