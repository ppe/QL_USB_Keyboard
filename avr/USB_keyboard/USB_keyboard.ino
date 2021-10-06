#include <hidboot.h>
#include <usbhub.h>
#include <TimerOne.h>
#include <avr/interrupt.h>

// Satisfy the IDE, which needs to see the include statment in the ino too.
#ifdef dobogusinclude
#include <spi4teensy3.h>
#include <SPI.h>
#endif

// Flag is a signal to (out) and from (in) QL that is used to coordinate mouse status
// communication with the QDOS driver
const byte flagIn = PD0;
const byte flagOut = PD1;
// Strobe is used to clock gate changes to the 8808 switch matrix
const byte strobe = PD2;
// Data is the state of the switch matrix switch to set (1==closed, 0==open)
const byte data = PD3;
// Resets the 8808 switch matrix (all switches off)
const byte reset = PD4;
// Pin that is connected to the QL resetLow line
const byte resetQL = PD5;
// Pin to control modifier keys that are implemented using the separate 74LVC4066 quad switch IC
const byte shift = PD6;
const byte ctrl = PD7;
const byte alt = PB0;

// const byte ledPin = PD1;

/*
 * USB keycode to switch matrix address translation table
 * Matrix addresses: y-address 3 bits, x-address 3 bits: 00yyyxxx
 * e.g. 00011010 => y=011=3, x=010=2 = 0x1A
 * 0xFF == not mapped
 */
const byte keyMapping[83] = {
   // 0, 0, 0, 0,         a=4:4,b=4:2,c=3:2,d=6:4,e=4:6,f=4:3,g=6:3,h=2:4,i=2:5,j=7:4,k=2:3,l=0:4,m=6:2,n=6:7,o=7:5,p=5:4
   0xFF, 0xFF, 0xFF, 0xFF, 0x24, 0x22, 0x1A, 0x34, 0x26, 0x23, 0x33, 0x14, 0x15, 0x3C, 0x13, 0x04, 0x32, 0x37, 0x3D, 0x2C,
//q=3:6,r=4:5,s=3:3,t=6:6,u=7:6,v=4:7,w=1:5,x=3:7,y=6:5,z=1:2,1=1:4,2=1:6,3=3:4,4=6:0,5=2:0,6=2:6,7=7:0,8=0:6,9=0:5,0=5:6
   0x1E, 0x25, 0x1B, 0x36, 0x3E, 0x27, 0x0D, 0x1F, 0x35, 0x0A, 0x1C, 0x40, 0x0C, 0x30, 0x10, 0x16, 0x38, 0x06, 0x05, 0x2E,
// Ent=0:1,Esc=3:1,Bkspace=??,Tab=3:5,SPC=6:1,-=5:5,==5:3,[=0:3,]=0:2,\=5:1,~=£??,;=7:3,'=7:2,´=???,','=7:7,'.'=2:2,/=???,
      0x01,   0x19,      0x80,   0x1D,   0x31, 0x2D, 0x2B, 0x03, 0x02, 0x29, 0x2A, 0x3B, 0x3A, 0xFF,   0x3F,   0x12, 0xFF,
// Caps=1:3,F1=1:0,F2=3:0,F3=4:0,F4=0:0,F5=5:0,F6=???,F7=???,F8=???,F9=???,F10=???,F11=???,F12=???
       0x0B,  0x08,  0x18,  0x20,  0x00,  0x28,  0x82,  0xFF,  0xFF,  0xFF,   0xFF,   0xFF,   0xFF,
// PrtScr=?,ScrLk=?,Pause=?,Ins=?,Home=?,PgUp=?,Del=?,End=?,PgDn=?,Rt=4:1,Lt=1:1,Dn=7:1,Up=2:1
        0xFF,  0xFF,   0xFF, 0xFF,  0xFF,  0xFF, 0x81, 0xFF,  0xFF,  0x21,  0x09,  0x39,  0x11
};

