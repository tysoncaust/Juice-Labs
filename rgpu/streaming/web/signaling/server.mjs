import { createServer } from 'node:https';
import { readFileSync } from 'node:fs';
import { WebSocketServer } from 'ws';

const port = Number(process.env.PORT || 8443);
const allowedOrigin = process.env.ALLOWED_ORIGIN;
const tlsCertificate = process.env.TLS_CERT;
const tlsKey = process.env.TLS_KEY;

if (!allowedOrigin) throw new Error('ALLOWED_ORIGIN is required');
if (!tlsCertificate || !tlsKey) throw new Error('TLS_CERT and TLS_KEY are required');

const server = createServer({
  cert: readFileSync(tlsCertificate),
  key: readFileSync(tlsKey),
}, (_request, response) => {
  response.writeHead(404);
  response.end();
});

const sessions = new Map();
const webSocketServer = new WebSocketServer({
  server,
  maxPayload: 64 * 1024,
  handleProtocols: (protocols) => protocols.has('rgpu-passive-v1')
    ? 'rgpu-passive-v1'
    : false,
  verifyClient: ({ origin }) => origin === allowedOrigin,
});

function leaveSession(socket) {
  const token = socket.sessionToken;
  if (!token) return;

  const peers = sessions.get(token);
  peers?.delete(socket);
  if (!peers?.size) sessions.delete(token);
  else {
    for (const peer of peers) {
      if (peer.readyState === 1) peer.send(JSON.stringify({ type: 'peer-left' }));
    }
  }

  socket.sessionToken = undefined;
  socket.sessionRole = undefined;
}

function authenticate(socket, message) {
  if (socket.sessionToken) throw new Error('already authenticated');
  if (message.mode !== 'passive') throw new Error('passive mode required');
  if (!['sender', 'receiver'].includes(message.role)) throw new Error('invalid peer role');
  if (typeof message.token !== 'string' || message.token.length < 16 || message.token.length > 128) {
    throw new Error('invalid session token');
  }

  let peers = sessions.get(message.token);
  if (!peers) {
    peers = new Set();
    sessions.set(message.token, peers);
  }
  if (peers.size >= 2) throw new Error('session full');
  if ([...peers].some((peer) => peer.sessionRole === message.role)) {
    throw new Error('peer role already present');
  }

  socket.sessionToken = message.token;
  socket.sessionRole = message.role;
  peers.add(socket);
  socket.send(JSON.stringify({
    type: 'authenticated',
    mode: 'passive',
    role: message.role,
  }));

  if (peers.size === 2) {
    for (const peer of peers) {
      if (peer.readyState === 1) peer.send(JSON.stringify({ type: 'ready' }));
    }
  }
}

function relay(socket, message) {
  if (!socket.sessionToken) throw new Error('authentication required');
  if (!['offer', 'answer', 'candidate'].includes(message.type)) {
    throw new Error('invalid signaling message');
  }
  if (message.mode && message.mode !== 'passive') throw new Error('passive mode required');

  const peers = sessions.get(socket.sessionToken);
  for (const peer of peers || []) {
    if (peer !== socket && peer.readyState === 1) {
      peer.send(JSON.stringify(message));
    }
  }
}

webSocketServer.on('connection', (socket) => {
  socket.sessionToken = undefined;
  socket.sessionRole = undefined;

  socket.on('message', (rawMessage) => {
    try {
      const message = JSON.parse(rawMessage.toString('utf8'));
      if (message.type === 'authenticate') authenticate(socket, message);
      else relay(socket, message);
    } catch (error) {
      socket.close(1008, error.message.slice(0, 100));
    }
  });

  socket.on('close', () => leaveSession(socket));
  socket.on('error', () => leaveSession(socket));
});

server.listen(port, () => {
  console.log(`RGPU_PASSIVE_SIGNALING=LISTENING port=${port}`);
});
