// OrionWspr.ino - Orion WSPR Beacon for pico-Balloon payloads using Arduino
//
// This code implements a very low power Arduino HF WSPR Beacon using a Silicon Labs Si5351a clock chip
// as the beacon transmitter and a GPS receiver module for time and location fix.
//
// This work is dedicated to the memory of my very dear friend Ken Louks, WA8REI(SK)
// - Michael Babineau, VE3WMB - March 4, 2019.
//
// Why is it called Orion?
// Serendipity! I was trying to think of a reasonable short name for this code one evening
// while sitting on the couch watching Netflix. I happened to look out at the night sky thorough
// the window and there was the constellation Orion staring back at me, so Orion it is.
//
// I wish that I could say that I came up with this code all on my own, but for the most part
// it is based on excellent work done by people far more clever than I am.
//
// It was derived from the Simple WSPR beacon for Arduino Uno, with the Etherkit Si5351A Breakout
// Board, by Jason Milldrum NT7S whose original code was itself based on Feld Hell beacon for Arduino by Mark
// Vandewettering K6HX and adapted for the Si5351A by Robert Liesenfeld AK6L <ak6l@ak6l.org>.
// Timer setup code by Thomas Knutsen LA3PNA. Time code adapted from the TimeSerial.ino example from the Time library.
//
// The original Si5351 Library code (from Etherkit .. aka NT7S) was replaced by self-contained SI5315 routines
// written Jerry Gaffke, KE7ER. The KE7ER code was modified by VE3WMB to use 64 bit precision in the calculations to
// enable the sub-Hz resolution needed for WSPR and to allow Software I2C usage via the inclusion of <SoftWire.h>.
//
// Code modified and added by Michael Babineau, VE3WMB for Project Aries pico-Balloon WSPR Beacon (2018/2019)
// This additional code is Copyright (C) 2018-2019 Michael Babineau <mbabineau.ve3wmb@gmail.com>


// Hardware Requirements
// ---------------------
// This firmware must be run on an Arduino or an AVR microcontroller with the Arduino Bootloader installed.
// Pay special attention, the Timer1 Interrupt code is highly dependant on processor clock speed.
//
// I have tried to keep the AVR/Arduino port usage in the implementation configurable via #defines in OrionXConfig.h so this
// code can more easily be setup to run on different beacon boards such as those designed by DL6OW and N2NXZ and in future
// the QRP Labs U3B. This was also the motivation for moving to a software I2C solution using SoftWire.h. The SoftWire interface
// mimics the Wire library so switching back to using hardware-enable I2C is simple.
//
// Required Libraries
// ------------------
// Etherkit JTEncode (Library Manager)  https://github.com/etherkit/JTEncode
// Time (Library Manager)   https://github.com/PaulStoffregen/Time - This provides a Unix-like System Time capability
// SoftWire (https://github.com/stevemarple/SoftWire) - (assumes that you are using Software I2C otherwise substitute Wire.h)
// TinyGPS (Library Manager) http://arduiniana.org/libraries/TinyGPS/
//
// License
// -------
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject
// to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include <JTEncode.h>
#include <int.h>
#include <TimeLib.h>
#include <TinyGPS.h>
//#include <Wire.h>     / Only uncomment this if you are using Hardware I2C to talk to the Si5351a
#include "OrionXConfig.h"
#include "OrionSi5351.h"
#include "OrionStateMachine.h"
#include "OrionSerialMonitor.h"

// NOTE THAT ALL #DEFINES THAT ARE INTENDED TO BE USER CONFIGURABLE ARE LOCATED IN OrionXConfig.h.
// DON'T TOUCH ANYTHING DEFINED IN THIS FILE WITHOUT SOME VERY CAREFUL CONSIDERATION.

// WSPR specific defines. DO NOT CHANGE THESE VALUES, EVER!
#define TONE_SPACING            146                 // ~1.46 Hz
#define SYMBOL_COUNT            WSPR_SYMBOL_COUNT

// Globals
JTEncode jtencode;
TinyGPS gps;

unsigned long g_beacon_freq_hz = BEACON_FREQ_HZ;      // The Beacon Frequency in Hz

// Use this on clk SI5351A_PARK_CLK_NUM to keep the SI5351a warm to avoid thermal drift during WSPR transmissions
uint64_t g_park_freq_hz = 108000000ULL;            // 108 MHz,in hertz

char g_beacon_callsign[7] = BEACON_CALLSIGN_6CHAR;

// Grid Square is hardcoded for now until the code is written to derive it from GPS Coordinates
char g_grid_loc[5] = BEACON_GRID_SQ_4CHAR;
char g_grid_sq_6char[7] = "";
uint8_t g_tx_pwr_dbm = BEACON_TX_PWR_DBM;
uint8_t g_tx_buffer[SYMBOL_COUNT];

