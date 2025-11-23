#include <Arduino_FreeRTOS.h>     // Библиотека FreeRTOS для Arduino
#include <queue.h>                // Поддержка очередей FreeRTOS для межзадачного взаимодействия
#include <Wire.h>                 // Библиотека I2C (Two Wire Interface) для связи с периферийными устройствами
#include <LiquidCrystal_I2C.h>    // Драйвер I2C LCD-дисплея 16x2
#include <Keypad.h>               // Драйвер матричной клавиатуры 4x4
#include <RTClib.h>               // Библиотека для работы с RTC (Real-Time Clock) на базе DS1307
#include <EEPROM.h>               // Доступ к встроенной энергонезависимой памяти микроконтроллера

// ========================
// Конфигурация аппаратной части: матричная клавиатура, подключённая к цифровым пинам 4–11
// ========================
const byte ROWS = 4;              // Количество строк клавиатуры
const byte COLS = 4;              // Количество столбцов клавиатуры

// Матрица символов, соответствующих физическим клавишам
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Пины микроконтроллера, подключённые к строкам клавиатуры (выходы)
byte rowPins[ROWS] = {8, 9, 10, 11};

// Пины микроконтроллера, подключённые к столбцам клавиатуры (входы с подтяжкой)
byte colPins[COLS] = {4, 5, 6, 7};

// Инициализация объекта клавиатуры с заданной разводкой
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ========================
// Инициализация периферийных устройств
// ========================
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C-адрес 0x27, размер дисплея 16 символов × 2 строки
RTC_DS1307 rtc;                    // Экземпляр часов реального времени, совместимый с модулем HW-111

// ========================
// Структура данных для хранения записей в EEPROM
// Каждая запись содержит временную метку, значение освещённости и текстовую метку
// ========================
struct LogEntry {
  uint16_t year;                  // Год (например, 2025)
  uint8_t month, day;             // Месяц и день (1–12, 1–31)
  uint8_t hour, minute, second;   // Время с точностью до секунды (0–23, 0–59, 0–59)
  float lux;                      // Измеренное значение освещённости в люксах
  char label[20];                 // Текстовая метка, задаваемая пользователем (фиксированная длина — 20 байт, завершается пробелами)
};

// Размер одной записи в байтах (для ATmega328P: 2 + 5×1 + 4 + 20 = 31 → выравнивание до 32 байт)
const uint16_t LOG_ENTRY_SIZE = sizeof(LogEntry); // Фактическое значение: 32 байта из-за выравнивания компилятором
const uint16_t EEPROM_SIZE = 1024;                // Объём EEPROM в ATmega328P (1024 байта)
// Максимальное количество записей, учитывая, что первые 2 байта используются для хранения индекса следующей записи
const uint16_t MAX_LOGS = (EEPROM_SIZE - 2) / LOG_ENTRY_SIZE; // = 31 запись

// ========================
// Глобальные переменные состояния системы
// ========================
QueueHandle_t xLuxQueue;          // Очередь FreeRTOS для передачи текущего значения освещённости от задачи LDR к задаче отображения
volatile bool dateTimeEditMode = false;    // Флаг: активен режим ручной настройки даты и времени
volatile bool labelInputMode = false;      // Флаг: активен режим ввода текстовой метки
volatile bool clearEepromMode = false;     // Флаг: активен режим подтверждения очистки лога
volatile bool viewLogMode = false;         // Флаг: активен режим просмотра последней записи
float lastLuxValue = 0.0f;                 // Последнее принятое значение освещённости для отображения

