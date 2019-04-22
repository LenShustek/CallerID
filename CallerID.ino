/**************************************************************************************************

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


   ------------------------------------------------------------------------------------------------
   Copyright (c) 2019, Len Shustek
     The MIT License (MIT)
   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
   and associated documentation files (the "Software"), to deal in the Software without
   restriction, including without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or
   substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
   ------------------------------------------------------------------------------------------------
  *** CHANGE LOG ***

   12 Mar 2019, V1.0, L. Shustek
      first version
   16 Apr 2019, V1.1, L. Shustek
      allow serial port to supply config info
      add diagnostic mode to show last bad packet
   21 Apr 2019, V1.2, L. Shustek
      add a mode to turn off backlight after a few seconds


   Programmers beware: the Feather ESP8266 has a watchdog timer that reboots the machine
   after a while (100 msec?), so every tight loop has to incorporate a call to yield().

 ************************************************************************************************/
#define VERSION "1.2"

#define TESTCALLS_GOOD false     // generate some good fake calls for testing?
#define TESTCALLS_BAD false      // generate some fake bad calls for testing?

#define UDP_PORT 3520  // Whozz Calling box parameters
#define MAX_LINES 8
#define BACKLIGHT_TIMEOUT_SECS 10

#include <Arduino.h>
#include <LiquidCrystal.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

// Feather pin assignments
#define UP_SWITCH 4
#define DN_SWITCH 5
#define BACKLIGHT 16
LiquidCrystal lcd(0, 2, 12, 13, 14, 15); // RS, E, D4, D5, D6, D7

#define MAX_CALLS 200       // number of call records we save in RAM
#define MAX_PACKET 255      // the biggest UDP packet to try to read
#define EEPROM_SIZE 4096    // on the Feather we get to choose up to 4096 of "EEPROM"
#define WIFI_NAMELENGTH 31  // max length for SSID name and password
#define LINE_NAMELENGTH 15  // max length for line names
#define DEBOUNCE 100        // debounce time in msec

WiFiUDP udp;
/*  WL_IDLE_STATUS      = 0,
    WL_NO_SSID_AVAIL    = 1,
    WL_SCAN_COMPLETED   = 2,
    WL_CONNECTED        = 3,
    WL_CONNECT_FAILED   = 4,
    WL_CONNECTION_LOST  = 5,
    WL_DISCONNECTED     = 6 */
char packet[MAX_PACKET];
char *pktptr;
int pktlen;
unsigned long backlight_turnon_time;
bool backlight_on;

// these arrays for fields that we parse from the packet are all
// one byte larger because they are 0-terminated as C strings
char unitnum[7] = {0 };
char serialnum[7] = {0 };
char phoneline[3] = {0 };
char duration[5] = {0 };
char atype[2] = {0 };
char datetime[16] = {0 };
char phonenum[16] = {0 };
char callername[16] = {0 };

struct call_record_t {  // what we record for each call
   byte phoneline;
   short int seconds;
   char datetime[16];
   char phonenum[16];
   char callername[16]; }
calls[MAX_CALLS];

short int num_calls = 0;     // the number of calls stored in RAM
short int oldest = 0;        // the array index of the oldest call
short int newest = 0;        // the array index of the newest call
short int displayed = 0;     // the array index of the call currently displayed
short int displayed_age = 1; // how far back from the newest that one is
int connect_attempts = 0;
int connect_successes = 0;
void badpacket_display(void);

//********* utility routines

char string[40];

void assert (boolean test, const char *msg) {
   if (!test) {
      lcd.clear(); lcd.print("** INTERNAL ERROR **");
      lcd.setCursor(0, 1);  lcd.print("Assertion failed:");
      lcd.setCursor(0, 2);  lcd.print(msg);
      while (true) yield(); } }

void center_message (byte row, const char *msg) {
   byte len = strlen(msg);
   assert (len <= 20, "bad center_message");
   byte nblanks = (20 - len) >> 1;
   lcd.setCursor(0, row);
   for (byte i = 0; i < nblanks; ++i) lcd.print(' ');
   lcd.print(msg);
   for (byte i = 0; i < (20 - nblanks) - len; ++i) lcd.print(' ');
   lcd.setCursor(0, row); // leave the cursor at the start of the line
}

