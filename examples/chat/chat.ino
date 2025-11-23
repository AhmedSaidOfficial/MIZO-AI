#include <WiFi.h>
#include <ArduinoGPTChat.h>
#include "Audio.h"

// Define I2S pins for audio output
#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC 45

// Define INMP441 microphone input pins (I2S standard mode)
// INMP441 wiring:
// VDD -> 3.3V (DO NOT use 5V!)
// GND -> GND
// L/R -> GND (select left channel)
// WS  -> GPIO 25 (left/right clock)
// SCK -> GPIO 32 (serial clock)
// SD  -> GPIO 33 (serial data)
#define I2S_MIC_SERIAL_CLOCK 5    // SCK - serial clock
#define I2S_MIC_LEFT_RIGHT_CLOCK 4 // WS - left/right clock
#define I2S_MIC_SERIAL_DATA 6     // SD - serial data

// Define boot button pin (GPIO0 is the boot button on most ESP32 boards)
#define BOOT_BUTTON_PIN 0

// Sample rate for recording
#define SAMPLE_RATE 8000

// I2S configuration for microphone (INMP441 settings)
// Different microphones may need different settings
#define I2S_MODE I2S_MODE_STD
#define I2S_BIT_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#define I2S_SLOT_MODE I2S_SLOT_MODE_MONO
#define I2S_SLOT_MASK I2S_STD_SLOT_LEFT

// WiFi settings
const char* ssid     = "your-wifi-ssid";
const char* password = "your-wifi-password";



// Global audio variable declaration for TTS playback
Audio audio;

// Initialize ArduinoGPTChat instance
// Option 1: Use default API key and URL from library (Only support for sTEB remote Compile)
//ArduinoGPTChat gptChat;

// Option 2: Use custom API key and URL (uncomment if using custom configuration)
//OpenAI apiBaseUrl : "https://api.openai.com"
const char* apiKey = "your-openai-api-key";
const char* apiBaseUrl = "your-openai-api-base-url";
ArduinoGPTChat gptChat(apiKey, apiBaseUrl);

// System prompt configuration (modify as needed)
const char* systemPrompt = "Please answer questions briefly, responses should not exceed 30 words. Avoid lengthy explanations, provide key information directly.";



// Button handling variables
bool buttonPressed = false;
bool wasButtonPressed = false;

void setup() {
  // Initialize serial port
  Serial.begin(115200);
  delay(1000); // Give serial port some time to initialize

  Serial.println("\n\n----- Voice Assistant System Starting -----");

  // Initialize boot button
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Connect to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    Serial.print('.');
    delay(1000);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Set I2S output pins (for TTS playback)
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    // Set volume
    audio.setVolume(100);

    // Set system prompt (modify the systemPrompt variable above as needed)
    gptChat.setSystemPrompt(systemPrompt);

    // Initialize recording with microphone pins and I2S configuration
    gptChat.initializeRecording(I2S_MIC_SERIAL_CLOCK, I2S_MIC_LEFT_RIGHT_CLOCK, I2S_MIC_SERIAL_DATA,
                               SAMPLE_RATE, I2S_MODE, I2S_BIT_WIDTH, I2S_SLOT_MODE, I2S_SLOT_MASK);

    Serial.println("\n----- System Ready -----");
    Serial.println("Hold BOOT button to record speech, release to send to ChatGPT");
  } else {
    Serial.println("\nFailed to connect to WiFi. Please check network credentials and retry.");
  }
}

void loop() {
  // Handle audio loop (TTS playback)
  audio.loop();

  // Handle boot button for push-to-talk
  buttonPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW); // LOW when pressed (pull-up resistor)

  // Button pressed - start recording
  if (buttonPressed && !wasButtonPressed && !gptChat.isRecording()) {
    Serial.println("\n----- Recording Started (Hold button) -----");
    if (gptChat.startRecording()) {
      Serial.println("Speak now... (release button to stop)");
      wasButtonPressed = true;
    }
  }
  // Button released - stop recording and process
  else if (!buttonPressed && wasButtonPressed && gptChat.isRecording()) {
    Serial.println("\n----- Recording Stopped -----");
    String transcribedText = gptChat.stopRecordingAndProcess();

    if (transcribedText.length() > 0) {
      Serial.println("\nRecognition result: " + transcribedText);
      Serial.println("\nSending recognition result to ChatGPT...");

      // Send message to ChatGPT
      String response = gptChat.sendMessage(transcribedText);

      if (response != "") {
        Serial.print("ChatGPT: ");
        Serial.println(response);

        // Automatically convert reply to speech
        if (response.length() > 0) {
          Serial.println("Converting text to speech...");
          bool success = gptChat.textToSpeech(response);

          if (success) {
            Serial.println("TTS audio is playing through speaker");
          } else {
            Serial.println("Failed to play TTS audio");
          }
        }
      } else {
        Serial.println("Failed to get ChatGPT response");
      }
    } else {
      Serial.println("Failed to recognize text or an error occurred.");
      Serial.println("Clear speech may not have been detected, please try again.");
    }

    wasButtonPressed = false;
  }
  // Continue recording while button is held
  else if (buttonPressed && gptChat.isRecording()) {
    gptChat.continueRecording();
  }
  // Update button state
  else if (!buttonPressed && !gptChat.isRecording()) {
    wasButtonPressed = false;
  }

  delay(10); // Small delay to prevent CPU overload
}