// Copyright © m2ag.labs (marc@m2ag.net). All rights reserved.
// Licensed under the MIT license.

#include <Arduino.h>
#include <RunningAverage.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_CCS811.h>
#include <Adafruit_PM25AQI.h>
#include <Adafruit_DotStar.h>
#include <Adafruit_SleepyDog.h>
#include <Arduino_JSON.h>
#include <AceButton.h>
#include <ArduinoUniqueID.h>
#include <FlashStorage.h>
//Included here. 
#include "CmdMessenger.h" 

using namespace ace_button;
// There is only one pixel on the board
#define VERSION "0.0.1";
#define NUMPIXELS 1
//SPI for dotstar
#define DATAPIN 7
#define CLOCKPIN 8

#define BUTTON_PIN 3
#define BUZZER_PIN 1
#define LED_PIN 13

#define ALERT_MAX_DURATION 60000
#define ALERT_TONE_SEPERATOR 1000

typedef struct ConfigOffsets
{
  boolean valid;
  double temperature;
  double eco2;
  double pm2;
  double pressure;
  double humidity;
} ConfigOffsets;

typedef struct Colors
{
  uint32_t GREEN;
  uint32_t YELLOW;
  uint32_t ORANGE;
  uint32_t RED;
  uint32_t PURPLE;
  uint32_t MAROON;
} Colors;

enum Cmds
{
  poll_all,
  report_all,
  set_offsets,
  error,
}; //For cmdmessenger

FlashStorage(config_storage, ConfigOffsets);
ConfigOffsets offsets = {false, 0, 0, 0, 0, 0};
AceButton button(BUTTON_PIN);
Adafruit_CCS811 ccs;
Adafruit_BME280 bme;
Adafruit_PM25AQI aqi = Adafruit_PM25AQI();
Adafruit_DotStar strip(NUMPIXELS, DATAPIN, CLOCKPIN, DOTSTAR_BGR);
Colors COLOR = {strip.Color(31, 225, 5),
                strip.Color(255, 255, 10),
                strip.Color(253, 104, 8),
                strip.Color(251, 0, 7),
                strip.Color(123, 40, 133),
                strip.Color(106, 0, 71)};

/* Initialize CmdMessenger -- this should match PyCmdMessenger instance */
const long BAUD_RATE = 57600;
CmdMessenger c = CmdMessenger(Serial, ',', ';', '/');
//Running averages
RunningAverage pm25_RA(150); //measured every 5 seconds (ish)
RunningAverage eco2_RA(75);  //measured every 10 seconds (ish)
//Buzzer and alert property
volatile boolean ALERTING = false;
uint8_t co2_ALERT_LEVEL = 1; //start at 1 and go to 6
uint8_t pm2_ALERT_LEVEL = 1; //start at 1 and go to 6
boolean ALERT_ENABLED = false;
uint32_t ALERT_START = 0; //Allow alert to only sound for one minute.
uint32_t ALERT_LAST = 0;
boolean IS_CO2 = true; //Flag for reporting
uint8_t BRIGHTNESS = 90;
JSONVar data;                    //to be sent back
unsigned long DELAY_TIME = 1000; //This is the run loop timer

//Forward declares
void buttonEvent(AceButton *, uint8_t, uint8_t);

//Cmd messneger
void attach_callbacks(void);
void on_poll_all(void);
void on_set_offsets(void);
void on_unknown_command(void);
//value Offset support
String padString(String, u_int8_t);
void storedData(boolean);
//for an entertaining startup delay
void rainbow(int);
//resets watchdog, services cmdmessenger and sounds alerts
void sensorDelay(uint32_t);
//Alert stuff
void serviceMessage(void);
void setAlert(int8_t);
void soundAlert(void);
void setAlertLevel(void);
void shouldAlert(void);
//Sensors 
void getAQIData(void);
void getBMEData(void);
void getCCSData(void); 


