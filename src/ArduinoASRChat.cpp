#include "ArduinoASRChat.h"

ArduinoASRChat::ArduinoASRChat(const char* apiKey, const char* cluster) {
  _apiKey = apiKey;
  _cluster = cluster;

  // Allocate send buffer
  _sendBuffer = new int16_t[_sendBatchSize / 2];
}

void ArduinoASRChat::setApiConfig(const char* apiKey, const char* cluster) {
  if (apiKey != nullptr) {
    _apiKey = apiKey;
  }
  if (cluster != nullptr) {
    _cluster = cluster;
  }
}

void ArduinoASRChat::setMicrophoneType(MicrophoneType micType) {
  _micType = micType;
}

void ArduinoASRChat::setAudioParams(int sampleRate, int bitsPerSample, int channels) {
  _sampleRate = sampleRate;
  _bitsPerSample = bitsPerSample;
  _channels = channels;
}

void ArduinoASRChat::setSilenceDuration(unsigned long duration) {
  _silenceDuration = duration;
}

void ArduinoASRChat::setMaxRecordingSeconds(int seconds) {
  _maxSeconds = seconds;
}

bool ArduinoASRChat::initPDMMicrophone(int pdmClkPin, int pdmDataPin) {
  _micType = MIC_TYPE_PDM;
  _I2S.setPinsPdmRx(pdmClkPin, pdmDataPin);

  if (!_I2S.begin(I2S_MODE_PDM_RX, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("PDM I2S initialization failed!");
    return false;
  }

  Serial.println("PDM microphone initialized");

  // Wait for hardware to stabilize and clear buffer
  delay(500);
  for (int i = 0; i < 2000; i++) {
    _I2S.read();
  }

  return true;
}

bool ArduinoASRChat::initINMP441Microphone(int i2sSckPin, int i2sWsPin, int i2sSdPin) {
  _micType = MIC_TYPE_INMP441;
  _I2S.setPins(i2sSckPin, i2sWsPin, -1, i2sSdPin);

  if (!_I2S.begin(I2S_MODE_STD, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("INMP441 I2S initialization failed!");
    return false;
  }

  Serial.println("INMP441 microphone initialized");

  // Wait for hardware to stabilize and clear buffer
  delay(500);
  for (int i = 0; i < 2000; i++) {
    _I2S.read();
  }

  return true;
}

String ArduinoASRChat::generateWebSocketKey() {
  uint8_t random_bytes[16];
  for (int i = 0; i < 16; i++) {
    random_bytes[i] = random(0, 256);
  }

  size_t output_len;
  unsigned char output[32];
  mbedtls_base64_encode(output, sizeof(output), &output_len, random_bytes, 16);

  return String((char*)output);
}

bool ArduinoASRChat::connectWebSocket() {
  Serial.println("Connecting WebSocket...");

  _client.setInsecure();

  if (!_client.connect(_wsHost, _wsPort)) {
    Serial.println("SSL connection failed");
    return false;
  }

  // Disable Nagle algorithm for immediate send
  _client.setNoDelay(true);

  // Generate WebSocket Key and send handshake request
  String ws_key = generateWebSocketKey();
  String request = String("GET ") + _wsPath + " HTTP/1.1\r\n";
  request += String("Host: ") + _wsHost + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += String("x-api-key: ") + _apiKey + "\r\n";
  request += "\r\n";

  _client.print(request);

  // Read response
  unsigned long timeout = millis();
  while (_client.connected() && !_client.available()) {
    if (millis() - timeout > 5000) {
      Serial.println("Response timeout");
      _client.stop();
      return false;
    }
    delay(10);
  }

  String response = "";
  bool headers_complete = false;
  while (_client.available() && !headers_complete) {
    String line = _client.readStringUntil('\n');
    response += line + "\n";
    if (line == "\r" || line.length() == 0) {
      headers_complete = true;
    }
  }

  // Check response
  if (response.indexOf("101") >= 0 && response.indexOf("Switching Protocols") >= 0) {
    Serial.println("WebSocket connected");
    _wsConnected = true;
    _endMarkerSent = false;  // Reset flag on new connection
    return true;
  } else {
    Serial.println("WebSocket handshake failed");
    Serial.println(response);
    _client.stop();
    return false;
  }
}

void ArduinoASRChat::disconnectWebSocket() {
  if (_wsConnected) {
    _client.stop();
    _wsConnected = false;
    Serial.println("WebSocket disconnected");
  }
}

bool ArduinoASRChat::isWebSocketConnected() {
  return _wsConnected && _client.connected();
}

bool ArduinoASRChat::startRecording() {
  // If end marker was sent, need to reconnect WebSocket
  if (_endMarkerSent) {
    Serial.println("Reconnecting WebSocket for new session...");
    disconnectWebSocket();
    delay(100);
    if (!connectWebSocket()) {
      Serial.println("Failed to reconnect WebSocket!");
      return false;
    }
    _endMarkerSent = false;
  }

  if (!_wsConnected) {
    Serial.println("WebSocket not connected!");
    return false;
  }

  if (_isRecording) {
    Serial.println("Already recording!");
    return false;
  }

  Serial.println("\n========================================");
  Serial.println("Recording started...");
  Serial.println("========================================");

  _isRecording = true;
  _shouldStop = false;
  _hasSpeech = false;
  _hasNewResult = false;
  _lastResultText = "";
  _recognizedText = "";
  _lastSpeechTime = 0;
  _recordingStartTime = millis();
  _sendBufferPos = 0;
  _sameResultCount = 0;
  _lastDotTime = millis();

  // 发送新的会话请求，开始新的识别会话
  sendFullRequest();
  delay(50);  // 等待服务器确认

  return true;
}

void ArduinoASRChat::stopRecording() {
  if (!_isRecording) {
    return;
  }

  // Send remaining buffer
  if (_sendBufferPos > 0) {
    sendAudioChunk((uint8_t*)_sendBuffer, _sendBufferPos * 2);
    _sendBufferPos = 0;
  }

  Serial.println("\n========================================");
  Serial.println("Recording stopped");
  Serial.print("Final result: ");
  Serial.println(_lastResultText);
  Serial.println("========================================\n");

  _isRecording = false;
  _shouldStop = true;
  _recognizedText = _lastResultText;
  _hasNewResult = true;

  sendEndMarker();
  _endMarkerSent = true;  // Mark that end marker has been sent

  // Trigger callback if set
  if (_resultCallback != nullptr && _recognizedText.length() > 0) {
    _resultCallback(_recognizedText);
  }
}

bool ArduinoASRChat::isRecording() {
  return _isRecording;
}

void ArduinoASRChat::loop() {
  if (!_wsConnected) {
    return;
  }

  // Check connection status
  if (!_client.connected()) {
    Serial.println("Connection lost");
    _wsConnected = false;
    _isRecording = false;
    return;
  }

  // Process audio sending during recording
  if (_isRecording && !_shouldStop) {
    processAudioSending();
    checkRecordingTimeout();
    checkSilence();
  }

  // Process received data
  if (_client.available()) {
    if (_isRecording) {
      // Process only one message to avoid blocking too long
      handleWebSocketData();
    } else {
      // Process all pending responses after recording
      while (_client.available()) {
        handleWebSocketData();
        delay(10);
      }
    }
  }
}

void ArduinoASRChat::processAudioSending() {
  // Print progress dot every second
  if (millis() - _lastDotTime > 1000) {
    Serial.print(".");
    _lastDotTime = millis();
  }

  // Read audio samples in a tight loop to keep up with I2S data rate
  // Must read fast enough to avoid buffer overflow and send data on time
  for (int i = 0; i < _samplesPerRead; i++) {
    if (!_I2S.available()) {
      break;  // No more data available
    }

    int sample = _I2S.read();

    // Filter invalid data
    if (sample != 0 && sample != -1 && sample != 1) {
      _sendBuffer[_sendBufferPos++] = (int16_t)sample;

      // Buffer full, send batch immediately
      if (_sendBufferPos >= _sendBatchSize / 2) {
        sendAudioChunk((uint8_t*)_sendBuffer, _sendBufferPos * 2);
        _sendBufferPos = 0;
      }
    }
  }

  yield();
}

void ArduinoASRChat::checkRecordingTimeout() {
  // Check max duration
  if (millis() - _recordingStartTime > _maxSeconds * 1000) {
    Serial.println("\nMax duration reached");

    // If no speech detected and callback is set, trigger timeout callback
    if (!_hasSpeech && _timeoutNoSpeechCallback != nullptr) {
      Serial.println("No speech detected during recording, exiting continuous mode");
      stopRecording();
      _timeoutNoSpeechCallback();
    } else {
      Serial.println("Stopping recording");
      stopRecording();
    }
  }
}

void ArduinoASRChat::checkSilence() {
  // Check silence - if speech detected and exceeded silence duration
  if (_hasSpeech && _lastSpeechTime > 0) {
    unsigned long silence = millis() - _lastSpeechTime;
    if (silence >= _silenceDuration) {
      Serial.printf("\nSilence detected (%.1fs), stopping\n", silence / 1000.0);
      stopRecording();
    }
  }
}

String ArduinoASRChat::getRecognizedText() {
  return _recognizedText;
}

bool ArduinoASRChat::hasNewResult() {
  return _hasNewResult;
}

void ArduinoASRChat::clearResult() {
  _hasNewResult = false;
}

void ArduinoASRChat::setResultCallback(ResultCallback callback) {
  _resultCallback = callback;
}

void ArduinoASRChat::setTimeoutNoSpeechCallback(TimeoutNoSpeechCallback callback) {
  _timeoutNoSpeechCallback = callback;
}

void ArduinoASRChat::sendFullRequest() {
  // Generate unique session ID using timestamp + random
  String reqid = String(millis()) + "_" + String(random(10000, 99999));

  // Use MAC address as stable user ID
  String uid = String(ESP.getEfuseMac(), HEX);

  StaticJsonDocument<512> doc;
  doc["app"]["cluster"] = _cluster;
  doc["user"]["uid"] = uid;
  doc["request"]["reqid"] = reqid;
  doc["request"]["nbest"] = 1;
  doc["request"]["workflow"] = "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate";
  doc["request"]["result_type"] = "full";
  doc["request"]["sequence"] = 1;
  doc["audio"]["format"] = "raw";
  doc["audio"]["rate"] = _sampleRate;
  doc["audio"]["bits"] = _bitsPerSample;
  doc["audio"]["channel"] = _channels;
  doc["audio"]["codec"] = "raw";

  String json_str;
  serializeJson(doc, json_str);

  Serial.print("Request ID: ");
  Serial.println(reqid);
  Serial.println("Sending config:");
  Serial.println(json_str);

  // Protocol header
  uint8_t header[4] = {0x11, (CLIENT_FULL_REQUEST << 4) | NO_SEQUENCE, 0x10, 0x00};
  uint32_t payload_len = json_str.length();
  uint8_t len_bytes[4];
  len_bytes[0] = (payload_len >> 24) & 0xFF;
  len_bytes[1] = (payload_len >> 16) & 0xFF;
  len_bytes[2] = (payload_len >> 8) & 0xFF;
  len_bytes[3] = payload_len & 0xFF;

  uint8_t* full_request = new uint8_t[8 + payload_len];
  memcpy(full_request, header, 4);
  memcpy(full_request + 4, len_bytes, 4);
  memcpy(full_request + 8, json_str.c_str(), payload_len);

  sendWebSocketFrame(full_request, 8 + payload_len, 0x02);
  delete[] full_request;
}

void ArduinoASRChat::sendAudioChunk(uint8_t* data, size_t len) {
  // Protocol header
  uint8_t header[4] = {0x11, (CLIENT_AUDIO_ONLY_REQUEST << 4) | NO_SEQUENCE, 0x10, 0x00};
  uint8_t len_bytes[4];
  len_bytes[0] = (len >> 24) & 0xFF;
  len_bytes[1] = (len >> 16) & 0xFF;
  len_bytes[2] = (len >> 8) & 0xFF;
  len_bytes[3] = len & 0xFF;

  uint8_t* audio_request = new uint8_t[8 + len];
  memcpy(audio_request, header, 4);
  memcpy(audio_request + 4, len_bytes, 4);
  memcpy(audio_request + 8, data, len);

  sendWebSocketFrame(audio_request, 8 + len, 0x02);
  delete[] audio_request;
}

void ArduinoASRChat::sendEndMarker() {
  uint8_t header[4] = {0x11, (CLIENT_AUDIO_ONLY_REQUEST << 4) | NEG_SEQUENCE, 0x10, 0x00};
  uint8_t len_bytes[4] = {0x00, 0x00, 0x00, 0x00};
  uint8_t end_request[8];
  memcpy(end_request, header, 4);
  memcpy(end_request + 4, len_bytes, 4);

  sendWebSocketFrame(end_request, 8, 0x02);
  Serial.println("End marker sent");
}

void ArduinoASRChat::sendPong() {
  uint8_t pong_data[1] = {0};
  sendWebSocketFrame(pong_data, 0, 0x0A);
}

void ArduinoASRChat::sendWebSocketFrame(uint8_t* data, size_t len, uint8_t opcode) {
  if (!_wsConnected || !_client.connected()) return;

  // Build WebSocket frame header
  uint8_t header[10];
  int header_len = 2;

  header[0] = 0x80 | opcode;
  header[1] = 0x80;

  // Length
  if (len < 126) {
    header[1] |= len;
  } else if (len < 65536) {
    header[1] |= 126;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    header_len = 4;
  } else {
    header[1] |= 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (len >> (56 - i * 8)) & 0xFF;
    }
    header_len = 10;
  }

  // Generate mask key
  uint8_t mask_key[4];
  for (int i = 0; i < 4; i++) {
    mask_key[i] = random(0, 256);
  }
  memcpy(header + header_len, mask_key, 4);
  header_len += 4;

  // Send frame header
  _client.write(header, header_len);

  // Mask data and send
  for (size_t i = 0; i < len; i++) {
    data[i] ^= mask_key[i % 4];
  }
  _client.write(data, len);
}

void ArduinoASRChat::handleWebSocketData() {
  // Read WebSocket frame
  uint8_t header[2];
  if (_client.readBytes(header, 2) != 2) {
    return;
  }

  bool fin = header[0] & 0x80;
  uint8_t opcode = header[0] & 0x0F;
  bool masked = header[1] & 0x80;
  uint64_t payload_len = header[1] & 0x7F;

  // Handle extended length
  if (payload_len == 126) {
    uint8_t len_bytes[2];
    _client.readBytes(len_bytes, 2);
    payload_len = (len_bytes[0] << 8) | len_bytes[1];
  } else if (payload_len == 127) {
    uint8_t len_bytes[8];
    _client.readBytes(len_bytes, 8);
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | len_bytes[i];
    }
  }

  // Read mask key
  uint8_t mask_key[4] = {0};
  if (masked) {
    _client.readBytes(mask_key, 4);
  }

  // Read payload
  if (payload_len > 0 && payload_len < 100000) {
    uint8_t* payload = new uint8_t[payload_len];
    size_t bytes_read = _client.readBytes(payload, payload_len);

    if (bytes_read == payload_len) {
      // Unmask
      if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
          payload[i] ^= mask_key[i % 4];
        }
      }

      // Handle different opcodes
      if (opcode == 0x01 || opcode == 0x02) {
        parseResponse(payload, payload_len);
      } else if (opcode == 0x08) {
        Serial.println("Server closed connection");
        _wsConnected = false;
        _client.stop();
      } else if (opcode == 0x09) {
        sendPong();
      }
    }

    delete[] payload;
  }
}

