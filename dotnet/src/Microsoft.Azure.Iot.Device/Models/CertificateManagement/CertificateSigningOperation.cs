// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.CertificateManagement
{
    /// <summary>
    /// Represents a two-phase certificate signing operation.
    /// Phase 1 (Accepted): IoT Hub acknowledges the CSR with a 202 response.
    /// Phase 2 (Completed): IoT Hub delivers the issued certificate with a 200 response.
    /// </summary>
    /// <remarks>
    /// If the operation fails at any point, both <see cref="Accepted"/> and <see cref="Completed"/> will
    /// throw a <see cref="CertificateSigningRequestFailedException"/> when awaited. The exception contains
    /// structured error details such as <see cref="CertificateSigningRequestFailedException.ErrorCode"/>,
    /// <see cref="CertificateSigningRequestFailedException.RetryAfterSeconds"/>, and the full
    /// <see cref="CertificateSigningRequestFailedException.Error"/> that IoT hub reported. A response from IoT hub that
    /// cannot be read also fails the operation this way rather than leaving it pending.
    /// <code>
    /// try
    /// {
    ///     CertificateSigningRequestAccepted accepted = await operation.Accepted;
    ///     CertificateSigningResponse completed = await operation.Completed;
    /// }
    /// catch (CertificateSigningRequestFailedException ex) when (ex.ErrorCode == 409005)
    /// {
    ///     // Conflict: another CSR operation is active. Use Replace = "*" to override.
    /// }
    /// catch (CertificateSigningRequestFailedException ex) when (ex.RetryAfterSeconds.HasValue)
    /// {
    ///     await Task.Delay(TimeSpan.FromSeconds(ex.RetryAfterSeconds.Value));
    ///     // Retry the operation.
    /// }
    /// </code>
    /// </remarks>
    public class CertificateSigningOperation
    {
        private readonly TaskCompletionSource<CertificateSigningRequestAccepted> _accepted
            = new TaskCompletionSource<CertificateSigningRequestAccepted>(TaskCreationOptions.RunContinuationsAsynchronously);

        private readonly TaskCompletionSource<CertificateSigningResponse> _completed
            = new TaskCompletionSource<CertificateSigningResponse>(TaskCreationOptions.RunContinuationsAsynchronously);

        /// <summary>
        /// A task that completes when IoT Hub accepts the certificate signing request (202 Accepted).
        /// The result contains the correlation ID and operation expiration time.
        /// </summary>
        /// <exception cref="CertificateSigningRequestFailedException">
        /// Thrown when the CSR is rejected by IoT Hub. Inspect <see cref="CertificateSigningRequestFailedException.ErrorCode"/>
        /// for the specific failure reason (e.g., 400040 for CSR decode failure, 409005 for an active conflicting operation,
        /// 429002/429003 for throttling).
        /// </exception>
        public Task<CertificateSigningRequestAccepted> Accepted => _accepted.Task;

        /// <summary>
        /// A task that completes when IoT Hub delivers the issued certificate (200 OK).
        /// The result contains the certificate chain and correlation ID.
        /// </summary>
        /// <exception cref="CertificateSigningRequestFailedException">
        /// Thrown when the certificate issuance fails after acceptance, or when IoT hub's issued certificate response
        /// could not be read. This can also be thrown if the initial request was rejected, since a failure at any phase
        /// propagates to both <see cref="Accepted"/> and <see cref="Completed"/> tasks. If the
        /// <see cref="AbstractConnectionClient.HandleCertificateSigningCompleteAsync"/> callback throws, this task fails
        /// with that exception instead.
        /// </exception>
        public Task<CertificateSigningResponse> Completed => _completed.Task;

        internal void SetAccepted(CertificateSigningRequestAccepted response) => _accepted.TrySetResult(response);

        internal void SetCompleted(CertificateSigningResponse response) => _completed.TrySetResult(response);

        internal void SetFailed(Exception ex)
        {
            _accepted.TrySetException(ex);
            _completed.TrySetException(ex);
        }

        internal void SetCanceled(CancellationToken cancellationToken)
        {
            _accepted.TrySetCanceled(cancellationToken);
            _completed.TrySetCanceled(cancellationToken);
        }
    }
}