OrionTelemetryData g_orion_telemetry; // This is the structure varible containing all of the telemetry data. Type defined in OrionXConfig.h

// Globals used by the Orion Scheduler
OrionAction g_current_action = NO_ACTION;

// We initialize this to 61 so the first time through the scheduler we can't possibly match the current second
// This forces the scheduler to run on its very first call. 
byte g_last_second = 61; 

// Global variables used in ISRs
volatile bool g_proceed = false;

// Timer interrupt vector.  This toggles the variable g_proceed which we use to gate
// each column of output to ensure accurate timing.  This ISR is called whenever
// Timer1 hits the WSPR_CTC value used below in setup().
ISR(TIMER1_COMPA_vect)
{
  g_proceed = true;
}

// ------- Functions ------------------

unsigned long get_tx_frequency(){
  /********************************************************************************   
  * Get the frequency for the next transmission cycle
  * This function also implements the QRM Avoidance feature
  * which applies a pseudo-random offset of 0 to 100 hz to the base TX frequency 
  ********************************************************************************/
  if (is_qrm_avoidance_on() == false)
    return BEACON_FREQ_HZ;
  else { 
    // QRM Avoidance - add a random number in range of 0 to 100 to the base TX frequency to help avoid QRM
    return (BEACON_FREQ_HZ + random(181)); // base freq + 0 to 180 hz random offset   
  }
   
}


void calculate_gridsquare_6char(float lat, float lon) {
    /*************************************************************************************** 
    * Calculate the 6 Character Maidenhead Gridsquare from the Lat and Long Coordinates
    * We follow the convention that West and South are negative Lat/Long.
    ***************************************************************************************/
  // For now this puts the calculated 6 character Maidenhead Grid square into the global character
  // array g_grid_sq_6char[]

  // Temporary variables for calculation
  int o1, o2, o3;
  int a1, a2, a3;
  float remainder;

  // longitude
  remainder = lon + 180.0;
  o1 = (int)(remainder / 20.0);
  remainder = remainder - (float)o1 * 20.0;
  o2 = (int)(remainder / 2.0);
  remainder = remainder - 2.0 * (float)o2;
  o3 = (int)(12.0 * remainder);

  // latitude
  remainder = lat + 90.0;
  a1 = (int)(remainder / 10.0);
  remainder = remainder - (float)a1 * 10.0;
  a2 = (int)(remainder);
  remainder = remainder - (float)a2;
  a3 = (int)(24.0 * remainder);

  // Generate the 6 character Grid Square
  g_grid_sq_6char[0] = (char)o1 + 'A';
  g_grid_sq_6char[1] = (char)a1 + 'A';
  g_grid_sq_6char[2] = (char)o2 + '0';
  g_grid_sq_6char[3] = (char)a2 + '0';
  g_grid_sq_6char[4] = (char)o3 + 'a';
  g_grid_sq_6char[5] = (char)a3 + 'a';
  g_grid_sq_6char[6] = (char)0;
} // calculate_gridsquare_6char


void get_position_and_telemetry() {
  /******************************************
  * Get the GPS position and telemetry data
  ******************************************/

  // For now this takes no arguments.
  // As it is expanded we will pass in a pointer to a structure that
  // we populate with all of the telemetry and position information.

  float temp_latitude;
  float temp_longitude;
  byte i;
  unsigned long age_of_fix_ms;

  gps.f_get_position(&temp_latitude, &temp_longitude, &age_of_fix_ms);
  calculate_gridsquare_6char(temp_latitude, temp_longitude); // Puts 6 character grid square into g_grid_sq_6char[]

  // Copy the first four characters of the Grid Square to g_grid_loc[] for use in the Primary WSPR Message
  for (i = 0; i < 4; i++ ) g_grid_loc[i] = g_grid_sq_6char[i];
  g_grid_loc[i] = (char) 0; // g_grid_loc[4]

  g_beacon_freq_hz = get_tx_frequency();

} //end get_position_and_telemetry