void long_message(byte row, const char *msg) { // print a string multiple lines long
   lcd.setCursor(0, row);
   for (int i = 0; i < strlen(msg); ++i) {
      lcd.print(msg[i]);
      if (i % 20 == 19) lcd.setCursor(0, ++row); } }

void turn_on_backlight(void) {
   digitalWrite(BACKLIGHT, HIGH);
   backlight_on = true;
   backlight_turnon_time = millis(); }

void turn_off_backlight(void) {
   digitalWrite(BACKLIGHT, LOW);
   backlight_on = false; }

//******* routines for managing the non-volatile memory

// In addition to configuration information, we store the most recent calls in
// non-volatile FLASH memory so that they are preserved across power loss in the box.

struct eeprom_hdr_t {  // the header at the start of non-volatile memory
   char id[8];                        // null-terminated 7-character ID string to mark validity
#define ID_STRING "Calllog"
   byte options;                        // various options
#define OPT_BACKLIGHT_OFF 0x01
   byte reserved[3];
   char ssid[WIFI_NAMELENGTH + 1];    // null-terminated WiFi SSID
   char password[WIFI_NAMELENGTH + 1];// null-terminated WiFi password
   char line_names[MAX_LINES][LINE_NAMELENGTH + 1]; // null-terminated line names
   short int num_calls;
   short int oldest, newest; }
eeprom_hdr;
#define NUM_EEPROM_CALLS ((EEPROM_SIZE - sizeof(struct eeprom_hdr_t)) / sizeof(struct call_record_t))
// After the header there is an array of NUM_EEPROM_CALLS call_record_t records

void eeprom_write(int addr, int length, byte *srcptr) {
   while (length--)
      EEPROM.write(addr++, *srcptr++); }

void eeprom_read(int addr, int length, byte *dstptr) {
   while (length--)
      *dstptr++ = EEPROM.read(addr++); }

void eeprom_load(void) {  // on startup, load calls saved in the FLASH memory archive
   eeprom_read(0, sizeof(struct eeprom_hdr_t), (byte *)&eeprom_hdr); // read the header
   if (memcmp(eeprom_hdr.id, ID_STRING, 8) != 0 // if FLASH isn't initialized,
         || digitalRead(DN_SWITCH) == 0 // or if a button is pressed during power up
         || digitalRead(UP_SWITCH) == 0) { //*** initialize FLASH memory
      memset(&eeprom_hdr, 0, sizeof(struct eeprom_hdr_t));
      strcpy(eeprom_hdr.id, ID_STRING);
      for (int i = 0; i < MAX_LINES; ++i)
         sprintf(eeprom_hdr.line_names[i], "line %d", i + 1);
      eeprom_write(0, sizeof(struct eeprom_hdr_t), (byte *)&eeprom_hdr);
      EEPROM.commit(); // write the header to FLASH
      center_message(1, "memory initialized");
      sprintf(string, "capacity: %d calls", NUM_EEPROM_CALLS);
      center_message(2, string); }
   else { //*** read calls from FLASH memory
      oldest = newest = 0;
      short int ndx = eeprom_hdr.oldest;
      for (num_calls = 0; eeprom_hdr.num_calls && num_calls < eeprom_hdr.num_calls; ++num_calls) {
         if (num_calls > 0) if (++newest >= MAX_CALLS) newest = 0;
         eeprom_read(sizeof(struct eeprom_hdr_t) + ndx * sizeof(struct call_record_t),
                     sizeof(struct call_record_t),
                     (byte *)&calls[newest]);
         if (++ndx >= NUM_EEPROM_CALLS) ndx = 0; }
      sprintf(string, "%d calls in memory", num_calls);
      center_message(1, string); }
   delay(2000); }

void eeprom_add_call(void) { // add the latest call to the FLASH memory archive
   if (eeprom_hdr.num_calls == 0) eeprom_hdr.num_calls = 1;
   else {
      if (++eeprom_hdr.newest >= NUM_EEPROM_CALLS) eeprom_hdr.newest = 0;
      if (eeprom_hdr.num_calls >= NUM_EEPROM_CALLS) {
         if (++eeprom_hdr.oldest >= NUM_EEPROM_CALLS) eeprom_hdr.oldest = 0; }
      else ++eeprom_hdr.num_calls; }
   eeprom_write( // rewrite the header
      0, sizeof(struct eeprom_hdr_t), (byte *)&eeprom_hdr);
   eeprom_write( // write the call record in the right location
      sizeof(struct eeprom_hdr_t) + eeprom_hdr.newest * sizeof(struct call_record_t),
      sizeof(struct call_record_t),
      (byte *)&calls[newest]);
   EEPROM.commit(); }

