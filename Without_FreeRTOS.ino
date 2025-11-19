#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <math.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;

unsigned long lastLuxMeasure = 0;
unsigned long lastLCDUpdate  = 0;
unsigned long lastRTCPrint   = 0;

float currentLux = 0.0;

float calculateLuxFromADC(int adcValue) {
    return (adcValue + 101.0f) / 3.0f;
}

void setup() {
    Serial.begin(9600);
    Wire.begin();

    lcd.init();
    lcd.backlight();

    if (!rtc.begin()) {
        lcd.print("RTC ERR");
        while (1);
    }

    lcd.clear();
    lcd.print("System OK");
    delay(1000);
}

void loop() {

    if (millis() - lastLuxMeasure >= 200) {
        lastLuxMeasure = millis();
        int adc = analogRead(A0);
        currentLux = calculateLuxFromADC(adc);
    }

    if (millis() - lastLCDUpdate >= 300) {
        lastLCDUpdate = millis();

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Lux: ");
        lcd.print(currentLux, 1);

        lcd.setCursor(0, 1);
        lcd.print("NO RTOS");
    }

    if (millis() - lastRTCPrint >= 1000) {
        lastRTCPrint = millis();

        DateTime t = rtc.now();
        Serial.print("Time: ");
        Serial.print(t.hour());
        Serial.print(":");
        Serial.print(t.minute());
        Serial.print(":");
        Serial.println(t.second());
    }
}
