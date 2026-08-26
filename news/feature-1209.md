`geoip()` FilterX function: added, performing a MaxMind GeoIP2/GeoLite2
database lookup for an IP address, mirroring the `$(geoip2)` template
function. Unlike the template function, it returns typed FilterX values
and, given a `field` pointing to a map or array entry (or `field=""` for
the whole record), returns a nested dict/list instead of a single scalar.
