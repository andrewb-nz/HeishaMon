#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DNSServer.h>

#include <ArduinoJson.h>

#include "webfunctions.h"
#include "decode.h"
#include "commands.h"

// maximum number of seconds between resets that
// counts as a double reset
#define DRD_TIMEOUT 0.1

// address to the block in the RTC user memory
// change it if it collides with another usage
// of the address block
#define DRD_ADDRESS 0x00

#define WAITTIME 5000 // wait before next data read from heatpump
#define SERIALTIMEOUT 2000 // wait until all 203 bytes are read, must not be too long to avoid blocking the code

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

// Default settings if config does not exists
const char* update_path = "/firmware";
const char* update_username = "admin";
char wifi_hostname[40] = "HeishaMon";
char ota_password[40] = "heisha";
char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_username[40];
char mqtt_password[40];

bool sending = false; // mutex for sending data
bool mqttcallbackinprogress = false; // mutex for processing mqtt callback
unsigned long nexttime = 0;
unsigned long readtime = 0;


//useful for debugging, outputs info to a separate mqtt topic
bool outputMqttLog = true;
//toggle to dump received hex data in log
bool outputHexDump = false;
// toggle to dump  extralog to serial1
// *needs implementation in this code and in the webpage*
bool outputSerial1 = true;

//1wire enabled?
bool use_1wire = false;
//global array for 1wire data
dallasData actDallasData[MAX_DALLAS_SENSORS];


// instead of passing array pointers between functions we just define this in the global scope
#define MAXDATASIZE 256
char data[MAXDATASIZE];
int data_length = 0;

// store actual data in an String array
String actData[NUMBER_OF_TOPICS];

// log message to sprintf to
char log_msg[256];

// mqtt topic to sprintf and then publish to
char mqtt_topic[256];

//buffer for commands to send
struct command_struct {
  byte value[128];
  unsigned int length;
  command_struct *next;
};
command_struct *commandBuffer;
unsigned int commandsInBuffer = 0;
#define MAXCOMMANDSINBUFFER 5 //can't have too much in buffer due to memory shortage


//doule reset detection
DoubleResetDetect drd(DRD_TIMEOUT, DRD_ADDRESS);

// mqtt
WiFiClient mqtt_wifi_client;
PubSubClient mqtt_client(mqtt_wifi_client);

void mqtt_reconnect()
{
  Serial1.println("Reconnecting to mqtt server ...");
  if (mqtt_client.connect(wifi_hostname, mqtt_username, mqtt_password, mqtt_willtopic, 1, true, "Offline"))
  {
    mqtt_client.subscribe(mqtt_set_quiet_mode_topic);
    mqtt_client.subscribe(mqtt_set_operationmode_topic);
    mqtt_client.subscribe(mqtt_set_heatpump_state_topic);
    mqtt_client.subscribe(mqtt_set_z1_heat_request_temperature_topic);
    mqtt_client.subscribe(mqtt_set_z1_cool_request_temperature_topic);
    mqtt_client.subscribe(mqtt_set_z2_heat_request_temperature_topic);
    mqtt_client.subscribe(mqtt_set_z2_cool_request_temperature_topic);
    mqtt_client.subscribe(mqtt_set_force_DHW_topic);
    mqtt_client.subscribe(mqtt_set_force_defrost_topic);
    mqtt_client.subscribe(mqtt_set_force_sterilization_topic);
    mqtt_client.subscribe(mqtt_set_holiday_topic);
    mqtt_client.subscribe(mqtt_set_powerful_topic);
    mqtt_client.subscribe(mqtt_set_dhw_temp_topic);
    
    mqtt_client.publish(mqtt_willtopic, "Online");
  }
}

void log_message(char* string)
{
  Serial1.println(string);
  if (outputMqttLog)
  {
    mqtt_client.publish(mqtt_logtopic, string);
  }
}