// ========================
// Объявления функций (прототипы)
// ========================
void TaskDisplay(void *pvParameters);        // Основная задача: управление интерфейсом и отображение данных на LCD
void TaskLDR(void *pvParameters);           // Задача: опрос датчика освещённости (LDR) и расчёт люксов
void TaskRTC(void *pvParameters);           // Задача: периодический вывод текущего времени в последовательный порт
float calculateLuxFromADC(int D);           // Преобразование значения АЦП в физическую величину (люксы)
void enterDateTimeMode();                   // Подрежим: ввод даты и времени через клавиатуру
void enterLabelInputMode();                 // Подрежим: ввод текстовой метки пользователем
void confirmEepromClear();                  // Подрежим: подтверждение сброса лога в EEPROM
void viewLastLog();                         // Подрежим: чтение и отображение последней сохранённой записи
uint16_t getWriteIndex();                   // Чтение текущего индекса записи из первых двух байт EEPROM
void setWriteIndex(uint16_t index);         // Запись нового индекса в первые два байта EEPROM
void saveLogToEEPROM(float lux, const char* label); // Сохранение новой записи в EEPROM

// ========================
// Функция инициализации системы (выполняется один раз при запуске)
// ========================
void setup() {
  // Инициализация последовательного порта для отладки (скорость 115200 бод)
  Serial.begin(115200);

  // Инициализация шины I2C (для RTC и LCD)
  Wire.begin();

  // Инициализация LCD-дисплея и включение подсветки
  lcd.init();
  lcd.backlight();

  // Проверка наличия и инициализация модуля RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    lcd.print("No RTC!");
    // Остановка выполнения при отсутствии часов
    while (1) vTaskDelay(1); // Бесконечная задержка в стиле FreeRTOS
  }

  // Проверка корректности инициализации RTC: если секунды >= 60, значит, время не установлено
  if (rtc.now().second() >= 60) {
    Serial.println("RTC uninitialized. Setting compile time.");
    // Установка времени компиляции как начального значения
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    delay(100); // Кратковременная задержка для стабилизации
  }

  // Проверка корректности индекса записи в EEPROM (защита от сброса памяти)
  uint16_t idx = getWriteIndex();
  if (idx > MAX_LOGS) {
    setWriteIndex(0); // Сброс индекса, если он выходит за допустимые пределы
  }

  // Создание очереди FreeRTOS для передачи значений освещённости (максимум 5 элементов, каждый — float)
  xLuxQueue = xQueueCreate(5, sizeof(float));
  if (!xLuxQueue) {
    Serial.println("Queue create failed");
    // В реальных системах здесь требуется обработка ошибки
  }

  // Создание задач FreeRTOS с указанием размера стека, приоритета и отсутствия дескриптора задачи (NULL)
  xTaskCreate(TaskDisplay, "Display", 256, NULL, 2, NULL); // Приоритет выше — актуализация интерфейса
  xTaskCreate(TaskLDR, "LDR", 128, NULL, 1, NULL);        // Низкий приоритет — фоновый опрос датчика
  xTaskCreate(TaskRTC, "RTC_Update", 128, NULL, 1, NULL); // Низкий приоритет — логирование времени

  // Запуск планировщика задач FreeRTOS (после этого функция loop() не вызывается)
  vTaskStartScheduler();
}

// Функция loop() не используется в FreeRTOS-приложениях, так как управление передаётся планировщику
void loop() {
  // Не используется
}

