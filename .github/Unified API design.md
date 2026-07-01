# Context

There is a desire from leadership for these new IoT SDKs to allow users to migrate from using an in-martket IoT Hub to the new Azure Event Grid-based IoT Hub without meaningful code changes on their devices (aside from adopting this SDK to begin with). However, there are a number of service-side inconsistencies between these two flavors of IoT Hub that make it difficult to design an SDK API that would behave consistently regardless of which type of IoT Hub the user's device connects with.

For additional timeline context, these SDKs are currently planned to GA at Ignite. At that time, the SDK will be compatible only with in-market IoT Hub. We will add support for communicating with the new Azure Event Grid IoT Hub in time for the GA of that flavor of IoT Hub.

## Definitions

- "Classic Hub" - refers to the current in-market IoT Hub which supports MQTTv3.1.1 traffic
- "Azure Event Grid Hub" aka "AEG Hub" - refers to the upcoming IoT Hub which is built on Azure Event Grid and supports MQTTv5 traffic

# Assumptions

- The new SDK will only support MQTT (and HTTP for file upload against classic hub only)

## Potential solutions

There are a couple of different proposals to consider here, each with their own set of pros/cons.

### Ship a lowest-common-denominator unified API

We could ship an SDK that offers the lowest common denominator of functionality between the two flavors of IoT Hub.

pros:
 - This approach offers a single way to use each high-level feature (twin/direct methods/d2c/etc).
   - This means fewer samples are required to demonstrate each feature
   - This also means that a user's device code is agnostic to which flavor of Hub is being used in happy path scenarios

cons:
 - This approach removes support for any feature that is only available in one flavor of IoT Hub.
   - See [this section](#features-only-available-in-classic-hub) and [this section](#features-only-available-in-aeg-hub) for the enumeration of what all is lost here
   - New service side features will largely only be added to AEG Hub which compounds this issue
 - This approach cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)

### Ship a unified API that also includes all AEG Hub-only features

See [above](#ship-a-lowest-common-denominator-unified-api), but we also tack on APIs for supporting any features that are only supported by AEG Hub (such as custom topic support).

pros:
 - This addresses the issue where we cannot add support to AEG Hub-specific features that hurts the above approach

cons:
 - If a user tries to use an AEG Hub-specific feature against a non-AEG Hub, the SDK will have to throw a ```NotSupportedException``` which breaks the illusion that these different Hub flavors are interchangeable
   - If DPS story enforces that any device using an AEG feature is provisioned to an AEG hub, this becomes a non-issue, but even that requires that users declare before provisioning what features they plan to use
 - This approach cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)

### Ship unified API and API that targets AEG Hub only

See [the first proposal](#ship-a-lowest-common-denominator-unified-api), but we additionally ship a set of APIs within the same package that specifically reflect only the AEG Hub's API surface. 

If a user wants to access [AEG Hub-specific features](#features-only-available-in-aeg-hub), they need to use these non-unified APIs.

pros:
 - The same pros as the [the first proposal](#ship-a-lowest-common-denominator-unified-api)
 - This approach simplifies the DPS story around provisioning
   - Any user of the AEG-specific API set can be assumed to need an AEG Hub at provisioning time
   - Any user of the unified API set can be assumed to be fine with either Hub flavor

cons:
 - This is a wider API surface that offers more than one way to accomplish something which can be confusing for users
 - This requires more samples as we would need to demonstrate each feature with each set of these APIs (unified APIs + AEG-specific APIs)
 - The unified API set cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)

### Ship a split API for each IoT Hub flavor

Instead of shipping an API set that works for both flavors of IoT Hub, we create one set of APIs that target classic Hub and one set of APIs that target AEG Hub

pros:
 - This approach caters well to users who never intend to use the Hub flavors interchangeably

cons:
 - This approach makes no effort to hide that IoT Hub has different flavors. Are we okay with users understanding this?


## Service-side inconsistencies

The current IoT Hub and future AEG Hub will behave differently from each other in the following ways:

### Features only available in classic Hub

 - Devices can subscribe and unsubscribe from twin/c2d/direct method topics at arbitrary times
 - Only a point-in-time issue, but C2D telemetry support
   - C2D telemetry will be added to AEG hub post GA

### Features only available in AEG Hub

 - Future features
   - Custom topic support
   - Device-to-device telemetry
   - MQTTv5 user property support
 - Fine-grain direct method request acceptance/rejection
   - AEG hub allows device to accept/reject methods before they are delivered in full. The only comparable feature in classic Hub is just subscribing/unsubscribing from all direct methods

### Subtle behavior differences between Hub flavors

 - Message size limits are inconsistent
 - Throttling limits?
 - IoT Hub File upload APIs are done over HTTP for classic Hubs, but done over MQTT for AEG Hubs
   - Mostly an issue because, if a device's environment prevents access to port 443, that device may work fine with AEG hubs but then break for classic Hubs
 - The service communicating over different MQTT protocols itself could cause inconsistencies
   - Depending on SDK language, MQTTv3.1.1 and MQTTv5 client libraries could behave differently (ie. there is a bug in the MQTTv5 stack but not in the MQTTv3 stack)