//******  parsing routines for WhozzCalling packets

bool match(const char *literal) { // match a literal string from the call line
   char *startptr = pktptr;
   int length = strlen(literal);
   while (length--)
      if (*literal++ != *pktptr++) {
         pktptr = startptr;
         return false; }
   return true; }

bool copy (char *dst, int len) { // copy a fixed-width variable string from the call line
   while (len--)
      if (pktptr < packet + pktlen)
         *dst++ = *pktptr++;
      else *dst++ = 0;
   return true; }

byte parse_phoneline(void) { // convert the character "line" identifier to an integer
   int linenum;
   if (sscanf(phoneline, "%d", &linenum) == 1
         && linenum >= 1 && linenum <= MAX_LINES)
      return (byte) linenum - 1;
   else return MAX_LINES - 1; // bad line identifier: map to the last line
}

//****** routines for managing the call list in memory

void add_call(void) { // add a call record to the database
   if (num_calls == 0) num_calls = 1;
   else {
      if (++newest >= MAX_CALLS) newest = 0;
      if (num_calls >= MAX_CALLS) {
         if (++oldest >= MAX_CALLS) oldest = 0; }
      else ++num_calls; }
   calls[newest].phoneline = parse_phoneline();
   calls[newest].seconds = 0;
   memcpy(calls[newest].datetime, datetime, sizeof(datetime));
   memcpy(calls[newest].phonenum, phonenum, sizeof(phonenum));
   memcpy(calls[newest].callername, callername, sizeof(callername)); }

void display_call(short int ndx) { // display a database record
   lcd.clear();
   displayed = ndx;
   if (ndx == newest) displayed_age = 1;
   lcd.print(displayed_age); lcd.print(": "); lcd.print(eeprom_hdr.line_names[calls[ndx].phoneline]);
   lcd.setCursor(0, 1); lcd.print(calls[ndx].datetime);
   if (calls[ndx].seconds > 0) {
      lcd.print(calls[ndx].seconds);
      lcd.print("s"); }
   lcd.setCursor(0, 2); lcd.print(calls[ndx].phonenum);
   lcd.setCursor(0, 3); lcd.print(calls[ndx].callername); }

void show_newest_call(void) {
   if (num_calls > 0) display_call(newest);
   else {
      lcd.clear();
      lcd.setCursor(6, 0); lcd.print("no calls"); } }

void update_duration(void) { // update the duration of a call alcready stored
   // We just got an "end call" record from WhozzCalling.
   // Search for the corresponding "start call" and update the duration.
   // We don't update the FLASH memory record for that call to keep down
   // the number of lifetime writes to it, which are limited.
   short int seconds;
   if (sscanf(duration, "%hd", &seconds) == 1
         && num_calls > 0) {
      short int ndx = newest;
      byte phoneline = parse_phoneline();
      while (1) {
         if (calls[ndx].phoneline == phoneline) {
            calls[ndx].seconds = seconds;
            if (displayed == ndx) display_call(ndx); // update display if this call is visible
            return; }
         if (ndx == oldest) return;
         if (--ndx < 0) ndx = MAX_CALLS - 1; } } }

//******* routines for processing the buttons

bool double_button_push = false;
unsigned long switch_pushed_time;

bool switch_push (int pin) { // true when button pushed and released
   yield();
   if (digitalRead(pin) == 0) {
      turn_on_backlight();
      delay(DEBOUNCE);
      unsigned long startpush = millis();
      while (digitalRead(pin) == 0) {
         if (digitalRead(DN_SWITCH) == 0 && digitalRead(UP_SWITCH) == 0)
            double_button_push = true;
         yield(); }
      delay(DEBOUNCE);
      switch_pushed_time = millis() - startpush;
      return true; }
   return false; }

