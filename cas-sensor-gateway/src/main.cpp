#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LoRa.h>
#include "packet.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>

#define PINLED 25 // pin of test led on the board
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  5        /* Time ESP32 will go to sleep (in seconds) */
#define LORA_PIN_SS 18
#define LORA_PIN_RST 23
#define LORA_PIN_DIO0 26
#define LORA_BAND 866E6 // set frequency of LoRa to 866[MHz], expressed in Hertz

#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 23
#define DIO0 26

RTC_DATA_ATTR int bootCount = 0; /* locazione di memoria che ci servirà per capire se è stata già avviato una prima volta */

Adafruit_SSD1306 display;
LoRaPacket packet;
bool configMode = false;
AsyncWebServer server(80);
const char* INPUT_SSID = "ssid";
const char* INPUT_PASS = "pass";
String ssid;
String pass;
unsigned long previous_millis = 0;
const long interval = 10000;

const char* mqtt_server = "broker.hivemq.com";
int mqtt_port = 1883;

WiFiClient wificlient;
PubSubClient mqttclient(wificlient);

StaticJsonDocument<300> jsondocument;

float round_to_dp( float in_value, int decimal_place )
{
	float multiplier = powf( 10.0f, decimal_place );
	in_value = roundf( in_value * multiplier ) / multiplier;
	return in_value;
}

