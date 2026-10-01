#include <Wire.h>
#include "web.h"          // Wi-Fi + live dashboard
#define TCA_ADDR    0x70
#define MAX_ADDR    0x57
#define NUM_SENSORS 4

#define ECG_PIN   34
#define LO_PLUS   32
#define LO_MINUS  33

// ================= TUNING =================
#define LOOP_US        5000     // 200 Hz processing loop
#define PRINT_DIV      4        // print every 4th loop -> 50 Hz output
#define FS_HZ          200.0f   // processing sample rate
#define MAINS_HZ       50.0f    // set to 60.0 if your mains is 60 Hz
#define NOTCH_Q        4.0f     // notch sharpness

#define ECG_OVERSAMPLE 16       // ADC reads averaged per loop
#define ECG_BASE_A     0.002f   // ECG high-pass speed
#define ECG_LP_A       0.45f    // ECG low-pass (lower = smoother)
#define ECG_GAIN       6.0f     // <<< amplify ECG
#define ECG_CLAMP      4000.0f

#define PPG_BASE_A     0.008f   // PPG high-pass speed
#define PPG_LP_A       0.30f    // PPG low-pass (lower = smoother)
#define PPG_GAIN       5.0f     // <<< amplify PPG
#define PPG_CLAMP      8000.0f
// ==========================================

// ---- biquad (used for the mains notch) ----
struct Biquad { float b0,b1,b2,a1,a2, x1,x2,y1,y2; };
Biquad ecgNotch;
void notchInit(Biquad &f, float f0, float fs, float Q){
  float w0 = 2.0f*PI*f0/fs, c = cosf(w0), s = sinf(w0), al = s/(2.0f*Q);
  float a0 = 1.0f+al;
  f.b0 = 1.0f/a0; f.b1 = -2.0f*c/a0; f.b2 = 1.0f/a0;
  f.a1 = -2.0f*c/a0; f.a2 = (1.0f-al)/a0;
  f.x1=f.x2=f.y1=f.y2=0;
}
float biquad(Biquad &f, float x){
  float y = f.b0*x + f.b1*f.x1 + f.b2*f.x2 - f.a1*f.y1 - f.a2*f.y2;
  f.x2=f.x1; f.x1=x; f.y2=f.y1; f.y1=y;
  return y;
}
float clampf(float v, float lim){ return v>lim?lim:(v<-lim?-lim:v); }

bool  present[NUM_SENSORS] = {false,false,false,false};
float ppgBase[NUM_SENSORS]={0}, ppgLP1[NUM_SENSORS]={0}, ppgLP2[NUM_SENSORS]={0}, ppgOut[NUM_SENSORS]={0};
uint32_t ppgHist[NUM_SENSORS][3]={{0}}; uint8_t ppgHi[NUM_SENSORS]={0};
float ecgBase=0, ecgLP1=0, ecgLP2=0, ecgOut=0;
unsigned long lastLoop=0; uint32_t loopCount=0;