#define INPUT_ROW 1
#define CHANGE_SWITCH UP_SWITCH
#define ACCEPT_SWITCH DN_SWITCH
static char alphabet[] = {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 '!$@^`,|%;.~()/\{}:?[]=-+_#!" };
int glyph, input_col;

void show_character(char ch) {
   int row, col;
   if (input_col < 20) {
      row = INPUT_ROW; col = input_col; }
   else {
      row = INPUT_ROW + 1; col = input_col - 20; }
   if (ch) {
      lcd.setCursor(col, row); lcd.print(ch); }
   lcd.setCursor(col, row); lcd.cursor(); }

char next_glyph(void) { // uses: input_col; sets: glyph; returns: character
   if (++glyph >= sizeof(alphabet)) glyph = 0;
   char ch = alphabet[glyph];
   show_character(ch);
   return ch; }

// get a configuration string, up to two lines long
void input_string(const char *title, char *dst, int length) {
   lcd.clear(); center_message(0, title);
   while (Serial.available() > 0) Serial.read(); // purge buffered characters
   if (*dst) { // there is a current value; should we use it?
      long_message(INPUT_ROW, dst);
      lcd.setCursor(0, 3); lcd.print("ok            ");
      do {  // wait for "ok" or "change"
         yield();
         if (switch_push(ACCEPT_SWITCH)) goto done; }
      while (!switch_push(CHANGE_SWITCH) && Serial.available() == 0);
      if (Serial.peek() == '\n' || Serial.peek() == '\r' ) {
         Serial.print("\r\n"); goto done; } }
   center_message(INPUT_ROW, "");
   center_message(INPUT_ROW + 1, "");
   memset(dst, 0, length);
   input_col = 0;
   glyph = -1;
   dst[input_col] = next_glyph(); // start with the first possible character
   while (true) { // for all columns
      lcd.setCursor(0, 3); lcd.print("character done");
      show_character(0); // show cursor
      do { // for all choices of character
         yield();
         if (digitalRead(CHANGE_SWITCH) == 0) { // top switch: try the next char in this position
            delay(DEBOUNCE);
            dst[input_col] = next_glyph();
            unsigned long startpush = millis();
            while (digitalRead(CHANGE_SWITCH) == 0) {
               yield();
               if (millis() > startpush + 1000) { // after 1 sec, fast forward through characters
                  dst[input_col] = next_glyph();
                  delay(250); } }
            delay(DEBOUNCE); // top switch released
         }
         if (Serial.available() > 0) { // or get a character from the serial port instead
            char ch = Serial.read();
            if (ch == '\n' || ch == '\r') {
               Serial.print("\r\n"); goto done; }
            Serial.write(ch);
            if (ch == '\b' || ch == 0x7f) {
               dst[input_col] = 0;
               if (input_col > 0) {
                  dst[--input_col] = 0; show_character(' '); }
               --input_col; }
            else { // we got a character
               dst[input_col] = ch;
               show_character(ch); }
            break; } // done with this character
      } // choices of character
      while (!switch_push(ACCEPT_SWITCH));
      lcd.setCursor(0, 3); lcd.print("name done     ");
      if (++input_col >= length) break;  // last character
      show_character(0); // just show cursor at the new position
      do { // wait for "name done" or "change", or serial input
         yield();
         if (switch_push(ACCEPT_SWITCH)) goto done; }
      while (!switch_push(CHANGE_SWITCH) && Serial.available() == 0); //
      if (Serial.available() == 0) {
         glyph = -1;
         dst[input_col] = next_glyph(); } // start with the first possible character
   } // next column
done:
   lcd.noCursor();
   Serial.print(title); Serial.print(":");
   Serial.print(dst); Serial.print("\r\n"); }

// ask a yes/no question, up to two lines long
bool ask_yesno(const char *question) {
   lcd.clear(); long_message(1, question);
   lcd.setCursor(0, 0); lcd.print("yes");
   lcd.setCursor(0, 3); lcd.print("no");
   while (true)
      if (switch_push(UP_SWITCH)) return true;
      else if (switch_push(DN_SWITCH)) return false; }

void do_configuration(void) { // ask for (or read from the serial port) the configuration information
   input_string("WiFi network", eeprom_hdr.ssid, WIFI_NAMELENGTH);
   input_string("WiFi password", eeprom_hdr.password, WIFI_NAMELENGTH);
   for (int i = 0; i < MAX_LINES; ++i) {
      sprintf(string, "Line %d name", i + 1);
      input_string(string, eeprom_hdr.line_names[i], LINE_NAMELENGTH); }
   if (ask_yesno("Turn backlight off  when idle?"))
      eeprom_hdr.options |= OPT_BACKLIGHT_OFF;
   else eeprom_hdr.options &= ~OPT_BACKLIGHT_OFF;
   eeprom_write(0, sizeof(struct eeprom_hdr_t), (byte *)&eeprom_hdr);
   EEPROM.commit(); // write the header to FLASH
   lcd.clear(); center_message(0, "Names recorded");
   delay(2000); }

void process_scroll_buttons(void) {
   double_button_push = false;
   if (switch_push(UP_SWITCH) && num_calls > 0 && displayed != oldest) {
      if (--displayed < 0) displayed = MAX_CALLS - 1;
      ++displayed_age;
      display_call(displayed); }
   if (switch_push(DN_SWITCH) && num_calls > 0 && displayed != newest) {
      if (++displayed >= MAX_CALLS) displayed = 0;
      --displayed_age;
      display_call(displayed); }
   if (double_button_push) {  // both buttons:
      if (switch_pushed_time > 3000) // if long push, show last bad packet
         badpacket_display();
      else { // if short push, do configuration
         do_configuration();
         show_newest_call(); } } }

//****** routines for reading packets from the WiFi network

void join_network(void) {
   lcd.clear(); lcd.print(eeprom_hdr.ssid);
   int tries = 0;
   center_message(2, "connecting");
   ++connect_attempts;
   WiFi.begin(eeprom_hdr.ssid, eeprom_hdr.password);
   while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      if (++tries > 10) {
         center_message(2, "could not connect");
         delay(1000);
         center_message(2, "retrying connection");
         ++connect_attempts;
         WiFi.begin(eeprom_hdr.ssid, eeprom_hdr.password); // reset and try again
         tries = 0; }
      if (digitalRead(DN_SWITCH) == 0 && digitalRead(UP_SWITCH) == 0)
         do_configuration(); }
   center_message(2, ""); lcd.print("connected");
   ++connect_successes;
   udp.begin(UDP_PORT);
   lcd.print(", listening");
   sprintf(string, "%d tries, %d ok", connect_attempts, connect_successes);
   center_message(3, string);
   delay(2000);
   show_newest_call(); }

