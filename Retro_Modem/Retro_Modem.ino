/*
 * ESP8266 based virtual modem
 * Copyright (C) 2016 Jussi Salin <salinjus@gmail.com>
 * 
 * Heavy modifications by Jim Agla <jim@digitalpcservices.com> for U64 User Port Adapter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ESP8266WiFi.h>
#include <algorithm>
#include <EEPROM.h>

//#define TELNET_DEBUG        // DEFINE for telnet debugging
#undef TELNET_DEBUG
bool TELNET_CTRL = true;      // Is telnet control code handling enabled
bool TELNET_CMD = false;

// Telnet codes
#define TELNET_SE 0xf0
#define TELNET_SB 0xfa
#define TELNET_GA 0xf9      // Go Ahead
#define TELNET_WILL 0xfb
#define TELNET_WONT 0xfc
#define TELNET_DO 0xfd
#define TELNET_DONT 0xfe
#define TELNET_IAC 0xff

#define TELNET_NULL 0x00
#define TELNET_ECHO 0x01
#define TELNET_SGA 0x03
#define TELNET_SENDLOC 0x17
#define TELNET_TERMTYPE 0x18
#define TELNET_NAWS 0x1f
#define TELNET_TERMSPEED 0x20

#define TELNET_LISTEN_PORT 23         // Listen to this if not connected. Set to zero to disable.
#define TX_BUF_SIZE 4096              // Buffer where to read from serial before writing to TCP
                                      // (that direction is very blocking by the ESP TCP stack,
                                      // so we can't do one byte a time.)
uint8_t TX_BUFFER[TX_BUF_SIZE];
unsigned long LEDTime = 0;
WiFiClient TCPCLIENT;
WiFiServer TCPSERVER(TELNET_LISTEN_PORT);

#define LED_PIN 2                     // Status LED
#define LED_TIME 250                  // How many ms to keep LED on at activity

#define CMD_MAX_LENGTH 256            // Maximum length for AT command
bool CMD_MODE = true;                 // AT command mode or connected mode
String CMD_STRING = "";               // Buffer for command string.
String CMD_USTRING = "";              // Buffer for UPPERCASE command string.
int CMD_CHARPOS = 0;                  // Where are we in processing the command string?
bool CMD_ERROR = false;               // Flag for error in command.
bool CMD_SHOWRESULT = true;           // If processing multiple commands, we only need to display result at the end. This "could" be removed by optimizing code.

//#define MODEM_DEBUG                 // DEFINE for modem debugging
#undef MODEM_DEBUG                    
#define MODEM_RING_INTERVAL 3000      // How often to print RING when having a new incoming connection (ms)
#define MODEM_DCD_PIN 14              // GPIO pin for Detect Carrier Detection (DCD)
#define MODEM_RTS_PIN 13              // GPIO pin for RTS
#define MODEM_CTS_PIN 15              // GPIO pin for CTS
#define MODEM_MAX_RINGS 10            // How many rings before there is "no answer" and telnet connection is disconnected.

int MODEM_BPS = 2400;                 // Default Baud Rate. Keep low for systems like C64 that can only do 2400 baud
bool MODEM_ECHO = true;               // Echo everything from the terminal in command mode by default.
bool MODEM_VERBOSE = true;            // All response codes are verbose by default.
bool MODEM_RESULTS = true;            // Display all response codes by default.
bool MODEM_ONHOOK = true;             // 
int MODEM_RINGS = 0;                  // Counter for how many times has the modem rang.
String MODEM_LASTCMD;                 // Buffer for the "repeat last command" command, "a/".
unsigned long MODEM_LASTRINGMS = 0;   // Time of last "RING" message (millis())
char MODEM_ESCAPE_COUNT = 0;          // Go to AT mode at "+++" sequence, that has to be counted
unsigned long MODEM_ESCAPE_TIME = 0;  // When did we last receive a "+++" sequence

// Modem S Registers
int MODEM_REG_S1;                     // Auto Answer
int MODEM_REG_S2;                     // Escape Character
int MODEM_REG_S3;                     // Carriage Return Character
int MODEM_REG_S4;                     // Line Feed Character
int MODEM_REG_S5;                     // Backspace Character

String WIFI_SSID;
String WIFI_KEY;
bool WIFI_CONNECTED = false;

void setup() {
  // Get Setting from EEPROM
  EEPROM.begin(512);
  MODEM_BPS = 2400; // Default value for 8-bit PC's like C64, etc.
  if (EEPROM_GET(0) == 0) {MODEM_BPS = 300;}
  if (EEPROM_GET(0) == 1) {MODEM_BPS = 600;}
  if (EEPROM_GET(0) == 2) {MODEM_BPS = 1200;}
  if (EEPROM_GET(0) == 3) {MODEM_BPS = 2400;}
  if (EEPROM_GET(0) == 4) {MODEM_BPS = 4800;}
  if (EEPROM_GET(0) == 5) {MODEM_BPS = 9600;}
  if (EEPROM_GET(0) == 6) {MODEM_BPS = 19200;}
  if (EEPROM_GET(0) == 7) {MODEM_BPS = 38400;}
  if (EEPROM_GET(0) == 8) {MODEM_BPS = 57600;}
  if (EEPROM_GET(0) == 9) {MODEM_BPS = 115200;}

  // Start Serial service.
  Serial.begin(MODEM_BPS);
  MODEM_RESET(0);
  START_MSG();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  
  pinMode(MODEM_DCD_PIN, OUTPUT);
  digitalWrite(MODEM_DCD_PIN, LOW);
}

void MODEM_RESET(int VALUE) {
  TCPCLIENT.stop();
  TCPSERVER.stop();

  pinMode(MODEM_DCD_PIN, OUTPUT);
  digitalWrite(MODEM_DCD_PIN, LOW);
  
  if (VALUE == 0) {
    MODEM_REG_S1 = 0;   // Auto Answer
    MODEM_REG_S2 = 43;  // Escape Character
    MODEM_REG_S3 = 13;  // Carriage Return Character
    MODEM_REG_S4 = 10;  // Line Feed Character
    MODEM_REG_S5 = 8;   // Backspace Character
    MODEM_ECHO = true;
    MODEM_RESULTS = true;
    MODEM_VERBOSE = true;
  } else if (VALUE == 1) {
    MODEM_REG_S1 = EEPROM_GET(8);
    MODEM_REG_S2 = EEPROM_GET(9);
    MODEM_REG_S3 = EEPROM_GET(10);
    MODEM_REG_S4 = EEPROM_GET(11);
    MODEM_REG_S5 = EEPROM_GET(12);
    if (EEPROM_GET(13) == 0) {MODEM_ECHO = false;} else {MODEM_ECHO = true;}
    if (EEPROM_GET(14) == 0) {MODEM_VERBOSE = false;} else {MODEM_VERBOSE = true;}
    if (EEPROM_GET(15) == 0) {MODEM_RESULTS = false;} else {MODEM_RESULTS = true;}
  } else if (VALUE == 2) {
    MODEM_REG_S1 = EEPROM_GET(16);
    MODEM_REG_S2 = EEPROM_GET(17);
    MODEM_REG_S3 = EEPROM_GET(18);
    MODEM_REG_S4 = EEPROM_GET(19);
    MODEM_REG_S5 = EEPROM_GET(20);
    if (EEPROM_GET(21) == 0) {MODEM_ECHO = false;} else {MODEM_ECHO = true;}
    if (EEPROM_GET(22) == 0) {MODEM_VERBOSE = false;} else {MODEM_VERBOSE = true;}
    if (EEPROM_GET(23) == 0) {MODEM_RESULTS = false;} else {MODEM_RESULTS = true;}
  }
  MODEM_ONHOOK = true;
  TCPSERVER.begin();
}

void START_MSG() {
  Serial.print("\r\n");
  Serial.print("\r\n");yield;
  Serial.print("retro modem emulator v1.0\r\n");
  Serial.print("-=-=-=-=-=-=-=-=-=-=-=-=-\r\n");
  Serial.print("\r\n");
  Serial.print("connect to wifi: wifi<ssid>,<key>\r\n");
  Serial.print("change terminal baud rate: baud<baud>\r\n");
  Serial.print("connect by tcp: atdt<host>:<port>\r\n");
  Serial.print("see my ip address: ati\r\n");
  Serial.print("to see help menu: help\r\n\r\n");
  Serial.print("disable telnet command handling: telnet0\r\n");
  Serial.print("\r\n");
  
  if (TELNET_LISTEN_PORT > 0) {
    Serial.print("listening to connections on port " + String(TELNET_LISTEN_PORT) + ".\r\n");
    TCPSERVER.begin();
  } else {
    Serial.print("incoming connections are disabled.\r\n");
  }
  Serial.print("\r\n");
}


/* Turn on the LED and store the time, so the LED will be shortly after turned off */
void LED_ON(void) {
  digitalWrite(LED_PIN, LOW);
  LEDTime = millis();
}