void ArduinoASRChat::parseResponse(uint8_t* data, size_t len) {
  if (len < 4) return;

  uint8_t msg_type = data[1] >> 4;
  uint8_t header_size = data[0] & 0x0f;

  if (len < header_size * 4) return;

  uint8_t* payload = data + header_size * 4;
  size_t payload_len = len - header_size * 4;

  if (msg_type == SERVER_FULL_RESPONSE && payload_len > 4) {
    payload += 4;
    payload_len -= 4;
  } else if (msg_type == SERVER_ACK && payload_len >= 8) {
    payload += 8;
    payload_len -= 8;
  } else if (msg_type == SERVER_ERROR_RESPONSE && payload_len >= 8) {
    payload += 8;
    payload_len -= 8;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload, payload_len);

  if (error) {
    return;
  }

  if (doc.containsKey("code")) {
    int code = doc["code"];
    if (code != 1000 && code != 1013) {
      // Ignore 1000 (success) and 1013 (silence detection)
      Serial.print("\nError: ");
      serializeJson(doc, Serial);
      Serial.println();
    }
  }

  if (doc.containsKey("result")) {
    JsonVariant result = doc["result"];
    String current_text = "";

    if (result.is<JsonArray>() && result.size() > 0) {
      if (result[0].containsKey("text")) {
        current_text = result[0]["text"].as<String>();
      }
    }

    if (current_text.length() > 0 && current_text != " ") {
      if (!_hasSpeech) {
        _hasSpeech = true;
        Serial.println("\nSpeech detected...");
      }

      // Update last speech time
      _lastSpeechTime = millis();

      if (current_text == _lastResultText) {
        _sameResultCount++;
        if (_sameResultCount <= 3) {
          Serial.printf("Recognizing: %s\n", current_text.c_str());
        } else if (_sameResultCount == 4) {
          Serial.printf("Result stable: %s\n", current_text.c_str());
        }

        // Only trigger stop if still recording
        if (_sameResultCount >= 10 && _isRecording && !_shouldStop) {
          Serial.println("\nResult stable, stopping recording");
          stopRecording();
        }
      } else {
        _sameResultCount = 1;
        _lastResultText = current_text;
        Serial.printf("Recognizing: %s\n", current_text.c_str());
      }
    }
  }
}
