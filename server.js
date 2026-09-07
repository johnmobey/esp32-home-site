const express = require('express');
const { WebSocketServer } = require('ws');
const path = require('path');
const http = require('http');
const Jimp = require('jimp');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(express.static(path.join(__dirname, 'public')));

let esp32Socket = null;

wss.on('connection', (ws, req) => {
  ws.on('message', (message) => {
    // Handle binary frames coming from ESP32 or text messages
    if (typeof message !== 'string' && !Buffer.isBuffer(message)) {
      message = Buffer.from(message);
    }

    const msgStr = typeof message === 'string' ? message : message.toString();
    
    if (msgStr === 'ESP_AUTH') {
      esp32Socket = ws;
      console.log('ESP32 Connected to Railway!');
      return;
    }

    // Server-side webcam frame processor
    if (msgStr.startsWith('CAM_FRAME:')) {
      const base64Data = msgStr.replace(/^CAM_FRAME:data:image\/jpeg;base64,/, '');
      const buffer = Buffer.from(base64Data, 'base64');
      
      Jimp.read(buffer, (err, image) => {
        if (err) return;
        image.resize(128, 64).greyscale();
        
        let xbmBuffer = Buffer.alloc(1024);
        let byteIdx = 0;
        
        for (let y = 0; y < 64; y++) {
          for (let x = 0; x < 128; x += 8) {
            let byte = 0;
            for (let bit = 0; bit < 8; bit++) {
              let pxColor = Jimp.intToRGBA(image.getPixelColor(x + bit, y));
              let gray = (pxColor.r * 0.299 + pxColor.g * 0.587 + pxColor.b * 0.114);
              if (gray < 128) {
                byte |= (1 << bit); // LSB first for XBM grid
              }
            }
            xbmBuffer[byteIdx++] = byte;
          }
        }
        
        if (esp32Socket && esp32Socket.readyState === ws.OPEN) {
          esp32Socket.send(xbmBuffer); // Dispatch binary grid directly
        }
      });
      return;
    }

    if (ws !== esp32Socket && esp32Socket && esp32Socket.readyState === ws.OPEN) {
      esp32Socket.send(message);
    }
    else if (ws === esp32Socket) {
      wss.clients.forEach(client => {
        if (client !== esp32Socket && client.readyState === client.OPEN) {
          client.send(message);
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