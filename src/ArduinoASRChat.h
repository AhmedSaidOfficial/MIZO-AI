#ifndef ArduinoASRChat_h
#define ArduinoASRChat_h

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

// Microphone type selection
enum MicrophoneType {
  MIC_TYPE_PDM,      // PDM microphone (e.g., ESP32-S3 onboard microphone)
  MIC_TYPE_INMP441   // INMP441 I2S MEMS microphone
};

// Message types for ByteDance ASR protocol
#define CLIENT_FULL_REQUEST 0b0001
#define CLIENT_AUDIO_ONLY_REQUEST 0b0010
#define SERVER_FULL_RESPONSE 0b1001
#define SERVER_ACK 0b1011
#define SERVER_ERROR_RESPONSE 0b1111

// Sequence flags
#define NO_SEQUENCE 0b0000
#define NEG_SEQUENCE 0b0010

class ArduinoASRChat {
  public:
    // Constructor
    ArduinoASRChat(const char* apiKey, const char* cluster = "volcengine_input_en");

    // Configuration methods
    void setApiConfig(const char* apiKey, const char* cluster = nullptr);
    void setMicrophoneType(MicrophoneType micType);
    void setAudioParams(int sampleRate = 16000, int bitsPerSample = 16, int channels = 1);
    void setSilenceDuration(unsigned long duration);
    void setMaxRecordingSeconds(int seconds);

    // Microphone initialization
    bool initPDMMicrophone(int pdmClkPin, int pdmDataPin);
    bool initINMP441Microphone(int i2sSckPin, int i2sWsPin, int i2sSdPin);

    // WebSocket connection
    bool connectWebSocket();
    void disconnectWebSocket();
    bool isWebSocketConnected();

    // Recording control
    bool startRecording();
    void stopRecording();
    bool isRecording();

    // Main loop processing - must be called in loop()
    void loop();

    // Get recognition result
    String getRecognizedText();
    bool hasNewResult();
    void clearResult();

    // Callback function type
    typedef void (*ResultCallback)(String text);
    void setResultCallback(ResultCallback callback);

    // Timeout without speech callback
    typedef void (*TimeoutNoSpeechCallback)();
    void setTimeoutNoSpeechCallback(TimeoutNoSpeechCallback callback);

  private:
    // WebSocket configuration
    const char* _apiKey;
    const char* _cluster;
    const char* _wsHost = "openspeech.bytedance.com";
    const int _wsPort = 443;
    const char* _wsPath = "/api/v2/asr";

    // Audio parameters
    int _sampleRate = 16000;
    int _bitsPerSample = 16;
    int _channels = 1;
    int _samplesPerRead = 800;  // 50ms of data
    int _sendBatchSize = 3200;  // 200ms of data
    unsigned long _silenceDuration = 1000;  // Silence detection duration (ms)
    int _maxSeconds = 50;  // Maximum recording duration

    // Microphone configuration
    MicrophoneType _micType = MIC_TYPE_INMP441;
    I2SClass _I2S;

    // WiFi client
    WiFiClientSecure _client;

    // State flags
    bool _wsConnected = false;
    bool _isRecording = false;
    bool _shouldStop = false;
    bool _hasSpeech = false;
    bool _hasNewResult = false;
    bool _endMarkerSent = false;  // Track if end marker has been sent

    // Recording state
    String _lastResultText = "";
    String _recognizedText = "";
    unsigned long _recordingStartTime = 0;
    unsigned long _lastSpeechTime = 0;
    int _sameResultCount = 0;
    unsigned long _lastDotTime = 0;

    // Audio buffer
    int16_t* _sendBuffer;
    int _sendBufferPos = 0;

    // Callback
    ResultCallback _resultCallback = nullptr;
    TimeoutNoSpeechCallback _timeoutNoSpeechCallback = nullptr;

    // Private helper methods
    String generateWebSocketKey();
    void handleWebSocketData();
    void sendWebSocketFrame(uint8_t* data, size_t len, uint8_t opcode);
    void sendFullRequest();
    void sendAudioChunk(uint8_t* data, size_t len);
    void sendEndMarker();
    void sendPong();
    void parseResponse(uint8_t* data, size_t len);
    void processAudioSending();
    void checkRecordingTimeout();
    void checkSilence();
};

#endif
