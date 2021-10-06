#include <hidboot.h>
#include <usbhub.h>
#include <TimerOne.h>
#include <avr/interrupt.h>

// Satisfy the IDE, which needs to see the include statment in the ino too.
#ifdef dobogusinclude
#include <spi4teensy3.h>
#include <SPI.h>
#endif

// PD0 == RXI point on board edge
// PD1 == TXO point on board edge
// Can run soft serial on these for debugging purposes

// Strobe is used to clock gate changes to the 8808 switch matrix
const byte strobe = PD2;
// Data is the state of the switch matrix switch to set (1==closed, 0==open)
const byte data = PD3;
// Resets the 8808 switch matrix (all switches off)
const byte reset = PD4;

// Pin that is connected to the QL resetLow line
// Not used at the moment, needs HW review
// const byte resetQL = PD5;

// Pin to control modifier keys that are implemented using the separate 74LVC4066 quad switch IC
const byte shift = PD6;
const byte ctrl = PD7;
const byte alt = PB0;

/*
 * USB keycode to switch matrix address translation table
 * Matrix addresses: y-address 3 bits, x-address 3 bits: 00yyyxxx
 * e.g. 00011010 => y=011=3, x=010=2 = 0x1A
 * 0xFF == not mapped
 * 0b10xxxxxx will be looked up in the specialKeys table below that represent combination key presses on the QL
 * e.g. 0x81 maps to PC keyboard Del to specialKeys row 1, or ctrl-right on the QL
 * 0x40 will be looked up in the pcToQlMapping table to support mapping, e.g. ctrl-2 on PC to ï on QL
 * currently hardcoded to only supporet "2" key mapping for testing purposes
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

// "Special" keys map to modifier+key on the QL but toggled in the switch matrix so that the
// modifier is toggled on and then on Timer expiration (1ms) the non-modifier key is switched on
// This makes e.g. delete-left work on the QL
// If both modifier and key are switched on simultaneously, the QL will not recognize the special combo
const byte specialKeys[3][2] = {
  // Backspace = Ctrl+Left
  {0x0F,0x09},
  // Delete = Ctrl+Right
  {0x0F,0x21},
  // F6 = Shift+F1
  {0x07,0x08}
};

const byte pcToQlMapping[1][5] = {
  // TODO currently only one possible mapping (used for "2" key)
  // TODO should probbaly use logic similar to specialKeys where row selected based on mapping table above
  // i.e. 0x40 == row 0, 0x41 == row 1, etc.
  // Mapping for USB event modifiers {Plain, Shift, Ctrl, Shift-Ctrl, AltGr}
  // Value in table -> Bit 7 controls ctrl-key on QL side, Bit 6 controls alt-key on QL side
  // 2 == 2, Shift-2(") == Shift-', Ctrl-2 == ï (orig), Shift-Ctrl-2 == Ä (mapped elsewhere), , AltGr-2 (@) == Shift-2
  // e.g. mapping of shift-2 to 0x7A (01 111 100) will activate shift modifier on QL and switch on matrix address 111 100
  {0x0E,0x7A,0x8E,0xCE,0x4E}
};

void setSwitch(byte address, byte state) {
    // Set lowest 6 bits of PORTC to 8808 address pins {AY[2-0],AX[2-0]}
    PORTC = (address | (PORTC & 0xC0));
    // State to set the matrix address switch to, HIGH (on) or LOW (off)
    digitalWrite( data, state );
    // Pulse the new state to 8808 using the strobe pin
    digitalWrite( strobe, HIGH );
    digitalWrite( strobe, LOW );
}

byte delayedAddr = 0xFF;
byte callCount = 0;
// Called by Timer when double key combinations are required
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
      // Can't have both a "special" (e.g. bksp == ctrl-left) and a pc->ql mapped key (e.g. ctrl-2 == ï)
      // flags on at the same time
      return;
    }
    byte row = address & 0x3F; // mapping table row in low six bits
    byte keySetting;
    MODIFIERKEYS modifiers;
    *((uint8_t*)&modifiers) = mod;
    
    // Map pc keys to ql keys
    if(specialKeyFlag == 0x40 ) {
      byte col = ((modifiers.bmLeftCtrl || modifiers.bmRightCtrl) << 1) | (modifiers.bmLeftShift || modifiers.bmRightShift);
      // AltGr on Finnish keyboard
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
      // TODO: Missing a "return" here?? if state==LOW will drop through to the else clause below?
    }
    if(specialKeyFlag == 0x80) { // If top two bits are '10' treat this key as a special combination key
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
    // Base case: map a key to a switch matrix address
    if(keycode > 3 && keycode < 83  && address < 0x40 ) {
      setSwitch(address, state);
    }
  }
}

class KbdRptParser : public KeyboardReportParser
{
  protected:
    void OnControlKeysChanged(uint8_t before, uint8_t after);

    void OnKeyDown (uint8_t mod, uint8_t key);
    void OnKeyUp (uint8_t mod, uint8_t key);
    void OnKeyPressed (uint8_t key);
};

void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key)
{
  setOutputState(key, mod, HIGH);
}

void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t*)&beforeMod) = before;

  MODIFIERKEYS afterMod;
  *((uint8_t*)&afterMod) = after;

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

USB     Usb;
// If Hub support required:
// USBHub     Hub(&Usb);

HIDBoot<HID_PROTOCOL_KEYBOARD>    HidKeyboard(&Usb);

KbdRptParser KbdPrs;

void setup()
{
  int i = 0;
  int j = 0;
  pinMode( data, OUTPUT);
  pinMode( strobe, OUTPUT);
  pinMode( reset, OUTPUT);
// Map a special keyboard combo to reset QL??
// Would need to check first that there aren't multiple outputs connected together on the PCB
//  pinMode( resetQL, OUTPUT);
  pinMode( shift, OUTPUT);
  pinMode( ctrl, OUTPUT);
  pinMode( alt, OUTPUT);
  digitalWrite( reset, HIGH );
  digitalWrite( reset, LOW );
  digitalWrite( data, LOW );
  digitalWrite( strobe, LOW );
  digitalWrite( shift, LOW );
  digitalWrite( ctrl, LOW );
  digitalWrite( alt, LOW );
  
  DDRC |= 0x3F; // Set C0-C5 to be output - these are the 8808 switch matrix address selection pins
  PCMSK2 |= 0b00000001; // PCINT16, watch changes on PD0

// I have absolutely no idea why this code is here
// reset above should clear all the switch states, according to datasheet
  digitalWrite( data, LOW );
  for (i=0;i<8;i++) {
    for (j=0;j<8;j++) {
      byte addr = ((j << 3) | i);
      PORTC = (addr | (PORTC & 0xC0));
      digitalWrite( strobe, HIGH );
      digitalWrite( strobe, LOW );
    }
  }
// end strange reset code
  // If USB init fails and returns -1 then what? Retry in a while?
  Usb.Init();
  HidKeyboard.SetReportParser(0, (HIDReportParser*)&KbdPrs);
  // Timer used for combined characters, e.g. backspace on USB keyboard maps to ctrl-left
  // QL needs to see the ctrl key go down before left key is pressed to register this
  // So, for combinations, we switch on the modifier, e.g. ctrl and then, using Timer, switch on the modified key
  // 1ms later
  Timer1.initialize(1000); // 1 ms
  Timer1.attachInterrupt(delayedSwitchOn);
  Timer1.start();
  Timer1.stop();
}

void loop()
{
  Usb.Task();
}

