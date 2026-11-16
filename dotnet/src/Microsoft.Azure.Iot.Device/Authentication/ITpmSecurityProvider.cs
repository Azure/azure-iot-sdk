// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// The operations <see cref="TpmAuthenticationProvider"/> needs from a Trusted Platform Module (TPM) 2.0.
    /// </summary>
    /// <remarks>
    /// This SDK does not talk to a TPM itself, so that it takes no dependency on any particular TPM software stack.
    /// Implement this interface with the TPM library of your choice; the TPM authentication sample shows an
    /// implementation built on the Microsoft.TSS library.
    /// <para>
    /// Calls are never made concurrently on the same <see cref="TpmAuthenticationProvider"/>, but an implementation
    /// shared between several providers must synchronize access to the TPM itself.
    /// </para>
    /// </remarks>
    public interface ITpmSecurityProvider
    {
        /// <summary>
        /// Gets the public part of the TPM's endorsement key (EK), marshalled as a TPM2B_PUBLIC structure.
        /// </summary>
        /// <remarks>
        /// The Base64 encoding of this value is what an individual enrollment in Device Provisioning Service is created
        /// with. Device Provisioning Service encrypts the keys it sends to this device with it.
        /// </remarks>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task<byte[]> GetEndorsementKeyAsync(CancellationToken cancellationToken = default);

        /// <summary>
        /// Gets the public part of the TPM's storage root key (SRK), marshalled as a TPM2B_PUBLIC structure.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task<byte[]> GetStorageRootKeyAsync(CancellationToken cancellationToken = default);

        /// <summary>
        /// Imports an encrypted symmetric key sent by Device Provisioning Service into the TPM, replacing (and persisting
        /// in place of) any key imported before.
        /// </summary>
        /// <remarks>
        /// The key is the decoded "authenticationKey" Device Provisioning Service sends: a TPM2B_ID_OBJECT, the encrypted
        /// secret, the duplication blob, the encrypted wrapping key, the key's public area and the encrypted HMAC key, as
        /// consumed by the TPM2_ActivateCredential and TPM2_Import commands. The key must stay usable by
        /// <see cref="SignAsync"/> across restarts of the device, since IoT Hub authentication relies on it.
        /// </remarks>
        /// <param name="encryptedKey">The encrypted key blob.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task ImportKeyAsync(byte[] encryptedKey, CancellationToken cancellationToken = default);

        /// <summary>
        /// Computes the HMAC-SHA256 of <paramref name="data"/> with the key last imported by <see cref="ImportKeyAsync"/>.
        /// </summary>
        /// <param name="data">The data to sign.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The 32-byte HMAC.</returns>
        Task<byte[]> SignAsync(byte[] data, CancellationToken cancellationToken = default);
    }
}
