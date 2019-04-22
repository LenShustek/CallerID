 Caller ID display

   This is the software for a small box that displays the caller ID information generated
   by the ethernet version of the "Whozz Calling?"(tm) multi-line caller ID hardware.
   See https://www.callerid.com/.

   Our display box, measuring 4.4" x 2.3" x 1.1" and powered by a USB wall-wart, contains:
      - a 4-line 20-character display
      - two pushbuttons for scrolling the display up and down
      - an Adafruit Feather Huzzah ESP8266 processor with WiFi

   The box receives, over WiFi, the call information that is broadcast by the WhoozCalling
   hardware for up to 8 phone lines. Multiple display boxes can be deployed and will display
   the same information. Each box records up to 200 calls while it is powered on.

   The most recent 70 calls (although without call duration) are kept in
   non-volatile memory and are reloaded into the display if the box is restarted.
   (In the code we call the non-volatile memory EEPROM, but it's implemented as
   one block of the FLASH memory that is written all at once.)

   There are several pieces of configuration information to set up:
     - the SSID of the WiFi network
     - the password for the WiFi network
     - the names to be displayed for lines 1 through 8
     - whether the backlight should be turned off when idle

   The configuration information is requested if it has never been set, or
   if both buttons are pushed at the same time and then released.

   The configuration strings can be set, painfully, using the two pushbuttons.
   The top button cycles through choices for a character, and the bottom button
   goes to the next character, or to the next configuration string.

   But you can also use a computer keyboard to set configuration strings as follows:
     - plug the device into a USB port of your computer
     - run a terminal emulator program set to 9600 baud
        (For PuTTY on Windows: choose a "Serial" connection line like "COM27"
     - type each name asked for, with "Enter" signaling done. Backspace works.

   The packet format is not thoroughly documented by Whozz Calling, so there may be 
   some I don't understand that generate a "bad packet" message. Pushing both
   buttons for more than three seconds and then releasing will display the contents
   of the last such puzzling packet so we can add a decode for it.
   
   Len Shustek
   April 2019