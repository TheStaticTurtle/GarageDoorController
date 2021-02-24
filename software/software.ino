#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <Syslog.h>
#include "Ticker.h"

#define DOORID "2"

const char* nodeName = "garagedoor"DOORID;
const char* ssid = "Host";
const char* password = "zdcrbhiy0389499447";

const char* syslog_ip = "192.168.1.53";
const int   syslog_port = 514;

const char* mqtt_ip = "192.168.1.51";
const char* mqtt_user = "garage_doors_finnaly";
const char* mqtt_pass = "garage_doors_finnaly";

const byte pin_motor_opening = D5; //15;
const byte pin_motor_closing = D0; //16;
const byte pin_sensor_door_open = D6; //12;
const byte pin_sensor_door_closed = D7; //13;
const byte pin_action = D8;//15;

enum door_status_t {
  DS_OPEN           = 0,
  DS_OPEN_PARTIAL   = 1,
  DS_OPENING        = 2,
  DS_CLOSING        = 3,
  DS_CLOSED         = 4,
  DS_ERROR_OPENING_AND_CLOSING   = 248,
  DS_ERROR_CLOSING_WHILE_OPENING = 249,
  DS_ERROR_OPEN_AND_CLOSED       = 250,
  DS_ERROR_OPENING_WHILE_CLOSED  = 251,
  DS_ERROR_UNKNOWN               = 252,
  DS_ERROR_ALL_ON                = 253,
  DS_UNKNOWN = 255,
};

enum move_direction_t {
  MV_STOP   = 0,
  MV_OPEN   = 1,
  MV_CLOSE  = 2,
  //Specials use case
  MV_ANY    = 3,
  MV_CHANGE = 4,
};

void loop_update_serial();
void loop_update_mqtt();
void loop_update_syslog();
void reconnect_mqtt();
void press_action();
void callback(char* topic, byte* payload, unsigned int length);
door_status_t getDoorStatus();
void send_pin_status_serial_mqtt();
void send_status_update_mqtt(door_status_t status);
void send_pin_status_serial();
void send_status_update_serial(door_status_t status);
void send_pin_status_syslog();
void send_status_update_syslog(door_status_t status);
void send_pin_status_all();
void send_status_update_all(door_status_t status);
bool move_door(move_direction_t direction, int _recur); //Call with _recur = 0

Ticker timer_loop_update_serial(loop_update_serial, 1 * 1000);
Ticker timer_loop_update_mqtt(loop_update_mqtt, 30 * 1000);
Ticker timer_loop_update_syslog(loop_update_syslog, 2 * 60 * 1000);

WiFiClient espClient;
WiFiUDP udpClient;
PubSubClient client(espClient);
Syslog syslog(udpClient, syslog_ip, syslog_port, nodeName, "main", LOG_KERN);
Syslog syslogmqtt(udpClient, syslog_ip, syslog_port, nodeName, "mqtt", LOG_KERN);