void logHex(char *hex, int hex_len) {
  char buffer [48];
  buffer[47] = 0;
  for (int i = 0; i < hex_len; i += 16) {
    for (int j = 0; ((j < 16) && ((i + j) < hex_len)); j++) {
      sprintf(&buffer[3 * j], "%02X ", hex[i + j]);
    }
    sprintf(log_msg, "data: %s", buffer ); log_message(log_msg);
  }

}

byte calcChecksum(byte* command, int length) {
  byte chk = 0;
  for ( int i = 0; i < length; i++)  {
    chk += command[i];
  }
  chk = (chk ^ 0xFF) + 01;
  return chk;
}

bool readSerial()
{
  while (Serial.available()) {
    data[data_length] = Serial.read(); //read available data and place it after the last received data
    data_length++;
  }
  //only enable this if you really want to see how the data is gathered in multiple tries
  //sprintf(log_msg, "received size : %d", data_length); log_message(log_msg);

  if (data_length > 203) return false; //panasonic never returns more than 203 bytes, so this is bad data

  if (data_length == 203) { //panasonic read is always 203 on valid receive, if not yet there wait for next read
    log_message((char*)"Received 203 bytes data");
    sending = false; //we received an answer after our last command so from now on we can start a new send request again
    if (outputHexDump) logHex(data, data_length);
    byte chk = 0;
    for ( int i = 0; i < data_length; i++)  {
      chk += data[i];
    }
    if ( chk == 0 ) {
      log_message((char*)"Checksum received ok!");
      data_length = 0; //for next attempt
      return true;
    }
    else {
      log_message((char*)"Checksum received false!");
      data_length = 0; //for next attempt
      return false;
    }
  }
  return false;
}

void popCommandBuffer() {
  if (commandBuffer) { //to make sure we can pop a command from the buffer
    send_command(commandBuffer->value, commandBuffer->length);
    command_struct* nextCommand = commandBuffer->next;
    free(commandBuffer);
    commandBuffer = nextCommand;
    commandsInBuffer--;
  }
}

void pushCommandBuffer(byte* command, int length) {
  if (commandsInBuffer < MAXCOMMANDSINBUFFER) {
    command_struct* newCommand = new command_struct;
    newCommand->length = length;
    for (int i = 0 ; i < length ; i++) {
      newCommand->value[i] = command[i];
    }
    newCommand->next = commandBuffer;
    commandBuffer = newCommand;
    commandsInBuffer++;
  }
  else {
    log_message((char*)"Too much commands already in buffer. Ignoring this commands.");
  }
}


bool send_command(byte* command, int length)
{
  if ( sending ) {
    log_message((char*)"Already sending data. Buffering this send request");
    pushCommandBuffer(command, length);
    return false;
  }
  sending = true;

  byte chk = calcChecksum(command, length);
  int bytesSent = Serial.write(command, length);
  bytesSent += Serial.write(chk);

  sprintf(log_msg, "sent bytes: %d with checksum: %d ", bytesSent, int(chk)); log_message(log_msg);
  if (outputHexDump) logHex((char*)command, length);
  readtime = millis() + SERIALTIMEOUT; //set readtime when to timeout the answer of this command
  return true;
}

// Callback function that is called when a message has been pushed to one of your topics.
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (mqttcallbackinprogress) {
    log_message((char*)"Already processing another mqtt callback. Ignoring this one");
  }
  else {
    mqttcallbackinprogress = true;
    char msg[length + 1];
    for (unsigned int i = 0; i < length; i++) {
      msg[i] = (char)payload[i];
    }
    msg[length] = '\0';
    send_heatpump_command(topic, msg, send_command, log_message);
    mqttcallbackinprogress = false;
  }
}

void setupOTA() {
  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(wifi_hostname);

  // Set authentication
  ArduinoOTA.setPassword(ota_password);

  ArduinoOTA.onStart([]() {
  });
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {

  });
  ArduinoOTA.onError([](ota_error_t error) {

  });
  ArduinoOTA.begin();
}

