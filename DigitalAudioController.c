/*
* TDA7313 Controller & UI
* James Barnett
*
* TODO:
* Save settings to EEPROM on power down or after period of inactivity
* Overtemp warning/shutdown
*/
#include <Encoder.h>
#include <OLEDFourBit.h>
#include <Wire.h>

// LCD pins
#define RS 12 //AVR 18
#define RW 10 //AVR 16
#define E 11  //AVR 17
#define DB4 4 //AVR 6
#define DB5 5 //AVR 11
#define DB6 6 //AVR 12
#define DB7 7 //AVR 13
// LM35
#define TEMP 0 //AVR 23
#define TEMP_ROLLING_AVG 10
// Encoder button
#define BTN 9 //AVR 15

// interface
#define SELECTION_MARKER '>'
#define VOLUME 0
#define TREB 1
#define BASS 2
#define VOLUME_MAX 0
#define VOLUME_MIN -75 // flipped since we are attenuating
#define TREB_MAX 14
#define TREB_MIN -14
#define BASS_MAX 14
#define BASS_MIN -14

OLEDFourBit oled(RS, RW, E, DB4, DB5, DB6, DB7);

Encoder enc(2,3);
long oldPosition = -999;
long newPosition = 0;
boolean encUpdated = true;

byte graphBlock[8] = {
  B00011111,
  B00111111,
  B01011111,
  B01111111,
  B10011111,
  B10111111,
  B11011111,
  B11100000
};

// Volume and tone
int cursorSelection;

double volPosition = -40; // value in db
byte volByte = 32; // 00100000 default vol to -40db
boolean volUpdateQueued = false;

int trebPosition = 0;
byte trebByte = 127; // 01111111 default treb to 0db
boolean trebUpdateQueued = false;

int bassPosition = 0;
byte bassByte = 111; // 0110111 default bass to 0db;
boolean bassUpdateQueued = false;

boolean volChanged = false;
boolean trebChanged = false;
boolean bassChanged = false;

// Button
boolean buttonState = false;
boolean prevButtonState = false;
boolean buttonDown = false;
boolean buttonUp = false;
boolean selectionUpdated = false;

// Temp sensor
boolean tempUpdated = true;
unsigned int tempSampleDelay = 0;
float avgTemp = 0; // init to 18c
float tempLog[TEMP_ROLLING_AVG];
int currentTempIndex = 0;
float totalTemp = 0;


float readTemp()
{
  // LM35 is 10mv/1deg c
  // analogRead maps 0-1.1v to 0-1023 (using internal reference)
  return analogRead(TEMP)/9.31;
}

void buttonCycled()
{
  //button cycled
  if(cursorSelection == 2)
  {
    cursorSelection = 0;
  }
  else
  {
    cursorSelection++;
  }
  buttonDown = false;
  buttonUp = false;
  selectionUpdated = true;
}

void renderEncoderChange()
{
  oled.setCursor(13,3);

  if(cursorSelection == VOLUME)
  {
    if(volPosition < -9) // 6 chars
    {
      oled.setCursor(12,3);
    }
    else if(volPosition == 0) // 4 chars
    {
      oled.setCursor(12,3);
      oled.print("  "); // clear sign any old digit
      oled.setCursor(14,3);
    }
    else // 5 chars
    {
      oled.setCursor(12,3);
      oled.print(" "); // clear sign any old digit
      oled.setCursor(13,3);
    }
    oled.print(volPosition);
  }

  else if(cursorSelection == TREB)
  {
    if(trebPosition > 9) // two digits
    {
      oled.setCursor(15,3);
      oled.print(" "); // clear old sign
      oled.setCursor(16,3);
    }
    else if(trebPosition >= 0) // one digit
    {
      oled.setCursor(15,3);
      oled.print("  ");
      oled.setCursor(17,3);
    }
    else if(trebPosition < -9)// sign + two digits
    {
      oled.setCursor(15,3);
    }
    else // sign + one digit
    {
      oled.setCursor(15,3);
      oled.print(" ");
      oled.setCursor(16,3);
    }
    oled.print(trebPosition, DEC);
  }

  else if(cursorSelection == BASS)
  {
    if(bassPosition > 9) // two digits
    {
      oled.setCursor(15,3);
      oled.print(" "); // clear old sign
      oled.setCursor(16,3);
    }
    else if(bassPosition >= 0) // one digit
    {
      oled.setCursor(15,3);
      oled.print("  ");
      oled.setCursor(17,3);
    }
    else if(bassPosition < -9)// sign + two digits
    {
      oled.setCursor(15,3);
    }
    else // sign + one digit
    {
      oled.setCursor(15,3);
      oled.print(" ");
      oled.setCursor(16,3);
    }
    oled.print(bassPosition, DEC);
  }

  if(volChanged)
  {
    oled.setCursor(5,0);
    renderVolumeGraph(volPosition);
    volChanged = false;
  }

  else if(trebChanged)
  {
    oled.setCursor(5,1);
    renderToneGraph(trebPosition);
    trebChanged = false;
  }

  else if(bassChanged)
  {
    oled.setCursor(5,2);
    renderToneGraph(bassPosition);
    bassChanged = false;
  }
}