void setup() {
  // put your setup code here, to run once:
  // inizializzazione seriale UART
  Serial.begin(115200);
  pinMode(PINLED, OUTPUT);
  digitalWrite(PINLED, LOW);
  // inizializzazione display
  Wire.begin(21, 22);   // initialize I2C bus
  display.begin(SSD1306_SWITCHCAPVCC, 0x3c, false, false); // initialize built-in screen on board
  display.setTextColor(WHITE); // set color of text to white
  display.setTextSize(1); // set text size to one
  display.clearDisplay(); // clear all write elements from built-in display
  Serial.println("Screen initialized");

  // inizializzazione File System
  if(!SPIFFS.begin(true)){
    Serial.println("File System initialization failed.");
    while (true);  
  }
  // cerco il file sul file system
  File file = SPIFFS.open("/config.txt");
  if(!file || file.isDirectory()){
    // modalità configurazione
    configMode = true;
    // se non presente dobbiamo renderlo access point
    WiFi.softAP("CAS-Gateway", NULL);
    // IP del gateway
    IPAddress ipgw = WiFi.softAPIP();
    display.println("Config Mode");
    display.println("Wi-Fi SSID:");
    display.println("CAS-Gateway");
    display.println(ipgw);
    display.display();
    // definire regole del webserver
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(SPIFFS,"/index.html","text/html",false);
    });
    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request){
      AsyncWebParameter* ssid_param = request->getParam(0);
      AsyncWebParameter* pass_param = request->getParam(1);
      if(ssid_param->isPost() && pass_param->isPost()){
        ssid = ssid_param->value();
        pass = pass_param->value();
        // salvataggio del contenuto all'interno di un file
        File file = SPIFFS.open("/config.txt", FILE_WRITE);
        // controllo se il file sono riuscito ad aprirlo in scrittura
        if(!file){
          request->send(200,"text/plain","Error on writing file the gateway will restart in 3 seconds");
          delay(3000);
          ESP.restart();
        }
        if(file.print(ssid + "##" + pass + '\n')){
          file.close();
          request->send(200,"text/plain", "Configuration saved, the gateway will restart in 3 seconds"); // passa alla mod. operativa
        } else{
          request->send(200,"text/plain", "Error on configuration saving, the gateway will restart in 3 seconds");
        }
        delay(3000);
        ESP.restart();
      }
    });
    server.serveStatic("/", SPIFFS, "/");
    server.begin();

  } else
  {
    // modalità operativa
    configMode = false;
    Serial.println("Operative mode");
    // caricamento dei dati di connessione Wi-Fi
    String filecontent;
    while(file.available()){
      filecontent = file.readString();
      break;
    }
    Serial.println(filecontent);
    int index = filecontent.indexOf('##');
    ssid = filecontent.substring(0,index);
    ssid.trim();
    pass = filecontent.substring(index+2,filecontent.length());
    pass.trim();
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Connecting to:");
    display.println(ssid);
    display.println("Wi-Fi network");
    display.display();
    Serial.println(ssid);
    Serial.println(pass);
    WiFi.begin(ssid.c_str(), pass.c_str());
    // verifico se si collega effettivamente
    unsigned long current_millis = millis();
    previous_millis = current_millis; // mi segno il tempo attuale
    while(WiFi.status()!= WL_CONNECTED){
      current_millis = millis();
      if(current_millis-previous_millis >= interval){
        // se non mi collego entro 10 secondi, riavvio il gateway eliminando il file di config
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("Error on connect");
        display.println(ssid);
        display.println("Wi-Fi network");
        display.println("Restarting in 3s");
        display.display();
        SPIFFS.remove("/config.txt");
        delay(3000);
        ESP.restart();
      }
    }
    // connessione MQTT
    mqttclient.setServer(mqtt_server,mqtt_port);
    if(!mqttclient.connect("")){
        Serial.println("Connection failed");
        delay(5000);
        ESP.restart();
    }
    Serial.println("MQTT connected");
    // inizializzazione LoRa
    SPI.begin(SCK, MISO, MOSI, LORA_PIN_SS);
    LoRa.setPins(LORA_PIN_SS, LORA_PIN_RST, LORA_PIN_DIO0);
    if (!LoRa.begin(LORA_BAND)) {
      Serial.println("LoRa initialization failed. Check configuration.");
      while (true);                       
    }
    Serial.println("LoRa initialized");
    bootCount++; // increase number of boot count
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Gateway started");
    display.println(WiFi.localIP());
    display.display(); // display generated elements to display
    delay(2000); // wait two seconds
  }
  

}
void loop() {
  // controllo se non sono in config mode
  if(!configMode){
    int packetsize = LoRa.parsePacket();
    if(packetsize){
      uint16_t typecheck;
      LoRa.readBytes((uint8_t *)&typecheck, 2);
      if(typecheck==TYPEAPPLICATION){
        LoRa.readBytes((uint8_t *)&packet+2, sizeof(LoRaPacket)-2);
        Serial.printf("Data received from: %x\n", packet.id);
        Serial.printf("Temperature: %f\n", packet.temperature);
        Serial.printf("Pressure: %f\n", packet.pressure);
        Serial.printf("Humidity: %f\n", packet.humidity);
        Serial.printf("Gas: %f\n", packet.gas);
        display.clearDisplay(); // clear all write elements from built-in display
        display.setCursor(0,0); // set first point, where write, to first upper-left pixel
        display.printf("Node: %x\n", packet.id);
        display.printf("T: %.1f*C | H: %.1f%% \n", packet.temperature, packet.humidity);
        display.printf("P: %.2fhPa \nQ: %.2f%% \n", packet.pressure, packet.gas);
        display.display();
        /* {"sensor_id" : id_sensore , "temperature" : temp , "pressure" : press , "humidity": hum , "gas" : gas} 
        */
       char idex[10] = "";
       ltoa(packet.id, idex, 16);
       jsondocument["id"] = String(idex);
       jsondocument["temp"] = round(packet.temperature * 100) / 100.0;
       //jsondocument["temp"] = packet.temperature;
       jsondocument["press"] = round(packet.pressure * 100) / 100.0;
       //jsondocument["press"] = packet.pressure;
       jsondocument["hum"] = round(packet.humidity * 100) / 100.0;
       //jsondocument["hum"] = packet.humidity;
       jsondocument["gas"] = round(packet.gas * 100) / 100.0;
       //jsondocument["gas"] = packet.gas;
       // utilizzo dell'MQTT client
       String output;
       serializeJson(jsondocument,output);
       Serial.println(output);
       mqttclient.publish("cas/sensor",output.c_str());
      }
    }
  }
  // put your main code here, to run repeatedly:
}