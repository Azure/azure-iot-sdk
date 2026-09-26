This folder contains two .NET 10 console apps. One for creating a test device identity ("SetupDevice" folder), and one for connecting to IoT hub as that test device and listening for twin/direct method messages ("TestDevice" directory).

Run the setup project once before running the test app project

The TestDevice project directly references the source code for the SDK.

This samples are only present for local testing purposes and should be deleted before we make this repo public since we don't want users skipping DPS