void setup()
{ 
  //Get the id string for this processor
  String id = "";
  for (int i = 0; i < UniqueIDsize; i++)
  {
    if (i > 10)
    {
      id += String(UniqueID[i], HEX);
    }
  }
  data["device_id"] = id;
  data["version"] = VERSION; 
  data["message"] = "device boot";
  storedData(true); // true = load
  Serial.begin(BAUD_RATE);
  strip.begin(); // Initialize pins for output
  strip.setBrightness(BRIGHTNESS);
  strip.setPixelColor(0, COLOR.PURPLE);
  strip.show();
  Watchdog.enable(5000);
  attach_callbacks();
  rainbow(13); // delay for sensors to boot
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ButtonConfig *buttonConfig = button.getButtonConfig();
  button.setEventHandler(buttonEvent);
  buttonConfig->setFeature(ButtonConfig::kFeatureLongPress);
  buttonConfig->setFeature(ButtonConfig::kFeatureDoubleClick);

  // Init and test for components

  if (!bme.begin(0x76))
  {
    data["message"] = "no bme280 found";
  }

  ccs.setDriveMode(CCS811_DRIVE_MODE_10SEC);
  ccs.disableInterrupt();
  if (!ccs.begin())
  {
    data["message"] = "no ccs811 found";
  }
  // Wait for the sensor to be ready
  int count = 0;
  while (!ccs.available())
  {
    sensorDelay(100);
    count++;
    if (count > 20)
    {
      data["message"] = "ccs811 never became available";
      break;
    }
  };

  if (!aqi.begin_I2C())
  {
    data["message"] = "no AQI sensor found";
  }
}

void loop()
{
  //5 seconds solid on -- for co2
  IS_CO2 = true;
  setAlertLevel();
  data["alerting"] = ALERTING;
  data["alert_enabled"] = ALERT_ENABLED;
  sensorDelay(DELAY_TIME);

  getBMEData();
  sensorDelay(DELAY_TIME);

  getCCSData();
  sensorDelay(DELAY_TIME);

  getAQIData();
  sensorDelay(DELAY_TIME);

  sensorDelay(DELAY_TIME);
  IS_CO2 = false;
  setAlertLevel(); //change to pm2
  data["alerting"] = ALERTING;
  data["alert_enabled"] = ALERT_ENABLED;
  strip.setBrightness(0);
  strip.show();
  sensorDelay(DELAY_TIME);

  getBMEData();
  strip.setBrightness(BRIGHTNESS);
  strip.show();
  sensorDelay(DELAY_TIME);

  //getCCSData(); ccs only every 10 seconds
  strip.setBrightness(0);
  strip.show();
  sensorDelay(DELAY_TIME);

  getAQIData();
  strip.setBrightness(BRIGHTNESS);
  strip.show();
  sensorDelay(DELAY_TIME);

  strip.setBrightness(0);
  strip.show();
  sensorDelay(DELAY_TIME);

  strip.setBrightness(BRIGHTNESS);
  strip.show();
}

/*
  Function to delay between sensor reads -- while delaying
  other featres are managed
  Wait in ms.
*/
void sensorDelay(uint32_t wait)
{
  uint32_t time_to_go = millis() + wait;
  do
  {
    button.check();
    c.feedinSerialData();
    Watchdog.reset();
    shouldAlert();
  } while (millis() < time_to_go);
}

/* config functions   */ 

void storedData(boolean read)
{
  if (read)
  {
    offsets = config_storage.read();
    if (offsets.valid == false)
    {
      offsets.eco2 = 0;
      offsets.pm2 = 0;
      offsets.temperature = 0;
      offsets.pressure = 0;
      offsets.humidity = 0;
      offsets.valid = true;
      config_storage.write(offsets);
    }
  }
  else
  {
    config_storage.write(offsets);
  }
  //send back to host in string format
  //String format:  "t-1h00p00b0000e0000" where b is pressure and p is pm2
  String _offsets = "t";
  _offsets += padString(String(offsets.temperature, 10), 4);
  _offsets += "h";
  _offsets += padString(String(offsets.humidity, 10), 4);
  _offsets += "p";
  _offsets += padString(String(offsets.pm2, 10), 4);
  _offsets += "b";
  _offsets += padString(String(offsets.pressure, 10), 6);
  _offsets += "e";
  _offsets += padString(String(offsets.eco2, 10), 6);
  data["offsets"] = _offsets;
}