void encode_and_tx_wspr_msg1() {
  /**************************************************************************
  * Transmit the Primary WSPR Message
  * Loop through the transmit buffer, transmitting one character at a time.
  * ************************************************************************/
  uint8_t i;

  // Encode the primary message paramters into the TX Buffer
  jtencode.wspr_encode(g_beacon_callsign, g_grid_loc, g_tx_pwr_dbm, g_tx_buffer);

  // Reset the tone to 0 and turn on the TX output
  si5351bx_setfreq(SI5351A_WSPRTX_CLK_NUM, (g_beacon_freq_hz * 100ULL));

  // Turn off the PARK clock
  si5351bx_enable_clk(SI5351A_PARK_CLK_NUM, SI5351_CLK_OFF);

  if (TX_LED_PIN != 0) { // If we are using the TX LED turn it on
    digitalWrite(TX_LED_PIN, HIGH);
  }
  // Now send the rest of the message
  for (i = 0; i < SYMBOL_COUNT; i++)
  {
    si5351bx_setfreq(SI5351A_WSPRTX_CLK_NUM, (g_beacon_freq_hz * 100ULL) + (g_tx_buffer[i] * TONE_SPACING));
    g_proceed = false;

    // We spin our wheels in TX here, waiting until the Timer1 Interrupt sets the g_proceed flag
    // Then we can go back to the top of the for loop to start sending the next symbol
    while (!g_proceed);
  }

  // Turn off the WSPR TX clock output, we are done sending the message
  si5351bx_enable_clk(SI5351A_WSPRTX_CLK_NUM, SI5351_CLK_OFF);

  // Re-enable the Park Clock
  si5351bx_enable_clk(SI5351A_PARK_CLK_NUM, SI5351_CLK_ON);

  if (TX_LED_PIN != 0) { // If we are using the TX LED turn it off
    digitalWrite(TX_LED_PIN, LOW);
  }

  delay(1000); // Delay one second
} // end of encode_and_tx_wspr_msg1()


void set_system_time() {
  /*********************************************
  * Get the time from the GPS when it is ready
  * ********************************************/
  while (Serial.available()) {
    if (gps.encode(Serial.read())) {

      // process gps messages when TinyGPS reports new data...
      unsigned long age;
      int Year;
      byte Month, Day, Hour, Minute, Second;

      gps.crack_datetime(&Year, &Month, &Day, &Hour, &Minute, &Second, NULL, &age);

      if (age < 500) {
        // set the Time to the latest GPS reading
        // Setting the clock this frequently seems like overkill but it works.
        // This strategy will be revisited as the code is expanded and updated with new functionality
        setTime(Hour, Minute, Second, Day, Month, Year);
      }
    }
  } // end while

  if (SYNC_LED_PIN != 0) {               // If we are using the SYNC_LED
    if (timeStatus() == timeSet) {
      digitalWrite(SYNC_LED_PIN, HIGH); // Turn LED on if the time is synced
    }
    else {
      digitalWrite(SYNC_LED_PIN, LOW);  // LED off if time needs refresh
    }
  }

}  // end set_system_time()



void process_orion_sm_action (OrionAction action) {
  /****************************************************************************************************
  * This is where all of the work gets triggered by processing Actions returned by the state machine.
  * We don't need to worry about state we just do what we are told.
  ****************************************************************************************************/
 
  switch (action) {


    case NO_ACTION :
      break;

    /*
          case DO_CALIBRATION_ACTION : {
          break;
          }

    */
    case GET_TELEMETRY_ACTION :
      get_position_and_telemetry();

      // Tell the Orion state machine that we have the telemetry
      g_current_action = orion_state_machine(TELEMETRY_DONE_EV);
      break;


    case TX_WSPR_MSG1_ACTION : {
        // Encode and transmit the Primary WSPR Message
        encode_and_tx_wspr_msg1();

        orion_log_wspr_tx(PRIMARY_WSPR_MSG, g_grid_sq_6char, g_beacon_freq_hz); // If TX Logging is enabled then ouput a log

        // Tell the Orion state machine that we are done tranmitting the Primary WSPR message and update the current_action
        g_current_action = orion_state_machine(PRIMARY_WSPR_TX_DONE_EV);
        break;
      }

    /*
      case TX_WSPR_MSG2_ACTION : {
      break;
      }
    */

    default : {
        break;
      }

  } // select

} //  process_orion_sm_action