/* Perform a command given in command mode */
void AT_COMMAND() {
  MODEM_LASTCMD = CMD_STRING;
  CMD_SHOWRESULT = true;
  CMD_ERROR = false;
  CMD_CHARPOS = 2;
  CMD_USTRING = CMD_STRING;
  CMD_USTRING.toUpperCase();

  if (CMD_USTRING.substring(0, 3) == "ATA") {
    MODEM_ANSWER();
  } else if (CMD_USTRING.substring(0, 3) == "ATD") {
    MODEM_DIAL(CMD_STRING.substring(4));
  } else if (CMD_USTRING.substring(0, 4) == "BAUD") {
    MODEM_SETBPS();
  } else if (CMD_USTRING.substring(0, 4) == "WIFI") {
    WIFI_CONNECT(CMD_STRING.substring(4));
  } else if (CMD_USTRING.substring(0, 4) == "HELP") {
    DISPLAY_HELP();
  } else if (CMD_USTRING.substring(0, 7) == "TELNET0") {
    TELNET_CTRL = false;
    Serial.print("\r\ntelnet handling off, use telnet1 to turn on.\r\n\r\n");
  } else if (CMD_USTRING.substring(0, 7) == "TELNET1") {
    TELNET_CTRL = true;
    Serial.print("\r\ntelnet handling on, use telnet1 to turn off.\r\n\r\n");
  } else if (CMD_USTRING.substring(0, 2) == "AT") {
    for (CMD_CHARPOS ; CMD_CHARPOS < CMD_USTRING.length() ; CMD_CHARPOS++) {
#ifdef MODEM_DEBUG
        Serial.print("CMD: ");
        Serial.print(CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1));
        Serial.print(" ");
#endif
      if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "A") {         BAD_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "B") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "C") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "D") {  BAD_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "E") {  E_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "F") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "G") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "H") {  H_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "I") {  I_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "J") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "K") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "L") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "M") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "N") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "O") {  O_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "P") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "Q") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "R") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "S") {  MODEM_SET_REG();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "T") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "U") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "V") {  V_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "W") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "X") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "Y") {  IGNORE_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "Z") {  Z_CMDS();
      } else if (CMD_USTRING.substring(CMD_CHARPOS, CMD_CHARPOS+1) == "&") {  AMP_CMDS();
      } else {
#ifdef MODEM_DEBUG
          Serial.print("COMMAND NOT IMPLEMENTED");
#endif
        CMD_ERROR = true;
      }
