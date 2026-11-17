// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Runtime.InteropServices;
using Tpm2Lib;

namespace TpmAuthenticationSample
{
    /// <summary>
    /// An <see cref="ITpmSecurityProvider"/> built on the Microsoft.TSS (Tpm2Lib) TPM software stack.
    /// </summary>
    /// <remarks>
    /// Connects to the platform TPM (through TBS on Windows or /dev/tpm0 on Linux) or to a TPM simulator over TCP. The
    /// endorsement key and storage root key are created at their well-known persistent handles if the TPM does not have
    /// them yet, and the key Device Provisioning Service sends is persisted in the TPM, so a device that restarts can
    /// authenticate with IoT Hub without provisioning again.
    /// <para>
    /// Calls to the TPM can throw <see cref="TssException"/> or <see cref="TpmException"/> if the TPM does not support
    /// a command.
    /// </para>
    /// </remarks>
    internal sealed class TssTpmSecurityProvider : ITpmSecurityProvider, IDisposable
    {
        /// <summary>The address the TPM simulator listens on by default.</summary>
        public const string DefaultSimulatorAddress = "127.0.0.1";

        /// <summary>The port the TPM simulator listens for TPM commands on by default.</summary>
        public const int DefaultSimulatorPort = 2321;

        private const uint StorageRootKeyHandle = ((uint)Ht.Persistent << 24) | 0x00000001;
        private const uint EndorsementKeyHandle = ((uint)Ht.Persistent << 24) | 0x00010001;
        private const uint PersistedKeyHandle = ((uint)Ht.Persistent << 24) | 0x00000100;

        // An NV index that earlier Azure IoT SDKs stored alongside the key; it is removed when a new key is imported.
        private const uint LegacyNvIndex = ((uint)Ht.NvIndex << 24) | 0x00400100;

        private const int MaxHmacBufferLength = 1024;

        private readonly object _lock = new();
        private readonly Tpm2 _tpm2;
        private readonly TpmPublic _endorsementKey;
        private readonly TpmPublic _storageRootKey;
        private bool _disposed;

        private TssTpmSecurityProvider(Tpm2Device device, bool startUp)
        {
            _tpm2 = new Tpm2(device);
            try
            {
                if (startUp)
                {
                    // A TPM that was just powered on must be started up before it accepts other commands.
                    _tpm2.Startup(Su.Clear);
                }

                _endorsementKey = ReadOrCreatePersistedKey(new TpmHandle(EndorsementKeyHandle), TpmHandle.RhEndorsement, CreateEndorsementKeyTemplate());
                _storageRootKey = ReadOrCreatePersistedKey(new TpmHandle(StorageRootKeyHandle), TpmHandle.RhOwner, CreateStorageRootKeyTemplate());
            }
            catch
            {
                // The Tpm2 owns the device and disposes it with itself.
                _tpm2.Dispose();
                throw;
            }
        }

        /// <summary>
        /// Connects to the platform TPM: through TPM Base Services (TBS) on Windows, or /dev/tpm0 on Linux.
        /// </summary>
        public static TssTpmSecurityProvider CreateForPlatformTpm()
        {
            Tpm2Device device;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                device = new TbsDevice();
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                device = new LinuxTpmDevice();
            }
            else
            {
                throw new PlatformNotSupportedException($"Microsoft.TSS does not support a platform TPM on {RuntimeInformation.OSDescription}.");
            }

            try
            {
                device.Connect();
            }
            catch
            {
                device.Dispose();
                throw;
            }

            return new TssTpmSecurityProvider(device, startUp: false);
        }

