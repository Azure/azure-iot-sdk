# API Design Decision Notes

## Scope

This document explores one design question only:

Can all runtime buffers be passed through API function calls, instead of being stored in the client options during initialization?

This is a design exploration. No implementation changes are proposed in this document.

## Current Model (Today)

The current API stores buffers in az_mqtt5_client_options during az_mqtt5_client_init. Those buffers are then reused by connect/process/publish/subscribe paths.

Current call pattern in the sample is effectively:

    az_mqtt5_client_options client_opts = {0};
    client_opts.transport = transport;
    client_opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
    client_opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buffer);

    // Shared by every received packet type (one packet is decoded at a time).
    client_opts.decode_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_decode_user_properties);
    client_opts.decode_codes = AZ_MQTT5_SPAN_FROM_ARRAY(s_decode_codes);

    az_mqtt5_client_init(&client, &client_opts);
    az_mqtt5_client_connect(&client, 10000);
    az_mqtt5_client_process_loop(&client, 1000);

## Is Per-Call Buffer Passing Possible?

Yes, it is technically possible.

The client operations already separate concerns clearly enough that send/receive scratch and decode scratch can be supplied per call. The largest impact is API ergonomics and call-site verbosity, not algorithmic feasibility.

## What That API Could Look Like

One practical shape is to group buffers into a reusable call-context object and pass that to each operation.

Proposed helper structs:

    typedef struct
    {
      az_span send_buffer;
      az_span receive_buffer;
    } az_mqtt5_io_buffers;

    typedef struct
    {
      az_span decode_user_properties;  // az_mqtt5_user_property[]
      az_span decode_codes;            // int32_t[]: subscription ids or reason codes
    } az_mqtt5_decode_buffers;

    typedef struct
    {
      az_mqtt5_io_buffers io;
      az_mqtt5_decode_buffers decode;
    } az_mqtt5_call_buffers;

Proposed function style:

    az_result az_mqtt5_client_connect(
        az_mqtt5_client* client,
        az_mqtt5_call_buffers const* buffers,
        int32_t timeout_ms);

    az_result az_mqtt5_client_process_loop(
        az_mqtt5_client* client,
        az_mqtt5_call_buffers const* buffers,
        int32_t timeout_ms);

    az_result az_mqtt5_client_publish(
        az_mqtt5_client* client,
        az_mqtt5_call_buffers const* buffers,
        az_mqtt5_publish_options const* options,
        uint16_t* out_packet_id);

    az_result az_mqtt5_client_subscribe(
        az_mqtt5_client* client,
        az_mqtt5_call_buffers const* buffers,
        az_mqtt5_subscription const* subscriptions,
        int32_t subscription_count,
        uint16_t* out_packet_id);

Sample usage in that model:

    az_mqtt5_call_buffers buffers = {0};
    buffers.io.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
    buffers.io.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buffer);
    buffers.decode.decode_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_decode_user_properties);
    buffers.decode.decode_codes = AZ_MQTT5_SPAN_FROM_ARRAY(s_decode_codes);

    az_mqtt5_client_connect(&client, &buffers, 10000);
    az_mqtt5_client_process_loop(&client, &buffers, 1000);

## Feasibility Notes

1. It is feasible with moderate refactoring.
2. Internal helpers currently read buffers from client->options, so internals would need to accept a call-scoped buffer source.
3. Callback decoding paths would use decode buffers supplied to that specific call.
4. Any stateful receive-buffer bookkeeping must remain in client state, or be explicitly represented in the call context.

## Tradeoffs

Pros:

1. Buffers become explicitly call-scoped, making lifetimes and ownership more obvious.
2. Easier to swap different scratch buffers between calls (for advanced usage patterns).
3. Reduces reliance on one large options struct for runtime scratch behavior.

Cons:

1. API calls become noisier and harder for first-time users.
2. Most users pass the same buffers every time, creating repetitive call-site boilerplate.
3. More chances for inconsistent buffer sets across calls.
4. Backward compatibility impact is high unless introduced as parallel APIs.

## Recommendation

If the goal is beginner friendliness, keep the current model as the default API.

If the goal is maximum explicitness or advanced control, consider adding an opt-in advanced API layer that accepts per-call buffers, while preserving the current simple API for most users.

A practical migration path would be:

1. Keep existing functions unchanged.
2. Add new suffixed functions (for example, az_mqtt5_client_process_loop_ex) that accept az_mqtt5_call_buffers.
3. Keep sample code using the simpler model unless writing an advanced memory-management example.