#ifdef MODEM_DEBUG
        Serial.println();
#endif
    }
    if (CMD_ERROR == true) {SENDRESULT(4);} else {SENDRESULT(0);}
  }
  CMD_ERROR = false;
  CMD_STRING = "";
  CMD_USTRING = "";
}

void DISPLAY_HELP() {
  Serial.print("\r\n");
  Serial.print("at commands:\r\n");
  Serial.print("\r\n");
  Serial.print("a  - answer         ");
  Serial.print("dt - dial/connect   ");
  Serial.print("e# - echo on/off    ");
  Serial.print("h# - on/off Hook    ");
  Serial.print("i# - information    ");
  Serial.print("s# - modem registers");
  Serial.print("v# - verbose on/off ");
  Serial.print("\r\nz# - reset (0-soft, 8-verbose, 9-hard) \r\n");
  Serial.print("\r\nmost other hayes commands are ignored but do not produce and error.\r\n");
  Serial.print("\r\n");
  Serial.print("dialing/connecting:\r\n");
  Serial.print("atdt<address>:<port> - connect to <address> on <port>\r\n");
  Serial.print(" ie: atdtconnect.digitalrealms.net:23\r\n");
  Serial.print("\r\n");
  Serial.print("other commands:\r\n");
  Serial.print("baud<#> - set serial baud rate.  ie: baud57600\r\n");
  Serial.print("valid baud rates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200.\r\n");
  Serial.print("help - display this help menu\r\n");
  Serial.print("wifi<ssid>,<key> - connect wifi to <ssid> with <key>\r\n");
  Serial.print(" ie: wifimywifissid,mywifipassword\r\n");
}


void BAD_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {             // Check for digit for Hayes command and ignore.
    CMD_CHARPOS++;
  }
  CMD_ERROR = true;
}

void IGNORE_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {             // Check for digit for Hayes command and ignore.
    CMD_CHARPOS++;
  } else {
    CMD_ERROR = true;
  }
}

void E_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {             // ECHO ON/OFF
    CMD_CHARPOS++;
    SET_ECHO(CMD_USTRING.charAt(CMD_CHARPOS));
  } else {
    CMD_ERROR = true;
  }
}

void H_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    SET_HOOK(CMD_STRING.charAt(CMD_CHARPOS));
  } else {
    SET_HOOK('0');
  }
}

void I_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    MODEM_INFO(CMD_STRING.charAt(CMD_CHARPOS));
  } else {
    MODEM_INFO('1');
  }
}

void O_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    if (TCPCLIENT.connected()) {
      CMD_MODE = false;
      REPORT_SPEED();
      CMD_SHOWRESULT = false;
    } else {
      CMD_ERROR = true;
    }
  } else {
    if (TCPCLIENT.connected()) {
      CMD_MODE = false;
      REPORT_SPEED();
      CMD_SHOWRESULT = false;
    } else {
      CMD_ERROR = true;
    }
  }
}

void Q_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    SET_RESULTS(CMD_STRING.charAt(CMD_CHARPOS));
  } else {
    CMD_ERROR = true;
  }
}

void V_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {             // ECHO ON/OFF
    CMD_CHARPOS++;
    SET_VERBOSE(CMD_USTRING.charAt(CMD_CHARPOS));
  } else {
    CMD_ERROR = true;
  }
}

void Z_CMDS() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '0') {MODEM_RESET(0);}
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '1') {MODEM_RESET(1);}
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '2') {MODEM_RESET(2);}
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '8') {MODEM_RESET(0);START_MSG();}
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '9') {ESP.reset();}
  } else {
    MODEM_RESET(0);
  }
}

void AMP_CMDS() {
  CMD_CHARPOS++;
  if(CMD_USTRING.charAt(CMD_CHARPOS) == 'W') {AMP_W_CMD();}
}

