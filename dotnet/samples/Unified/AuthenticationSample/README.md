# Authentication sample

This sample demonstrates several of the most common ways to authenticate your device at boot time and during normal operation. It also demonstrates using the certificate signing features in the Device Provisioning Service and IoT Hub as part of your authentication strategy.

A device will need a strategy for authenticating when booting up (provisioning with Device Provisioning Service) and a strategy for authenticating while operating (while communicating with IoT Hub). To that end, this SDK offers the ```BootAuthenticationProvider``` and ```OperationalAuthenticationProvider``` abstract classes that define the specific responsibilities of a given concrete boot/operational authentication strategy.

Below is an outline of the boot and operational authentication strategies that this SDK supports out-of-the-box. Because the boot and operational authentication providers are separate, you are allowed to mix and match different combinations.

Note that the ```BootAuthenticationProvider``` and ```OperationalAuthenticationProvider``` classes do allow you to provide custom authentication providers as well.

## Boot authentication strategies

This strategy defines how your device will authenticate when provisioning with the Device Provisioning Service.

### X509BootAuthenticationProvider

** This is the recommended approach for boot authentication **

This approach relies on defining an enrollment in Device Provisioning Service that uses an x509 attestation method. The device itself must provide its device-specific x509 certificates when constructing this type.

## SymmetricKeyBootAuthenticationProvider

This approach relies on defining an enrollment in Device Provisioning Service that uses a symmetric key attestation method. The device must provide its device-specific symmetric key when constructing this type.

## Operational authentication strategies

This strategy defines how your device will authenticate when connecting with IoT Hub after provisioning.

### SymmetricKeyOperationalAuthenticationProvider

This approach can only be used if the boot authentication type is ```SymmetricKeyBootAuthenticationProvider```. 

This approach will use the boot symmetric key to authenticate with IoT Hub.

This approach optionally allows you to use issued client certificates + the device's local x509 private key to authenticate rather than the symmetric key if you provisioned the device with a certificate signing request.

### X509OperationalAuthenticationProvider

This approach can only be used if the boot authentication type is ```X509BootAuthenticationProvider```. 

This approach will use the boot x509 certificates to authenticate with IoT Hub.

This approach optionally allows you to use issued client certificates + the device's local x509 private key to authenticate rather than the symmetric key if you provisioned the device with a certificate signing request.

### IssuedCertificateOperationalAuthenticationProvider

This approach can be used regardless of boot authentication type as long as the provisioning includes a certificate signing request.

This approach uses the issued client certificates + the device's local x509 private key to authenticate with IoT Hub rather than the boot credentials. 