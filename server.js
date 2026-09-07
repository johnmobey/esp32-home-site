const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const http = require('http');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(express.static(path.join(__dirname, 'public')));

let esp32Socket = null;

wss.on('connection', (ws, req) => {
  ws.on('message', (message) => {
    const msgStr = message.toString();
    
    if (msgStr === 'ESP_AUTH') {
      esp32Socket = ws;
      console.log('ESP32 Connected to Railway!');
      return;
    }

    if (ws !== esp32Socket && esp32Socket && esp32Socket.readyState === ws.OPEN) {
      esp32Socket.send(msgStr);
    }
    else if (ws === esp32Socket) {
      wss.clients.forEach(client => {
        if (client !== esp32Socket && client.readyState === client.OPEN) {
          client.send(msgStr);
        }
      });
    }
  });

  ws.on('close', () => {
    if (ws === esp32Socket) {
      esp32Socket = null;
      console.log('ESP32 Disconnected');
    }
  });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log(`Server running on port ${PORT}`));