void AMP_W_CMD() {
  if (isDigit(CMD_USTRING.charAt(CMD_CHARPOS+1))) {
    CMD_CHARPOS++;
    EEPROM_SET(1, 0);
    EEPROM_SET(2, 0);
    EEPROM_SET(3, 0);
    EEPROM_SET(4, 0);
    EEPROM_SET(5, 0);
    EEPROM_SET(6, 0);
    EEPROM_SET(7, 0);
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '0') {
      EEPROM_SET(8, MODEM_REG_S1);
      EEPROM_SET(9, MODEM_REG_S2);
      EEPROM_SET(10, MODEM_REG_S3);
      EEPROM_SET(11, MODEM_REG_S4);
      EEPROM_SET(12, MODEM_REG_S5);
      if (MODEM_ECHO) {EEPROM_SET(13, 1);} else {EEPROM_SET(13, 0);}
      if (MODEM_VERBOSE) {EEPROM_SET(14, 1);} else {EEPROM_SET(14, 0);}
      if (MODEM_RESULTS) {EEPROM_SET(15, 1);} else {EEPROM_SET(15, 0);}
    }
    if (CMD_USTRING.charAt(CMD_CHARPOS) == '1') {
      EEPROM_SET(16, MODEM_REG_S1);
      EEPROM_SET(17, MODEM_REG_S2);
      EEPROM_SET(18, MODEM_REG_S3);
      EEPROM_SET(19, MODEM_REG_S4);
      EEPROM_SET(20, MODEM_REG_S5);
      if (MODEM_ECHO) {EEPROM_SET(21, 1);} else {EEPROM_SET(21, 0);}
      if (MODEM_VERBOSE) {EEPROM_SET(22, 1);} else {EEPROM_SET(22, 0);}
      if (MODEM_RESULTS) {EEPROM_SET(23, 1);} else {EEPROM_SET(23, 0);}
    }
  } else {
    CMD_ERROR = true;
  }
}

void MODEM_SETBPS() {
#ifdef MODEM_DEBUG
    Serial.print("BAUD CHANGE: ");
#endif
  String VALUE = CMD_STRING.substring(4);
  
  int NEWBPS = 0;

  if (VALUE == "300") {NEWBPS = 300;EEPROM_SET(0,0);}
  else if (VALUE == "600") {NEWBPS = 600;EEPROM_SET(0,1);}
  else if (VALUE == "1200") {NEWBPS = 1200;EEPROM_SET(0,2);}
  else if (VALUE == "2400") {NEWBPS = 2400;EEPROM_SET(0,3);}
  else if (VALUE == "4800") {NEWBPS = 4800;EEPROM_SET(0,4);}
  else if (VALUE == "9600") {NEWBPS = 9600;EEPROM_SET(0,5);}
  else if (VALUE == "19200") {NEWBPS = 19200;EEPROM_SET(0,6);}
  else if (VALUE == "38400") {NEWBPS = 38400;EEPROM_SET(0,7);}
  else if (VALUE == "57600") {NEWBPS = 57600;EEPROM_SET(0,8);}
  else if (VALUE == "115200") {NEWBPS = 115200;EEPROM_SET(0,9);}
  else {
    CMD_ERROR = true;
  }

  if (NEWBPS > 0) {
#ifdef MODEM_DEBUG
    Serial.print(VALUE);
#endif
    delay(250);
    MODEM_BPS = NEWBPS;
    Serial.begin(NEWBPS);
  } else {
#ifdef MODEM_DEBUG
    Serial.print(VALUE);
    Serial.print(" Invalid");
#endif
  }
}

int FIND_CHR(String VALUE) {
  return CMD_USTRING.indexOf(VALUE, CMD_CHARPOS);
}

int FIND_STR(int VALUE) {
  for (VALUE ; VALUE < CMD_USTRING.length() ; VALUE++) {
    if (isDigit(CMD_USTRING.charAt(VALUE))) {
    } else {
      break;
    }
  }
  return VALUE;
}

int FIND_NUM(int VALUE) {
  for (VALUE ; VALUE < CMD_USTRING.length() ; VALUE++) {
    if (isDigit(CMD_USTRING.charAt(VALUE))) {
      break;
    }
  }
  return VALUE;
}

void MODEM_SET_REG() {
#ifdef MODEM_DEBUG
    Serial.print("SET REGISTER ");
#endif
  
  String REG_NUM;
  String REG_VALUE;
  int TEMP;

  // Find the seperator "=" and put the register number into a string for now.
  TEMP = FIND_CHR("=");
  REG_NUM = CMD_USTRING.substring(CMD_CHARPOS + 1, TEMP);
#ifdef MODEM_DEBUG
    Serial.print(REG_NUM);
    Serial.print(" = ");
#endif
  TEMP++;
  CMD_CHARPOS = TEMP;

  // Find the first non-numeric character.
  TEMP = FIND_STR(TEMP);
  // Now put the value into a string for now...
  REG_VALUE = CMD_USTRING.substring(CMD_CHARPOS, TEMP);
#ifdef MODEM_DEBUG
    Serial.print(REG_VALUE);
#endif
  if (REG_NUM == "1") {MODEM_REG_S1 = REG_VALUE.toInt();}
  if (REG_NUM == "2") {MODEM_REG_S2 = REG_VALUE.toInt();}
  if (REG_NUM == "3") {MODEM_REG_S3 = REG_VALUE.toInt();}
  if (REG_NUM == "4") {MODEM_REG_S4 = REG_VALUE.toInt();}
  if (REG_NUM == "5") {MODEM_REG_S5 = REG_VALUE.toInt();}
  CMD_CHARPOS = TEMP - 1;
}

