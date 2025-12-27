#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TFT_eSPI.h>
#include <SPI.h>

// WiFi credentials
const char* ssid = "SomeWifiNetwork";
const char* password = "SmellySocksReek";

// Server details
const char* host = "prod.monitman.com";
const int httpsPort = 443;
const char* endpoint = "/water.php";

// Water meter configuration
#define WATER_METER_PIN 15  //Interrupt pin from water meter
#define PULSES_PER_GALLON 1  // DAE MJ-75n outputs 1 pulse per gallon
#define MAX_GPM 25.0  // Maximum GPM for 3/4" pipe (conservative estimate)
#define MIN_PULSE_INTERVAL_MS (unsigned long)((60.0 / MAX_GPM / PULSES_PER_GALLON) * 1000)  // ~2400ms for 25 GPM
#define LATCH_TIMEOUT_MS 300000  // 5 minutes - if pin stays high this long, it's latched
#define DEBOUNCE_MS 50  // Debounce time in milliseconds
#define POST_INTERVAL_MS 10800000  // 3 hours in milliseconds

// Display configuration
TFT_eSPI tft = TFT_eSPI();

// Volatile variables for ISR
volatile unsigned long pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
volatile bool pulseError = false;
volatile unsigned long lastValidPulseTime = 0;

// Non-volatile state
unsigned long totalGallons = 0;
unsigned long lastPostTime = 0;
bool wifiConnected = false;
bool latchDetected = false;
unsigned long lastDisplayUpdate = 0;
float currentGPM = 0.0;

// Task handle for HTTP posting
TaskHandle_t postTaskHandle = NULL;

// Mutex for protecting shared data
SemaphoreHandle_t dataMutex;

// ISR for water meter pulses
void IRAM_ATTR waterMeterISR() {
    unsigned long currentTime = millis();
    unsigned long timeSinceLastPulse = currentTime - lastPulseTime;
    
    // Debounce check
    if (timeSinceLastPulse < DEBOUNCE_MS) {
        return;
    }
    
    // Flow rate validation
    if (timeSinceLastPulse < MIN_PULSE_INTERVAL_MS && lastPulseTime > 0) {
        pulseError = true;  // Flow too fast, possible error
        return;
    }
    
    // Valid pulse
    pulseCount++;
    lastValidPulseTime = currentTime;
    lastPulseTime = currentTime;
    pulseError = false;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Create mutex
    dataMutex = xSemaphoreCreateMutex();
    
    // Initialize display
    tft.init();
    tft.setRotation(0);  // Adjust rotation as needed
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    
    displayStatus("Initializing...");
    
    // Configure water meter pin
    pinMode(WATER_METER_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(WATER_METER_PIN), waterMeterISR, FALLING);
    
    // Connect to WiFi
    connectWiFi();
    
    // Create posting task on core 0 (Arduino loop runs on core 1)
    xTaskCreatePinnedToCore(
        postingTask,      // Task function
        "PostTask",       // Task name
        8192,             // Stack size
        NULL,             // Parameters
        1,                // Priority
        &postTaskHandle,  // Task handle
        0                 // Core 0
    );
    
    lastPostTime = millis();
    displayStatus("Ready");
    delay(2000);
}

void loop() {
    unsigned long currentTime = millis();
    
    // Check for latch condition
    checkLatchCondition();
    
    // Update display every second
    if (currentTime - lastDisplayUpdate >= 1000) {
        updateDisplay();
        lastDisplayUpdate = currentTime;
    }
    
    // Process pulse counts
    if (xSemaphoreTake(dataMutex, 10) == pdTRUE) {
        if (pulseCount > 0) {
            unsigned long newGallons = pulseCount / PULSES_PER_GALLON;
            totalGallons += newGallons;
            pulseCount = 0;
            
            Serial.printf("Total gallons: %lu\n", totalGallons);
        }
        xSemaphoreGive(dataMutex);
    }
    
    // Calculate current GPM
    calculateGPM();
    
    delay(100);
}

void checkLatchCondition() {
    unsigned long currentTime = millis();
    int pinState = digitalRead(WATER_METER_PIN);
    
    // If pin is LOW (active) for too long after last pulse, it's latched
    if (pinState == LOW && lastValidPulseTime > 0) {
        if (currentTime - lastValidPulseTime > LATCH_TIMEOUT_MS) {
            if (!latchDetected) {
                latchDetected = true;
                Serial.println("WARNING: Reed switch appears to be latched!");
                displayStatus("LATCH DETECTED!");
                delay(3000);
                
                // Detach and reattach interrupt to clear
                detachInterrupt(digitalPinToInterrupt(WATER_METER_PIN));
                delay(100);
                attachInterrupt(digitalPinToInterrupt(WATER_METER_PIN), waterMeterISR, FALLING);
                
                // Reset pulse error flag
                pulseError = false;
                lastValidPulseTime = currentTime;
            }
        }
    } else if (pinState == HIGH) {
        latchDetected = false;
    }
}

