# Passive signaling relay

Minimal WSS relay for exactly one `sender` and one `receiver`. It enforces an allowed browser origin, bounded payload size, a 16-128 character memory-only session token, unique peer roles, passive-only messages, and a two-peer `ready` barrier before SDP exchange.

Authentication occurs as the first JSON message inside WSS; tokens are not placed in URLs or WebSocket subprotocol headers. The relay stores no SDP, ICE candidates, media, or persistent session state. It forwards only `offer`, `answer`, and `candidate` messages after authentication and informs the remaining peer when the other disconnects.

Deploy behind a hardened HTTPS boundary and use coturn for TURN/STUN. The native sender must authenticate with role `sender`; the PWA uses role `receiver`.
