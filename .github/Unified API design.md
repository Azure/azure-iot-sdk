# Context

There is a desire from leadership for these new IoT SDKs to allow users to migrate from using an in-market IoT Hub to the new Azure Event Grid-based IoT Hub without meaningful code changes on their devices (aside from adopting this SDK to begin with). However, there are a number of service-side inconsistencies between these two flavors of IoT Hub that make it difficult to design an SDK API that would behave consistently regardless of which type of IoT Hub the user's device connects with.

This document will cover what these inconsistencies are, will put for some potential solutions to handle them, and then provide a single proposal out of those solutions.

## Definitions

- "Classic Hub" - refers to the current in-market IoT Hub which supports MQTTv3.1.1 traffic
- "Azure Event Grid Hub" aka "AEG Hub" - refers to the upcoming IoT Hub which is built on Azure Event Grid and supports MQTTv5 traffic

## Inconsistencies between Classic Hub and AEG hub

The current IoT Hub and future AEG Hub will behave differently from each other in the following ways:

### High-level Features only available in classic Hub

 - Devices can subscribe and unsubscribe from twin/c2d/direct method topics at arbitrary times
 - Only a point-in-time issue, but C2D telemetry support
   - C2D telemetry will be added to AEG hub post AEG Hub GA

### High-level Features only available in AEG Hub

 - Future standalone features
   - Custom topic support
   - Device-to-device telemetry
   - MQTTv5 user property support
   - Multi-twin support
 - Specific error reporting via MQTTv5
   - MQTTv3 just severs the connection on any error, but MQTTv5 allows Hub to communicate specific errors (auth error vs 500 error, etc.)
 - If-match filtering of twin properties when using ```GetTwin``` API
   - AEG hub allows you to not retrieve desired/reported properties if their version matches the version provided in the ```GetTwin``` request
 - Fine-grain direct method request acceptance/rejection
   - AEG hub allows device to accept/reject methods before they are delivered in full. The only comparable feature in classic Hub is just subscribing/unsubscribing from all direct methods
 - Potentially allowing device to reject C2D messages

### Subtle behavior differences between Hub flavors

 - Message size limits are inconsistent
 - How throttling is handled
   - Classic hub had an odd behavior of just slowing down handling requests rather than sending a 429 type error message
   - Is AEG hub potentially just faster at handling MQTT publishes?
 - IoT Hub File upload APIs are done over HTTP for classic Hubs, but done over MQTT for AEG Hubs
   - Mostly an issue because, if a device's environment prevents access to port 443, that device may work fine with AEG hubs but then break when connected to a classic Hub
 - The service communicating over different MQTT protocols itself could cause inconsistencies
   - Depending on SDK language, MQTTv3.1.1 and MQTTv5 client libraries could behave differently (ie. there is a bug in the MQTTv5 stack but not in the MQTTv3 stack)
   - This is especially important for any user who is bringing their own MQTT library to this IoT SDK since that user wouldn't necessarily have the level of validation that we have on the MQTT clients we plan to use by default.
 - TLS versions supported
   - Is it possible that TLS 1.4 could be added to AEG hub in the future, but not classic Hub? Hypothetically, a user could require their device use TLS 1.4 for security purposes, but that could break if provisioned to a classic hub

## Potential Solutions

There are a couple of different proposals to consider here, each with their own set of pros/cons.

### 1. Ship a lowest-common-denominator unified API

We could ship an SDK that offers the lowest common denominator of functionality between the two flavors of IoT Hub.

pros:
 - This approach offers a single way to use each high-level feature (twin/direct methods/d2c/etc).
   - This means fewer samples are required to demonstrate each feature
   - This also means that a user's device code is __mostly__ agnostic to which flavor of Hub is being used in happy path scenarios
 - A single API set means there aren't redundant ways to do things

cons:
 - This approach removes support for any feature that is only available in one flavor of IoT Hub.
   - See [this section](#features-only-available-in-classic-hub) and [this section](#features-only-available-in-aeg-hub) for what all is lost here
   - AEG Hub-specific features will grow over time, but this API shape can't ingest them
 - This approach cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)
 - This approach doesn't allow users to capitilize on any design improvements made in AEG hub (for instance, if-match filtering of properties when getting twin)
 - This approach requires that the AEG-hub design won't change much after we GA this SDK. Since the AEG hub GA's after this SDK, we can't take this for granted.
 - It is very complicated to try to unify the error contracts that Classic Hub and AEG hub have since we don't have a good record of Classic Hub error codes to expect

### 2. Ship a unified API that also includes all AEG Hub-only features