/* Command messenger fuctions */

void on_poll_all(void)
{
  c.sendCmd(report_all, JSON.stringify(data));
}


void on_unknown_command(void)
{
  c.sendCmd(error, "Command without callback.");
}

void on_set_offsets(void)
{
  //String format:  "t-1.3h0000p0000b000000e000000" where b is pressure and p is pm2
  JSONVar cg;
  String config = "";
  String tag = "";

  for (int j = 0; j < 3; j++)
  {
    tag = c.readStringArg();
    config = c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    cg[tag] = config.toDouble();
    config = "";
  }

  for (int j = 0; j < 2; j++)
  {
    tag = c.readStringArg();
    config = c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    config += c.readStringArg();
    cg[tag] = config.toDouble();
    config = "";
  }

  boolean changed = 0;
  if (cg.hasOwnProperty("t") && offsets.temperature != (double)cg["t"])
  {
    offsets.temperature = (double)cg["t"];
    changed = true;
  }
  if (cg.hasOwnProperty("h") && offsets.humidity != (double)cg["h"])
  {
    offsets.humidity = (double)cg["h"];
    changed = true;
  }
  if (cg.hasOwnProperty("b") && offsets.pressure != (double)cg["b"])
  {
    offsets.pressure = (double)cg["b"];
    changed = true;
  }
  if (cg.hasOwnProperty("e") && offsets.eco2 != (double)cg["e"])
  {
    offsets.eco2 = (double)cg["e"];
    changed = true;
  }
  if (cg.hasOwnProperty("p") && offsets.pm2 != (double)cg["p"])
  {
    offsets.pm2 = (double)cg["p"];
    changed = true;
  }
  if (changed)
  {
    storedData(false);
  }
}

/* Attach callbacks for CmdMessenger commands */
void attach_callbacks(void)
{
  c.attach(poll_all, on_poll_all);
  c.attach(set_offsets, on_set_offsets);
  c.attach(on_unknown_command);
}

/* alert functions */

void setAlert(int8_t level)
{
  int8_t ALERT_LEVEL = pm2_ALERT_LEVEL;

  if (IS_CO2)
  {
    ALERT_LEVEL = co2_ALERT_LEVEL;
  }

  if (level > 2 && level > ALERT_LEVEL)
  {
    ALERTING = true;
  }
  switch (level)
  {
  case 1:
    ALERT_LEVEL = 1;
    strip.setPixelColor(0, COLOR.GREEN);
    break;
  case 2:
    ALERT_LEVEL = 2;
    strip.setPixelColor(0, COLOR.YELLOW);
    break;
  case 3:
    ALERT_LEVEL = 3;
    strip.setPixelColor(0, COLOR.ORANGE);
    break;
  case 4:
    ALERT_LEVEL = 4;
    strip.setPixelColor(0, COLOR.RED);
    break;
  case 5:
    ALERT_LEVEL = 5;
    strip.setPixelColor(0, COLOR.PURPLE);
    break;
  case 6:
    ALERT_LEVEL = 6;
    strip.setPixelColor(0, COLOR.MAROON);
    break;
  }
  //Starting at 1 simplifies detecting alert level changes
  //and corresponds to epa's system of colors and levels.
  //Start at 0 out side of the arduino

  if (IS_CO2)
  {
    co2_ALERT_LEVEL = ALERT_LEVEL;
    data["eco2_alert_level"] = (int)ALERT_LEVEL - 1;
  }
  else
  {
    pm2_ALERT_LEVEL = ALERT_LEVEL;
    data["pm25_alert_level"] = (int)ALERT_LEVEL - 1;
  }
  strip.show();
}