void SET_ECHO(int VALUE) {
#ifdef MODEM_DEBUG
    Serial.print("ECHO ");
#endif
  if (VALUE == '0') {
    MODEM_ECHO = false;
#ifdef MODEM_DEBUG
      Serial.print("OFF");
#endif
  } else if (VALUE == '1') {
    MODEM_ECHO = true;
#ifdef MODEM_DEBUG
      Serial.print("ON");
#endif
  } else {
    CMD_ERROR = true;
  }
}

void SET_HOOK(int VALUE) {
#ifdef MODEM_DEBUG
    Serial.print("HOOK ");
#endif
  if (VALUE == '0') {
#ifdef MODEM_DEBUG
      Serial.print("ON");
#endif
    MODEM_ONHOOK = true;
    if (TCPCLIENT.connected()) {
      TCPCLIENT.stop();
      SENDRESULT(3);
      CMD_SHOWRESULT = false;
    }
    TCPSERVER.begin();
  } else if (VALUE == '1') {
#ifdef MODEM_DEBUG
      Serial.print("OFF");
#endif
    MODEM_ONHOOK = false;
    TCPSERVER.stop();
  } else {
    CMD_ERROR = true;
  }
}

void SET_VERBOSE(int VALUE) {
#ifdef MODEM_DEBUG
    Serial.print("VERBOSE ");
#endif
  if (VALUE == '0') {
    MODEM_VERBOSE = false;
#ifdef MODEM_DEBUG
      Serial.print("OFF");
#endif
  } else if (VALUE == '1') {
    MODEM_VERBOSE = true;
#ifdef MODEM_DEBUG
      Serial.print("ON");
#endif
  } else {
    CMD_ERROR = true;
  }
}

void EEPROM_SET(int ADDRESS, int VALUE) {
  EEPROM.write(ADDRESS, VALUE);
  EEPROM.commit();
}

int EEPROM_GET(int ADDRESS) {
  return EEPROM.read(ADDRESS);
}

void MODEM_INFO(int VALUE) {
  if (VALUE == '1'){
    Serial.println();
    if (MODEM_BPS > 300) {
      Serial.printf("mac address: ");
      Serial.println(WiFi.macAddress());
    }
    Serial.print("ip address : ");
    Serial.println(WiFi.localIP());
    if (MODEM_BPS > 300) {
      Serial.print("subnet mask: ");
      Serial.println(WiFi.subnetMask());
      Serial.print("gateway ip : ");
      Serial.println(WiFi.gatewayIP());
      Serial.print("dns 1      : ");
      Serial.println(WiFi.dnsIP(0));
      Serial.print("dns 2      : ");
      Serial.println(WiFi.dnsIP(1));
      Serial.print("hostname   : ");
      Serial.println(WiFi.hostname());
      Serial.print("status     : ");
      Serial.println(WiFi.status());
      Serial.print("ssid       : ");
      Serial.println(WiFi.SSID());
      Serial.print("psk        : ");
      Serial.println(WiFi.psk());
    }
  }
  
  if (VALUE == '2') {
    Serial.print("\r\nmodem registers:\r\n\r\n");
    Serial.print("s1 - auto answer                  : ");Serial.print(MODEM_REG_S1);Serial.print("\r\n");
    Serial.print("s2 - escape character             : ");Serial.print(MODEM_REG_S2);Serial.print("\r\n");
    Serial.print("s3 - carriage return character    : ");Serial.print(MODEM_REG_S3);Serial.print("\r\n");
    Serial.print("s4 - line Feed character          : ");Serial.print(MODEM_REG_S4);Serial.print("\r\n");
    Serial.print("s5 - backspace char               : ");Serial.print(MODEM_REG_S5);Serial.print("\r\n");
  }
  if (VALUE == '9') {
    Serial.println("\r\neeprom contents (first 24 bytes)");
    Serial.println();

    int EVALUE;
    int COL = 0;
    for (int i=0 ; i < 24 ; i++) {
      if (i < 100) {Serial.print(" ");}
      if (i < 10) {Serial.print(" ");}
      Serial.print(i);
      Serial.print("=");
      EVALUE = EEPROM_GET(i);
      Serial.print(EVALUE);
      if (EVALUE < 100) {Serial.print(" ");}
      if (EVALUE < 10) {Serial.print(" ");}
      COL++;if (COL == 8) {Serial.println();COL = 0;}else{Serial.print(" ");}
    }
    Serial.println();
  }
}

void WIFI_CONNECT(String VALUE) {
  int STR_POS = VALUE.indexOf(",");

  if (STR_POS != -1) {
    WIFI_SSID = VALUE.substring(0, STR_POS);
    WIFI_KEY = VALUE.substring(STR_POS + 1, VALUE.length());
  } else {
    WIFI_SSID = VALUE.substring(0, VALUE.length());
    WIFI_KEY = "";
  }

  if (WIFI_SSID != "") {
    char *ssidChr = new char[WIFI_SSID.length() + 1];
    WIFI_SSID.toCharArray(ssidChr, WIFI_SSID.length() + 1);
    char *keyChr = new char[WIFI_KEY.length() + 1];
    WIFI_KEY.toCharArray(keyChr, WIFI_KEY.length() + 1);
  
    Serial.print("connecting to ");
    Serial.print(WIFI_SSID);
  
    WiFi.begin(ssidChr, keyChr);
    for (int i=0; i<100; i++) {
      delay(100);
      Serial.print(".");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        SENDRESULT(0);
        break;
      }
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println();
      SENDRESULT(4);
    }
    delete ssidChr;
    delete keyChr;
  } else {
    WiFi.disconnect();
  }
}

