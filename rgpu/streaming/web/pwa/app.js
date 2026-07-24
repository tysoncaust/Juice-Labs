const video = document.querySelector('#remoteVideo');
const status = document.querySelector('#status');
const stats = document.querySelector('#stats');
const sessionForm = document.querySelector('#sessionForm');
const connectButton = document.querySelector('#connect');
const disconnectButton = document.querySelector('#disconnect');

let peerConnection = null;
let signalingSocket = null;
let statsTimer = null;

function setStatus(message) {
  status.textContent = message;
}

function enforcePassiveMode(peer) {
  peer.addEventListener('datachannel', (event) => {
    event.channel.close();
    setStatus('Rejected unexpected data channel in passive mode.');
  });
}

async function collectStats() {
  if (!peerConnection) return;

  const report = await peerConnection.getStats();
  const snapshot = {
    timestamp: new Date().toISOString(),
    inbound: {},
  };

  report.forEach((entry) => {
    if (entry.type === 'inbound-rtp' && !entry.isRemote) {
      snapshot.inbound[entry.kind || entry.mediaType] = {
        packetsReceived: entry.packetsReceived,
        packetsLost: entry.packetsLost,
        jitter: entry.jitter,
        framesDecoded: entry.framesDecoded,
        framesDropped: entry.framesDropped,
        jitterBufferDelay: entry.jitterBufferDelay,
        jitterBufferEmittedCount: entry.jitterBufferEmittedCount,
        bytesReceived: entry.bytesReceived,
      };
    }

    if (entry.type === 'candidate-pair' && entry.state === 'succeeded') {
      snapshot.rttSeconds = entry.currentRoundTripTime;
    }
  });

  stats.textContent = JSON.stringify(snapshot, null, 2);
}

function probeCompositorFrames() {
  if (!video.requestVideoFrameCallback) return;

  video.requestVideoFrameCallback((callbackTime, metadata) => {
    const metric = {
      callbackTime,
      mediaTime: metadata.mediaTime,
      captureTime: metadata.captureTime ?? null,
      receiveTime: metadata.receiveTime ?? null,
      presentationTime: metadata.presentationTime ?? null,
      expectedDisplayTime: metadata.expectedDisplayTime,
      processingDuration: metadata.processingDuration ?? null,
      presentedFrames: metadata.presentedFrames,
    };

    sessionStorage.setItem('rgpu-last-frame-metric', JSON.stringify(metric));
    probeCompositorFrames();
  });
}

async function sendOffer() {
  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);
  signalingSocket.send(JSON.stringify({
    type: 'offer',
    sdp: offer.sdp,
    mode: 'passive',
  }));
}

async function connect() {
  const url = document.querySelector('#signalUrl').value;
  const tokenInput = document.querySelector('#token');
  const token = tokenInput.value;

  if (!/^wss:\/\//.test(url)) throw new Error('WSS is required');
  if (token.length < 16 || token.length > 128) {
    throw new Error('Session token must contain 16-128 characters');
  }

  peerConnection = new RTCPeerConnection({
    bundlePolicy: 'max-bundle',
    rtcpMuxPolicy: 'require',
  });
  enforcePassiveMode(peerConnection);
  peerConnection.addTransceiver('video', { direction: 'recvonly' });
  peerConnection.addTransceiver('audio', { direction: 'recvonly' });

  peerConnection.ontrack = (event) => {
    video.srcObject = event.streams[0] ?? new MediaStream([event.track]);
  };
  peerConnection.onconnectionstatechange = () => {
    setStatus(`WebRTC: ${peerConnection.connectionState}`);
  };
  peerConnection.onicecandidate = (event) => {
    if (event.candidate && signalingSocket?.readyState === WebSocket.OPEN) {
      signalingSocket.send(JSON.stringify({
        type: 'candidate',
        candidate: event.candidate,
      }));
    }
  };

  signalingSocket = new WebSocket(url, 'rgpu-passive-v1');
  signalingSocket.onopen = () => {
    signalingSocket.send(JSON.stringify({
      type: 'authenticate',
      token,
      mode: 'passive',
      role: 'receiver',
    }));
    tokenInput.value = '';
  };
  signalingSocket.onmessage = async (event) => {
    const message = JSON.parse(event.data);
    if (message.type === 'authenticated') {
      setStatus('Authenticated; waiting for the native sender.');
    } else if (message.type === 'ready') {
      await sendOffer();
    } else if (message.type === 'answer') {
      await peerConnection.setRemoteDescription(message);
    } else if (message.type === 'candidate' && message.candidate) {
      await peerConnection.addIceCandidate(message.candidate);
    }
  };
  signalingSocket.onerror = () => setStatus('Signaling connection failed.');

  connectButton.disabled = true;
  disconnectButton.disabled = false;
  statsTimer = setInterval(collectStats, 1000);
  probeCompositorFrames();
}

function disconnect() {
  clearInterval(statsTimer);
  statsTimer = null;
  signalingSocket?.close();
  peerConnection?.close();
  signalingSocket = null;
  peerConnection = null;
  video.srcObject = null;
  connectButton.disabled = false;
  disconnectButton.disabled = true;
  setStatus('Disconnected; passive mode has no input state.');
}

sessionForm.addEventListener('submit', (event) => {
  event.preventDefault();
  connect().catch((error) => {
    setStatus(`Error: ${error.message}`);
    disconnect();
  });
});
disconnectButton.addEventListener('click', disconnect);
window.addEventListener('pagehide', disconnect);

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./service-worker.js').catch((error) => {
    setStatus(`Service worker registration failed: ${error.message}`);
  });
}
