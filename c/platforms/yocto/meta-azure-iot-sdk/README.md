# meta-azure-iot-sdk

Yocto layer for the Azure IoT SDK C library. Compatible with scarthgap; depends on `core` only.

| Recipe | Builds |
|---|---|
| `azure-iot-sdk` | Static libraries, headers, CMake package and pkg-config files. |
| `azure-iot-sdk-pkgconfig-test` | The `c/tests/install` tests, built with pkg-config alone against the sysroot. |

## Use

```sh
bitbake-layers add-layer <repo>/c/platforms/yocto/meta-azure-iot-sdk
bitbake azure-iot-sdk
```

Add `azure-iot-sdk` to a consumer's `DEPENDS` and link with
`pkg-config --static --cflags --libs azure-iot-sdk-<component>`, or use
`find_package(azure-iot-sdk)`. The runtime package is empty; everything is in
`azure-iot-sdk-dev` and `azure-iot-sdk-staticdev`.

`c/platforms/yocto/kas.yml` builds both recipes for `qemux86-64`: `kas build c/platforms/yocto/kas.yml`.

## PACKAGECONFIG

| Option | Default | Component |
|---|---|---|
| `paho` | on | `adapter_paho` |
| `su-crypto-openssl` | on | `su_crypto_openssl` |
| `certificate-provider-managed` | on | `certificate_provider_managed` |
| `rust-mqtt` | off | `adapter_rust_mqtt` |
| `mbedtls` | off | `su_crypto_mbedtls` (needs meta-oe's `mbedtls`, 3.6 LTS or 4.1+) |

A selected component that does not build fails `do_install`.

## Sources and revisions

- The library: `SRCREV_sdk` in `azure-iot-sdk-src.inc`. To bump it, set it to a
  commit on `main`.
- azure-sdk-for-c and Eclipse Paho MQTT C are fetched by bitbake, not by CMake,
  and passed with `FETCHCONTENT_SOURCE_DIR_*`. Their `SRCREV`s in
  `azure-iot-sdk_git.bb` must match `AZ_SDK_C_TAG` and `PAHO_C_TAG`.
- To build another source, e.g. a local checkout, set in `local.conf`:

  ```
  AZ_IOT_SDK_GIT_URI = "git:///path/to/azure-iot-sdk;protocol=file;nobranch=1"
  SRCREV_sdk = "<commit>"
  ```

  Fetching the default HTTPS URI needs read access to the repository.

## Notes

- Paho's static libraries and headers are installed with the library, so
  `azure-iot-sdk-dev` conflicts with meta-oe's `paho-mqtt-c-dev` in one sysroot.
- Licenses: MIT (this library, azure-sdk-for-c) and EPL-2.0 or EDL-1.0 (Paho).