void reconnect_mqtt() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(nodeName, mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      syslogmqtt.log(LOG_INFO, "MQTT Connection success");
      client.subscribe("iot/garrage/door/"DOORID"/command");
      
      send_status_update_all(getDoorStatus());
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      
      syslogmqtt.logf(LOG_ERR, "MQTT Connection failed rc=%d",client.state());
      for(int i=0; i<500; i++) {  delay(10); ArduinoOTA.handle(); }
    }
  }
}
void setup() {
  pinMode(pin_motor_opening,INPUT);
  pinMode(pin_motor_closing,INPUT);
  pinMode(pin_sensor_door_open,INPUT_PULLUP);
  pinMode(pin_sensor_door_closed,INPUT_PULLUP);
  pinMode(pin_action,OUTPUT);
  
  Serial.begin(115200);
  Serial.println("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  syslog.logf(LOG_INFO, "Just rebooted. Reset cause=%s", ESP.getResetReason().c_str());

  ArduinoOTA.setHostname(nodeName);
  ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart([]() { syslog.log(LOG_WARNING, "OTA updated started"); if (ArduinoOTA.getCommand() == U_FLASH) Serial.println("Start updating scketch"); else Serial.println("Start updating file"); });
  ArduinoOTA.onEnd([]() { syslog.log(LOG_WARNING, "OTA updated finished"); Serial.println("\nEnd of the update"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { syslog.logf(LOG_DEBUG, "Update progress: %u%%\r", (progress / (total / 100))); Serial.printf("Update progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)         { Serial.println("Auth Failed"); syslog.log(LOG_ERR, "OTA error. Auth Failed"); }
    else if (error == OTA_BEGIN_ERROR)   { Serial.println("Begin Failed"); syslog.log(LOG_ERR, "OTA error. Begin Failed"); }
    else if (error == OTA_CONNECT_ERROR) { Serial.println("Connect Failed"); syslog.log(LOG_ERR, "OTA error. Connect Failed"); }
    else if (error == OTA_RECEIVE_ERROR) { Serial.println("Receive Failed"); syslog.log(LOG_ERR, "OTA error. Receive Failed"); }
    else if (error == OTA_END_ERROR)     { Serial.println("End Failed"); syslog.log(LOG_ERR, "OTA error. End Failed"); }
  });
  ArduinoOTA.begin();
  syslog.log(LOG_INFO, "OTA Started");
  
  if (!MDNS.begin(nodeName, WiFi.localIP())) {
    Serial.println("[ERROR] Failed to setup MDNS responder!");
    syslog.log(LOG_ERR, "Failed to setup MDNS responder!");
  }
  
  delay(50);
  Serial.println("Hellow world");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  client.setServer(mqtt_ip, 1883);
  client.setCallback(callback);

  timer_loop_update_serial.start();
  timer_loop_update_mqtt.start();
  timer_loop_update_syslog.start();
}

void callback(char* topic, byte* payload, unsigned int length) {
  char buffer[length+1];
  strncpy(buffer,(char*)payload,length);
  buffer[length] = '\0';
  
  syslogmqtt.logf(LOG_INFO, "Received: %s (%s)", topic, buffer);

  if(strcmp(topic, "iot/garrage/door/"DOORID"/command") == 0) {
    if(strcmp(buffer, "open") == 0)  move_door(MV_OPEN,0);
    if(strcmp(buffer, "close") == 0) move_door(MV_CLOSE,0);
    if(strcmp(buffer, "stop") == 0) move_door(MV_STOP,0);
  }
}

door_status_t getDoorStatus() {
  bool isOpen    = !digitalRead(pin_sensor_door_open);
  bool isOpening = !digitalRead(pin_motor_opening);
  bool isClosing = !digitalRead(pin_motor_closing);
  bool isClosed  = !digitalRead(pin_sensor_door_closed);

  if( !isOpen && !isOpening && !isClosing && !isClosed  )   { return DS_OPEN_PARTIAL;                }
  if(  isOpen && !isOpening && !isClosing && !isClosed  )   { return DS_OPEN;                        }
  if( !isOpen &&  isOpening && !isClosing && !isClosed  )   { return DS_OPENING;                     }
  if(  isOpen &&  isOpening && !isClosing && !isClosed  )   { return DS_OPEN;                        }
  if( !isOpen && !isOpening &&  isClosing && !isClosed  )   { return DS_CLOSING;                     }
  if(  isOpen && !isOpening &&  isClosing && !isClosed  )   { return DS_OPEN;                        }
  if( !isOpen &&  isOpening &&  isClosing && !isClosed  )   { return DS_ERROR_OPENING_AND_CLOSING;   }
  if(  isOpen &&  isOpening &&  isClosing && !isClosed  )   { return DS_ERROR_CLOSING_WHILE_OPENING; }
  if( !isOpen && !isOpening && !isClosing &&  isClosed  )   { return DS_CLOSED;                      }
  if(  isOpen && !isOpening && !isClosing &&  isClosed  )   { return DS_ERROR_OPEN_AND_CLOSED;       }
  if( !isOpen &&  isOpening && !isClosing &&  isClosed  )   { return DS_CLOSED;                      }
  if(  isOpen &&  isOpening && !isClosing &&  isClosed  )   { return DS_ERROR_OPENING_WHILE_CLOSED;  }
  if( !isOpen && !isOpening &&  isClosing &&  isClosed  )   { return DS_CLOSED;                      }
  if(  isOpen && !isOpening &&  isClosing &&  isClosed  )   { return DS_ERROR_UNKNOWN;               }
  if( !isOpen &&  isOpening &&  isClosing &&  isClosed  )   { return DS_ERROR_UNKNOWN;               }
  if(  isOpen &&  isOpening &&  isClosing &&  isClosed  )   { return DS_ERROR_ALL_ON;                }
}

void send_pin_status_serial_mqtt() {
   client.publish("iot/garrage/door/"DOORID"/pins/open",    (!digitalRead(pin_sensor_door_open))    ? "1" : "0" );
   client.publish("iot/garrage/door/"DOORID"/pins/opening", (!digitalRead(pin_motor_opening))       ? "1" : "0" );
   client.publish("iot/garrage/door/"DOORID"/pins/closing", (!digitalRead(pin_motor_closing))       ? "1" : "0" );
   client.publish("iot/garrage/door/"DOORID"/pins/closed",  (!digitalRead(pin_sensor_door_closed))  ? "1" : "0" );
}
void send_status_update_mqtt(door_status_t status) {
  switch(status) {
    case DS_OPEN:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status", "open");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "open");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "100");
      send_pin_status_serial_mqtt();
      break;
    case DS_OPEN_PARTIAL:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status", "open_partial");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "open");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "50");
      send_pin_status_serial_mqtt();
      break;
    case DS_OPENING:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "opening");
      client.publish("iot/garrage/door/"DOORID"/door_status", "opening");
      send_pin_status_serial_mqtt();
      break;
    case DS_CLOSING:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status", "closing");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closing");
      send_pin_status_serial_mqtt();
      break;
    case DS_CLOSED:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_UNKNOWN:
      client.publish("iot/garrage/door/"DOORID"/available", "yes");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "none");
      client.publish("iot/garrage/door/"DOORID"/door_status", "unknown");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_OPENING_AND_CLOSING:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "opening_and_closing");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_CLOSING_WHILE_OPENING:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "closing_while_opening");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_OPEN_AND_CLOSED:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "open_and_closed");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_OPENING_WHILE_CLOSED:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "opening_while_closed");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_UNKNOWN:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "unknown");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
    case DS_ERROR_ALL_ON:
      client.publish("iot/garrage/door/"DOORID"/available", "no");
      client.publish("iot/garrage/door/"DOORID"/last_error_state", "all_on");
      client.publish("iot/garrage/door/"DOORID"/door_status", "error");
      client.publish("iot/garrage/door/"DOORID"/door_status_4state", "closed");
      client.publish("iot/garrage/door/"DOORID"/door_status_int", "0");
      send_pin_status_serial_mqtt();
      break;
  }
}