/*
    pm2.5 
    green = 0 to 12.0
    yellow = 12.1 to 35.4
    orange = 35.5 to 55.4
    red = 55.5 ti 150.4
    purple = 150.5 to 250.4
    maroon = 250.5 to 500.5 > 250.5 
*/

/* 
   tVoc
   green < 1000
   yellow < 1500
   orange < 2000
   red = < 3000
   purple < 5000
   maroon greater than 5000
*/

void setAlertLevel()
{
  //TODO: flash for particles, steady for co2.

  // https://blissair.com/what-is-pm-2-5.htm for pm25 levels
  //
  if (IS_CO2)
  {
    int t_level = (int)data["eco2_avg"];
    if (t_level < 1000)
    {
      setAlert(1);
    }
    else if (t_level < 1500)
    {
      setAlert(2);
    }
    else if (t_level < 2000)
    {
      setAlert(3);
    }
    else if (t_level < 3000)
    {
      setAlert(4);
    }
    else if (t_level < 5000)
    {
      setAlert(5);
    }
    else
    {
      setAlert(6);
    }
  }
  else
  {
    double p_level = data["pm25_avg"];
    if (p_level < 12.1)
    {
      setAlert(1);
    }
    else if (p_level < 35.4)
    {
      setAlert(2);
    }
    else if (p_level < 55.4)
    {
      setAlert(3);
    }
    else if (p_level < 150.4)
    {
      setAlert(4);
    }
    else if (p_level < 250.4)
    {
      setAlert(5);
    }
    else
    {
      setAlert(6);
    }
  }
}

void soundAlert()
{
  if (ALERTING && ALERT_ENABLED)
  {
    tone(BUZZER_PIN, 220, 125);
    delay(125);
    button.check();
    if (!ALERTING)
      return;
    tone(BUZZER_PIN, 550, 125);
    delay(125);
    button.check();
    if (!ALERTING)
      return;
    tone(BUZZER_PIN, 220, 125);
    delay(125);
    button.check();
    if (!ALERTING)
      return;
    tone(BUZZER_PIN, 550, 125);
  }
}

void shouldAlert()
{
  //We only want the allert tone to play once per second or so.
  if (ALERTING)
  {
    if (ALERT_LAST == 0)
    {
      ALERT_LAST = millis();
      ALERT_START = millis();
      soundAlert();
    }
    else if (millis() - ALERT_LAST > ALERT_TONE_SEPERATOR)
    {
      ALERT_LAST = millis();
      soundAlert();
    }
    //If alert has been sounding for too long turn it off.
    if (millis() - ALERT_START > ALERT_MAX_DURATION)
    {
      ALERTING = false;
    }
  }
  else
  {
    if (ALERT_START > 0)
    {
      ALERT_START = 0;
      ALERT_LAST = 0;
    }
  }
}


/* the button */ 

void buttonEvent(AceButton * /* button */, uint8_t eventType, uint8_t /* buttonState */)
{
  switch (eventType)
  {
  case AceButton::kEventPressed:
    if (ALERTING)
    {
      ALERTING = false;
      noTone(BUZZER_PIN);
    }
    tone(BUZZER_PIN, 40, 100);
    digitalWrite(LED_PIN, HIGH);
    break;
  case AceButton::kEventReleased:
    digitalWrite(LED_PIN, LOW);
    strip.setBrightness(BRIGHTNESS);
    strip.show();
    break;
  case AceButton::kEventLongPressed:
    BRIGHTNESS += 10;
    if (BRIGHTNESS > 100)
    {
      BRIGHTNESS = 0;
    }
    strip.setBrightness(BRIGHTNESS);
    strip.show();
    break;
  case AceButton::kEventDoubleClicked:
    ALERT_ENABLED = !ALERT_ENABLED;
    if (ALERT_ENABLED)
    {
      tone(BUZZER_PIN, 100, 100);
      delay(125);
      tone(BUZZER_PIN, 330, 250);
    }
    else
    {
      tone(BUZZER_PIN, 330, 100);
      delay(125);
      tone(BUZZER_PIN, 100, 250);
    }
    break;
  }
}