const byte specialKeys[3][2] = {
  // Backspace = Ctrl+Left
  {0x0F,0x09},
  // Delete = Ctrl+Right
  {0x0F,0x21},
  // F6 = Shift+F1
  {0x07,0x08}
};

// { Num-switches (1-3), Sw1, Sw2, Sw3, Shift-Num-switches (1-3), Shift-Sw1, Shift-Sw2, Shift-Sw3, AltGr-Num-Switches, AltGr-Sw1, AltGr-Sw2, AltGr-Sw3}
const byte pcToQlMapping[1][5] = {
  // Plain, Shift, Ctrl, Shift-Ctrl, AltGr
  // 2 == 2, Shift-2(") == Shift-', Ctrl-2 == ï (orig), Shift-Ctrl-2 == Ä (mapped elsewhere), , AltGr-2 (@) == Shift-2
  {0x0E,0x7A,0x8E,0xCE,0x4E}
  // {0x01, 0x0E, 0xFF, 0xFF, 0x02, 0x07, 0x3A, 0xFF, 0x02, 0x07,0x0E, 0xFF }
};

void setSwitch(byte address, byte state) {
    PORTC = (address | (PORTC & 0xC0));
    digitalWrite( data, state );
    digitalWrite( strobe, HIGH );
    digitalWrite( strobe, LOW );
}

byte delayedAddr = 0xFF;
byte callCount = 0;
void delayedSwitchOn() {
  if (callCount == 0) {
    callCount += 1;
    return;
  }
  if(delayedAddr != 0xFF) {
      setSwitch(delayedAddr, HIGH);
  }
  delayedAddr = 0xFF;
  Timer1.stop();
  callCount = 0;
}

void setOutputState(uint8_t keycode, uint8_t mod, byte state) {
  byte address = keyMapping[keycode];
  // If top two bits are '01' check for AltGr (right Alt) and if pressed treat as a special case
  byte specialKeyFlag = address & 0xC0;
  if( specialKeyFlag != 0 ) {
    byte switchAddress1;
    byte switchAddress2;
    if(specialKeyFlag == 0xC0) {
      return;
    }
    byte row = address & 0x3F;
    byte keySetting;
    MODIFIERKEYS modifiers;
    *((uint8_t*)&modifiers) = mod;
    
    if(specialKeyFlag == 0x40 ) {
      byte col = ((modifiers.bmLeftCtrl || modifiers.bmRightCtrl) << 1) | (modifiers.bmLeftShift || modifiers.bmRightShift);
      if( modifiers.bmRightAlt == 1) {
        keySetting = pcToQlMapping[row][4];
      } else {
        keySetting = pcToQlMapping[row][col];
      }
      if(keySetting & _BV(7)) { // Switch Ctrl on if mapped
        //setSwitch(0x0F, state);
        digitalWrite(ctrl,state);
      }
      if(keySetting & _BV(6)) { // Switch Shift on if mapped
        //setSwitch(0x07, state);
        digitalWrite(shift,state);
      }
      if(modifiers.bmLeftAlt) {
        //setSwitch(0x17, state);
        digitalWrite(alt,state);
      }
      if(state == HIGH) {
        delayedAddr = keySetting & 0x3F;
        Timer1.start();
        return;
      } else { // Key lifted, clear key switch, leave modifiers
        byte addr = keySetting & 0x3F;
        setSwitch(addr, LOW);
      }
    }
    if(specialKeyFlag == 0x80) { // If top two bits are '10' treat this key as a special key
      switchAddress1 = specialKeys[row][0];
      switchAddress2 = specialKeys[row][1];
    }
    if(state == HIGH) {
      setSwitch(switchAddress1, state);
      delayedAddr = switchAddress2;
      Timer1.start();
    } else {
      Timer1.stop();
      delayedAddr = 0xFF;
      setSwitch(switchAddress1, state);
      setSwitch(switchAddress2, state);
    }
  } else {
    if(keycode > 3 && keycode < 83  && address < 0x40 ) {
      setSwitch(address, state);
    }
  }
}