void MODEM_ANSWER() {
#ifdef MODEM_DEBUG
    Serial.print("Answering Modem: ");
#endif
  if (TCPSERVER.hasClient()) {
    TCPCLIENT = TCPSERVER.available();
    TCPCLIENT.setNoDelay(true); // try to disable naggle
    TCPSERVER.stop();
    REPORT_SPEED();
    CMD_MODE = false;
    Serial.flush();
#ifdef MODEM_DEBUG
      Serial.println("Answered.");
#endif
  } else {
    SENDRESULT(3);
#ifdef MODEM_DEBUG
    Serial.println("Nothing to answer.");
#endif
  }
}

void MODEM_DIAL(String ADDRESS) {
  int TELNET_SEPERATOR = ADDRESS.indexOf(":");
  String TELNET_HOST, TELNET_PORT;

  if (WiFi.status() == 3) {
    if (TELNET_SEPERATOR != -1) {
      TELNET_HOST = ADDRESS.substring(0, TELNET_SEPERATOR);
      TELNET_PORT = ADDRESS.substring(TELNET_SEPERATOR + 1, ADDRESS.length());
    } else {
      TELNET_HOST = ADDRESS;
      TELNET_PORT = "23";
    }
  
    Serial.print("connecting to ");
    Serial.print(TELNET_HOST);
    Serial.print(":");
    Serial.println(TELNET_PORT);

    char *hostChr = new char[TELNET_HOST.length() + 1];
    TELNET_HOST.toCharArray(hostChr, TELNET_HOST.length() + 1);
    int portInt = TELNET_PORT.toInt();
    TCPCLIENT.setNoDelay(true); // Try to disable naggle
    
    if (TCPCLIENT.connect(hostChr, portInt)) {
      TCPCLIENT.setNoDelay(true); // Try to disable naggle
      REPORT_SPEED();
      CMD_MODE = false;
      Serial.flush();
      if (TELNET_LISTEN_PORT > 0) TCPSERVER.stop();
    } else {
      SENDRESULT(3);
    }
    delete hostChr;
  } else {
    SENDRESULT(6);
  }
}

void REPORT_SPEED() {
  CMD_SHOWRESULT = true;
  switch(MODEM_BPS) {
    case 300:SENDRESULT(1);break;
    case 1200:SENDRESULT(5);break;
    case 2400:SENDRESULT(10);break;
    case 4800:SENDRESULT(11);break;
    case 7200:SENDRESULT(24);break;
    case 9600:SENDRESULT(12);break;
    case 19200:SENDRESULT(14);break;
    case 38400:SENDRESULT(28);break;
    default:SENDRESULT(99);break;
  }
}

void SET_RESULTS(int VALUE) {
  if (VALUE == '0') {
    MODEM_RESULTS = true;
  } else if (VALUE == '1') {
    MODEM_RESULTS = false;
  } else {
    CMD_ERROR = true;
  }
}

void SENDRESULT(int CODE) {
  if (CMD_SHOWRESULT && MODEM_RESULTS) {
    if (MODEM_VERBOSE) {
      switch(CODE) {
        case 0:Serial.println("\r\nok");break;
        case 1:Serial.println("\r\nconnect");break;
        case 2:Serial.println("\r\nring");break;
        case 3:Serial.println("\r\nno carrier");break;
        case 4:Serial.println("\r\nerror");break;
        case 5:Serial.println("\r\nconnect 1200");break;
        case 6:Serial.println("\r\nno dialtone");break;
        case 7:Serial.println("\r\nbusy");break;
        case 8:Serial.println("\r\nno answer");break;
        case 10:Serial.println("\r\nconnect 2400");break;
        case 11:Serial.println("\r\nconnect 4800");break;
        case 12:Serial.println("\r\nconnect 9600");break;
        case 14:Serial.println("\r\nconnect 19200");break;
        case 24:Serial.println("\r\nconnect 7200");break;
        case 28:Serial.println("\r\nconnect 38400");break;
        case 99:
          Serial.print("\r\nconnect ");
          Serial.println(MODEM_BPS);
          break;
      }
    } else {
      if (CODE < 99) {
        Serial.println(String(CODE));
      } else {
        Serial.println("1");
      }
    }
  }
  CMD_SHOWRESULT = false;
}

