# Azure IoT SDK

[![ci-c](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-c.yml/badge.svg?branch=main)](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-c.yml)
[![ci-dotnet](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-dotnet.yml/badge.svg?branch=main)](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-dotnet.yml)

Device SDKs for Azure IoT Hub Device Provisioning Service (DPS), Azure IoT Hub and Software Update, all in one repository.

## Table of Contents

- [Documentation](#documentation)
- [The GitHub Repository](#the-github-repository)
  - [SDKs](#sdks)
  - [Structure](#structure)
  - [Main Branch](#main-branch)
- [Getting Started](#getting-started)
- [Contributing](#contributing)
  - [Helpful Links for Contributors](#helpful-links-for-contributors)
  - [Reporting Security Issues](#reporting-security-issues)
  - [License](#license)
  - [Trademarks](#trademarks)

## Documentation

For documentation specific to each SDK library, please refer to:

- .NET: [README.md](dotnet/README.md)
- C: [README.md](c/README.md)

## The GitHub Repository

To get help, post a suggestion or comment, please file a [GitHub issue](https://github.com/Azure/azure-iot-sdk/issues/new).

### SDKs

| SDK | Folder |
| --- | --- |
| C (C99) | [c/](c/) |
| .NET | [dotnet/](dotnet/) |

### Structure

- `/c` - C SDK: sources, samples, tests and docs.
- `/dotnet` - .NET SDK: sources, samples and tests.
- `/common` - Shared assets (e.g. protobuf definitions).

### Main Branch

`main` has the latest code, with new features and bug fixes. It is not a General Availability (GA) release.

## Getting Started

Pick your language and follow its README:

- [C SDK](c/README.md) - build, samples ([c/samples](c/samples/README.md)) and connecting a device ([c/docs/connecting.md](c/docs/connecting.md)).
- [.NET SDK](dotnet/README.md) - samples ([dotnet/samples](dotnet/samples/README.md)).

## Contributing

Contributions and suggestions are welcome! Most contributions require you to agree to a Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us the rights to use your contribution. For details, visit [https://cla.opensource.microsoft.com](https://cla.opensource.microsoft.com).

When you submit a pull request, a CLA bot will automatically determine whether you need to provide a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions provided by the bot. You will only need to do this once across all repositories using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

### Helpful Links for Contributors

- [Good first issues](https://github.com/Azure/azure-iot-sdk/issues?q=is%3Aopen+is%3Aissue+label%3A%22good+first+issue%22)
- [Issues where help is wanted](https://github.com/Azure/azure-iot-sdk/issues?q=is%3Aopen+is%3Aissue+label%3A%22help+wanted%22)

### Reporting Security Issues

Please do not report security vulnerabilities through public GitHub issues. See [SECURITY.md](SECURITY.md).

### License

Licensed under the [MIT](LICENSE) license.

### Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft trademarks or logos is subject to and must follow [Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/legal/intellectualproperty/trademarks/usage/general). Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship. Any use of third-party trademarks or logos are subject to those third-party's policies.