class KbdRptParser : public KeyboardReportParser
{
  protected:
    void OnControlKeysChanged(uint8_t before, uint8_t after);

    void OnKeyDown  (uint8_t mod, uint8_t key);
    void OnKeyUp  (uint8_t mod, uint8_t key);
    void OnKeyPressed(uint8_t key);
};

void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key)
{
//  digitalWrite(ledPin, HIGH);
  uint8_t c = OemToAscii(mod, key);

  if (c) {
    OnKeyPressed(c);
  }
  setOutputState(key, mod, HIGH);
}

void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t*)&beforeMod) = before;

  MODIFIERKEYS afterMod;
  *((uint8_t*)&afterMod) = after;
/*
  setSwitch(0x07, (afterMod.bmRightShift == 1 || afterMod.bmLeftShift == 1) ? HIGH : LOW);
  setSwitch(0x0F, (afterMod.bmRightCtrl == 1 || afterMod.bmLeftCtrl == 1) ? HIGH : LOW);
  setSwitch(0x17, (afterMod.bmLeftAlt == 1) ? HIGH : LOW);
*/
  digitalWrite(shift, (afterMod.bmRightShift == 1 || afterMod.bmLeftShift == 1) ? HIGH : LOW);
  digitalWrite(ctrl, (afterMod.bmRightCtrl == 1 || afterMod.bmLeftCtrl == 1) ? HIGH : LOW);
  digitalWrite(alt, (afterMod.bmLeftAlt == 1) ? HIGH : LOW);

}

void KbdRptParser::OnKeyUp(uint8_t mod, uint8_t key)
{
//  digitalWrite(ledPin, LOW);
  setOutputState(key, mod, LOW);
}

void KbdRptParser::OnKeyPressed(uint8_t key)
{
};

class MouseRptParser : public MouseReportParser
{
  int8_t mouseDeltaX = 0;
  int8_t mouseDeltaY = 0;
  public:
    int8_t getMouseDeltaX(void);
    int8_t getMouseDeltaY(void);
  protected:
    void OnMouseMove(MOUSEINFO *mi);
    void OnLeftButtonUp(MOUSEINFO *mi);
    void OnLeftButtonDown(MOUSEINFO *mi);
    void OnRightButtonUp(MOUSEINFO *mi);
    void OnRightButtonDown(MOUSEINFO *mi);
    void OnMiddleButtonUp(MOUSEINFO *mi);
    void OnMiddleButtonDown(MOUSEINFO *mi);
};
int8_t MouseRptParser::getMouseDeltaX(void) {
  return mouseDeltaX;
}
int8_t MouseRptParser::getMouseDeltaY(void) {
  return mouseDeltaY;
}

void MouseRptParser::OnMouseMove(MOUSEINFO *mi)
{
  uint16_t newDeltaX = (uint16_t)mouseDeltaX + (uint16_t)(mi->dX);
  if( newDeltaX < -127) {
    mouseDeltaX = -127;
  } else if (newDeltaX > 127) {
    mouseDeltaX = 127;
  } else {
    mouseDeltaX = newDeltaX;
  }
//  Serial.print("dx=");
//  Serial.print(mi->dX, DEC);
//  Serial.print(" dy=");
//  Serial.println(mi->dY, DEC);
};
void MouseRptParser::OnLeftButtonUp  (MOUSEINFO *mi)
{
//  Serial.println("L Butt Up");
};
void MouseRptParser::OnLeftButtonDown (MOUSEINFO *mi)
{
  cli(); // Disable interrupts
  // If flag from QL is set then QL is currently reading mouse status
  // In this case we won't do anything
  if(digitalRead(flagIn)) {
    sei(); // Re-enable interrupts
    return;
  }
  PCICR |= 0b00000100; // Turn on pin change interrupts on port D
  sei(); // Enable interrupts
  digitalWrite(flagOut,1); // Signal to QL that we want to report new mouse status
//  Serial.println("L Butt Dn");
};
void MouseRptParser::OnRightButtonUp  (MOUSEINFO *mi)
{
//  Serial.println("R Butt Up");
};
void MouseRptParser::OnRightButtonDown  (MOUSEINFO *mi)
{
//  Serial.println("R Butt Dn");
};
void MouseRptParser::OnMiddleButtonUp (MOUSEINFO *mi)
{
//  Serial.println("M Butt Up");
};
void MouseRptParser::OnMiddleButtonDown (MOUSEINFO *mi)
{
//  Serial.println("M Butt Dn");
};