void setupHttp() {
  httpUpdater.setup(&httpServer, update_path, update_username, ota_password);
  httpServer.on("/", [] {
    handleRoot(&httpServer);
  });
  httpServer.on("/tablerefresh", [] {
    handleTableRefresh(&httpServer, actData, actDallasData);
  });
  httpServer.on("/json", [] {
    handleJsonOutput(&httpServer, actData, actDallasData);
  });
  httpServer.on("/factoryreset", [] {
    handleFactoryReset(&httpServer);
  });
  httpServer.on("/reboot", [] {
    handleReboot(&httpServer);
  });
  httpServer.on("/settings", [] {
    handleSettings(&httpServer, wifi_hostname, ota_password, mqtt_server, mqtt_port, mqtt_username, mqtt_password, use_1wire);
  });
  httpServer.on("/togglelog", [] {
    log_message((char*)"Toggled mqtt log flag");
    outputMqttLog ^= true;
    handleRoot(&httpServer);
  });
  httpServer.on("/togglehexdump", [] {
    log_message((char*)"Toggled hexdump log flag");
    outputHexDump ^= true;
    handleRoot(&httpServer);
  });
  httpServer.begin();
}

void setupSerial() {
  //debug line on serial1 (D4, GPIO2)
  Serial1.begin(115200);

  //boot issue's first on normal serial
  Serial.begin(115200);
  Serial.flush();
}

void switchSerial() {
  Serial.println("Switching serial to connect to heatpump. Look for debug on serial1 (GPIO2) and mqtt log topic.");
  //serial to cn-cnt
  Serial.flush();
  Serial.end();
  Serial.begin(9600, SERIAL_8E1);
  Serial.flush();
  //swap to gpio13 (D7) and gpio15 (D8)
  Serial.swap();

  //enable gpio15 after boot using gpio5 (D1)
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
}


void setupMqtt() {
  mqtt_client.setServer(mqtt_server, atoi(mqtt_port));
  mqtt_client.setCallback(mqtt_callback);
  mqtt_reconnect();
}

void setup() {
  setupSerial();
  setupWifi(drd, wifi_hostname, ota_password, mqtt_server, mqtt_port, mqtt_username, mqtt_password, use_1wire);
  MDNS.begin(wifi_hostname);
  setupOTA();
  setupMqtt();
  setupHttp();
  if (use_1wire) initDallasSensors(actDallasData,log_message);
  switchSerial();

}

void send_panasonic_query() {
  String message = "Requesting new panasonic data (uptime: " + getUptime() + ")";
  log_message((char*)message.c_str());
  if (commandBuffer) {
    log_message((char *)"Sending command from buffer");
    popCommandBuffer();
  }
  else {
    send_command(panasonicQuery, PANASONICQUERYSIZE);

  }
}

void read_panasonic_data() {
  if (sending && (millis() > readtime)) {
    log_message((char*)"Previous read data attempt failed due to timeout!");
    data_length = 0; //clear any data in array
    sending = false; //receiving the answer from the send command timeout, so we are allowed to send a new command
  }
  if (sending && (Serial.available() > 0)) { //only read data if we have sent a command so we expect an answer
    // read the serial and decode if data is valid
    if ( readSerial() ) decode_heatpump_data(data, actData, mqtt_client, log_message);
  }
}

void loop() {
  // Handle OTA first.
  ArduinoOTA.handle();
  // then handle HTTP
  httpServer.handleClient();
  // Allow MDNS processing
  MDNS.update();

  if (!mqtt_client.connected())
  {
    mqtt_reconnect();
  }
  mqtt_client.loop();

  read_panasonic_data();

  if (use_1wire) dallasLoop(actDallasData,mqtt_client, log_message);

  // run the data query only each WAITTIME
  if (millis() > nexttime) {
    nexttime = millis() + WAITTIME;
    send_panasonic_query();
    MDNS.announce();
    //Make sure the LWT is set to Online, even if the broker have marked it dead.
    mqtt_client.publish(mqtt_willtopic, "Online");
  }
}
