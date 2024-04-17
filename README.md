# WRK-MultiProtocol

**a TCP/HTTP & QUIC/HTTP benchmarking tool based on wrk and wrk2**

On top of the new QUIC support, WRK-MP still fully supports WRK1/2 features, with even some improvements.

This repository is a modified version of WRK2 [http://github.com/gilene/wrk2](wrk2) with support for QUIC and various small improvements

QUIC support is now limited to HTTP 0.9 over QUIC, made to work against PicoQUIC HTTP server (using the hq ALPN). It also only supports disabled keep-alive for now.

The QUIC support is built using the picoquic library. Refer to picoquic for installation. To enable QUIC support, you must use the -q flag. Except for that most wrk2 documentation applies. We just changed a bit the default behavior, so if you omit the rate (or give -R -1) WRK will behave like the original version, trying to push as many request as possible.

## Building

```
./autogen.sh
./configure
make
sudo make install
```

## Support for QUIC

To support QUIC, you must have [https://github.com/private-octopus/picoquic](https://github.com/private-octopus/picoquic) installed first.
```
git clone https://github.com/private-octopus/picoquic.git
cmake -DPICOQUIC_FETCH_PTLS=Y .
make
```

## QUIC usage example

Launch a QUIC server, for instance using picoquic.
```
./picoquicdemo -p 4433
```

Launch wrk-mp with:
```
./wrk -a -R 0 --timeout 100 -q -t 1 -c 1 https://127.0.0.1:4433/1000
Running 10s test @ https://127.0.0.1:4433/1000
  1 threads and 1 connections
Rate is disabled, latency is not corrected  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   667.37us  191.23us   5.13ms   85.06%
    Req/Sec       -nan      -nan   0.00      0.00%
  12728 requests in 10.00s, 12.55MB read
Requests/sec:   1272.78
Transfer/sec:      1.26MB
```
