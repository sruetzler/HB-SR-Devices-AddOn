# Homematic Thermostat ↔ Valve Protocol Notes

*(Observed behavior while emulating a valve using AskSin++)*

## Overview

These notes summarize observations from sniffed radio communication between a **Homematic thermostat** and a **radiator valve**.
The goal was to implement a **valve emulator using AskSin++** that behaves identically to a real valve so that both the **thermostat** and the **CCU** accept it as a normal device.

The protocol observations below are based on real traffic captures.

---

# Communication Model

The thermostat actively controls the valve.
Communication generally follows this pattern:

```
Thermostat → Valve     : command (0x58)
Valve → Thermostat     : ACK_EVENT (0x02) with valve position
```

The thermostat then updates its internal state using the valve's reported position.

---

# Message Types Observed

| Type   | Direction              | Description                                 |
| ------ | ---------------------- | ------------------------------------------- |
| `0x58` | Thermostat → Valve     | Command / setpoint message                  |
| `0x02` | Valve → Thermostat     | `ACK_EVENT` response containing valve state |
| `0x70` | Thermostat → Broadcast | Status broadcast (temperature etc.)         |

---

# Valve Setpoint Command (0x58)

The thermostat sends the valve target position using message type `0x58`.

Example packet:

```
0B 1C A2 58 20209C 13E142 03 FA
```

## Structure

| Byte | Meaning                  |
| ---- | ------------------------ |
| 0    | Length                   |
| 1    | Counter                  |
| 2    | Flags (`0xA2`)           |
| 3    | Message Type (`0x58`)    |
| 4-6  | Sender (thermostat HMID) |
| 7-9  | Destination (valve HMID) |
| 10   | payload0                 |
| 11   | payload1                 |

### Important fields

```
payload0 = 0x03
payload1 = valve setpoint (0..255)
```

The thermostat uses a **0..255 scale**.

Example:

```
payload1 = 0xFA = 250
250 / 255 ≈ 98 %
```

---

# Mapping to Valve Raw Value

Real Homematic valves internally use **0..200** for valve position.

Conversion used by the emulator:

```
valveRaw = payload1 * 200 / 255
```

Example:

```
payload1 = 250
→ valveRaw ≈ 196
→ 196 / 200 ≈ 98 %
```

---

# Valve Response (ACK_EVENT)

After receiving a command the valve replies with `ACK_EVENT`.

Example:

```
0E 1C 82 02 13E142 20209C 01 01 C4 00 21
```

## Structure

| Field      | Meaning              |
| ---------- | -------------------- |
| Type       | `0x02` (`ACK_EVENT`) |
| Flags      | `0x82`               |
| From       | Valve HMID           |
| To         | Thermostat HMID      |
| Channel    | `01`                 |
| Subcommand | `01`                 |
| ValveRaw   | position `0..200`    |
| Error      | error flags          |
| Extra      | status byte          |

Example interpretation:

```
C4 = 196
196 / 200 ≈ 98 %
```

This value is what the **thermostat and CCU display as valve position**.

---

# Status Byte (extra)

Real valves alternate between two values:

```
0x20
0x21
```

Observed behavior suggests:

```
extra = 0x20 | toggleBit
```

A valve emulator can mimic this by toggling between `0x20` and `0x21` for each response.

---

# Other 0x58 Variants

Not every `0x58` packet contains a valve setpoint.

Observed variant:

```
payload0 = 0x00
```

Example:

```
0B 47 A2 58 20209C 13E142 00 A9
```

These messages are **not valve position commands** and should be ignored.

Possible meanings:

* poll / keep-alive
* thermostat status exchange
* other internal commands

Only packets with

```
payload0 == 0x03
```

should be interpreted as valve commands.

---

# Thermostat Broadcast (0x70)

Thermostats periodically send broadcast status packets.

Example:

```
0C 46 86 70 20209C 000000 00 C2
```

## Observed structure

```
payload1 ≈ temperature * 10
```

Example:

```
C2 = 194
→ 19.4 °C
```

These packets are **not relevant for valve control**.

---

# Valve Status Updates

Real valves typically **do not send periodic status updates**.

Instead:

1. Thermostat sends command (`0x58`)
2. Valve responds (`ACK_EVENT`)
3. Thermostat updates displayed valve position

Optional asynchronous status events may occur but are not required for correct operation.

---

# Emulator Behaviour

A working valve emulator should implement the following logic:

1. Receive `0x58`
2. Verify message is addressed to the valve
3. If `payload0 == 0x03`

   * treat `payload1` as valve setpoint (0..255)
   * convert to `0..200`
   * store as current valve position
4. Send `ACK_EVENT` with current valve position

---

# Example Communication

```
Thermostat → Valve
0B 1C A2 58 20209C 13E142 03 FA
                 │
                 └─ valve setpoint = 250 ≈ 98 %

Valve → Thermostat
0E 1C 82 02 13E142 20209C 01 01 C4 00 21
                           │
                           └─ valveRaw = 196 ≈ 98 %
```

The thermostat will then display **98 % valve opening**.

---

# Result

Using the behavior above it is possible to emulate a Homematic valve with AskSin++.

The thermostat and CCU will:

* accept the device
* update valve position correctly
* behave identically to a real valve.

---

# Notes

These observations are based on real radio captures and may not cover every edge case of the Homematic protocol.