/* Arduino main loop function */     
void loop() {
  // Handle DCD (Detect Carrier Detect)
  if (TCPCLIENT.connected()) {
    pinMode(MODEM_DCD_PIN, OUTPUT);
    digitalWrite(MODEM_DCD_PIN, HIGH);
  } else {
    pinMode(MODEM_DCD_PIN, OUTPUT);
    digitalWrite(MODEM_DCD_PIN, LOW);
  }

  if (WiFi.status() == 3 && WIFI_CONNECTED == false) {          // Report WiFi connect
    Serial.print("\r\nwifi connected - ip: ");
    Serial.println(WiFi.localIP());
    WIFI_CONNECTED = true;
  }
  if (WiFi.status() != 3 && WIFI_CONNECTED == true) {          // Report WiFi disconnect
    Serial.println("\r\nwifi disconnected");
    WIFI_CONNECTED = false;
  }
  
  if (CMD_MODE == true) { /**** AT command mode ****/
    if ((TELNET_LISTEN_PORT > 0) && (TCPSERVER.hasClient())) {  // In command mode but new unanswered incoming connection on server listen socket
      if ((millis() - MODEM_LASTRINGMS) > MODEM_RING_INTERVAL) {      // Print RING every now and then while the new incoming connection exists
        CMD_SHOWRESULT = true;
        SENDRESULT(2);
        MODEM_LASTRINGMS = millis();
        MODEM_RINGS++;
        if (MODEM_REG_S1 > 0) {                                 // Auto Answer
          if (MODEM_RINGS >= MODEM_REG_S1) {
            MODEM_ANSWER();
          }
        }
        if (MODEM_RINGS >= MODEM_MAX_RINGS) {                  // Disconnect after so many rings
            TCPCLIENT = TCPSERVER.available();
            TCPCLIENT.setNoDelay(true); // try to disable naggle
            TCPCLIENT.stop();
            MODEM_RINGS = 0;
        }
      }
    }
    
    // In command mode - don't exchange with TCP but gather characters to a string
    if (Serial.available()) {
      LED_ON();
      char chr = Serial.read();
      if ((chr == MODEM_REG_S4) || (chr == MODEM_REG_S3)) { // Return, enter, new line, carriage return.. anything goes to end the command
        if (MODEM_ECHO) {Serial.println();}
        AT_COMMAND();
      } else if ((chr == MODEM_REG_S5) || (chr == 127) || (chr == 20)) {  // Backspace or delete deletes previous character
        CMD_STRING.remove(CMD_STRING.length() - 1);
        if (MODEM_ECHO) {
          // We don't assume that backspace is destructive, clear with a space.
          Serial.write(chr);
          Serial.write(' ');
          Serial.write(chr);
        }
      } else if ((CMD_STRING.substring(0,1) == "a" || CMD_STRING.substring(0,1) == "A") && chr == '/') {  // Process "a/" command.
        if (MODEM_ECHO) {Serial.println(chr);}
        CMD_STRING = MODEM_LASTCMD;
        AT_COMMAND();
      } else {
        if (CMD_STRING.length() < CMD_MAX_LENGTH) {CMD_STRING.concat(chr);}
        if (MODEM_ECHO) {Serial.print(chr);}
      }
    }
  } else {  /**** Connected mode ****/
    // Transmit from terminal to TCP
    if (Serial.available()) {SERIAL_IN();}

    // Transmit from TCP to terminal
    while (TCPCLIENT.available()) {
      if (TELNET_CMD == true) {
        DOTELNET();
      } else {
        LED_ON();
        uint8_t rxByte = TCPCLIENT.read();
        if ((TELNET_CTRL == true) && (rxByte == TELNET_IAC)) {
          TELNET_CMD = true;
        } else {
          // Non-control codes pass through freely
          Serial.write(rxByte); 
          Serial.flush();
          if (Serial.available()) {SERIAL_IN();}
        }
      }
      yield();
    }
  }

  // If we have received "+++" as last bytes from serial port and there
  // has been over a second without any more bytes, disconnect
  if (MODEM_ESCAPE_COUNT >= 3) {
    if (millis() - MODEM_ESCAPE_TIME > 1000) {
      CMD_SHOWRESULT = true;
      CMD_STRING = "";
      CMD_MODE = true;
      SENDRESULT(0);
      MODEM_ESCAPE_COUNT = 0;
    }
  }

  // Go to command mode if TCP disconnected and not in command mode
  if ((!TCPCLIENT.connected()) && (CMD_MODE == false)) {
    CMD_STRING = "";
    CMD_MODE = true;
    CMD_SHOWRESULT = true;
    SENDRESULT(3);
    if (TELNET_LISTEN_PORT > 0) TCPSERVER.begin();
  }

  // Turn off tx/rx led if it has been lit long enough to be visible
  if (millis() - LEDTime > LED_TIME) digitalWrite(LED_PIN, HIGH);

  
}