void send_pin_status_serial() {
   Serial.print   ((!digitalRead(pin_sensor_door_open))    ? "DO1 " : "DO0 " );
   Serial.print   ((!digitalRead(pin_motor_opening))       ? "MO1 " : "MO0 " );
   Serial.print   ((!digitalRead(pin_motor_closing))       ? "MC1 " : "MC0 " );
   Serial.println ((!digitalRead(pin_sensor_door_closed))  ? "DC1 " : "DC0 " );
}
void send_status_update_serial(door_status_t status) {
  switch(status) {
    case DS_OPEN:
      Serial.print("[STATUS] DOOR:open PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_OPEN_PARTIAL:
      Serial.print("[STATUS] DOOR:open_partial PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_OPENING:
      Serial.print("[STATUS] DOOR:opening PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_CLOSING:
      Serial.print("[STATUS] DOOR:closing PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_CLOSED:
      Serial.print("[STATUS] DOOR:closed PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_UNKNOWN:
      Serial.print("[STATUS] DOOR:unknown PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_OPENING_AND_CLOSING:
      Serial.print("[STATUS] DOOR:error OPENING_AND_CLOSING PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_CLOSING_WHILE_OPENING:
      Serial.print("[STATUS] DOOR:error CLOSING_WHILE_OPENING PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_OPEN_AND_CLOSED:
      Serial.print("[STATUS] DOOR:error OPEN_AND_CLOSED PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_OPENING_WHILE_CLOSED:
      Serial.print("[STATUS] DOOR:error OPENING_WHILE_CLOSED PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_UNKNOWN:
      Serial.print("[STATUS] DOOR:error UNKNOWN PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
    case DS_ERROR_ALL_ON:
      Serial.print("[STATUS] DOOR:error ALL_ON PINS:");
      send_pin_status_serial();
      Serial.println();
      break;
  }
}

void send_pin_status_syslog() {
   syslog.logf(LOG_DEBUG, "Pin status: DO%d MO%d MC%d DC%d", !digitalRead(pin_sensor_door_open), !digitalRead(pin_motor_opening), !digitalRead(pin_motor_closing), !digitalRead(pin_sensor_door_closed));
}
void send_status_update_syslog(door_status_t status) {
  switch(status) {
    case DS_OPEN:
      syslog.log(LOG_INFO, "DOOR: open");
      break;
    case DS_OPEN_PARTIAL:
      syslog.log(LOG_INFO, "DOOR: open_partial");
      break;
    case DS_OPENING:
      syslog.log(LOG_INFO, "DOOR: opening");
      break;
    case DS_CLOSING:
      syslog.log(LOG_INFO, "DOOR: closing");
      break;
    case DS_CLOSED:
      syslog.log(LOG_INFO, "DOOR: closed");
      break;
    case DS_UNKNOWN:
      syslog.log(LOG_INFO, "DOOR: unknown");
      break;
    case DS_ERROR_OPENING_AND_CLOSING:
      syslog.log(LOG_ERR, "DOOR Error: OPENING_AND_CLOSING");
      break;
    case DS_ERROR_CLOSING_WHILE_OPENING:
      syslog.log(LOG_ERR, "DOOR Error: CLOSING_WHILE_OPENING");
      break;
    case DS_ERROR_OPEN_AND_CLOSED:
      syslog.log(LOG_ERR, "DOOR Error: OPEN_AND_CLOSED");
      break;
    case DS_ERROR_OPENING_WHILE_CLOSED:
      syslog.log(LOG_ERR, "DOOR Error: OPENING_WHILE_CLOSED");
      break;
    case DS_ERROR_UNKNOWN:
      syslog.log(LOG_ERR, "DOOR Error: UNKNOWN");
      break;
    case DS_ERROR_ALL_ON:
      syslog.log(LOG_ERR, "DOOR Error: ALL_ON");
      break;
  }
}

void send_pin_status_all() {
  send_pin_status_serial_mqtt();
  send_pin_status_serial();
  send_pin_status_syslog();
}
void send_status_update_all(door_status_t status) {
  send_status_update_mqtt(status);
  send_status_update_serial(status);
  send_status_update_syslog(status);
}

void loop_update_serial() {
  door_status_t status = getDoorStatus();
  send_status_update_serial(status);
}
void loop_update_mqtt() {
  door_status_t status = getDoorStatus();
  if (client.connected()) {
    send_status_update_mqtt(status);
  }
}
void loop_update_syslog() {
  door_status_t status = getDoorStatus();
  send_status_update_syslog(status);
}


void press_action() {
  digitalWrite(pin_action, true);
  delay(150);
  digitalWrite(pin_action, false);
  delay(250);
}
bool wait_for_motor_dir(move_direction_t direction, int timeout) {
  unsigned long start_time = millis();
  bool isOpening = !digitalRead(pin_motor_opening);
  bool isClosing = !digitalRead(pin_motor_closing);
  bool isOpening_original = isOpening;
  bool isClosing_original = isClosing;
  
  while((start_time+timeout) > millis()) {
    isOpening = !digitalRead(pin_motor_opening);
    isClosing = !digitalRead(pin_motor_closing);
    if(direction==MV_OPEN  && isOpening) return true;
    if(direction==MV_CLOSE && isClosing) return true;
    if(direction==MV_ANY   && (isClosing || isOpening)) return true;
    if(direction==MV_STOP  && !isOpening && !isClosing) return true;
    if(direction==MV_CHANGE  && (isOpening != isOpening_original || isClosing != isClosing_original)) return true;
    ESP.wdtFeed();
  }

  delay(75);
  return false;
}
bool move_door(move_direction_t direction, int _recur) {
  door_status_t status = getDoorStatus();
  if(direction == MV_OPEN  && (status == DS_OPEN || status == DS_OPENING)) return true;
  if(direction == MV_CLOSE && (status == DS_CLOSED || status == DS_CLOSING)) return true;
  if(direction == MV_STOP  && !(status == DS_OPENING || status == DS_CLOSING)) return true;
  
  if(_recur >= 5) {
    syslog.log(LOG_WARNING, "move_door maximum recusivity was hit");
    send_pin_status_all(); send_status_update_all(getDoorStatus());
    return false;
  }
  
  press_action();
  
  if(wait_for_motor_dir(MV_CHANGE, 2000)) {
    syslog.log(LOG_ERR, "wait_for_motor_dir MV_ANY timeout.");
    send_pin_status_all(); send_status_update_all(getDoorStatus());
    return false;
  }
  delay(50);
  send_status_update_all(getDoorStatus());
  return move_door(direction, _recur+1);
}
/*bool move_door(move_direction_t direction) {
  door_status_t status = getDoorStatus();
  
  //Avoid doing something stupid if the door is already where we want it to be or it's going to be where we want it to be
  if(direction == MV_OPEN  && (status == DS_OPEN || status == DS_OPENING)) return true;
  if(direction == MV_CLOSE && (status == DS_CLOSED || status == DS_CLOSING)) return true;
  if(direction == MV_STOP  && !(status == DS_OPENING || status == DS_CLOSING)) return true;

  //No need to reinvent the wheel here, Just start moving and relaunch the function
  if(status == DS_OPEN_PARTIAL) {
    press_action(); 
    if(wait_for_motor_dir(MV_ANY,2000)) { // Verify
      delay(100);
      return move_door(direction);
    } else  {
      syslog.log(LOG_ERR, "wait_for_motor_dir MV_ANY timeout.");
      send_pin_status_all(); send_status_update_all(getDoorStatus());
      return false;
    }
  }
  
  //Stop the door
  if(direction == MV_STOP) { 
    press_action(); 
    send_status_update_all(getDoorStatus());
    return true; 
  }

  //Open the door
  if(direction == MV_OPEN) { 
    if(status == DS_CLOSED) {  //If it's closed on push should re-open it
      press_action(); 
      if(wait_for_motor_dir(MV_OPEN,2000)) { // Verify
        send_status_update_all(getDoorStatus());
        return true;
      } else  {
        syslog.log(LOG_ERR, "wait_for_motor_dir MV_OPEN timeout.");
        send_pin_status_all(); send_status_update_all(getDoorStatus());
        return false;
      }
    }
    if(status == DS_CLOSING) { 
      press_action(); //Stop the door 
      send_status_update_all(getDoorStatus());
      
      if(wait_for_motor_dir(MV_STOP,2000)) { //Verify that the door has stoped
        press_action(); //Restart the door (Direction should be inverted now)
        send_status_update_all(getDoorStatus());
        
        if(wait_for_motor_dir(MV_OPEN,2000)) { // Verify that
          send_status_update_all(getDoorStatus());
          return true;
          
        } else {
          syslog.log(LOG_ERR, "wait_for_motor_dir MV_OPEN timeout.");
          send_pin_status_all(); send_status_update_all(getDoorStatus());
          return false;
          
        }
      } else {
        syslog.log(LOG_ERR, "wait_for_motor_dir MV_STOP timeout.");
        send_pin_status_all(); send_status_update_all(getDoorStatus());
        return false;
        
      }
    }
    syslog.log(LOG_WARNING, "move_door MV_OPEN was called and no control path was found!");
    send_pin_status_all(); send_status_update_all(getDoorStatus());
    return false;
  }
  
  //Close the door
  if(direction == MV_CLOSE) { 
    if(status == DS_OPEN) {   //If it's open on push should re-open it
      press_action();
      if(wait_for_motor_dir(MV_OPEN,2000)) { // Verify
        send_status_update_all(getDoorStatus());
        return true;
      } else  {
        syslog.log(LOG_ERR, "wait_for_motor_dir MV_CLOSE timeout.");
        send_pin_status_all(); send_status_update_all(getDoorStatus());
        return false;
      }
    }
    if(status == DS_OPEN) { 
      press_action(); //Stop the door 
      send_status_update_all(getDoorStatus());
      
      if(wait_for_motor_dir(MV_STOP,2000)) { //Verify that the door has stoped
        press_action(); //Restart the door (Direction should be inverted now)
        send_status_update_all(getDoorStatus());
        
        if(wait_for_motor_dir(MV_CLOSE,2000)) { // Verify that
          send_status_update_all(getDoorStatus());
          return true;
          
        } else {
          send_pin_status_all(); send_status_update_all(getDoorStatus());
          syslog.log(LOG_ERR, "wait_for_motor_dir MV_CLOSED timeout.");
          return false;
        }
      } else {
        send_pin_status_all(); send_status_update_all(getDoorStatus());
        syslog.log(LOG_ERR, "wait_for_motor_dir MV_STOP timeout.");
        return false;
      }
    }
    send_pin_status_all(); send_status_update_all(getDoorStatus());
    syslog.log(LOG_WARNING, "move_door MV_CLOSE was called and no control path was found!");
    return false;
  }
  
  send_pin_status_all(); send_status_update_all(getDoorStatus());
  syslog.log(LOG_WARNING, "move_door was called and no controll path was found!");
  return false;
}
*/

door_status_t old_status;
door_status_t status;
void loop() {
  ArduinoOTA.handle();
  if (!client.connected()) { reconnect_mqtt(); } client.loop();
  timer_loop_update_serial.update();
  timer_loop_update_mqtt.update();
  timer_loop_update_syslog.update();
  
  status = getDoorStatus();
  if(status != old_status) {
    old_status = status;
    
    send_status_update_serial(status);
    if (client.connected())
      send_status_update_mqtt(status);
    send_status_update_syslog(status);
  }
}