void renderSelectionChange()
{
  // clear existing cursors
  oled.setCursor(3,0);
  oled.print(' ');
  oled.setCursor(3,1);
  oled.print(' ');
  oled.setCursor(3,2);
  oled.print(' ');
  // print updated location
  oled.setCursor(3,cursorSelection);
  oled.print(SELECTION_MARKER);
  // clear old encoder value
  oled.setCursor(10,3);
  oled.print("        ");
  // encoder value will also have changed to its relevant vol/treb/bass value
  renderEncoderChange();
}

void renderTempChange()
{
  oled.setCursor(0,3);
  oled.print("TEMP ");
  oled.print(avgTemp, 1);
  oled.print("c");
}

void sendByte(byte data)
{
  Wire.beginTransmission(0x44); // TDA7313 7bit addr 01000100
  Wire.write(data);
  Wire.endTransmission();
}

void tdaInit()
{
  Wire.beginTransmission(0x44); // 01000100
  Wire.write(0x45); // input 2, 11.25db gain, loud mode off
  Wire.write(0x6F); // bass flat
  Wire.write(0x7F); // treb flat
  Wire.write(0x9F); // mute lf
  Wire.write(0xBF); // mute rf
  Wire.write(0xC0); // 0db attn RL
  Wire.write(0xE0); // 0db attn RR
  Wire.write(0x16); // vol atten to -40db
  Wire.endTransmission();
}

void encoderInc()
{
  if(cursorSelection == VOLUME)
  {
    if(volPosition < VOLUME_MAX)
    {
      volPosition += 1.25; // 1.25db resolution
      volByte -= 1; // reduce vol atten by 1.25db
      volChanged = true;
      volUpdateQueued = true;
    }
  }
  // increment
  else if(cursorSelection == TREB)
  {
    if(trebPosition < TREB_MAX)
    {
      if(trebPosition > 0) // we are already boosting, so increace boost
      {
        trebByte -= 1; // confusingly we decrement to increace boost
      }
      else if(trebPosition < 0) // reduce attenuation
      {
        trebByte += 1; // increment to reduce
      }
      else // flat and incrementing, so want atten bit c3(5) = 1
      {
        trebByte = 126; // 01111110
      }
      trebPosition += 2; // 2db resolution
      trebChanged = true;
      trebUpdateQueued = true;
    }
  }
  // increment
  else if(cursorSelection == BASS)
  {
    if(bassPosition < BASS_MAX)
    {
      if(bassPosition > 0) // we are already boosting, so increce boost
      {
        bassByte -= 1; // confusingly we decrement to increace boost
      }
      else if(bassPosition < 0) // reduce attenuation
      {
        bassByte += 1; // increment to reduce
      }
      else
      {
        bassByte = 110; // 01111110
      }
      bassPosition += 2; // 2db resolution
      bassChanged = true;
      bassUpdateQueued = true;
    }
  }
}

void encoderDec()
{
  if(cursorSelection == VOLUME)
  {
    if(volPosition > VOLUME_MIN)
    {
      volPosition -= 1.25;
      volByte += 1; //increace atten by 1.25db;
      volChanged = true;
      volUpdateQueued = true;
    }
  }
  // decrement
  else if(cursorSelection == TREB)
  {
    if(trebPosition > TREB_MIN)
    {
      if(trebPosition > 0) // reduce the boost
      {
        trebByte += 1; // confusingly we still increment. eg 14db to 12db is 01111000 -> 01111001
      }
      else if(trebPosition < 0) // we are already attenuating, just decrement
      {
        trebByte -= 1;
      }
      else // flat and decrementing, so want atten bit c3(5) = 0
      {
        trebByte = 118; // 01110110 = -2db;
      }
      trebPosition -= 2; // 2db resolution
      trebChanged = true;
      trebUpdateQueued = true;
    }
  }
  else if(cursorSelection == BASS)
  {
    if(bassPosition > BASS_MIN)
    {
      if(bassPosition > 0) // reduce the boost
      {
        bassByte += 1; // confusingly we still increment. eg 14db to 12db is 01101000 -> 01101001
      }
      else if(bassPosition < 0) // we are already attenuating, just decrement
      {
        bassByte -= 1;
      }
      else // flat and decrementing, so want atten bit c3(5) = 0
      {
        bassByte = 102; // 01100110 = -2db;
      }
      bassPosition -= 2; // 2db resolution
      bassChanged = true;
      bassUpdateQueued = true;
    }
  }
}