void DOTELNET() {
  uint8_t rxByte = TCPCLIENT.read();

  if (rxByte == TELNET_IAC) {
    // 2 times 0xff is just an escaped real 0xff
    Serial.write(TELNET_IAC);
    Serial.flush();
    TELNET_CMD = false;
  } else {
    uint8_t CMDBYTE1 = rxByte;
    rxByte = TCPCLIENT.read();
    uint8_t CMDBYTE2 = rxByte;
    #ifdef TELNET_DEBUG
      Serial.print("\r\nTELNET RECV: ");
      TELNET_CMDS(CMDBYTE1);
      TELNET_SCMDS(CMDBYTE2);
    #endif
    switch(CMDBYTE1) {
      case TELNET_DO:
        switch(CMDBYTE2) {
          case TELNET_SENDLOC:TELNET_SEND(TELNET_WONT, TELNET_SENDLOC);break;
          case TELNET_TERMTYPE:TELNET_SEND(TELNET_WILL, TELNET_TERMTYPE);break;
          case TELNET_TERMSPEED:TELNET_SEND(TELNET_WONT, TELNET_TERMSPEED);break;
          case TELNET_NAWS:
            TELNET_SEND(TELNET_WILL, TELNET_NAWS);
            TELNET_SEND(TELNET_SB, TELNET_NAWS);
            TCPSEND(0x0);
            TCPSEND(0x50);    // 80 Cols
            TCPSEND(0x0);
            TCPSEND(0x18);    // 24 Rows
            TCPSEND(TELNET_IAC);
            TCPSEND(TELNET_SE);
            break;
          default:TELNET_SEND(TELNET_WONT, CMDBYTE2);break;
        }
        break;
      case TELNET_WILL:
        // Server wants to do any option, allow it
        TELNET_SEND(TELNET_DO, CMDBYTE2);
        break;
      case TELNET_SB:
        switch(CMDBYTE2) {
          case TELNET_TERMTYPE:
            rxByte = TCPCLIENT.read();
            if (rxByte == 0x1) {
              rxByte = TCPCLIENT.read();
              rxByte = TCPCLIENT.read();
              TELNET_SEND(TELNET_SB, TELNET_TERMTYPE);
              TCPSEND(0);
              TCPSEND('A');
              TCPSEND('N');
              TCPSEND('S');
              TCPSEND('I');
              TCPSEND(TELNET_IAC);
              TCPSEND(TELNET_SE);
            }
            break;
        }
        break;
    }
  }
  TELNET_CMD = false;
  pinMode(15, OUTPUT);
  digitalWrite(15, LOW);
}

void TCPSEND(byte DATA) {
  TCPCLIENT.write(DATA);
  #ifdef TELNET_DEBUG
    Serial.print(" ");
    if (DATA < 10) {Serial.print("0");}
    Serial.print(DATA, HEX);
  #endif
}

void TELNET_SEND(byte CMD1, byte CMD2) {
  #ifdef TELNET_DEBUG
    Serial.print("\r\nTELNET SEND: ");
    TELNET_CMDS(CMD1);
    TELNET_SCMDS(CMD2);
  #endif
  TCPSEND(TELNET_IAC);
  TCPSEND(CMD1);
  TCPSEND(CMD2);
}

void TELNET_CMDS(byte COMMAND) {
  switch(COMMAND) {
    case TELNET_SE:Serial.print("se");break;
    case TELNET_SB:Serial.print("sb");break;
    case TELNET_GA:Serial.print("go ahead");break;
    case TELNET_DO:Serial.print("do");break;
    case TELNET_DONT:Serial.print("don't");break;
    case TELNET_WILL:Serial.print("will");break;
    case TELNET_WONT:Serial.print("won't");break;
  }
  Serial.print("(");
  Serial.print(COMMAND);
  Serial.print(") ");
}

void TELNET_SCMDS(byte COMMAND) {
  switch(COMMAND) {
    case TELNET_ECHO:Serial.print("echo");break;
    case TELNET_SGA:Serial.print("supress-go-ahead");break;
    case TELNET_SENDLOC:Serial.print("send-location");break;
    case TELNET_TERMTYPE:Serial.print("terminal-type");break;
    case TELNET_NAWS:Serial.print("negotiate about window size");break;
    case TELNET_TERMSPEED:Serial.print("terminal-speed");break;
  }
  Serial.print("(");
  Serial.print(COMMAND);
  Serial.print(")");
  Serial.flush();
}

void SERIAL_IN() {
      LED_ON();

      // In telnet in worst case we have to escape every byte so leave half of the buffer always free
      int RX_BUF_SIZE;
      if (TELNET_CTRL == true) {
        RX_BUF_SIZE = TX_BUF_SIZE / 2;
      } else {
        RX_BUF_SIZE = TX_BUF_SIZE;
      }

      // Read from serial, the amount available up to maximum size of the buffer
      size_t len = std::min(Serial.available(), RX_BUF_SIZE);
      Serial.readBytes(&TX_BUFFER[0], len);

      // Disconnect if going to AT mode with "+++" sequence
      for (int i=0; i<(int)len; i++) {
        if (TX_BUFFER[i] == MODEM_REG_S2) {MODEM_ESCAPE_COUNT++;} else {MODEM_ESCAPE_COUNT = 0;}
        if (MODEM_ESCAPE_COUNT >= 3) {MODEM_ESCAPE_TIME = millis();}
        if (TX_BUFFER[i] != MODEM_REG_S2) {MODEM_ESCAPE_COUNT = 0;}
      }

      // Double (escape) every 0xff for telnet, shifting the following bytes towards the end of the buffer from that point
      if (TELNET_CTRL == true) {
        for (int i = len - 1; i >= 0; i--) {
          if (TX_BUFFER[i] == TELNET_IAC) {
            for (int j = TX_BUF_SIZE - 1; j > i; j--) {
              TX_BUFFER[j] = TX_BUFFER[j - 1];
            }
            len++;
          }
        }
      }

      // Write the buffer to TCP finally
      TCPCLIENT.write(&TX_BUFFER[0], len);
      yield();
}