See [proposal 1](#ship-a-lowest-common-denominator-unified-api), but we also tack on APIs for supporting any [standalone features that are only supported by AEG Hub](#features-only-available-in-aeg-hub). 

Note that this approach does __not__ include supporting any AEG-specific features of existing twin/direct methods APIs. For instance this means that the SDK would abstract the AEG Hub-specific probe message that is part of the direct method flow since it is only relevant to AEG hubs. We would still present twin/direct method APIs as Hub-agnostic as described in [proposal 1](#1-ship-a-lowest-common-denominator-unified-api).

pros:
 - This addresses the issue in proposal 1 where we cannot add support to __some__ AEG Hub-specific features
 - A single API set means there aren't redundant ways to do things

cons:
 - If a user tries to use an AEG Hub-specific feature against a non-AEG Hub, the SDK will have to throw a ```NotSupportedException``` which breaks the illusion that these different Hub flavors are interchangeable
   - If DPS enforces that any device using an AEG feature is provisioned to an AEG hub, this becomes a non-issue, but even that requires that users declare before provisioning what features they plan to use which feels clunky
 - This approach cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)
 - This approach still loses out on some AEG Hub-specific design features that overlap with existing twin/direct method features
   - For example, a ```GetTwin``` SDK API would not be able to allow users to do if-match filtering since classic Hub doesn't support it
   - For example, the ```DirectMethodClient``` would not be allowed to expose any details around the probe message that only AEG hub uses. Without that, users cannot do fine-grain direct method request filtering.
 - It is very complicated to try to unify the error contracts that Classic Hub and AEG hub have since we don't have a good record of Classic Hub error codes to expect

### 3. Ship unified API set and an API set that targets AEG Hub only

See [proposal 1](#ship-a-lowest-common-denominator-unified-api), but we additionally ship a parallel set of APIs within the same package that specifically reflects only the AEG Hub's API surface. 

If a user wants to access [AEG Hub-specific features](#features-only-available-in-aeg-hub), they need to use these non-unified APIs.

pros:
 - The same pros as the [the first proposal](#ship-a-lowest-common-denominator-unified-api)
 - This approach caters well to future users that may only ever have AEG Hubs as they can just ignore the unified APIs.
 - This approach simplifies the DPS story around provisioning
   - Any user of the AEG-specific API set can be assumed to need an AEG Hub at provisioning time
   - Any user of the unified API set can be assumed to be fine with either Hub flavor
 - This approach supports users who want to migrate their devices from classic hub to AEG hub without code changes if they target the unified API set.
   - After migrating, users can switch to the AEG-specific API set on the device to start using AEG Hub-specific features.

cons:
 - This is a wider API surface that offers more than one way to accomplish something which can be confusing for users
 - The unified API set cannot abstract [certain service behavior differences](#subtle-behavior-differences-between-hub-flavors)
 - It is very complicated to try to unify the error contracts that Classic Hub and AEG hub have since we don't have a good record of Classic Hub error codes to expect
   - This problem is not relevant to the AEG Hub-specific API set though

### 4. Ship a split API for each IoT Hub flavor

Instead of shipping an API set that works for both flavors of IoT Hub, we create one set of APIs that target classic Hub and one set of APIs that target AEG Hub

pros:
 - This approach caters well to users who never intend to use the Hub flavors interchangeably. Most notably, future adopters that may only ever have AEG Hubs
 - The split APIs can easily grow in step with IoT hub service API
 - This approach simplifies the DPS story around provisioning
   - Any user of the AEG Hub-specific API set can be assumed to need an AEG Hub at provisioning time
   - Any user of the classic Hub-specific API set can be assumed to need a classic Hub at provisioning time

cons:
 - What kind of user would want to switch to a new SDK to access the classic Hub API set when it contains a subset of features compared to in-market SDKs?
 - The story around users migrating to the new hub type now requires non-trivial device-side code changes as well. 
   - A user would have to setup their device to use one API set when connected to classic hub and the other API set when connected to AEG hub in anticipation of migrating their service to AEG hub
 - This approach makes no effort to hide that IoT Hub has different flavors. Are we okay with users understanding this?

## Recommended Proposal

[Proposal 3](#3-ship-unified-api-and-api-that-targets-aeg-hub-only) is a good middle ground approach that covers the classic hub -> AEG hub migration story well. Users of in-market SDKs won't lose much by switching to the set of unified APIs. 

This approach also gives us a strong position to encourage users to migrate to the AEG-specific API set to capitalize on upcoming AEG-hub features.

No other proposal offers both strong migration support and the flexibility to evolve the SDK as the AEG hub's API grows. 

# Open questions that may impact this topic

 - The story around how DPS picks which type of Hub to provision a device to is still under construction
   - Broadly, it seems like the device will declare to DPS at provisioning time what features it requires, but the details aren't set
 - AEG Hub C2D design isn't settled on the service side yet. It sounds like the only difference from classic hub may be that they add support for rejecting a C2D message, but this isn't settled yet.