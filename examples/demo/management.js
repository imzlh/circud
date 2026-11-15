import { AbortSignal } from 'circud';

const evbus = import.meta.use('evbus');

evbus.on('circud.host', 'call', event => {
  switch (event.target) {
    case 'status.get':
      event.respondWith({
        state: 'running',
        message: event.signal instanceof AbortSignal
          ? 'demo is ready'
          : 'runtime cancellation contract is unavailable',
      });
      break;
    case 'echo':
      event.respondWith(event.arguments);
      break;
  }
});

evbus.on('circud.host', 'endpoint', event => {
  if (event.protocol === 'websocket') {
    const socket = event.acceptWebSocket();
    if (socket && event.path !== '/ws')
      socket.close(1008, 'unknown demo endpoint');
    return;
  }

  switch (event.path) {
    case '/':
      // Package files are not exposed automatically. The core reads this file
      // only because the package explicitly delegates this request.
      event.serveStatic('static/index.html');
      break;

    case '/hello':
      event.respondWith({
        headers: { 'cache-control': 'no-store' },
        body: {
          message: 'hello from io.circud.demo',
          method: event.method,
          path: event.path,
          query: event.query,
        },
      });
      break;

    case '/events': {
      const stream = event.acceptSse();
      if (!stream) break;
      stream.send('event: ready\ndata: {"message":"SSE is connected"}\n\n');
      stream.send('event: demo\ndata: {"sequence":1}\n\n');
      stream.close();
      break;
    }

    case '/ws':
      event.respondWith({
        status: 426,
        headers: { 'content-type': 'text/plain; charset=utf-8' },
        body: 'Use a WebSocket upgrade for this endpoint.\n',
      });
      break;

    default:
      event.respondWith({
        status: 404,
        headers: { 'content-type': 'text/plain; charset=utf-8' },
        body: 'No demo endpoint exists at this path.\n',
      });
      break;
  }
});

// WebSocket input returns to the package through EvBus. Sending the same
// value preserves text versus binary framing.
evbus.on('circud.host', 'endpoint.message', event => {
  event.session.send(event.data);
});
