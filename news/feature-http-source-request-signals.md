`http()` source: added a request extension mechanism and Splunk HEC authentication

The `http()` source now emits a `signal_http_source_request` signal (analogous
to the signals of the `http()` destination) before it ingests a request body.
A `LogDriverPlugin` can connect a slot to inspect the request (method, path,
headers, peer) and reject it, overriding the HTTP status and body that is sent
back — the hook used to implement authentication.

Building on it, the `http-adapters` module gained a `hec-auth()` plugin that
authenticates requests like a Splunk HTTP Event Collector: clients must present
an `Authorization: Splunk <token>` (or `Bearer <token>`) header carrying one of
the configured tokens, otherwise the request is rejected with a Splunk-style
401 JSON error.

```
source s_hec {
    http(
        port(8088)
        path("/services/collector/event")
        hec-auth(token("s3cr3t") token("another-token"))
    );
};
```
