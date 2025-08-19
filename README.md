# ESPNOW Gateway (serial)
This project is part of the geophone footfall detection project. This code is to be flashed to the gateway device. This gateway forwards JSON payloads between ESP-NOW clients and Node-RED running on the host PC over the board’s USB Serial/JTAG interface.

## Overview

- Gateway listens for ESPNOW packets from clients and forwards the JSON payload (the JSON bytes inside the ESPNOW user payload) to the host via USB serial (newline terminated).

- Node-RED (host) sends newline-terminated JSON commands over the same USB serial connection to the gateway. The gateway parses those commands and forwards appropriate ESPNOW unicast messages to the target client MAC.

- Discovery / registration: a client that does not yet know the gateway MAC will broadcast a register payload. Gateway listens for that and replies unicast with gateway_info (so clients save the gateway MAC and switch to unicast).

- Gateway does not persist client lists or configs — it only forwards messages.

## JSON Commands
All commands / message types — summary
### A. Messages sent by clients to the gateway (sensor → gateway → Node-RED)

- **sensor**
Event-based sensor event sent unicast to gateway (after gateway known).
Example:
```json
{"type":"sensor","payload":{"mac":"AA:BB:CC:DD:EE:FF","status":true}}
```

- **heartbeat**
(Optional) heartbeat message — client sends to gateway to indicate liveness (every 5 minutes).
Example:
```json
{"type":"heartbeat","payload":{"mac":"AA:BB:CC:DD:EE:FF"}}
```

- **register (discovery)** — broadcast
Client broadcasts this when it does not have gateway MAC or on double-press (GPIO9). Gateway replies unicast with gateway_info.
Example:
```json
{"type":"register","payload":{"mac":"AA:BB:CC:DD:EE:FF"}}
```

- **config_response**
Reply from client after a gateway-initiated config_request — contains cfg0..cfg4 values.
Example:
```json
{"type":"config_response","payload":{"cfg0":10,"cfg1":20,"cfg2":30,"cfg3":40,"cfg4":50}}
```
