# WiFi Retro Modem

Heavily modified from Jussi Salin's ESP8266 based virtual modem code.

Emulates a Hayes modem in an ESP8266 WiFi module. Supports important Hayes commands. DCD (Detect Carrier Detect) is on GPIO15 and works correctly for programs that need it.

Supported AT commands:

A  - answer         
DT - dial/connect   
E# - echo on/off    
H# - on/off Hook    
I# - information    
S# - modem registers
V# - verbose on/off 
Z# - reset (0-soft, 8-verbose, 9-hard) 

Most other hayes commands are ignored but do not produce and error.

Dialing/Connecting:
ATDT<address>:<port> - connect to <address> on <port>
 Example: ATDTconnect.digitalrealms.net:23

Other Commands:
BAUD<#> - set serial baud rate.  ie: BAUD57600
valid baud rates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200.
WIFI<ssid>,<key> - connect WiFi to <ssid> with <key>
 Example: WIFImywifissid,mywifipassword

Modem Registers:

S1 - Auto Answer
S2 - Escape Character
S3 - Carriage Return Character
S4 - Line Feed Character
S5 - Backspace Character