int read_packet(void) { // read a packet and return its length
   static const char *testcalls[] = {
      #if TESTCALLS_GOOD  // fake good calls we used for testing without the WhozzCalling box
      "^^<U>123456<S>123456$01 I S 0000 G A0 03/14 10:55 PM 111 111 1111   Caller one",
      "^^<U>123456<S>123456$01 I E 0003 G A2 03/14 10:56 PM 111 111 1111   Caller one",
      "^^<U>123456<S>123456$03 I S 0000 G A0 03/14 11:00 PM 222 222 2222   Caller two",
      "^^<U>123456<S>123456$01 I S 0000 G A0 03/14 12:22 PM 333 333 3333   Caller three",
      "^^<U>123456<S>123456$01 I E 0007 G A2 03/14 12:23 PM 333 333 3333   Caller three",
      "^^<U>123456<S>123456$03 I E 0005 G A2 03/14 13:01 PM 222 222 2222   Caller two",
      #endif
      #if TESTCALLS_BAD // fake bad calls we used to test our bad packet processing
      "^^<U>123456<S>123456$01 I S 0000 G x0 03/14 10:55 PM 111 111 1111   Caller one",
      #endif
      0 };
   static int testcallnum = 0;
   if (testcalls[testcallnum] != 0) { // use a prestored test call
      strcpy(packet, testcalls[testcallnum++]);
      delay(1000);
      return strlen(packet); }
   int packet_size = udp.parsePacket(); // otherwise check for a real UDP packet
   if (packet_size)
      return udp.read(packet, MAX_PACKET);
   else return 0; }

//********  bad packet processing

// We currently save only the most recent bad packet
int badpacket_count = 0, badpacket_length, badpacket_type;
char badpacket_data[MAX_PACKET];