USB     Usb;
USBHub     Hub(&Usb);

HIDBoot<HID_PROTOCOL_MOUSE>    HidMouse(&Usb);
HIDBoot<HID_PROTOCOL_KEYBOARD>    HidKeyboard(&Usb);

KbdRptParser KbdPrs;
MouseRptParser MousePrs;

/*
void blinks(int times) {
  for(int i=0; i < times; i++) {
    digitalWrite(ledPin, HIGH);   // turn the LED on (HIGH is the voltage level)
    delay(100);              // wait for a second
    digitalWrite(ledPin, LOW);    // turn the LED off by making the voltage LOW
    delay(100);              // wait for a second
  }
}
*/
void setup()
{
  int i = 0;
  int j = 0;
  pinMode( strobe, OUTPUT);
  pinMode( data, OUTPUT);
  pinMode( reset, OUTPUT);
//  pinMode( resetQL, OUTPUT);
  pinMode( shift, OUTPUT);
  pinMode( ctrl, OUTPUT);
  pinMode( alt, OUTPUT);
  pinMode( flagOut, OUTPUT);
  digitalWrite( reset, HIGH );
  digitalWrite( reset, LOW );
  digitalWrite( strobe, LOW );
  digitalWrite( shift, LOW );
  digitalWrite( ctrl, LOW );
  digitalWrite( alt, LOW );
  digitalWrite( data, LOW );
  
  DDRC |= 0x3F; // Set C0-C5 to be output - these are the 8808 switch matrix address selection pins
  PCMSK2 |= 0b00000001; // PCINT16, watch changes on PD0
  /*
  if (Usb.Init() == -1)
    blinks(10);
  */
  digitalWrite( data, LOW );
  for (i=0;i<8;i++) {
    for (j=0;j<8;j++) {
      byte addr = ((j << 3) | i);
      PORTC = (addr | (PORTC & 0xC0));
      digitalWrite( strobe, HIGH );
      digitalWrite( strobe, LOW );
    }
  }
  Usb.Init();
  HidKeyboard.SetReportParser(0, (HIDReportParser*)&KbdPrs);
  HidMouse.SetReportParser(0, (HIDReportParser*)&MousePrs);
  Timer1.initialize(1000); // 1 ms
  Timer1.attachInterrupt(delayedSwitchOn);
  Timer1.start();
  Timer1.stop();
}

// Interrupt handler for port D pin change interrupts
ISR(PCINT2_vect) {
  PCICR &= 0b11111011; // Turn off pin change interrupts on port D
  if(digitalRead(flagIn)) {
    // QL set flag indicating it is waiting to read mouse state
    digitalWrite(reset,HIGH);
    digitalWrite(reset,LOW);
    int8_t dX = MousePrs.getMouseDeltaX();
    for ( uint8_t i=0; i < 8; i++) {
      if( dX & _BV(i)) {
        uint8_t yaddr = i << 3;
        uint8_t switchAddr = yaddr + 1;
        setSwitch(switchAddr, HIGH);
      }
    }
  } else {
    // QL cleared flag indicating it has read mouse state
    digitalWrite(reset,HIGH);
    digitalWrite(reset,LOW);
  }
}

void loop()
{
  Usb.Task();
}

