# Passive WebRTC PWA receiver

This installable PWA is a receive-only WebRTC client. It creates recv-only audio/video transceivers, rejects every incoming data channel, requires WSS signaling, samples `RTCPeerConnection.getStats()`, and records compositor-level frame timing through `requestVideoFrameCallback()` where supported.

The session token is sent only as the first message inside the encrypted WSS connection, is cleared from the form immediately, and is not placed in a URL, WebSocket subprotocol, local storage, or session storage. The receiver authenticates with role `receiver` and waits for the signaling server's two-peer `ready` barrier before creating an offer, preventing offer loss when the native sender joins later.

The PWA does not capture the Windows desktop and contains no input controls or accepted data channels. The native Windows WebRTC media sender is not yet implemented; SRT remains the tested reference transport.