void tcaSelect(uint8_t ch){ Wire.beginTransmission(TCA_ADDR); Wire.write(1<<ch); Wire.endTransmission(); }
void writeReg(uint8_t r,uint8_t v){ Wire.beginTransmission(MAX_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
uint8_t readReg(uint8_t r){
  Wire.beginTransmission(MAX_ADDR); Wire.write(r); Wire.endTransmission(false);
  Wire.requestFrom(MAX_ADDR,(uint8_t)1); return Wire.available()?Wire.read():0;
}

bool initSensor(uint8_t ch){
  tcaSelect(ch);
  if(readReg(0xFF) != 0x15) return false;
  writeReg(0x09,0x40); delay(50);
  writeReg(0x08,0x50);        // sample-avg 4 + rollover
  writeReg(0x09,0x03);        // RED + IR
  writeReg(0x0A,0x2B);        // 200 Hz, 18-bit (-> 50 Hz after avg-4)
  writeReg(0x0C,0x30); writeReg(0x0D,0x30);   // LED ~10 mA
  writeReg(0x04,0x00); writeReg(0x05,0x00); writeReg(0x06,0x00);
  return true;
}

uint32_t median3(uint32_t a, uint32_t b, uint32_t c){
  if(a>b){uint32_t t=a;a=b;b=t;} if(b>c){uint32_t t=b;b=c;c=t;} if(a>b){uint32_t t=a;a=b;b=t;}
  return b;
}

void updatePPG(uint8_t ch){
  tcaSelect(ch);
  uint8_t wr = readReg(0x04) & 0x1F;
  uint8_t rd = readReg(0x06) & 0x1F;
  int n = (wr - rd) & 0x1F;
  for(int s=0; s<n; s++){
    Wire.beginTransmission(MAX_ADDR); Wire.write(0x07); Wire.endTransmission(false);
    Wire.requestFrom(MAX_ADDR,(uint8_t)6);
    if(Wire.available()<6) return;
    uint8_t b[6]; for(int i=0;i<6;i++) b[i]=Wire.read();
    uint32_t raw = (((uint32_t)b[3]&0x03)<<16)|((uint32_t)b[4]<<8)|b[5];

    // median-of-3 removes single-sample motion spikes
    ppgHist[ch][ppgHi[ch]] = raw; ppgHi[ch] = (ppgHi[ch]+1)%3;
    uint32_t ir = median3(ppgHist[ch][0], ppgHist[ch][1], ppgHist[ch][2]);

    if(ppgBase[ch]==0) ppgBase[ch]=ir;
    ppgBase[ch] += PPG_BASE_A * (ir - ppgBase[ch]);   // high-pass
    float ac = (float)ir - ppgBase[ch];
    ppgLP1[ch] += PPG_LP_A * (ac        - ppgLP1[ch]); // 2-pole low-pass
    ppgLP2[ch] += PPG_LP_A * (ppgLP1[ch]- ppgLP2[ch]);
    ppgOut[ch] = clampf(ppgLP2[ch] * PPG_GAIN, PPG_CLAMP);   // amplify
  }
}

float readECG(){
  if((digitalRead(LO_PLUS)==HIGH)||(digitalRead(LO_MINUS)==HIGH)) return 0;
  long sum=0;
  for(int i=0;i<ECG_OVERSAMPLE;i++) sum += analogRead(ECG_PIN);
  float raw = (float)sum / ECG_OVERSAMPLE;
  ecgBase += ECG_BASE_A * (raw - ecgBase);            // high-pass
  float hp  = raw - ecgBase;
  float nf  = biquad(ecgNotch, hp);                    // mains notch (IIR)
  ecgLP1 += ECG_LP_A * (nf     - ecgLP1);              // 2-pole low-pass
  ecgLP2 += ECG_LP_A * (ecgLP1 - ecgLP2);
  return clampf(ecgLP2 * ECG_GAIN, ECG_CLAMP);         // amplify
}

void setup(){
  Serial.begin(115200); delay(500);
  Wire.begin(21,22); Wire.setClock(400000);
  pinMode(LO_PLUS,INPUT); pinMode(LO_MINUS,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(ECG_PIN, ADC_11db);
  notchInit(ecgNotch, MAINS_HZ, FS_HZ, NOTCH_Q);
  for(uint8_t ch=0; ch<NUM_SENSORS; ch++){
    present[ch]=initSensor(ch);
    Serial.printf("PPG ch%d: %s\n", ch, present[ch]?"OK":"not found");
  }
  webBegin();
  lastLoop = micros();
}

void loop(){
  if((uint32_t)(micros() - lastLoop) < LOOP_US) return;
  lastLoop += LOOP_US;
  float ecg = readECG();                 // 200 Hz
  webAddEcg(ecg);
  if(++loopCount % PRINT_DIV == 0){      // 50 Hz output
    for(uint8_t ch=0; ch<NUM_SENSORS; ch++) if(present[ch]) updatePPG(ch);
    Serial.print(ecg);
    for(uint8_t ch=0; ch<NUM_SENSORS; ch++){
      if(!present[ch]) continue;
      Serial.print('\t'); Serial.print(ppgOut[ch]);
    }
    Serial.println();
    webAddPpg(ppgOut, present, ppgBase,
              digitalRead(LO_PLUS)==HIGH || digitalRead(LO_MINUS)==HIGH);
  }
}