/*  get the sensor data */ 

/*
    This is where the sensor data is collected. The current
    readings are stored in a json object. The json object is
    returned when requested via command messnger
*/
void getBMEData()
{
  data["temperature"] = bme.readTemperature() + (float)offsets.temperature;
  data["pressure"] = (bme.readPressure() / 100.0) + (float)offsets.pressure;
  data["humidity"] = bme.readHumidity() + (float)offsets.humidity;
  //set the ccs compesation here too:
  ccs.setEnvironmentalData((int)data["humidity"], (double)data["temperature"]);
}

void getCCSData()
{
  if (ccs.available())
  {
    if (!ccs.readData())
    {
      float value = ccs.geteCO2() + (uint16_t)offsets.eco2;
      if (value < 400.0F)
      {
        value = 400.0F;
      }
      eco2_RA.addValue(value);
      data["eco2"] = (uint32_t)value;
      data["eco2_avg"] = (uint32_t)eco2_RA.getAverage();
      //data["eco2_min"] = (uint32_t)eco2_RA.getMin();
      //data["eco2_max"] = (uint32_t)eco2_RA.getMax();
    }
    else
    {
      data["eco2"] = -1;
      data["eco2_avg"] = -1;
    }
  }
}

void getAQIData()
{
  PM25_AQI_Data _data;

  if (aqi.read(&_data))
  {
    float value = _data.pm25_env + (uint16_t) offsets.pm2;
    pm25_RA.addValue(value);
    data["pm25"] = (uint32_t)value;
    data["pm25_avg"] = (uint32_t)pm25_RA.getAverage();
    //data["pm25_min"] = (uint32_t)pm25_RA.getMin();
    //data["pm25_max"] = (uint32_t)pm25_RA.getMax();
  }
  else
  {
    data["pm25"] = -1;
    data["pm25_avg"] = -1;
  }
}

// Rainbow cycle along whole strip. Pass delay time (in ms) between frames.
// Stolen from adafruit. 
void rainbow(int wait)
{
  uint32_t time_to_go = 0;
  // Hue of first pixel runs 5 complete loops through the color wheel.
  // Color wheel has a range of 65536 but it's OK if we roll over, so
  // just count from 0 to 5*65536. Adding 256 to firstPixelHue each time
  // means we'll make 5*65536/256 = 1280 passes through this outer loop:
  for (long firstPixelHue = 0; firstPixelHue < 5 * 65536; firstPixelHue += 256)
  {
    for (int i = 0; i < strip.numPixels(); i++)
    { // For each pixel in strip...
      // Offset pixel hue by an amount to make one full revolution of the
      // color wheel (range of 65536) along the length of the strip
      // (strip.numPixels() steps):
      int pixelHue = firstPixelHue + (i * 65536L / strip.numPixels());
      // strip.ColorHSV() can take 1 or 3 arguments: a hue (0 to 65535) or
      // optionally add saturation and value (brightness) (each 0 to 255).
      // Here we're using just the single-argument hue variant. The result
      // is passed through strip.gamma32() to provide 'truer' colors
      // before assigning to each pixel:
      strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
    }
    strip.show(); // Update strip with new contents
    time_to_go = millis() + wait;
    do
    {
      c.feedinSerialData();
      Watchdog.reset();
    } while (millis() < time_to_go);
  }
}

String padString(String value, u_int8_t pad)
{
  int escape = 4;
  if(value.length()< pad){
    while (value.length() < pad)
    {
      value = "0" + value;
      escape--;
      if (escape < 0)
      {
        break;
      }
    }
  } else {
    value = value.substring(0, pad);
  }
  return value;
}