// -----------------------
// Задача отображения состояния системы и обработки пользовательского ввода
// Реализует конечный автомат с режимами: основной, ввод даты, ввод метки, подтверждение очистки, просмотр лога
// -----------------------
void TaskDisplay(void *pvParameters) {
  float lux;                    // Буфер для приёма данных из очереди
  bool showLuxMode = true;      // Флаг: отображать ли основной экран (освещённость + время)

  for (;;) { // Бесконечный цикл задачи (обязательно в FreeRTOS)
    // Приём последнего значения освещённости из очереди без блокировки (таймаут = 0 тиков)
    if (xQueueReceive(xLuxQueue, &lux, 0) == pdPASS) {
      lastLuxValue = lux; // Обновление глобального значения для отображения
    }

    // Опрос клавиатуры
    char key = keypad.getKey();

    // Обработка нажатия клавиши только в основном режиме (не внутри подрежимов)
    if (key && !dateTimeEditMode && !labelInputMode && !clearEepromMode && !viewLogMode) {
      switch (key) {
        case 'A': // Вход в режим настройки даты и времени
          dateTimeEditMode = true;
          showLuxMode = false;
          enterDateTimeMode(); // Вызов подрежима
          showLuxMode = true;
          break;
        case 'B': // Вход в режим ввода текстовой метки и сохранения записи
          labelInputMode = true;
          showLuxMode = false;
          enterLabelInputMode();
          showLuxMode = true;
          break;
        case 'C': // Вход в режим подтверждения очистки лога
          clearEepromMode = true;
          showLuxMode = false;
          confirmEepromClear();
          showLuxMode = true;
          break;
        case 'D': // Просмотр последней записи в логе
          viewLogMode = true;
          showLuxMode = false;
          viewLastLog();
          showLuxMode = true;
          break;
      }
    }

    // Отображение основного экрана (если не в подрежиме)
    if (showLuxMode) {
      DateTime now = rtc.now(); // Получение текущего времени от RTC
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Lux: ");
      lcd.print(lastLuxValue, 1); // Вывод с точностью до одного знака после запятой
      lcd.setCursor(0, 1);
      // Форматирование времени с ведущими нулями (HH:MM)
      lcd.print(now.hour() < 10 ? "0" : "");
      lcd.print(now.hour());
      lcd.print(":");
      lcd.print(now.minute() < 10 ? "0" : "");
      lcd.print(now.minute());
    }

    // Задержка 100 мс между итерациями для снижения нагрузки на процессор
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// -----------------------
// Подрежим: ввод пользовательской метки (до 20 символов)
// Поддерживает ввод цифр, букв A–Z, пробела, отмену последнего символа (*), подтверждение (#)
// -----------------------
void enterLabelInputMode() {
  char label[21] = {0};         // Буфер для ввода (20 символов + терминатор)
  uint8_t pos = 0;              // Текущая позиция ввода
  bool confirmed = false;       // Флаг завершения ввода

  lcd.clear();
  lcd.print("Enter label:");

  // Цикл ввода до подтверждения или заполнения 20 символов
  while (!confirmed && pos < 20) {
    lcd.setCursor(0, 1);
    lcd.print(label);           // Отображение текущего содержимого
    lcd.print("_");             // Курсор

    char key = keypad.getKey();
    if (key) {
      if (key == '#') {
        confirmed = true;       // Подтверждение ввода
      } else if (key == '*') {
        if (pos > 0) {          // Отмена последнего символа
          pos--;
          label[pos] = '\0';
        }
      } else if ((key >= '0' && key <= '9') ||
                 (key >= 'A' && key <= 'Z') ||
                 key == ' ') {
        label[pos] = key;       // Добавление допустимого символа
        pos++;
        label[pos] = '\0';
      }
      vTaskDelay(pdMS_TO_TICKS(150)); // Антидребезг
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Дополнение метки пробелами до 20 символов (для корректного хранения в EEPROM)
  while (pos < 20) {
    label[pos] = ' ';
    pos++;
  }
  label[20] = '\0';

  // Сохранение записи в энергонезависимую память
  saveLogToEEPROM(lastLuxValue, label);
  labelInputMode = false;       // Выход из подрежима
}

// -----------------------
// Сохранение структуры LogEntry в EEPROM с учётом текущего индекса записи
// -----------------------
void saveLogToEEPROM(float lux, const char* label) {
  uint16_t idx = getWriteIndex();       // Получение индекса следующей записи
  if (idx >= MAX_LOGS) {
    lcd.clear();
    lcd.print("Log FULL!");
    delay(1000);
    return; // Прерывание при переполнении EEPROM
  }

  // Формирование записи
  DateTime now = rtc.now();
  LogEntry entry;
  entry.year = now.year();
  entry.month = now.month();
  entry.day = now.day();
  entry.hour = now.hour();
  entry.minute = now.minute();
  entry.second = now.second();
  entry.lux = lux;
  strncpy(entry.label, label, 20);      // Копирование метки с ограничением длины

  // Адрес записи: смещение 2 байта (для индекса) + idx × размер записи
  uint16_t addr = 2 + idx * LOG_ENTRY_SIZE;
  uint8_t* p = (uint8_t*)&entry;        // Преобразование структуры в массив байтов

  // Побайтовая запись в EEPROM (стандартная функция Arduino)
  for (uint16_t i = 0; i < LOG_ENTRY_SIZE; i++) {
    EEPROM.write(addr + i, p[i]);
  }

  // Обновление индекса следующей записи
  setWriteIndex(idx + 1);

  // Вывод отладочной информации в Serial
  Serial.print("Saved: ");
  Serial.print(label);
  Serial.print(" | Lux=");
  Serial.print(lux, 1);
  Serial.print(" | idx=");
  Serial.println(idx);

  // Визуальное подтверждение на LCD
  lcd.clear();
  lcd.print("Saved!");
  delay(800);
}

// -----------------------
// Подрежим: подтверждение очистки лога (сброс индекса в EEPROM)
// -----------------------
void confirmEepromClear() {
  lcd.clear();
  lcd.print("Clear log?");
  lcd.setCursor(0, 1);
  lcd.print("#=Yes *=No");

  bool confirmed = false;
  bool done = false;

  // Ожидание нажатия # (да) или * (нет)
  while (!done) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') {
        confirmed = true;
        done = true;
      } else if (key == '*') {
        confirmed = false;
        done = true;
      }
      vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (confirmed) {
    // Сброс индекса записи на 0 (логически — очистка лога)
    setWriteIndex(0);
    EEPROM.write(0, 0);
    EEPROM.write(1, 0);
    Serial.println("EEPROM log cleared (index reset).");
    lcd.clear();
    lcd.print("Cleared!");
    delay(800);
  } else {
    lcd.clear();
    lcd.print("Cancelled");
    delay(500);
  }

  clearEepromMode = false;
}

// -----------------------
// Подрежим: чтение и отображение последней записи из EEPROM
// -----------------------
void viewLastLog() {
  uint16_t idx = getWriteIndex();

  if (idx == 0) {
    lcd.clear();
    lcd.print("No logs");
    delay(1000);
    viewLogMode = false;
    return;
  }

  // Адрес последней записи
  uint16_t addr = 2 + (idx - 1) * LOG_ENTRY_SIZE;
  LogEntry entry;
  uint8_t* p = (uint8_t*)&entry;

  // Побайтовое чтение из EEPROM
  for (uint16_t i = 0; i < LOG_ENTRY_SIZE; i++) {
    p[i] = EEPROM.read(addr + i);
  }

  // Экран 1: текстовая метка (2 секунды)
  lcd.clear();
  lcd.print("Label:");
  lcd.setCursor(0, 1);
  char displayLabel[21];
  strncpy(displayLabel, entry.label, 20);
  displayLabel[20] = '\0';
  lcd.print(displayLabel);
  delay(2000);

  // Экран 2: освещённость, дата (DD/MM/YY) и время (HH:MM:SS) (3 секунды)
  lcd.clear();
  lcd.print("L:");
  lcd.print(entry.lux, 1);
  lcd.print(" ");
  lcd.print(entry.day < 10 ? "0" : "");
  lcd.print(entry.day);
  lcd.print("/");
  lcd.print(entry.month < 10 ? "0" : "");
  lcd.print(entry.month);
  lcd.print("/");
  lcd.print(entry.year - 2000); // Отображение года в формате YY
  lcd.setCursor(0, 1);
  lcd.print(entry.hour < 10 ? "0" : "");
  lcd.print(entry.hour);
  lcd.print(":");
  lcd.print(entry.minute < 10 ? "0" : "");
  lcd.print(entry.minute);
  lcd.print(":");
  lcd.print(entry.second < 10 ? "0" : "");
  lcd.print(entry.second);
  delay(3000);

  viewLogMode = false;
}

// -----------------------
// Подрежим: интерактивный ввод даты и времени с валидацией по диапазонам
// -----------------------
void enterDateTimeMode() {
  // Буфер для вводимых значений: [секунды, минуты, часы, день, месяц, год]
  uint16_t input[6] = {0};
  // Подсказки для пользователя
  const char* prompts[] = {"Sec", "Min", "Hour", "Day", "Month", "Year"};
  // Максимально допустимые значения для каждого поля
  const uint8_t maxVals[] = {59, 59, 23, 31, 12, 2099};

  // Инициализация значений текущим временем для редактирования
  DateTime now = rtc.now();
  input[0] = now.second();
  input[1] = now.minute();
  input[2] = now.hour();
  input[3] = now.day();
  input[4] = now.month();
  input[5] = now.year();

  // Последовательный ввод каждого компонента времени
  for (int i = 0; i < 6; i++) {
    bool confirmed = false;
    uint16_t val = input[i];
    while (!confirmed) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Set ");
      lcd.print(prompts[i]); // Текущее поле
      lcd.setCursor(0, 1);
      lcd.print(val);        // Текущее значение

      char key = keypad.getKey();
      if (key) {
        if (key >= '0' && key <= '9') {
          val = val * 10 + (key - '0'); // Накопление числа
          if (val > maxVals[i]) val = maxVals[i]; // Ограничение сверху
        } else if (key == '#') {
          input[i] = val;      // Подтверждение значения
          confirmed = true;
        } else if (key == '*') {
          val = 0;             // Сброс значения
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  // Установка нового времени в RTC
  rtc.adjust(DateTime(input[5], input[4], input[3], input[2], input[1], input[0]));
  dateTimeEditMode = false;
}

// -----------------------
// Задача периодического опроса аналогового датчика освещённости (LDR на пине A0)
// -----------------------
void TaskLDR(void *pvParameters) {
  for (;;) {
    int adc = analogRead(A0);           // Чтение 10-битного значения АЦП (0–1023)
    float lux = calculateLuxFromADC(adc); // Преобразование в люксы
    // Перезапись очереди (гарантируется наличие актуального значения)
    xQueueOverwrite(xLuxQueue, &lux);
    vTaskDelay(pdMS_TO_TICKS(500));     // Опрос каждые 500 мс
  }
}

// -----------------------
// Задача логирования текущего времени в последовательный порт каждую секунду
// -----------------------
void TaskRTC(void *pvParameters) {
  for (;;) {
    DateTime now = rtc.now();
    // Формат: YYYY/MM/DD HH:MM:SS
    Serial.print("RTC: ");
    Serial.print(now.year(), DEC);
    Serial.print('/');
    Serial.print(now.month(), DEC);
    Serial.print('/');
    Serial.print(now.day(), DEC);
    Serial.print(' ');
    Serial.print(now.hour(), DEC);
    Serial.print(':');
    Serial.print(now.minute(), DEC);
    Serial.print(':');
    Serial.println(now.second(), DEC);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Период — 1 секунда
  }
}

// -----------------------
// Эмпирическая формула преобразования значения АЦП в освещённость (в люксах)
// Основана на калибровке фоторезистора (LDR) с делителем напряжения
// -----------------------
float calculateLuxFromADC(int D) {
  // Экспоненциальная аппроксимация: L = a * exp(-b * D)
  return 1096.7f * expf(-0.007f * (float)D);
}

// -----------------------
// Функции управления указателем записи в EEPROM
// Индекс хранится в первых двух байтах (младший байт — по адресу 0, старший — по адресу 1)
// -----------------------
uint16_t getWriteIndex() {
  return (EEPROM.read(1) << 8) | EEPROM.read(0); // Little-endian формат
}

void setWriteIndex(uint16_t index) {
  EEPROM.write(0, index & 0xFF);        // Запись младшего байта
  EEPROM.write(1, (index >> 8) & 0xFF); // Запись старшего байта
}