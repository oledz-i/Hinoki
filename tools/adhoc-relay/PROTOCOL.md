# Hinoki ad hoc relay protocol (version 1)

Hinoki turns a PS Vita game's ad hoc (local wireless) traffic into traffic through a relay
server, so players anywhere can play together as if their Vitas were next to each other.

Each emulator keeps one TCP connection to the relay. The relay puts players into **rooms**
(`<title id>/<room name>`), gives each player a **virtual IP** in `10.77.0.0/16`, and forwards
packets between players in the same room. The relay never looks inside game data.

## Framing

Every message: `u32 length (big-endian, counts type + body)` · `u8 type` · `body`.
All integers are big-endian. Maximum length: 70000 bytes.

## Control messages

| Type | Name | Direction | Body |
|---|---|---|---|
| 0x01 | HELLO | C→S | `u16 version (1)` · `u32 preferred vip (0 = any)` · `u8 len` room · `u8 len` nickname · `u8 len` key |
| 0x02 | WELCOME | S→C | `u32 vip` · `u16 players in room (including you)` |
| 0x03 | ERROR | S→C | UTF-8 text; the server closes the connection after it |
| 0x04 | PEER_JOIN | S→C | `u32 vip` · `u8 len` nickname |
| 0x05 | PEER_LEAVE | S→C | `u32 vip` |
| 0x06 | PING | either | `u64` opaque |
| 0x07 | PONG | either | the PING's `u64`, echoed |

## Routed messages

The body starts with `u32 peer`. From a client, `peer` is the destination virtual IP. The
server replaces it with the sender's virtual IP and forwards the message to the destination.
A DGRAM sent to `255.255.255.255` or `10.77.255.255` goes to everyone else in the room.

A "port pair" is the Vita's `(port, vport)`: ad hoc sockets all use UDP port 3658 and are told
apart by the virtual port.

| Type | Name | Body after `peer` |
|---|---|---|
| 0x10 | DGRAM | `u16 src port` · `u16 src vport` · `u16 dst port` · `u16 dst vport` · payload |
| 0x11 | S_OPEN | `u32 sid` · `u16 src port` · `u16 src vport` · `u16 dst port` · `u16 dst vport` |
| 0x13 | S_DATA | `u32 sid` · bytes |
| 0x14 | S_CLOSE | `u32 sid` |

Streams (the Vita's P2P TCP sockets): the side that connects picks `sid` (bit 31 clear). The
side that accepted sets bit 31 on every `sid` it sends, so both sides can tell whose stream it
is. Data may follow S_OPEN at once; if nothing listens on the destination, the receiver answers
with S_CLOSE.