        /// <summary>
        /// Connects to a TPM simulator, such as the TSS.MSR simulator, over TCP and powers it on.
        /// </summary>
        /// <remarks>
        /// A simulator offers no security; use it for development and testing only. It accepts one connection at a time.
        /// </remarks>
        /// <param name="address">The address the simulator listens on.</param>
        /// <param name="port">The port the simulator listens for TPM commands on. Its platform port must be the next one.</param>
        public static TssTpmSecurityProvider CreateForSimulator(string address = DefaultSimulatorAddress, int port = DefaultSimulatorPort)
        {
            var device = new TcpTpmDevice(address, port);
            try
            {
                device.Connect();
                device.SetSocketTimeout(30);
                device.PowerCycle();
            }
            catch
            {
                device.Dispose();
                throw;
            }

            return new TssTpmSecurityProvider(device, startUp: true);
        }

        /// <inheritdoc/>
        public Task<byte[]> GetEndorsementKeyAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return Task.FromResult(_endorsementKey.GetTpm2BRepresentation());
        }

        /// <inheritdoc/>
        public Task<byte[]> GetStorageRootKeyAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return Task.FromResult(_storageRootKey.GetTpm2BRepresentation());
        }

        /// <inheritdoc/>
        public Task ImportKeyAsync(byte[] encryptedKey, CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(encryptedKey);

            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);

                DeletePersistedKey();

                // Take the pieces out of the blob Device Provisioning Service sent.
                var marshaller = new Marshaller(encryptedKey, DataRepresentation.Tpm);
                Tpm2bIdObject credential = marshaller.Get<Tpm2bIdObject>();
                byte[] encryptedSecret = marshaller.GetArray<byte>(marshaller.Get<ushort>(), "encryptedSecret");
                TpmPrivate duplicationBlob = marshaller.Get<TpmPrivate>();
                byte[] encryptedWrappingKey = marshaller.GetArray<byte>(marshaller.Get<ushort>(), "encWrapKey");
                _ = marshaller.Get<ushort>(); // The size of the public area that follows.
                TpmPublic keyPublic = marshaller.Get<TpmPublic>();

                // Authorize use of the endorsement key with its policy.
                var policy = new PolicyTree(_endorsementKey.nameAlg);
                policy.SetPolicyRoot(new TpmPolicySecret(TpmHandle.RhEndorsement, false, 0, Array.Empty<byte>(), Array.Empty<byte>()));
                AuthSession endorsementKeySession = _tpm2.StartAuthSessionEx(TpmSe.Policy, _endorsementKey.nameAlg);
                endorsementKeySession.RunPolicy(_tpm2, policy);
                endorsementKeySession.Attrs &= ~SessionAttr.ContinueSession;

                // Only this TPM's endorsement key can recover the secret that the key is wrapped with.
                byte[] activationSecret = _tpm2[Array.Empty<byte>(), endorsementKeySession].ActivateCredential(
                    new TpmHandle(StorageRootKeyHandle),
                    new TpmHandle(EndorsementKeyHandle),
                    credential.credential,
                    encryptedSecret);

                TpmPrivate importedKey = _tpm2.Import(
                    new TpmHandle(StorageRootKeyHandle),
                    activationSecret,
                    keyPublic,
                    duplicationBlob,
                    encryptedWrappingKey,
                    new SymDefObject(TpmAlgId.Aes, 128, TpmAlgId.Cfb));

                // Persist the key so that it survives restarts, then unload the transient copy.
                TpmHandle loadedKey = _tpm2.Load(new TpmHandle(StorageRootKeyHandle), importedKey, keyPublic);
                try
                {
                    _tpm2.EvictControl(TpmHandle.RhOwner, loadedKey, new TpmHandle(PersistedKeyHandle));
                }
                finally
                {
                    _tpm2.FlushContext(loadedKey);
                }
            }

            return Task.CompletedTask;
        }

        /// <inheritdoc/>
        public Task<byte[]> SignAsync(byte[] data, CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(data);

            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);

                var key = new TpmHandle(PersistedKeyHandle);
                if (data.Length <= MaxHmacBufferLength)
                {
                    return Task.FromResult(_tpm2.Hmac(key, data, TpmAlgId.Sha256));
                }

                // The TPM limits how much it hashes in one command, so feed longer data to it in pieces.
                TpmHandle sequence = _tpm2.HmacStart(key, Array.Empty<byte>(), TpmAlgId.Sha256);
                int offset = 0;
                while (data.Length - offset > MaxHmacBufferLength)
                {
                    _tpm2.SequenceUpdate(sequence, data[offset..(offset + MaxHmacBufferLength)]);
                    offset += MaxHmacBufferLength;
                }

                return Task.FromResult(_tpm2.SequenceComplete(sequence, data[offset..], TpmHandle.RhNull, out _));
            }
        }

        /// <summary>
        /// Removes the key Device Provisioning Service sent from the TPM, for example when the device is decommissioned.
        /// </summary>
        public void DeletePersistedKey()
        {
            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);

                // Either may not exist, so ignore failures.
                _tpm2._AllowErrors().NvUndefineSpace(TpmHandle.RhOwner, new TpmHandle(LegacyNvIndex));
                _tpm2._AllowErrors().EvictControl(TpmHandle.RhOwner, new TpmHandle(PersistedKeyHandle), new TpmHandle(PersistedKeyHandle));
            }
        }

        public void Dispose()
        {
            lock (_lock)
            {
                if (_disposed)
                {
                    return;
                }

                _disposed = true;
                _tpm2.Dispose();
            }
        }

        private TpmPublic ReadOrCreatePersistedKey(TpmHandle persistentHandle, TpmHandle hierarchy, TpmPublic template)
        {
            TpmPublic keyPublic = _tpm2._AllowErrors().ReadPublic(persistentHandle, out _, out _);
            if (_tpm2._LastCommandSucceeded())
            {
                return keyPublic;
            }

            TpmHandle keyHandle = _tpm2.CreatePrimary(
                hierarchy,
                new SensitiveCreate(),
                template,
                Array.Empty<byte>(),
                Array.Empty<PcrSelection>(),
                out keyPublic,
                out _,
                out _,
                out _);

            try
            {
                _tpm2.EvictControl(TpmHandle.RhOwner, keyHandle, persistentHandle);
            }
            finally
            {
                _tpm2.FlushContext(keyHandle);
            }

            return keyPublic;
        }

        // The default RSA 2048 endorsement key template from the TCG EK Credential Profile.
        private static TpmPublic CreateEndorsementKeyTemplate()
        {
            return new TpmPublic(
                TpmAlgId.Sha256,
                ObjectAttr.FixedTPM | ObjectAttr.FixedParent | ObjectAttr.SensitiveDataOrigin
                    | ObjectAttr.AdminWithPolicy | ObjectAttr.Restricted | ObjectAttr.Decrypt,
                new byte[]
                {
                    0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xb3, 0xf8, 0x1a, 0x90, 0xcc, 0x8d, 0x46, 0xa5, 0xd7, 0x24,
                    0xfd, 0x52, 0xd7, 0x6e, 0x06, 0x52, 0x0b, 0x64, 0xf2, 0xa1, 0xda, 0x1b, 0x33, 0x14, 0x69, 0xaa,
                },
                new RsaParms(new SymDefObject(TpmAlgId.Aes, 128, TpmAlgId.Cfb), new NullAsymScheme(), 2048, 0),
                new Tpm2bPublicKeyRsa(new byte[2048 / 8]));
        }

        private static TpmPublic CreateStorageRootKeyTemplate()
        {
            return new TpmPublic(
                TpmAlgId.Sha256,
                ObjectAttr.FixedTPM | ObjectAttr.FixedParent | ObjectAttr.SensitiveDataOrigin
                    | ObjectAttr.UserWithAuth | ObjectAttr.NoDA | ObjectAttr.Restricted | ObjectAttr.Decrypt,
                Array.Empty<byte>(),
                new RsaParms(new SymDefObject(TpmAlgId.Aes, 128, TpmAlgId.Cfb), new NullAsymScheme(), 2048, 0),
                new Tpm2bPublicKeyRsa(new byte[2048 / 8]));
        }
    }
}