OrionAction orion_scheduler() {
  /*********************************************************************
  * This is the scheduler code that determines the Orion Beacon schedule
  **********************************************************************/
  byte Second; // The current second
  byte Minute; // The current minute
  byte i;
  OrionAction action = NO_ACTION;

  // We need to figure out how to determine that we are close to Beaconing and grab the latest Telemetry data
  // this includes calculating the 6 character GRID Square. This would ideally happen at 59, 9, 19, 29 and 49 minutes and a couple of
  // seconds before.
  if (timeStatus() == timeSet) { // We have valid time from the GPS otherwise do nothing
    
    // The scheduler will get called many times per second but we only want it to run once per second,
    // otherwise we might generate multiples of the same time event on these extra passes through the scheduler.
    Second = second();
    
    // If we are on the same second as the last time we went through here then bail-out with NO_ACTION returned.
    if (Second == g_last_second) return action; 
  
    // If the current second is different from the last time then it has been updated then we run.
    
    g_last_second = Second; // Remember what second we are on for the next time the scheduler is called
      
    Minute = minute();

    // We beacon every 10th minute of the hour so we use minute() modulo 10 (i.e. on 00, 10, 20, 30, 40, 50)
    if (((Minute % 10) == 0) && (Second == 0)) {
      // Primary WSPR transmission should start on the 1st second of the minute, but there's a slight delay
      // in this code because it is limited to 1 second resolution.
      action = orion_state_machine(PRIMARY_WSPR_TX_TIME_EV);
    }
    else if (Second == 30) { // Note the choice of a second value of 30 is arbitrary

      // check once per minute at 30 seconds after the minute to see if it is time to collect new telemetry data
      for (i = 9; i < 60; i = i + 10) {
        // i.e. i is 9, 19, 29, 39, 49, 59 which is one minute prior to transmission time

        if (Minute == i) {
          // If are about thirty seconds prior to a scheduled Beacon Transmission collect new telemetry info
          action = orion_state_machine(TELEMETRY_TIME_EV);
          break;
        }
      } // end for

    } // end else if

  } // end if timestatus == timeset

  return action;
} // end orion_scheduler()


void setup(){

  // Use the TX_LED_PIN as a transmit indicator, but only if it is non-zero
  if (TX_LED_PIN != 0) {
    pinMode(TX_LED_PIN, OUTPUT);
    digitalWrite(TX_LED_PIN, LOW);
  }

  // Use the SYNC_LED_PIN to indicate the beacon has valid time from the GPS, but only if it is non-zero
  if (SYNC_LED_PIN != 0) {
    pinMode(SYNC_LED_PIN, OUTPUT);
    digitalWrite(SYNC_LED_PIN, LOW);
  }

  // Start Hardware serial communications with the GPS
  Serial.begin(GPS_SERIAL_BAUD);

  // Initialize the Si5351
  si5351bx_init();

  // Setup WSPR TX output
  si5351bx_setfreq(SI5351A_WSPRTX_CLK_NUM, (g_beacon_freq_hz * 100ULL));
  si5351bx_enable_clk(SI5351A_WSPRTX_CLK_NUM, SI5351_CLK_OFF); // Disable the clock initially

  // Set PARK CLK Output - Note that we leave SI5351A_PARK_CLK_NUM running at 108 Mhz to keep the SI5351 temperature more constant
  // This minimizes thermal induced drift during WSPR transmissions. The idea is borrowed from G0UPL's PARK feature on the QRP Labs U3S
  si5351bx_setfreq(SI5351A_PARK_CLK_NUM, (g_park_freq_hz * 100ULL)); // Turn on Park Clock

  // Set up Timer1 for interrupts every symbol period (i.e 1.46 Hz)
  // The formula to calculate this is CPU_CLOCK_SPEED_HZ / (PRESCALE_VALUE) x (WSPR_CTC + 1)
  // In this case we are using a prescale of 1024.
  noInterrupts();          // Turn off interrupts.
  TCCR1A = 0;              // Set entire TCCR1A register to 0; disconnects
  //   interrupt output pins, sets normal waveform
  //   mode.  We're just using Timer1 as a counter.
  TCNT1  = 0;              // Initialize counter value to 0.
  TCCR1B = (1 << CS12) |   // Set CS12 and CS10 bit to set prescale
           (1 << CS10) |   //   to /1024
           (1 << WGM12);   //   turn on CTC
  //   which gives, 64 us ticks
  TIMSK1 = (1 << OCIE1A);  // Enable timer compare interrupt.

  // Note the the OCR1A value is processor clock speed dependant.
  // WSPR_CTC must be defined as 5336 for an 8Mhz processor clock or 10672 for a 16 Mhz Clock
  OCR1A = WSPR_CTC;       // Set up interrupt trigger count.

  interrupts();            // Re-enable interrupts.

  // Setup the software serial port for the serial monitor interface
  serial_monitor_begin();

  // Set the intial state for the Orion Beacon State Machine
  orion_sm_begin();

  // Tell the state machine that we are done SETUP
  g_current_action = orion_state_machine(SETUP_DONE_EV);

  // Use pin A0 (not connected) to generate a random seed for QRM avoidance feature
  randomSeed(analogRead(0));

} // end setup()


void loop(){
  /****************************************************************************************************************************
  * This loop constantly updates the system time from the GPS and calls the scheduler for the operation of the Orion Beacon
  ****************************************************************************************************************************/

  // Update the system clock time using the GPS
  set_system_time();

  // Call the scheduler to determine if it is time for any action
  g_current_action = orion_scheduler();

  // This triggers actual work when the state machine returns an OrionAction
  if (g_current_action != NO_ACTION) {
    process_orion_sm_action(g_current_action);
  }
  
  // Process serial monitor input
  serial_monitor_interface();

} // end loop ()