void setup()
{
  // I2C for TDA7313
  Wire.begin();
  tdaInit();

  // use internal 1V1 reference for maximum LM35 resolution
  analogReference(INTERNAL);

  oled.begin(20,4);
  //oled.print("Loading...");
  oled.createChar(0, graphBlock);
  oled.setCursor(0,0);
  //delay(2000);

  oled.print("VOL>|");
  oled.write(0);
  oled.write(0);
  oled.print("            |"); // print default volume graph for -40db
  oled.setCursor(0,1);
  oled.print("TRB |     FLAT     |");
  oled.setCursor(0,2);
  oled.print("BAS |     FLAT     |");
  oled.setCursor(18,3);
  oled.print("dB");

  volChanged = true;
  renderEncoderChange(); // show initial volume

  // init temp sensor readings
  for(int i = 0; i < TEMP_ROLLING_AVG; i++)
  {
    tempLog[i] = 0;
  }
}

void loop()
{
  // encoder
  newPosition = enc.read()/4; // 4 pulses per notch
  if(newPosition > oldPosition)
  {
    encoderInc();
    oldPosition = newPosition;
    encUpdated = true;
  }
  else if (newPosition < oldPosition)
  {
    encoderDec();
    oldPosition = newPosition;
    encUpdated = true;
  }
  else
  {
    encUpdated = false;
  }

  // read temp and avg
  if(tempSampleDelay > 10000)
  {
    totalTemp -= tempLog[currentTempIndex]; // remove last reading at current index
    tempLog[currentTempIndex] = readTemp();
    totalTemp += tempLog[currentTempIndex];
    currentTempIndex++;
    if(currentTempIndex == TEMP_ROLLING_AVG)
    {
      currentTempIndex = 0;
    }
    avgTemp = totalTemp / TEMP_ROLLING_AVG;
    tempUpdated = true;
    tempSampleDelay = 0;
  }
  else
  {
    tempUpdated = false;
    tempSampleDelay++;
  }

  // button
  buttonState = digitalRead(BTN);
  if(buttonState != prevButtonState)
  {
    if(buttonState == HIGH)
    {
      buttonDown = true;
    }
    else
    {
      buttonUp = true;
    }
  }
  // if we have completed a full button cycle
  if(buttonDown == true && buttonUp == true)
  {
    buttonCycled();
  }
  else
  {
    selectionUpdated = false;
  }
  prevButtonState = buttonState;

  // update TDA7313
  // only one update should ever be sent per loop iteration
  if(volUpdateQueued)
  {
    sendByte(volByte);
    volUpdateQueued = false;
  }
  else if(trebUpdateQueued)
  {
    sendByte(trebByte);
    trebUpdateQueued = false;
  }
  else if(bassUpdateQueued)
  {
    sendByte(bassByte);
    bassUpdateQueued = false;
  }

  // update display
  if(tempUpdated)
  {
    renderTempChange();
  }
  if(encUpdated)
  {
    renderEncoderChange();
  }
  if(selectionUpdated)
  {
    renderSelectionChange();
  }

}

// using a mapping function (like the volume graph) is much nicer, but this is probably faster
void renderToneGraph(int position)
{
  switch (position)
  {
    case -14:
      oled.write(0);
      oled.print("             ");
      break;
    case -12:
      oled.write(0);oled.write(0);
      oled.print("            ");
      break;
    case -10:
      oled.write(0);oled.write(0);oled.write(0);
      oled.print("           ");
      break;
    case -8:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("          ");
      break;
    case -6:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("         ");
      break;
    case -4:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("        ");
      break;
    case -2:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("       ");
      break;
    case 0:
      oled.print("     FLAT     ");
      break;
    case 2:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("      ");
      break;
    case 4:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("     ");
      break;
    case 6:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("    ");
      break;
    case 8:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("   ");
      break;
    case 10:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print("  ");
      break;
    case 12:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      oled.print(" ");
      break;
    case 14:
      oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);oled.write(0);
      break;
  }
}

int mapVolumeValues(int val)
{
  return ((abs(val)-50)/(50.0))*(-14.0);
}

void renderVolumeGraph(int position)
{
  if(position > -50)
  {
    int segments = mapVolumeValues(position);
    oled.setCursor(5,0);
    for(int i=0; i< segments; i++)
    {
      oled.write(0);
    }
    int blankSpace = 14-segments;
    for(int j=0; j< blankSpace; j++)
    {
      oled.print(" ");
    }
  }
}