void badpacket(int type) {
   lcd.clear();
   lcd.print("bad "); lcd.print(type); lcd.print(", length "); lcd.print(pktlen);
   lcd.setCursor(0, 1); lcd.print("IP:");
   IPAddress remote = udp.remoteIP();
   for (int i = 0; i < 4; ++i) { // show the IP address
      lcd.print(remote[i], HEX);
      if (i < 3) lcd.print("."); }
   lcd.setCursor(0, 2); // show some of the data
   for (int i = 0; i < 10; ++i) lcd.print(pktptr[i], HEX);
   ++badpacket_count;
   badpacket_length = pktlen;
   badpacket_type = type;
   memcpy(badpacket_data, packet, pktlen);  }

void badpacket_display(void) {
   lcd.clear();
   lcd.setCursor(0, 4); lcd.print("next");
   lcd.setCursor(0, 0);
   if (badpacket_count == 0) {
      lcd.print("no bad packets");
      while (!switch_push(DN_SWITCH)) yield(); }
   else {
      char str[25];
      sprintf(str, "%d bad, L%d T%d",
              badpacket_count, badpacket_length, badpacket_type);
      lcd.print(str);
      int offset = 0;
      while (offset < badpacket_length) { // display 8 bytes at a time
         sprintf(str, "%3d:", offset); // in both ASCII and hex
         lcd.setCursor(0, 1); lcd.print(str);
         for (int ndx = 0; ndx < min(8, badpacket_length - offset); ++ndx) {
            yield();
            char ch = badpacket_data[offset + ndx];
            sprintf(str, " %c", isprint(ch) ? ch : '.');
            lcd.setCursor(4 + ndx * 2, 1); lcd.print(str);
            sprintf(str, "%02X", ch);
            lcd.setCursor(4 + ndx * 2, 2); lcd.print(str); }
         offset += 8;
         while (!switch_push(DN_SWITCH)) yield();
         center_message(1, ""); center_message(2, ""); } }
   show_newest_call(); }


//*******  startup code

void setup(void) {
   lcd.begin(20, 4);
   center_message(0, "CallerID V" VERSION);
   delay(1000);
   pinMode(UP_SWITCH, INPUT_PULLUP);
   pinMode(DN_SWITCH, INPUT_PULLUP);
   pinMode(BACKLIGHT, OUTPUT);
   turn_on_backlight();
   EEPROM.begin(EEPROM_SIZE);
   eeprom_load();
   Serial.begin(9600);
   Serial.println("\r\nCallerID started");
   if (eeprom_hdr.ssid[0] == 0 /*|| Serial.available() > 0*/)
      do_configuration();
   join_network(); }

//****** the main loop: process call packets and button pushes

void loop(void) {

   process_scroll_buttons();

   if (WiFi.status() != WL_CONNECTED)
      join_network();

   if (pktlen = read_packet()) {
      turn_on_backlight();
      pktptr = packet;

      if (pktlen < 52
            || !match("^^<U>")
            || !copy(unitnum, 6)
            || !match("<S>")
            || !copy(serialnum, 6)
            || !match("$")
            || !copy(phoneline, 2)
            || !match(" ")) {
         //**** a malformed packet preamble
         badpacket(1); }

      else if (pktlen == 52 && match("V")) {
         //**** WhozzCalling unit just booted up
         lcd.clear(); lcd.print("WhozzCalling started");
         lcd.setCursor(0, 1); lcd.print("SN ");
         for (int i = 0; i < 6; ++i) lcd.print(serialnum[i], HEX);
         delay(2000);
         show_newest_call(); }

      else if (pktlen >= 70 && pktlen <= 83
               && match("I S ")
               && copy(duration, 4)
               && (match(" G ") || match(" B "))
               && match("A")
               && copy(atype, 1)
               && match(" ")
               && copy(datetime, 15)
               && copy(phonenum, 15)
               && copy(callername, 15)) {
         //***** start call
         add_call();
         eeprom_add_call();
         show_newest_call(); }

      else if (pktlen >= 70 && pktlen <= 83
               && match("I E ")
               && copy(duration, 4)) {
         //**** end call
         update_duration(); }

      else {
         //**** a packet we don't handle
         badpacket(2); } }

   if (eeprom_hdr.options & OPT_BACKLIGHT_OFF
         && backlight_on
         && millis() - backlight_turnon_time > BACKLIGHT_TIMEOUT_SECS * 1000)
      turn_off_backlight(); }
//*

