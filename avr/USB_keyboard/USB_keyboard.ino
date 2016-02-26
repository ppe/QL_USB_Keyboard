#include <hidboot.h>
#include <usbhub.h>

// Satisfy the IDE, which needs to see the include statment in the ino too.
#ifdef dobogusinclude
#include <spi4teensy3.h>
#include <SPI.h>
#endif

const byte strobe = A0;
const byte data = A1;
const byte reset = A2;

/*
 * USB keycode to switch matrix address translation table
 * Matrix addresses: y-address 3 bits, x-address 3 bits: yyyxxx00
 * e.g. 01101000 => y=011=3, x=010=2 = 0x68
 * 0xFF == not mapped
 */
const byte keyMapping[60] = {
   // 0, 0, 0, 0,         a=4:4,b=4:2,c=3:2,d=6:4,e=4:6,f=4:3,g=6:3,h=2:4,i=2:5,j=7:4,k=2:3,l=0:4,m=6:2,n=6:7,o=0:5,p=5:4
   0xFF, 0xFF, 0xFF, 0xFF, 0x90, 0x88, 0x68, 0xD0, 0x98, 0x8C, 0xCC, 0x50, 0x54, 0xF0, 0x4C, 0x10, 0xC8, 0xDC, 0x14, 0xB0,
//q=3:6,r=4:5,s=3:3,t=6:6,u=7:6,v=4:7,w=1:5,x=3:7,y=6:5,z=1:2,1=1:4,2=1:6,3=3:4,4=6:0,5=2:0,6=2:6,7=7:0,8=0:6,9=7:5,0=5:6
   0x78, 0x94, 0x6C, 0xD8, 0xF8, 0x9C, 0x34, 0x7C, 0xD4, 0x28, 0x30, 0x38, 0x70, 0xC0, 0x40, 0x58, 0xE0, 0x18, 0xF4, 0xB8,
// Ent=0:1,Esc=3:1,Bkspace=??,Tab=3:5,SPC=6:1,-=5:5,==5:3,[=0:3,]=0:2,\=5:1,~=???,;=7:3,'=???,´=???,','=???,'.'=???,/=???,
      0x04,   0x64,      0xFF,   0x74,   0xC4, 0xB4, 0xAC, 0x0C, 0x08, 0xA4, 0xFF, 0xEC, 0xFF, 0xFF,   0xFF,   0xFF, 0xFF,
// Caps=????,F1=1:0,F2=3:0,
        0xFF, 0x20,   0x60
};

void setOutputOn(uint8_t keycode) {
  if(keycode > 3 && keycode < 60  && keyMapping[keycode] != 0xFF ) {
    byte old = PORTD;
    Serial.print("PORTD = " );
    Serial.println(old, HEX);
    PORTD = (keyMapping[keycode] | (old & 3));
    digitalWrite( data, HIGH );
    digitalWrite( strobe, HIGH );
    digitalWrite( strobe, LOW );
  }
}

void setOutputOff(uint8_t keycode) {
  if(keycode > 3 && keycode < 60 && keyMapping[keycode] != 0xFF ) {
    PORTD = (keyMapping[keycode] | (PORTD & 3));
    digitalWrite( data, LOW );
    digitalWrite( strobe, HIGH );
    digitalWrite( strobe, LOW );
  }
}

class KbdRptParser : public KeyboardReportParser
{
    void PrintKey(uint8_t mod, uint8_t key);

  protected:
    void OnControlKeysChanged(uint8_t before, uint8_t after);

    void OnKeyDown	(uint8_t mod, uint8_t key);
    void OnKeyUp	(uint8_t mod, uint8_t key);
    void OnKeyPressed(uint8_t key);
};

void KbdRptParser::PrintKey(uint8_t m, uint8_t key)
{
  MODIFIERKEYS mod;
  *((uint8_t*)&mod) = m;
  Serial.print((mod.bmLeftCtrl   == 1) ? "C" : " ");
  Serial.print((mod.bmLeftShift  == 1) ? "S" : " ");
  Serial.print((mod.bmLeftAlt    == 1) ? "A" : " ");
  Serial.print((mod.bmLeftGUI    == 1) ? "G" : " ");

  Serial.print(" >");
  PrintHex<uint8_t>(key, 0x80);
  Serial.print("< ");

  Serial.print((mod.bmRightCtrl   == 1) ? "C" : " ");
  Serial.print((mod.bmRightShift  == 1) ? "S" : " ");
  Serial.print((mod.bmRightAlt    == 1) ? "A" : " ");
  Serial.println((mod.bmRightGUI    == 1) ? "G" : " ");
};

void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key)
{
  Serial.print("DN ");
  PrintKey(mod, key);
  uint8_t c = OemToAscii(mod, key);

  if (c) {
    OnKeyPressed(c);
  }
  setOutputOn(key);
}

void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t*)&beforeMod) = before;

  MODIFIERKEYS afterMod;
  *((uint8_t*)&afterMod) = after;

  if (beforeMod.bmLeftCtrl != afterMod.bmLeftCtrl) {
    Serial.println("LeftCtrl changed");
  }
  if (beforeMod.bmLeftShift != afterMod.bmLeftShift) {
    Serial.println("LeftShift changed");
  }
  if (beforeMod.bmLeftAlt != afterMod.bmLeftAlt) {
    Serial.println("LeftAlt changed");
  }
  if (beforeMod.bmLeftGUI != afterMod.bmLeftGUI) {
    Serial.println("LeftGUI changed");
  }

  if (beforeMod.bmRightCtrl != afterMod.bmRightCtrl) {
    Serial.println("RightCtrl changed");
  }
  if (beforeMod.bmRightShift != afterMod.bmRightShift) {
    Serial.println("RightShift changed");
  }
  if (beforeMod.bmRightAlt != afterMod.bmRightAlt) {
    Serial.println("RightAlt changed");
  }
  if (beforeMod.bmRightGUI != afterMod.bmRightGUI) {
    Serial.println("RightGUI changed");
  }

}

void KbdRptParser::OnKeyUp(uint8_t mod, uint8_t key)
{
  Serial.print("UP ");
  setOutputOff(key);
  PrintKey(mod, key);
}

void KbdRptParser::OnKeyPressed(uint8_t key)
{
  Serial.print("ASCII: ");
  Serial.println((char)key);
};

USB     Usb;
//USBHub     Hub(&Usb);
HIDBoot<HID_PROTOCOL_KEYBOARD>    HidKeyboard(&Usb);

KbdRptParser Prs;

void setup()
{
  pinMode( strobe, OUTPUT);
  pinMode( data, OUTPUT);
  pinMode( reset, OUTPUT);
  digitalWrite( reset, HIGH );
  digitalWrite( reset, LOW );
  digitalWrite( strobe, LOW );
  digitalWrite( data, LOW );
  
  DDRD |= 0xFC; // Set D2-D7 to be output
  
  Serial.begin( 9600 );
  Serial.println("Start");

  if (Usb.Init() == -1)
    Serial.println("OSC did not start.");

  Serial.println("OSC started");

  HidKeyboard.SetReportParser(0, (HIDReportParser*)&Prs);
}

void loop()
{
  Usb.Task();
}

