`altp-proto`: added ALTP, `transport(altp())` on the `network()` and `syslog()` sources and destinations

ALTP (Advanced Log Transport Protocol) is an acknowledged, session-based log transport: a Sender resumes its
Session after a reconnect or a restart of either side, and the Receiver acknowledges a batch of frames only
once they are durable (e.g. stored on disk or acked by a downstream
receiver).

A `tls()` block on either side offers STARTTLS and uses it when the peer does too, while a plaintext session is
still accepted; `tls-policy(required)` refuses plaintext with `505 STARTTLS required`, and `tls-policy(none)` never
negotiates TLS.

Example source:
    source s_altp {
      network(port(35514)
              transport(altp(ack-timeout(900) session-expiration(2592000) tls-policy(required)
                             allow-compression(yes)))
              tls(key-file("/etc/syslog-ng/altp.key") cert-file("/etc/syslog-ng/altp.crt")
                  peer-verify(optional-untrusted)));
    };

Example destination:
    destination d_altp {
      network("receiver.example.com" port(35514)
              transport(altp(ack-timeout(900) max-frame-size(65536) tls-policy(required)
                             compression(yes) compression-level(6)))
              tls(ca-file("/etc/syslog-ng/ca.crt") peer-verify(yes))
              flush-lines(1000)
              disk-buffer(reliable(yes) disk-buf-size(1G)));
    };

ALTP source options:
  * ack-timeout(900)
  * ack-timeout-action(close / partial-ack)
  * session-expiration()
  * max-sessions()
  * tls-policy(none / optional / required)
  * allow-compression(yes / no)
  * compression-level(6)

ALTP destination options:
  * ack-timeout(900)
  * response-timeout(900)
  * max-frame-size(65536)
  * batch-size()
  * tls-policy(none / optional / required)
  * compression(yes)
  * compression-level(6)

Normal options for the network() and syslog() drivers also apply.

Compatibility: the option spellings of existing ALTP deployments are accepted as aliases of the canonical ones
above; only the spellings those deployments already flag as obsolete (`allow-compress()`, `flush-lines()`,
`flush-timeout()`) warn and name the replacement. On both sides `tls-required(yes)` is `tls-policy(required)`,
`tls-required(no)` is `tls-policy(none)`, `message-acknowledgement-timeout()` is `ack-timeout()` and
`compress-level()` is `compression-level()`. On a source `allow-plain-compress()` and `allow-compress()` are
`allow-compression()`, while `serialization()`, `response-timeout()`, `flush-lines()` and `flush-timeout()` are
accepted with no effect. On a destination `allow-plain-compress()` and `allow-compress()` are `compression()`,
`flush-lines()` inside `altp()` is `batch-size()`, and `flush-timeout()` is accepted with no effect.
