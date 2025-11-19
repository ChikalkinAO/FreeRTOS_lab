#include <Arduino_FreeRTOS.h>
#include <queue.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <math.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
QueueHandle_t xLuxQueue;

float calculateLuxFromADC(int adcValue) {
    return (adcValue + 101.0f) / 3.0f;
}

void vTaskMeasureLuminosity(void *pvParameters);
void vTaskDisplay(void *pvParameters);
void vTaskRTC(void *pvParameters);

void setup() {
    Serial.begin(9600);
    Wire.begin();
    lcd.init();
    lcd.backlight();
    if (!rtc.begin()) {
        lcd.print("RTC ERR");
        while (1);
    }
    xLuxQueue = xQueueCreate(5, sizeof(float));
    if (xLuxQueue == NULL) {
        lcd.print("Q ERR");
        while (1);
    }
    xTaskCreate(vTaskMeasureLuminosity, "Measure", 128, NULL, 3, NULL);
    xTaskCreate(vTaskDisplay,           "Display", 128, NULL, 2, NULL);
    xTaskCreate(vTaskRTC,               "RTC",     128, NULL, 1, NULL);
    vTaskStartScheduler();
}

void loop() {}

void vTaskMeasureLuminosity(void *pvParameters) {
    for (;;) {
        int adc = analogRead(A0);
        float lux = calculateLuxFromADC(adc);
        xQueueSend(xLuxQueue, &lux, pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void vTaskDisplay(void *pvParameters) {
    float lux;
    for (;;) {
        if (xQueueReceive(xLuxQueue, &lux, portMAX_DELAY) == pdPASS) {
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Lux:");
            lcd.print(lux, 1);
            lcd.setCursor(0, 1);
            lcd.print("RTOS OK");
        }
    }
}

void vTaskRTC(void *pvParameters) {
    for (;;) {
        DateTime t = rtc.now();
        Serial.print("Time: ");
        Serial.print(t.hour(), DEC);
        Serial.print(":");
        Serial.print(t.minute(), DEC);
        Serial.print(":");
        Serial.println(t.second(), DEC);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}