void calculateGPM() {
    unsigned long currentTime = millis();
    
    if (lastPulseTime > 0 && (currentTime - lastPulseTime) < 60000) {
        // Calculate GPM based on last pulse interval
        float minutesSinceLastPulse = (currentTime - lastPulseTime) / 60000.0;
        if (minutesSinceLastPulse > 0) {
            currentGPM = (1.0 / PULSES_PER_GALLON) / minutesSinceLastPulse;
        }
    } else {
        currentGPM = 0.0;  // No recent flow
    }
}

void updateDisplay() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 10);
    tft.println("Water Meter");
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 40);
    tft.setTextSize(3);
    tft.printf("%lu", totalGallons);
    tft.setTextSize(2);
    tft.println(" gal");
    
    tft.setCursor(10, 80);
    tft.setTextSize(2);
    tft.printf("Flow: %.1f GPM", currentGPM);
    
    // Display warnings
    if (pulseError) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setCursor(10, 110);
        tft.println("FLOW TOO FAST!");
    } else if (latchDetected) {
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setCursor(10, 110);
        tft.println("LATCH DETECTED");
    }
    
    // WiFi status
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 140);
    if (wifiConnected) {
        tft.println("WiFi: Connected");
    } else {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.println("WiFi: Disconnected");
    }
    
    // Time to next post
    unsigned long timeSincePost = millis() - lastPostTime;
    unsigned long timeToPost = POST_INTERVAL_MS - timeSincePost;
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 170);
    tft.printf("Next post: %lum", timeToPost / 60000);
}

void displayStatus(const char* message) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 100);
    tft.println(message);
}

void connectWiFi() {
    displayStatus("Connecting WiFi...");
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        wifiConnected = false;
        Serial.println("\nWiFi connection failed");
    }
}

void postingTask(void* parameter) {
    while (true) {
        unsigned long currentTime = millis();
        
        if (currentTime - lastPostTime >= POST_INTERVAL_MS) {
            if (WiFi.status() != WL_CONNECTED) {
                connectWiFi();
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                postReading();
                lastPostTime = currentTime;
            }
        }
        
        // Check every 10 seconds
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void postReading() {
    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate validation (use with caution in production)
    
    Serial.println("Connecting to server...");
    
    if (!client.connect(host, httpsPort)) {
        Serial.println("Connection failed");
        wifiConnected = false;
        return;
    }
    
    wifiConnected = true;
    
    // Get current reading safely
    unsigned long gallons;
    float gpm;
    if (xSemaphoreTake(dataMutex, 100) == pdTRUE) {
        gallons = totalGallons;
        gpm = currentGPM;
        xSemaphoreGive(dataMutex);
    } else {
        Serial.println("Failed to acquire mutex");
        return;
    }
    
    // Manual JSON construction (no ArduinoJson)
    String jsonData = "{";
    jsonData += "\"device\":\"water_meter_01\",";
    jsonData += "\"timestamp\":" + String(millis()) + ",";
    jsonData += "\"total_gallons\":" + String(gallons) + ",";
    jsonData += "\"current_gpm\":" + String(gpm, 2) + ",";
    jsonData += "\"latch_detected\":" + String(latchDetected ? "true" : "false") + ",";
    jsonData += "\"flow_error\":" + String(pulseError ? "true" : "false");
    jsonData += "}";
    
    // HTTP POST request
    String request = "POST " + String(endpoint) + " HTTP/1.1\r\n";
    request += "Host: " + String(host) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + String(jsonData.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += jsonData;
    
    client.print(request);
    
    Serial.println("Data posted:");
    Serial.println(jsonData);
    
    // Read response
    unsigned long timeout = millis();
    while (client.available() == 0) {
        if (millis() - timeout > 5000) {
            Serial.println(">>> Client Timeout !");
            client.stop();
            return;
        }
    }
    
    // Read all the lines of the reply from server
    while (client.available()) {
        String line = client.readStringUntil('\r');
        Serial.print(line);
    }
    
    Serial.println("\nClosing connection");
    client.stop();
}
