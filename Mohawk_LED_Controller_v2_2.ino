/*
  Mohawk LED Controller v2.2 — Recipe Defaults + Partial Reverse Decay
  ====================================================
  STANDALONE TEST (default):  set MOHAWK_STANDALONE_TEST_MODE 1
    Creates Wi-Fi AP  MiniBot_LED_UI / fifabot123
    Serves UI at     http://192.168.4.1
    Works with no LEDs and no FMS.

  FMS INTEGRATION:  set MOHAWK_FMS_INTEGRATED_MODE 1
    Does NOT create AP or WebServer.
    FMS calls:  setup_mohawk();
                registerMohawkRoutes(fmsServer);
                mohawk_service();  // in FMS loop()
    All routes under /api/mohawk/ — no conflicts.

  PHYSICAL LEDs:  MOHAWK_PHYSICAL_LEDS_ENABLED 0 = software only.
*/

#define MOHAWK_STANDALONE_TEST_MODE  1
#define MOHAWK_FMS_INTEGRATED_MODE   0
#define MOHAWK_PHYSICAL_LEDS_ENABLED 1

#if MOHAWK_STANDALONE_TEST_MODE && MOHAWK_FMS_INTEGRATED_MODE
#error "Cannot enable both modes simultaneously."
#endif
#if !MOHAWK_STANDALONE_TEST_MODE && !MOHAWK_FMS_INTEGRATED_MODE
#error "Enable at least one mode."
#endif

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>

// ---- Hardware ----
#define MOHAWK_LED_COUNT    18
#define MOHAWK_LED_PIN      21
#define LED_CHIPSET         WS2812B
#define LED_COLOR_ORDER     GRB
#define LED_UPDATE_HZ       60
#define LED_UPDATE_MS       (1000 / LED_UPDATE_HZ)
#define MOHAWK_AP_SSID      "MiniBot_LED_UI"
#define MOHAWK_AP_PASS      "fifabot123"

// ---- Physical solenoid (set pin >= 0 to enable) ----
#define SOLENOID_TEST_PIN         -1
#define SOLENOID_TEST_PULSE_MS    50
#define SOLENOID_TEST_ACTIVE_HIGH 1
#define SOLENOID_MIN_INTERVAL_MS  3000
#define SOLENOID_ARM_TIMEOUT_MS   15000
#define SHOT_PARTIAL_DECAY_STEP_MS 320  // reverse-clear time per shot pair when spam stops before lockout

// ---- LED layout ----
static const uint8_t BOTTOM_LED_A = 8;
static const uint8_t BOTTOM_LED_B = 9;
static const uint8_t SHOT_PAIRS[5][2] = {{0,17},{1,16},{2,15},{3,14},{4,13}};

// ============================================================
// ENUMS
// ============================================================
enum MohawkTeamColor     : uint8_t { TC_BLUE=0, TC_ORANGE=1 };
enum MohawkDiscMode      : uint8_t {
  DISC_RAINBOW=0,DISC_SEARCH_SWEEP,DISC_WIFI_PING,DISC_LOST_SIGNAL,
  DISC_HANDSHAKE,DISC_AURORA,DISC_RADAR,DISC_PAIR_BREATH,
  DISC_COMET,DISC_BEACON,DISC_SIGNAL_BARS,DISC_DIAG_SCANNER,DISC_MODE_COUNT };
enum MohawkTeamIdleMode  : uint8_t {
  IDLE_FLAT_BREATH=0,IDLE_DIAGONAL,IDLE_ROTATING_DIAG,IDLE_COMET_ORBIT,
  IDLE_SPARKLE,IDLE_POWER_CORE,IDLE_RAIL_GLOW,IDLE_SHIELD,
  IDLE_STEALTH,IDLE_PULSE_RINGS,IDLE_EQUALIZER,IDLE_STADIUM_WAVE,IDLE_MODE_COUNT };
enum MohawkChargeMode    : uint8_t {
  CHARGE_BOTTOM_CORE=0,CHARGE_RAIL_GATHER,CHARGE_CAP_FILL,CHARGE_SCANNER_ARM,
  CHARGE_PLASMA_CORE,CHARGE_VOLTAGE_RAMP,CHARGE_MAG_PAIR,CHARGE_LOCK_ON,
  CHARGE_DANGER_HOLD,CHARGE_COIL_WIND,CHARGE_PULSE_COMPRESS,CHARGE_READY_SNAP,CHARGE_MODE_COUNT };
enum MohawkFireMode      : uint8_t {
  FIRE_DUAL_RAIL=0,FIRE_FORWARD_WAVE,FIRE_MUZZLE_BURST,FIRE_RECOIL_SHOCK,
  FIRE_PLASMA_LANCE,FIRE_LIGHTNING_FORK,FIRE_SONIC_BOOM,FIRE_SPARKLE_BLAST,
  FIRE_TEAM_IMPACT,FIRE_CLEAN_FLASH,FIRE_HYPERDRIVE,FIRE_ENERGY_KICK,FIRE_MODE_COUNT };
enum MohawkShotMeterMode : uint8_t {
  SHOT_PAIR_FILL=0,SHOT_GRADIENT_FILL,SHOT_HEAT_STACK,SHOT_AMMO_COUNTER,
  SHOT_PRESSURE_GAUGE,SHOT_FOURTH_WARNING,SHOT_FINAL_REDLOCK,
  SHOT_SPARK_CONFIRM,SHOT_TEAM_TINTED,SHOT_MINIMAL_COMP,SHOT_MODE_COUNT };
enum MohawkCooldownMode  : uint8_t {
  COOL_REVERSE_PAIR=0,COOL_HEAT_DISSOLVE,COOL_TEAM_RECLAIM,COOL_BREATH,
  COOL_MINIMAL_FADE,COOL_SAFETY_FLASH,COOL_LOCKOUT_WARN,COOL_SMOOTH_RETURN,COOL_MODE_COUNT };
enum MohawkCountdownMode : uint8_t {
  CDN_U_DRAIN=0,CDN_TRAFFIC_BUILD,CDN_STADIUM_PULSE,CDN_COMET_TICK,
  CDN_SIDE_LAUNCH,CDN_GATE_OPEN,CDN_GREEN_BLAST,CDN_REFEREE,
  CDN_TEAM_SPLIT,CDN_ENERGY_BUILD,CDN_PULSE_STACK,CDN_CALM_PRO,CDN_MODE_COUNT };
enum MohawkLowBatMode    : uint8_t {
  LOWBAT_RED_FLASH=0,LOWBAT_DOUBLE_BLINK,LOWBAT_HEARTBEAT,LOWBAT_EMBER,
  LOWBAT_VOLTAGE_TIER,LOWBAT_SERVICE,LOWBAT_SILENT,LOWBAT_MODE_COUNT };
enum MohawkSelfTestStep  : uint8_t {
  ST_IDLE=0,ST_DISCONNECTED,ST_CONNECTED_IDLE,ST_COUNTDOWN,ST_CHARGE,
  ST_FIRE_ONE,ST_BURST,ST_COOLDOWN_WAIT,ST_LOW_BATTERY,ST_IDENTIFY,ST_DONE };

// ============================================================
// SETTINGS STRUCT
// ============================================================
struct MohawkSettings {
  uint8_t  globalBrightness;
  uint8_t  disconnectedMode; uint8_t disconnectedSpeed; uint8_t disconnectedBrightness;
  uint8_t  team; uint32_t teamBlue; uint32_t teamOrange;
  uint8_t  teamIdleMode; uint8_t teamBreathSpeed;
  uint8_t  teamBreathMinBrightness; uint8_t teamBreathMaxBrightness;
  bool     teamBreathDiagonal; bool teamBreathRotate;
  uint8_t  teamBreathAngle; uint8_t teamBreathRotationSpeed;
  uint8_t  chargeMode; uint8_t chargeSpeed; uint8_t chargePixels;
  uint8_t  chargeBrightness; uint32_t chargeColor;
  uint8_t  fireMode; uint8_t fireSpeed; uint8_t firePixels;
  uint8_t  fireFade; uint8_t fireBrightness; uint32_t fireColor;
  uint8_t  shotMeterMode; uint8_t maxBurstShots;
  uint16_t burstWindowMs; uint16_t cooldownMs;
  uint8_t  cooldownMode; uint8_t cooldownBrightness;
  uint32_t shotPairColors[5];
  uint8_t  countdownMode; uint8_t countdownSeconds;
  uint8_t  countdownSpeed; uint8_t countdownBrightness;
  uint32_t countdownColorA; uint32_t countdownColorB;
  uint8_t  lowBatteryMode; uint8_t lowBatteryBrightness;
};

// ============================================================
// RUNTIME STRUCT
// ============================================================
struct MohawkRuntime {
  bool connected; bool lowBattery;
  bool chargeActive; uint32_t chargeStartMs;
  bool fireActive; uint32_t fireStartMs;
  uint32_t totalShotsFired; uint32_t fireEventSeq;
  uint8_t burstShotCount; uint32_t burstWindowStartMs;
  uint32_t cooldownUntilMs; uint32_t shotMeterHoldUntilMs;
  uint8_t partialDecayStartCount; uint32_t partialDecayStartMs;
  bool countdownActive; uint32_t countdownStartMs;
  int identifyLed; uint32_t identifyUntilMs;
  uint32_t lastEventMs;
};

// ---- Event log ----
struct EvtEntry { uint32_t ms; uint8_t sev; char msg[52]; };
static const int EVT_LOG_SIZE = 40;
static EvtEntry  evtLog[EVT_LOG_SIZE];
static int       evtHead = 0, evtCount = 0;

// ---- Globals ----
static CRGB leds[MOHAWK_LED_COUNT];
static struct { float x,y; } xyMap[MOHAWK_LED_COUNT];
static MohawkSettings cfg;
static MohawkRuntime  rt;
static Preferences    prefs;
static WebServer     *_srv = nullptr;
#if MOHAWK_STANDALONE_TEST_MODE
static WebServer _standaloneServer(80);
#endif
// Self-test
static MohawkSelfTestStep gSTStep   = ST_IDLE;
static uint32_t           gSTStepMs = 0;
static uint8_t            gSTLedIdx = 0, gSTBurstN = 0;
// Non-blocking solenoid
static bool     solArmed=false, solPulseActive=false;
static uint32_t solPulseStartMs=0, solPulseCoolUntil=0, solArmTimeoutMs=0;

// ============================================================
// UTILITIES
// ============================================================
static inline uint8_t  clampU8 (int v,int lo=0,int hi=255){return(uint8_t)constrain(v,lo,hi);}
static inline uint16_t clampU16(int v,int lo,int hi){return(uint16_t)constrain(v,lo,hi);}
static inline CRGB     u32c(uint32_t v){return CRGB((v>>16)&0xFF,(v>>8)&0xFF,v&0xFF);}
static inline uint32_t c32(CRGB c){return((uint32_t)c.r<<16)|((uint32_t)c.g<<8)|c.b;}
static inline CRGB     teamColor(){return u32c(cfg.team==TC_BLUE?cfg.teamBlue:cfg.teamOrange);}
static inline CRGB     sc(CRGB c,uint8_t b){CRGB o=c;o.nscale8_video(b);return o;}
static inline void     blendLed(uint8_t i,CRGB c,uint8_t a){if(i<MOHAWK_LED_COUNT)leds[i]=blend(leds[i],c,a);}
static float spdRate(uint8_t s,float lo,float hi){return lo+(hi-lo)*constrain((float)s,1.f,100.f)/100.f;}
static String hexStr(uint32_t v){char b[8];snprintf(b,sizeof(b),"#%02X%02X%02X",(unsigned)((v>>16)&0xFF),(unsigned)((v>>8)&0xFF),(unsigned)(v&0xFF));return String(b);}
static String hexCode(uint32_t v){char b[9];snprintf(b,sizeof(b),"0x%06X",(unsigned)(v&0xFFFFFF));return String(b);}
static uint32_t parseCol(const String &s){String t=s;t.replace("#","");t.replace("%23","");if(t.length()!=6)return 0;return(uint32_t)strtoul(t.c_str(),nullptr,16);}

// ---- XY map ----
static void buildXYMap(){
  for(int i=0;i<MOHAWK_LED_COUNT;i++){
    if(i<=7){xyMap[i].x=0;xyMap[i].y=7.f-i;}
    else if(i==8){xyMap[i].x=0.8f;xyMap[i].y=0;}
    else if(i==9){xyMap[i].x=1.2f;xyMap[i].y=0;}
    else{xyMap[i].x=2.f;xyMap[i].y=(float)(i-10);}
  }
}
static float spatialPos(uint8_t i){
  if(i>=MOHAWK_LED_COUNT)return 0;
  float x=xyMap[i].x,y=xyMap[i].y;
  if(x<0.4f)return constrain((7.f-y)/17.f,0.f,1.f);
  if(x<1.0f)return 8.f/17.f;
  if(x<1.6f)return 9.f/17.f;
  return constrain((10.f+y)/17.f,0.f,1.f);
}
static uint8_t ledAtStep(uint8_t step){
  if(step>=MOHAWK_LED_COUNT)step=MOHAWK_LED_COUNT-1;
  float t=(float)step/17.f; uint8_t best=0; float bestE=999;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float e=fabsf(spatialPos(i)-t);if(e<bestE){bestE=e;best=i;}}
  return best;
}

// ---- Event log ----
static void logEvent(uint8_t sev,const char *msg){
  EvtEntry &e=evtLog[evtHead]; e.ms=millis(); e.sev=sev;
  strncpy(e.msg,msg,51); e.msg[51]=0;
  evtHead=(evtHead+1)%EVT_LOG_SIZE; if(evtCount<EVT_LOG_SIZE)evtCount++;
}

// ---- Factory defaults ----
static void setFactoryDefaults(){
  // v2.2 defaults copied from the user's tuned web-UI recipe.
  cfg.globalBrightness=255;
  cfg.disconnectedMode=DISC_RAINBOW; cfg.disconnectedSpeed=100; cfg.disconnectedBrightness=244;
  cfg.team=TC_BLUE; cfg.teamBlue=0x0050FF; cfg.teamOrange=0xFF5000;
  cfg.teamIdleMode=IDLE_ROTATING_DIAG; cfg.teamBreathSpeed=28;
  cfg.teamBreathMinBrightness=5; cfg.teamBreathMaxBrightness=255;
  cfg.teamBreathDiagonal=true; cfg.teamBreathRotate=true;
  cfg.teamBreathAngle=10; cfg.teamBreathRotationSpeed=100;
  cfg.chargeMode=CHARGE_BOTTOM_CORE; cfg.chargeSpeed=48;
  cfg.chargePixels=6; cfg.chargeBrightness=255; cfg.chargeColor=0x00FFAA;
  cfg.fireMode=FIRE_HYPERDRIVE; cfg.fireSpeed=100; cfg.firePixels=3;
  cfg.fireFade=255; cfg.fireBrightness=107; cfg.fireColor=0xFFFFFF;
  cfg.shotMeterMode=SHOT_PAIR_FILL; cfg.maxBurstShots=5;
  cfg.burstWindowMs=2000; cfg.cooldownMs=2000;
  cfg.cooldownMode=COOL_REVERSE_PAIR; cfg.cooldownBrightness=255;
  cfg.shotPairColors[0]=0xFF0202; cfg.shotPairColors[1]=0xFF0606;
  cfg.shotPairColors[2]=0xFF0000; cfg.shotPairColors[3]=0xFE0000;
  cfg.shotPairColors[4]=0xFF0000;
  cfg.countdownMode=CDN_TRAFFIC_BUILD; cfg.countdownSeconds=10;
  cfg.countdownSpeed=50; cfg.countdownBrightness=240;
  cfg.countdownColorA=0xFFBE00; cfg.countdownColorB=0x00FF00;
  cfg.lowBatteryMode=LOWBAT_RED_FLASH; cfg.lowBatteryBrightness=255;
}

static void validateSettings(){
  cfg.globalBrightness=clampU8(cfg.globalBrightness,1,255);
  cfg.disconnectedMode=clampU8(cfg.disconnectedMode,0,DISC_MODE_COUNT-1);
  cfg.disconnectedSpeed=clampU8(cfg.disconnectedSpeed,1,100);
  cfg.disconnectedBrightness=clampU8(cfg.disconnectedBrightness,1,255);
  cfg.team=clampU8(cfg.team,0,1);
  cfg.teamIdleMode=clampU8(cfg.teamIdleMode,0,IDLE_MODE_COUNT-1);
  cfg.teamBreathSpeed=clampU8(cfg.teamBreathSpeed,1,100);
  cfg.teamBreathMinBrightness=clampU8(cfg.teamBreathMinBrightness,0,254);
  cfg.teamBreathMaxBrightness=clampU8(cfg.teamBreathMaxBrightness,1,255);
  if(cfg.teamBreathMinBrightness>=cfg.teamBreathMaxBrightness)
    cfg.teamBreathMinBrightness=max(0,(int)cfg.teamBreathMaxBrightness-1);
  cfg.teamBreathRotationSpeed=clampU8(cfg.teamBreathRotationSpeed,1,100);
  cfg.chargeMode=clampU8(cfg.chargeMode,0,CHARGE_MODE_COUNT-1);
  cfg.chargeSpeed=clampU8(cfg.chargeSpeed,1,100);
  cfg.chargePixels=clampU8(cfg.chargePixels,1,MOHAWK_LED_COUNT);
  cfg.chargeBrightness=clampU8(cfg.chargeBrightness,1,255);
  cfg.fireMode=clampU8(cfg.fireMode,0,FIRE_MODE_COUNT-1);
  cfg.fireSpeed=clampU8(cfg.fireSpeed,1,100);
  cfg.firePixels=clampU8(cfg.firePixels,1,MOHAWK_LED_COUNT);
  cfg.fireFade=clampU8(cfg.fireFade,0,255);
  cfg.fireBrightness=clampU8(cfg.fireBrightness,1,255);
  cfg.shotMeterMode=clampU8(cfg.shotMeterMode,0,SHOT_MODE_COUNT-1);
  cfg.maxBurstShots=clampU8(cfg.maxBurstShots,1,5);
  cfg.burstWindowMs=clampU16(cfg.burstWindowMs,400,8000);
  cfg.cooldownMs=clampU16(cfg.cooldownMs,200,8000);
  cfg.cooldownMode=clampU8(cfg.cooldownMode,0,COOL_MODE_COUNT-1);
  cfg.cooldownBrightness=clampU8(cfg.cooldownBrightness,1,255);
  cfg.countdownMode=clampU8(cfg.countdownMode,0,CDN_MODE_COUNT-1);
  cfg.countdownSeconds=clampU8(cfg.countdownSeconds,3,30);
  cfg.countdownSpeed=clampU8(cfg.countdownSpeed,1,100);
  cfg.countdownBrightness=clampU8(cfg.countdownBrightness,1,255);
  cfg.lowBatteryMode=clampU8(cfg.lowBatteryMode,0,LOWBAT_MODE_COUNT-1);
  cfg.lowBatteryBrightness=clampU8(cfg.lowBatteryBrightness,1,255);
}

// ---- Save / Load ----
static const uint32_t SETTINGS_MAGIC = 0x4D4C3232;
static void saveSettings(){
  prefs.begin("mohawkv22",false);
  prefs.putUInt("magic",SETTINGS_MAGIC);
  prefs.putUInt("cfgSz",(uint32_t)sizeof(MohawkSettings));
  prefs.putBytes("cfg",&cfg,sizeof(MohawkSettings));
  prefs.end(); logEvent(0,"Settings saved");
}
static void loadSettings(){
  setFactoryDefaults();
  prefs.begin("mohawkv22",true);
  uint32_t mg=prefs.getUInt("magic",0), sz=prefs.getUInt("cfgSz",0);
  if(mg==SETTINGS_MAGIC && sz==sizeof(MohawkSettings)){
    prefs.getBytes("cfg",&cfg,sizeof(MohawkSettings));
    logEvent(0,"Settings loaded from NVS");
  } else { logEvent(1,"NVS mismatch - using defaults"); }
  prefs.end(); validateSettings();
}
static void factoryReset(){
  setFactoryDefaults();
  prefs.begin("mohawkv22",false); prefs.clear(); prefs.end();
  logEvent(1,"Factory reset");
}
static void initRuntime(){
  rt={};
  rt.identifyLed=-1;
}

// ============================================================
// PUBLIC API
// ============================================================
void mohawk_set_connected(bool v){
  if(rt.connected!=v){rt.connected=v;rt.lastEventMs=millis();logEvent(0,v?"Connected":"Disconnected");}
}
void mohawk_set_low_battery(bool v){
  if(rt.lowBattery!=v){rt.lowBattery=v;rt.lastEventMs=millis();logEvent(v?2:0,v?"Low battery ON":"Low battery OFF");}
}
void mohawk_start_solenoid_charge(){
  rt.chargeActive=true; rt.chargeStartMs=millis(); rt.lastEventMs=rt.chargeStartMs; logEvent(0,"Charge started");
}
void mohawk_stop_solenoid_charge(){
  if(rt.chargeActive){rt.chargeActive=false;rt.lastEventMs=millis();logEvent(0,"Charge stopped");}
}
void mohawk_reset_shot_meter(){
  rt.totalShotsFired=rt.burstShotCount=rt.burstWindowStartMs=rt.cooldownUntilMs=rt.shotMeterHoldUntilMs=0;
  rt.partialDecayStartCount=0; rt.partialDecayStartMs=0;
  logEvent(0,"Shot meter reset");
}
void mohawk_trigger_solenoid_animation(){
  const uint32_t now=millis();
  if(now<rt.cooldownUntilMs){rt.shotMeterHoldUntilMs=rt.cooldownUntilMs;return;}
  if(rt.burstWindowStartMs==0||(now-rt.burstWindowStartMs)>cfg.burstWindowMs){rt.burstWindowStartMs=now;rt.burstShotCount=0;}
  rt.partialDecayStartCount=0; rt.partialDecayStartMs=0; // a new shot cancels any partial reverse-clear animation
  rt.burstShotCount++; if(rt.burstShotCount>cfg.maxBurstShots)rt.burstShotCount=cfg.maxBurstShots;
  rt.totalShotsFired++; rt.fireEventSeq++;
  rt.fireActive=true; rt.fireStartMs=now; rt.chargeActive=false;
  rt.shotMeterHoldUntilMs=now+cfg.burstWindowMs; rt.lastEventMs=now;
  char buf[40]; snprintf(buf,sizeof(buf),"Fire seq=%lu burst=%d",(unsigned long)rt.fireEventSeq,(int)rt.burstShotCount);
  logEvent(0,buf);
  if(rt.burstShotCount>=cfg.maxBurstShots){
    rt.fireActive=false; rt.cooldownUntilMs=now+cfg.cooldownMs;
    rt.shotMeterHoldUntilMs=rt.cooldownUntilMs; logEvent(1,"Shot limit - cooldown");
  }
}
void mohawk_start_countdown(uint8_t s){
  if(s==0)s=10; cfg.countdownSeconds=clampU8(s,3,30);
  rt.countdownStartMs=millis(); rt.countdownActive=true; rt.lastEventMs=rt.countdownStartMs;
  char buf[30]; snprintf(buf,sizeof(buf),"Countdown: %ds",(int)s); logEvent(0,buf);
}
void mohawk_stop_countdown(){
  if(rt.countdownActive){rt.countdownActive=false;rt.lastEventMs=millis();logEvent(0,"Countdown stopped");}
}

// ---- Non-blocking solenoid ----
void armSolenoid(){solArmed=true;solArmTimeoutMs=millis()+SOLENOID_ARM_TIMEOUT_MS;logEvent(1,"Solenoid ARMED");}
void disarmSolenoid(){if(solArmed){solArmed=false;logEvent(1,"Solenoid disarmed");}}
void triggerSolenoidPulse(){
#if SOLENOID_TEST_PIN >= 0
  if(!solArmed){logEvent(2,"Sol: not armed");return;}
  if(millis()<solPulseCoolUntil){logEvent(1,"Sol: cooldown");return;}
  if(solPulseActive)return;
  solPulseActive=true; solPulseStartMs=millis();
  digitalWrite(SOLENOID_TEST_PIN,SOLENOID_TEST_ACTIVE_HIGH?HIGH:LOW);
  logEvent(1,"Solenoid pulse triggered");
#else
  logEvent(2,"No physical solenoid (pin=-1)");
#endif
}
static void serviceSolenoidPulse(uint32_t now){
#if SOLENOID_TEST_PIN >= 0
  if(solArmed&&now>=solArmTimeoutMs)disarmSolenoid();
  if(!solPulseActive)return;
  if(!solArmed){digitalWrite(SOLENOID_TEST_PIN,SOLENOID_TEST_ACTIVE_HIGH?LOW:HIGH);solPulseActive=false;return;}
  if(now-solPulseStartMs>=SOLENOID_TEST_PULSE_MS){
    digitalWrite(SOLENOID_TEST_PIN,SOLENOID_TEST_ACTIVE_HIGH?LOW:HIGH);
    solPulseActive=false; solPulseCoolUntil=now+SOLENOID_MIN_INTERVAL_MS; logEvent(1,"Solenoid pulse done");
  }
#else
  (void)now;
#endif
}

// ---- Self-test state machine ----
void startSelfTest(){
  if(gSTStep!=ST_IDLE){logEvent(1,"Self-test already running");return;}
  gSTStep=ST_DISCONNECTED; gSTStepMs=millis(); gSTLedIdx=gSTBurstN=0;
  mohawk_set_connected(false); mohawk_reset_shot_meter(); logEvent(0,"Self-test started");
}
void cancelSelfTest(){gSTStep=ST_IDLE;mohawk_set_connected(true);rt.identifyLed=-1;logEvent(1,"Self-test canceled");}
static void serviceSelfTest(uint32_t now){
  if(gSTStep==ST_IDLE)return;
  uint32_t el=now-gSTStepMs;
  switch(gSTStep){
    case ST_DISCONNECTED: if(el>3000){gSTStep=ST_CONNECTED_IDLE;gSTStepMs=now;mohawk_set_connected(true);logEvent(0,"ST: connected idle");} break;
    case ST_CONNECTED_IDLE: if(el>3000){gSTStep=ST_COUNTDOWN;gSTStepMs=now;mohawk_start_countdown(5);logEvent(0,"ST: countdown");} break;
    case ST_COUNTDOWN: if(el>6000){gSTStep=ST_CHARGE;gSTStepMs=now;mohawk_stop_countdown();mohawk_start_solenoid_charge();logEvent(0,"ST: charge");} break;
    case ST_CHARGE: if(el>2000){gSTStep=ST_FIRE_ONE;gSTStepMs=now;mohawk_trigger_solenoid_animation();logEvent(0,"ST: fire 1");} break;
    case ST_FIRE_ONE: if(el>2000){gSTStep=ST_BURST;gSTStepMs=now;gSTBurstN=0;mohawk_reset_shot_meter();logEvent(0,"ST: burst");} break;
    case ST_BURST:
      if(el>300&&gSTBurstN<5){gSTStepMs=now;mohawk_trigger_solenoid_animation();gSTBurstN++;
        if(gSTBurstN>=5){gSTStep=ST_COOLDOWN_WAIT;logEvent(0,"ST: wait cooldown");}} break;
    case ST_COOLDOWN_WAIT:
      if(rt.cooldownUntilMs==0||now>=rt.cooldownUntilMs){gSTStep=ST_LOW_BATTERY;gSTStepMs=now;mohawk_set_low_battery(true);logEvent(0,"ST: low bat");} break;
    case ST_LOW_BATTERY:
      if(el>3000){mohawk_set_low_battery(false);gSTStep=ST_IDENTIFY;gSTStepMs=now;gSTLedIdx=0;logEvent(0,"ST: identify");} break;
    case ST_IDENTIFY:
      if(el>180){gSTStepMs=now;rt.identifyLed=(int)gSTLedIdx;rt.identifyUntilMs=now+220;gSTLedIdx++;
        if(gSTLedIdx>=MOHAWK_LED_COUNT){rt.identifyLed=-1;gSTStep=ST_DONE;logEvent(0,"ST: done");}} break;
    case ST_DONE: gSTStep=ST_IDLE; logEvent(0,"Self-test finished OK"); break;
    default: gSTStep=ST_IDLE; break;
  }
}

// ============================================================
// DISCONNECTED ANIMATIONS
// ============================================================
static void renderRainbowWash(uint32_t now){
  float rate=spdRate(cfg.disconnectedSpeed,0.000025f,0.00035f);
  uint8_t hB=(uint8_t)((float)now*rate*255.f);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++) leds[i]=CHSV(hB+(uint8_t)(spatialPos(i)*120.f),255,cfg.disconnectedBrightness);
}
static void renderSearchSweep(uint32_t now){
  fill_solid(leds,MOHAWK_LED_COUNT,CRGB::Black);
  float rate=spdRate(cfg.disconnectedSpeed,0.0003f,0.004f);
  float t=fmodf((float)now*rate,2.f), pos=(t<1.f)?t:2.f-t;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float d=fabsf(spatialPos(i)-pos);
    if(d<0.12f) leds[i]=CRGB(0,cfg.disconnectedBrightness,cfg.disconnectedBrightness);
    else if(d<0.26f){uint8_t v=(uint8_t)((1.f-(d-0.12f)/0.14f)*cfg.disconnectedBrightness*0.5f);leds[i]=CRGB(0,v,v);}
  }
}
static void renderWifiPing(uint32_t now){
  fill_solid(leds,MOHAWK_LED_COUNT,CRGB::Black);
  float rate=spdRate(cfg.disconnectedSpeed,0.0003f,0.003f);
  float t=fmodf((float)now*rate,1.f), ring=t*9.f;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float dx=xyMap[i].x-1.f,dy=xyMap[i].y,dist=sqrtf(dx*dx+dy*dy),d=fabsf(dist-ring);
    if(d<0.7f){uint8_t v=(uint8_t)((1.f-d/0.7f)*(1.f-t)*cfg.disconnectedBrightness);leds[i]=CRGB(0,v>>1,v);}
  }
  uint8_t core=beatsin8(12,60,cfg.disconnectedBrightness);
  leds[8]=CRGB(0,core>>1,core); leds[9]=CRGB(0,core>>1,core);
}
static void renderAurora(uint32_t now){
  float t=(float)now*0.0001f;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float sp=spatialPos(i);
    float w1=sinf(sp*6.28f+t)*0.5f+0.5f, w2=sinf(sp*3.14f-t*0.7f)*0.5f+0.5f;
    leds[i]=CHSV((uint8_t)(w1*60.f+w2*30.f+130.f),210,(uint8_t)((w1*0.6f+0.4f)*cfg.disconnectedBrightness));
  }
}
static void renderWaitingComet(uint32_t now){
  float rate=spdRate(cfg.disconnectedSpeed,0.0006f,0.006f);
  float pos=fmodf((float)now*rate,1.f);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float d=fabsf(spatialPos(i)-pos); if(d>0.5f)d=1.f-d;
    if(d<0.28f) leds[i]=CHSV((uint8_t)(pos*255.f)+(uint8_t)(d*50.f),255,(uint8_t)((1.f-d/0.28f)*cfg.disconnectedBrightness));
    else leds[i]=CRGB::Black;
  }
}
static void renderSignalBars(uint32_t now){
  fill_solid(leds,MOHAWK_LED_COUNT,sc(CRGB(0,30,60),60));
  float rate=spdRate(cfg.disconnectedSpeed,0.0004f,0.002f);
  uint8_t bars=(uint8_t)fmodf((float)now*rate,6.f);
  for(uint8_t p=0;p<5&&p<bars;p++){
    uint8_t v=(p+1<bars)?cfg.disconnectedBrightness:beatsin8(8,100,cfg.disconnectedBrightness);
    CRGB c=(p<2)?CRGB(0,v>>1,v):(p<4)?CRGB(v>>1,v,0):CRGB(v,v>>2,0);
    blendLed(SHOT_PAIRS[p][0],c,255); blendLed(SHOT_PAIRS[p][1],c,255);
  }
}
static void renderRadarSweep(uint32_t now){
  float rate=spdRate(cfg.disconnectedSpeed,0.0003f,0.003f);
  float angle=fmodf((float)now*rate,TWO_PI),cs=cosf(angle),sn=sinf(angle);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float dx=xyMap[i].x-1.f,dy=xyMap[i].y-3.f,dist=fabsf(dx*(-sn)+dy*cs);
    if(dist<0.5f) blendLed(i,CHSV((uint8_t)(angle/TWO_PI*255.f),255,(uint8_t)((1.f-dist/0.5f)*cfg.disconnectedBrightness)),220);
    else leds[i]=sc(CRGB(0,20,10),80);
  }
}
static void renderDisconnectedBase(uint32_t now){
  switch(cfg.disconnectedMode){
    case DISC_RAINBOW:      renderRainbowWash(now);   break;
    case DISC_SEARCH_SWEEP: renderSearchSweep(now);   break;
    case DISC_WIFI_PING:    renderWifiPing(now);      break;
    case DISC_AURORA:       renderAurora(now);        break;
    case DISC_COMET:        renderWaitingComet(now);  break;
    case DISC_SIGNAL_BARS:  renderSignalBars(now);    break;
    case DISC_RADAR:        renderRadarSweep(now);    break;
    default:                renderRainbowWash(now);   break;
  }
}

// ============================================================
// TEAM IDLE ANIMATIONS
// ============================================================
static void renderFlatBreath(uint32_t now){
  uint8_t bpm=8+(cfg.teamBreathSpeed/6);
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  uint8_t mx=max((uint8_t)(mn+1),cfg.teamBreathMaxBrightness);
  fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),beatsin8(bpm,mn,mx)));
}
static void renderDiagonalBreath(uint32_t now,bool rotate){
  uint8_t bpm=8+(cfg.teamBreathSpeed/6);
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  uint8_t mx=max((uint8_t)(mn+1),cfg.teamBreathMaxBrightness);
  CRGB tc=teamColor();
  const float cx=(xyMap[4].x+xyMap[13].x)*0.5f, cy=(xyMap[4].y+xyMap[13].y)*0.5f;
  float angle=rotate?fmodf((float)now*(0.00015f+(float)cfg.teamBreathRotationSpeed*0.000006f),TWO_PI)
                     :(float)cfg.teamBreathAngle/255.f*TWO_PI;
  float cs=cosf(angle),sn=sinf(angle);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float proj=(xyMap[i].x-cx)*cs+(xyMap[i].y-cy)*sn;
    leds[i]=sc(tc,beatsin8(bpm,mn,mx,0,(uint8_t)(proj*92.f)));
  }
}
static void renderCometOrbit(uint32_t now){
  CRGB tc=teamColor();
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  float rate=spdRate(cfg.teamBreathSpeed,0.0003f,0.003f);
  float pos=fmodf((float)now*rate,1.f);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float d=fabsf(spatialPos(i)-pos); if(d>0.5f)d=1.f-d;
    leds[i]=sc(tc,(d<0.3f)?max(mn,(uint8_t)((1.f-d/0.3f)*cfg.teamBreathMaxBrightness)):mn);
  }
}
static void renderTeamSparkle(uint32_t now){
  CRGB tc=teamColor();
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  static uint32_t spkUntil[MOHAWK_LED_COUNT]={};
  fill_solid(leds,MOHAWK_LED_COUNT,sc(tc,mn));
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    if(random8()<6)spkUntil[i]=now+random16(200,700);
    if(now<spkUntil[i]) leds[i]=sc(tc,max(mn,(uint8_t)(min((uint32_t)500,spkUntil[i]-now)*cfg.teamBreathMaxBrightness/500)));
  }
}
static void renderPowerCore(uint32_t now){
  CRGB tc=teamColor();
  uint8_t bpm=8+(cfg.teamBreathSpeed/6);
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  uint8_t wave=beatsin8(bpm,mn,cfg.teamBreathMaxBrightness);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float dist=fabsf(spatialPos(i)-0.47f)*2.f, fade=max(0.3f,1.f-dist*0.7f);
    leds[i]=sc(tc,max(mn,(uint8_t)(wave*fade)));
  }
}
static void renderRailGlow(uint32_t now){
  CRGB tc=teamColor();
  uint8_t bpm=6+(cfg.teamBreathSpeed/8);
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  uint8_t mx=max((uint8_t)(mn+1),cfg.teamBreathMaxBrightness);
  uint8_t wL=beatsin8(bpm,mn,mx,0,0), wR=beatsin8(bpm,mn,mx,0,128);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)
    leds[i]=sc(tc,(xyMap[i].x<0.5f)?wL:(xyMap[i].x>1.5f)?wR:mn);
}
static void renderShield(uint32_t now){
  CRGB tc=teamColor();
  uint8_t mn=min(cfg.teamBreathMinBrightness,(uint8_t)(cfg.teamBreathMaxBrightness-1));
  uint8_t mx=max((uint8_t)(mn+1),cfg.teamBreathMaxBrightness);
  uint8_t wave=beatsin8(10+(cfg.teamBreathSpeed/5),0,255);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float dx=xyMap[i].x-1.f,dy=xyMap[i].y-3.f;
    float norm=sqrtf(dx*dx+dy*dy)/4.f;
    float pat=sinf(norm*3.14f-(float)wave/255.f*TWO_PI)*0.5f+0.5f;
    leds[i]=sc(tc,(uint8_t)(mn+(mx-mn)*pat));
  }
}
static void renderTeamIdleBase(uint32_t now){
  switch(cfg.teamIdleMode){
    case IDLE_FLAT_BREATH:   renderFlatBreath(now);          break;
    case IDLE_DIAGONAL:      renderDiagonalBreath(now,false);break;
    case IDLE_ROTATING_DIAG: renderDiagonalBreath(now,true); break;
    case IDLE_COMET_ORBIT:   renderCometOrbit(now);          break;
    case IDLE_SPARKLE:       renderTeamSparkle(now);         break;
    case IDLE_POWER_CORE:    renderPowerCore(now);           break;
    case IDLE_RAIL_GLOW:     renderRailGlow(now);            break;
    case IDLE_SHIELD:        renderShield(now);              break;
    default:                 renderFlatBreath(now);          break;
  }
}

// ============================================================
// CHARGE OVERLAYS
// ============================================================
static void renderBottomCoreCharge(uint32_t now){
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  uint8_t p=beatsin8(18+(cfg.chargeSpeed/4),70,255);
  blendLed(8,c,p);blendLed(9,c,p);blendLed(7,c,scale8(p,120));blendLed(10,c,scale8(p,120));
  blendLed(6,c,scale8(p,70));blendLed(11,c,scale8(p,70));
}
static void renderRailGatherCharge(uint32_t now){
  float rate=spdRate(cfg.chargeSpeed,0.00018f,0.00220f);
  float phase=fmodf((float)(now-rt.chargeStartMs)*rate,1.f);
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  float headY=7.f*(1.f-phase), railW=0.3f+(float)cfg.chargePixels/18.f*5.5f;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    if(i==8||i==9){blendLed(i,c,beatsin8(20,120,255));continue;}
    float d=fabsf(xyMap[i].y-headY);
    if(d<railW)blendLed(i,c,(uint8_t)((1.f-d/railW)*255.f));
  }
}
static void renderCapFillCharge(uint32_t now){
  float rate=spdRate(cfg.chargeSpeed,0.00018f,0.00220f);
  float phase=fmodf((float)(now-rt.chargeStartMs)*rate,1.f);
  float width=constrain((float)cfg.chargePixels/(float)MOHAWK_LED_COUNT,0.05f,0.75f);
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float pos=spatialPos(i);
    if(pos<=phase)blendLed(i,c,210);
    else if(pos<phase+width)blendLed(i,c,(uint8_t)((1.f-(pos-phase)/width)*180.f));
  }
}
static void renderScannerArmCharge(uint32_t now){
  float rate=spdRate(cfg.chargeSpeed,0.00018f,0.00220f);
  float phase=fmodf((float)(now-rt.chargeStartMs)*rate,1.f);
  float angle=phase*TWO_PI,cs=cosf(angle),sn=sinf(angle);
  float lineW=0.18f+(float)cfg.chargePixels/18.f*1.6f;
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){
    float dx=xyMap[i].x-1.f,dy=xyMap[i].y-3.f,dist=fabsf(dx*(-sn)+dy*cs);
    if(dist<lineW)blendLed(i,c,(uint8_t)((1.f-dist/lineW)*220.f));
  }
}
static void renderPlasmaCoreCharge(uint32_t now){
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  uint8_t p=beatsin8(28+(cfg.chargeSpeed/3),90,255);
  blendLed(8,c,p);blendLed(9,c,p);blendLed(7,c,scale8(p,130));blendLed(10,c,scale8(p,130));
  blendLed(6,c,scale8(p,100));blendLed(11,c,scale8(p,100));blendLed(5,c,scale8(p,60));blendLed(12,c,scale8(p,60));
}
static void renderVoltageRampCharge(uint32_t now){
  float rate=spdRate(cfg.chargeSpeed,0.00025f,0.003f);
  float fill=fmodf((float)(now-rt.chargeStartMs)*rate,1.f);
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  float fillY=7.f*fill;
  for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)
    if(xyMap[i].y<=fillY)blendLed(i,c,(uint8_t)(120.f+135.f*(1.f-xyMap[i].y/7.f)));
}
static void renderMagPairCharge(uint32_t now){
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  for(uint8_t p=0;p<5;p++){
    uint8_t pulse=beatsin8(15+(cfg.chargeSpeed/6),60,255,0,p*51);
    blendLed(SHOT_PAIRS[p][0],c,pulse); blendLed(SHOT_PAIRS[p][1],c,pulse);
  }
}
static void renderLockOnCharge(uint32_t now){
  float charge=constrain((float)(now-rt.chargeStartMs)/3000.f,0.f,1.f);
  CRGB c=sc(u32c(cfg.chargeColor),cfg.chargeBrightness);
  uint8_t pLit=(uint8_t)(charge*5.f+0.5f), pulse=beatsin8(20+(cfg.chargeSpeed/5),100,255);
  for(uint8_t p=0;p<5&&p<pLit;p++){
    uint8_t a=(p==pLit-1)?pulse:220;
    blendLed(SHOT_PAIRS[p][0],c,a); blendLed(SHOT_PAIRS[p][1],c,a);
  }
}
static void renderChargeOverlay(uint32_t now){
  if(!rt.chargeActive)return;
  if(rt.cooldownUntilMs>0&&now<rt.cooldownUntilMs)return;
  switch(cfg.chargeMode){
    case CHARGE_BOTTOM_CORE:  renderBottomCoreCharge(now);  break;
    case CHARGE_RAIL_GATHER:  renderRailGatherCharge(now);  break;
    case CHARGE_CAP_FILL:     renderCapFillCharge(now);     break;
    case CHARGE_SCANNER_ARM:  renderScannerArmCharge(now);  break;
    case CHARGE_PLASMA_CORE:  renderPlasmaCoreCharge(now);  break;
    case CHARGE_VOLTAGE_RAMP: renderVoltageRampCharge(now); break;
    case CHARGE_MAG_PAIR:     renderMagPairCharge(now);     break;
    case CHARGE_LOCK_ON:      renderLockOnCharge(now);      break;
    default:                  renderBottomCoreCharge(now);  break;
  }
}

// ============================================================
// SHOT METER + COOLDOWN
// ============================================================
static bool partialDecayActive(uint32_t now)
{
  if (rt.partialDecayStartCount == 0) return false;
  const uint32_t totalMs = (uint32_t)rt.partialDecayStartCount * SHOT_PARTIAL_DECAY_STEP_MS;
  if ((now - rt.partialDecayStartMs) >= totalMs)
  {
    rt.partialDecayStartCount = 0;
    rt.partialDecayStartMs = 0;
    rt.burstShotCount = 0;
    rt.burstWindowStartMs = 0;
    rt.shotMeterHoldUntilMs = 0;
    return false;
  }
  return true;
}

static void beginPartialShotDecay(uint32_t now)
{
  if (rt.burstShotCount == 0) return;
  rt.partialDecayStartCount = min((uint8_t)5, rt.burstShotCount);
  rt.partialDecayStartMs = now;
  rt.burstShotCount = 0;
  rt.burstWindowStartMs = 0;
  rt.shotMeterHoldUntilMs = now + ((uint32_t)rt.partialDecayStartCount * SHOT_PARTIAL_DECAY_STEP_MS);
  logEvent(0, "Partial shot meter decay");
}

static void renderPartialShotDecay(uint32_t now)
{
  if (!partialDecayActive(now)) return;

  const uint32_t elapsed = now - rt.partialDecayStartMs;
  const uint8_t stepsCleared = min((uint8_t)rt.partialDecayStartCount, (uint8_t)(elapsed / SHOT_PARTIAL_DECAY_STEP_MS));
  uint8_t visibleCount = rt.partialDecayStartCount - stepsCleared;
  if (visibleCount == 0) return;

  const uint32_t localMs = elapsed % SHOT_PARTIAL_DECAY_STEP_MS;
  const uint8_t fadeOut = (uint8_t)map(localMs, 0, SHOT_PARTIAL_DECAY_STEP_MS - 1, 255, 40);
  const uint8_t newestVisiblePair = visibleCount - 1;

  for (uint8_t p = 0; p < visibleCount; p++)
  {
    CRGB pc = sc(u32c(cfg.shotPairColors[p]), cfg.chargeBrightness);
    uint8_t a = (p == newestVisiblePair) ? fadeOut : 235;
    blendLed(SHOT_PAIRS[p][0], pc, a);
    blendLed(SHOT_PAIRS[p][1], pc, a);
  }
}

static void renderCooldownLayer(uint32_t now){
  uint32_t cdMs=max((uint16_t)1,cfg.cooldownMs);
  uint32_t cdStart=(rt.cooldownUntilMs>cdMs)?(rt.cooldownUntilMs-cdMs):now;
  uint32_t el=now-cdStart;
  float prog=constrain((float)el/(float)cdMs,0.f,1.f);

  // Chirp blink: instantaneous frequency sweeps linearly fSlow->fFast over the cooldown.
  // Phase = integral of f(t): phi(t) = fSlow*t + (fFast-fSlow)*t^2/(2*T)
  // This gives perfectly smooth acceleration with no per-frame state.
  const float fSlow=0.9f, fFast=16.0f;
  float tSec=(float)el*0.001f, TSec=(float)cdMs*0.001f;
  float bPhase=fSlow*tSec+(fFast-fSlow)*tSec*tSec/(2.f*max(TSec,0.001f));
  float sinVal=sinf(bPhase*TWO_PI)*0.5f+0.5f;
  // Never fully dark -- floor at ~12% so color is always readable
  uint8_t blinkBrt=max((uint8_t)(cfg.cooldownBrightness>>3),
                       (uint8_t)(sinVal*(float)cfg.cooldownBrightness));

  uint8_t total=constrain(cfg.maxBurstShots,(uint8_t)1,(uint8_t)5);
  CRGB red=sc(CRGB(255,0,0),blinkBrt), tc=teamColor();
  switch(cfg.cooldownMode){
    case COOL_REVERSE_PAIR: default:{
      uint8_t ret=(uint8_t)floorf(prog*(float)(total+0.001f));
      if(ret>total)ret=total; uint8_t rem=total-ret;
      for(uint8_t p=0;p<rem;p++){leds[SHOT_PAIRS[p][0]]=red;leds[SHOT_PAIRS[p][1]]=red;}
      break;}
    case COOL_TEAM_RECLAIM:{
      CRGB c=sc(blend(CRGB(255,0,0),tc,(uint8_t)(prog*255.f)),blinkBrt);
      for(uint8_t p=0;p<total;p++){leds[SHOT_PAIRS[p][0]]=c;leds[SHOT_PAIRS[p][1]]=c;} break;}
    case COOL_SMOOTH_RETURN:{
      float ease=prog*prog*(3.f-2.f*prog);
      CRGB c=sc(blend(CRGB(255,0,0),tc,(uint8_t)(ease*255.f)),blinkBrt);
      for(uint8_t p=0;p<total;p++){leds[SHOT_PAIRS[p][0]]=c;leds[SHOT_PAIRS[p][1]]=c;} break;}
    case COOL_MINIMAL_FADE:{
      // Fade amplitude also drops as blink speeds up -- dying energy effect
      uint8_t fadeBrt=scale8(blinkBrt,(uint8_t)((1.f-prog)*255.f));
      CRGB c=sc(CRGB(255,0,0),max((uint8_t)(cfg.cooldownBrightness>>3),fadeBrt));
      for(uint8_t p=0;p<total;p++){leds[SHOT_PAIRS[p][0]]=c;leds[SHOT_PAIRS[p][1]]=c;} break;}
    case COOL_HEAT_DISSOLVE:{
      uint8_t hphase=(uint8_t)(prog*255.f);
      CRGB base=(hphase<128)?blend(CRGB(255,0,0),CRGB(255,80,0),(uint8_t)(hphase*2))
                             :blend(CRGB(255,80,0),tc,(uint8_t)((hphase-128)*2));
      for(uint8_t p=0;p<total;p++){leds[SHOT_PAIRS[p][0]]=sc(base,blinkBrt);leds[SHOT_PAIRS[p][1]]=sc(base,blinkBrt);} break;}
  }
}
static void renderShotMeterOverlay(uint32_t now){
  if(rt.cooldownUntilMs>0&&now>=rt.cooldownUntilMs){mohawk_reset_shot_meter();return;}
  const bool inCd=(rt.cooldownUntilMs>0&&now<rt.cooldownUntilMs);

  if(inCd){renderCooldownLayer(now);return;}

  // If the user fired fewer than maxBurstShots and then stopped spamming,
  // reverse-clear the meter instead of snapping it off.
  if(rt.burstShotCount>0&&rt.shotMeterHoldUntilMs>0&&now>rt.shotMeterHoldUntilMs){
    beginPartialShotDecay(now);
  }

  if(partialDecayActive(now)){
    renderPartialShotDecay(now);
    return;
  }

  if(rt.burstShotCount==0&&now>rt.shotMeterHoldUntilMs)return;

  uint8_t filled=min((uint8_t)5,rt.burstShotCount), livePulse=beatsin8(14,175,255);
  for(uint8_t p=0;p<filled;p++){
    CRGB pc=sc(u32c(cfg.shotPairColors[p]),cfg.chargeBrightness);
    uint8_t a=(p==filled-1)?livePulse:245;
    blendLed(SHOT_PAIRS[p][0],pc,a); blendLed(SHOT_PAIRS[p][1],pc,a);
  }
}

// ============================================================
// FIRE OVERLAYS
// ============================================================
static void renderFireOverlay(uint32_t now){
  if(!rt.fireActive)return;
  if(rt.cooldownUntilMs>0&&now<rt.cooldownUntilMs){rt.fireActive=false;return;}
  uint16_t dur=(uint16_t)map(constrain(cfg.fireSpeed,(uint8_t)1,(uint8_t)100),1,100,1200,180);
  uint32_t el=now-rt.fireStartMs;
  if(el>=dur){rt.fireActive=false;return;}
  float prog=(float)el/(float)dur;
  float width=(float)cfg.firePixels/(float)MOHAWK_LED_COUNT;
  CRGB c=sc(u32c(cfg.fireColor),cfg.fireBrightness);
  switch(cfg.fireMode){
    case FIRE_DUAL_RAIL:
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float sp=constrain(xyMap[i].y/7.f,0.f,1.f),d=fabsf(sp-prog);
        if(d<width)blendLed(i,c,255);else if(d<width+0.18f)blendLed(i,c,(uint8_t)((1.f-(d-width)/0.18f)*cfg.fireFade));} break;
    case FIRE_FORWARD_WAVE:
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float d=fabsf(spatialPos(i)-prog);
        if(d<width)blendLed(i,c,255);else if(d<width+0.20f)blendLed(i,c,(uint8_t)((1.f-(d-width)/0.20f)*cfg.fireFade));} break;
    case FIRE_MUZZLE_BURST:{
      uint8_t a=(uint8_t)((1.f-prog)*255.f);
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)blendLed(i,c,scale8(a,70));
      blendLed(0,c,a);blendLed(17,c,a);blendLed(1,c,scale8(a,220));blendLed(16,c,scale8(a,220));
      blendLed(2,c,scale8(a,160));blendLed(15,c,scale8(a,160));break;}
    case FIRE_RECOIL_SHOCK:{float head=1.f-prog;
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float d=fabsf(spatialPos(i)-head);
        if(d<width)blendLed(i,c,255);else if(d<width+0.20f)blendLed(i,c,(uint8_t)((1.f-(d-width)/0.20f)*cfg.fireFade));} break;}
    case FIRE_PLASMA_LANCE:
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float sp=spatialPos(i);if(sp<=prog){float it=1.f-(prog-sp)*3.f;if(it>0)blendLed(i,blend(c,sc(CRGB::White,cfg.fireBrightness),(uint8_t)(it*200.f)),(uint8_t)(it*255.f));}} break;
    case FIRE_LIGHTNING_FORK:
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float d=fabsf(spatialPos(i)-prog);
        if(d<width)blendLed(i,c,255);else if(d<width+0.25f&&random8()<190)blendLed(i,c,random8(50,200));} break;
    case FIRE_SONIC_BOOM:{float w1=prog*0.5f,w2=prog*0.3f;
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float sp=spatialPos(i),d1=fabsf(fabsf(sp-0.5f)-w1),d2=fabsf(fabsf(sp-0.5f)-w2),d=min(d1,d2);
        if(d<width)blendLed(i,c,255);else if(d<width+0.15f)blendLed(i,c,(uint8_t)((1.f-(d-width)/0.15f)*cfg.fireFade));} break;}
    case FIRE_SPARKLE_BLAST:{uint8_t dec=(uint8_t)((1.f-prog)*255.f),thr=(uint8_t)map(dec,0,255,15,170);
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)if(random8()<thr)blendLed(i,c,random8(80,255));break;}
    case FIRE_TEAM_IMPACT:{CRGB mix=blend(c,teamColor(),(uint8_t)(prog*255.f));
      uint8_t bv=(uint8_t)((1.f-prog*0.8f)*255.f);for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)blendLed(i,sc(mix,bv),240);break;}
    case FIRE_CLEAN_FLASH:{uint8_t a=(uint8_t)((1.f-prog)*(1.f-prog)*255.f);
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)blendLed(i,sc(CRGB::White,scale8(a,cfg.fireBrightness)),a);break;}
    case FIRE_HYPERDRIVE:{float tP=prog*2.f;
      for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float dist=fabsf(spatialPos(i)-0.5f)*2.f,d=fabsf(dist-tP);
        if(d<width)blendLed(i,c,255);else if(d<width+0.15f)blendLed(i,c,(uint8_t)((1.f-(d-width)/0.15f)*cfg.fireFade));} break;}
    case FIRE_ENERGY_KICK:{uint8_t dec=(uint8_t)((1.f-prog)*255.f);
      for(uint8_t p=0;p<5;p++){blendLed(SHOT_PAIRS[p][0],c,scale8(dec,200+p*10));blendLed(SHOT_PAIRS[p][1],c,scale8(dec,200+p*10));}break;}
    default:for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float d=fabsf(spatialPos(i)-prog);if(d<width)blendLed(i,c,255);}break;
  }
}

// ============================================================
// COUNTDOWN OVERLAY
// ============================================================
static void renderCountdownOverlay(uint32_t now){
  if(!rt.countdownActive)return;
  uint32_t durMs=(uint32_t)cfg.countdownSeconds*1000UL, el=now-rt.countdownStartMs;
  if(el>=durMs){rt.countdownActive=false;fill_solid(leds,MOHAWK_LED_COUNT,sc(u32c(cfg.countdownColorB),cfg.countdownBrightness));return;}
  uint8_t secLeft=cfg.countdownSeconds-(uint8_t)(el/1000);
  float prog=(float)el/(float)durMs;
  CRGB cA=sc(u32c(cfg.countdownColorA),cfg.countdownBrightness), cB=sc(u32c(cfg.countdownColorB),cfg.countdownBrightness);
  uint16_t tickP=(uint16_t)map(constrain(cfg.countdownSpeed,(uint8_t)1,(uint8_t)100),1,100,1400,300);
  uint8_t tickPulse=sin8((uint8_t)((el%tickP)*255/tickP));
  switch(cfg.countdownMode){
    case CDN_U_DRAIN: default:
      fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),18));
      {uint8_t cnt=(uint8_t)((1.f-prog)*MOHAWK_LED_COUNT);for(uint8_t s=0;s<cnt;s++)blendLed(ledAtStep(s),cA,220);} break;
    case CDN_TRAFFIC_BUILD:
      fill_solid(leds,MOHAWK_LED_COUNT,CRGB::Black);
      {CRGB tc=(secLeft<=3)?cB:(secLeft<=6)?cA:sc(CRGB(255,0,0),cfg.countdownBrightness);
       uint8_t fillN=(uint8_t)map(cfg.countdownSeconds-secLeft+1,0,cfg.countdownSeconds,1,MOHAWK_LED_COUNT);
       for(uint8_t s=0;s<fillN&&s<MOHAWK_LED_COUNT;s++)blendLed(ledAtStep(s),tc,240);
       blendLed(8,sc(CRGB::White,cfg.countdownBrightness),scale8(tickPulse,160));
       blendLed(9,sc(CRGB::White,cfg.countdownBrightness),scale8(tickPulse,160));} break;
    case CDN_STADIUM_PULSE:
      {CRGB pc=(secLeft<=1)?cB:cA;fill_solid(leds,MOHAWK_LED_COUNT,sc(pc,(uint8_t)map(tickPulse,0,255,45,cfg.countdownBrightness)));} break;
    case CDN_COMET_TICK:
      fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),18));
      {uint8_t head=(uint8_t)(((el%1000)/1000.f)*MOHAWK_LED_COUNT);
       for(int8_t o=-3;o<=3;o++){uint8_t step=(uint8_t)((head+o+MOHAWK_LED_COUNT)%MOHAWK_LED_COUNT);
         blendLed(ledAtStep(step),cA,255-min(220,abs(o)*58));}} break;
    case CDN_SIDE_LAUNCH:
      {fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),22));
       float p=(el%1000)/1000.f; CRGB lc=(secLeft<=2)?cB:cA;
       for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++){float sp=spatialPos(i);
         if(fabsf(sp-p)<0.10f||fabsf(sp-(1.f-p))<0.10f)blendLed(i,lc,240);}} break;
    case CDN_GATE_OPEN:
      {fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),15));
       for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)if(fabsf(spatialPos(i)-0.5f)*2.f>=prog)blendLed(i,cA,220);} break;
    case CDN_ENERGY_BUILD:
      {float fillY=7.f*(1.f-prog);
       for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++)if(xyMap[i].y>=fillY)blendLed(i,cA,(uint8_t)(80.f+175.f*((xyMap[i].y-fillY)/7.f)));} break;
    case CDN_PULSE_STACK:
      {fill_solid(leds,MOHAWK_LED_COUNT,sc(teamColor(),15));
       uint8_t pLit=min((uint8_t)5,(uint8_t)(cfg.countdownSeconds-secLeft+1));
       for(uint8_t p=0;p<pLit;p++){uint8_t pp=beatsin8(8+p*3,80,cfg.countdownBrightness,0,p*50);
         CRGB pc=(p>=3)?sc(cB,pp):sc(cA,pp);blendLed(SHOT_PAIRS[p][0],pc,240);blendLed(SHOT_PAIRS[p][1],pc,240);}} break;
    case CDN_REFEREE:
      {bool lOn=(secLeft%2)==0; uint8_t bv=scale8(tickPulse,cfg.countdownBrightness);
       for(uint8_t i=0;i<MOHAWK_LED_COUNT;i++) leds[i]=((xyMap[i].x<0.5f)==lOn)?sc(cA,bv):sc(cB,bv>>1);} break;
  }
}

// ============================================================
// LOW BATTERY (always last) + IDENTIFY
// ============================================================
static void renderLowBatteryOverlay(uint32_t now){
  if(!rt.lowBattery)return;
  switch(cfg.lowBatteryMode){
    case LOWBAT_RED_FLASH: default:{bool on=((now/220)%2)==0;CRGB w=on?sc(CRGB(255,0,0),cfg.lowBatteryBrightness):CRGB(22,0,0);leds[8]=w;leds[9]=w;break;}
    case LOWBAT_DOUBLE_BLINK:{uint32_t t=now%800;bool on=(t<80)||(t>170&&t<260);CRGB w=on?sc(CRGB(255,0,0),cfg.lowBatteryBrightness):CRGB(8,0,0);leds[8]=w;leds[9]=w;break;}
    case LOWBAT_HEARTBEAT:{uint32_t t=now%1200;float pk;
      if(t<100)pk=(float)t/100.f;else if(t<200)pk=1.f-(float)(t-100)/100.f;
      else if(t<320)pk=(float)(t-200)/120.f*0.6f;else if(t<430)pk=0.6f-(float)(t-320)/110.f*0.6f;else pk=0.f;
      uint8_t v=(uint8_t)(pk*cfg.lowBatteryBrightness);leds[8]=CRGB(v,0,0);leds[9]=CRGB(v,0,0);break;}
    case LOWBAT_EMBER:{uint8_t e=beatsin8(4,40,cfg.lowBatteryBrightness);leds[8]=CRGB(e,0,0);leds[9]=CRGB(e,0,0);break;}
    case LOWBAT_VOLTAGE_TIER:{uint8_t v=cfg.lowBatteryBrightness;leds[8]=CRGB(v,0,0);leds[9]=CRGB(v,0,0);
      if((now/500)%2==0){leds[SHOT_PAIRS[0][0]]=CRGB(v>>1,0,0);leds[SHOT_PAIRS[0][1]]=CRGB(v>>1,0,0);}break;}
    case LOWBAT_SERVICE:{bool alt=((now/700)%2)==0;CRGB w=alt?CRGB(cfg.lowBatteryBrightness,cfg.lowBatteryBrightness/4,0):CRGB(cfg.lowBatteryBrightness,0,0);leds[8]=w;leds[9]=w;break;}
    case LOWBAT_SILENT:{uint8_t dim=cfg.lowBatteryBrightness>>3;leds[8]=CRGB(dim,0,0);leds[9]=CRGB(dim,0,0);break;}
  }
}
static void renderIdentifyOverlay(uint32_t now){
  if(rt.identifyLed<0||rt.identifyLed>=MOHAWK_LED_COUNT)return;
  if(now>=rt.identifyUntilMs){rt.identifyLed=-1;return;}
  leds[rt.identifyLed]=((now/80)%2)==0?CRGB::White:CRGB::Black;
}

// ============================================================
// MAIN TICK + SERVICE
// ============================================================
static void mohawk_tick(uint32_t now){
#if MOHAWK_PHYSICAL_LEDS_ENABLED
  FastLED.setBrightness(cfg.globalBrightness);
#endif
  fill_solid(leds,MOHAWK_LED_COUNT,CRGB::Black);
  if(!rt.connected) renderDisconnectedBase(now); else renderTeamIdleBase(now);
  renderChargeOverlay(now);
  renderShotMeterOverlay(now);
  renderFireOverlay(now);
  renderCountdownOverlay(now);
  renderIdentifyOverlay(now);
  renderLowBatteryOverlay(now); // ALWAYS LAST
#if MOHAWK_PHYSICAL_LEDS_ENABLED
  FastLED.show();
#endif
}
void mohawk_service(){
  static uint32_t lastMs=0; uint32_t now=millis();
  if(now-lastMs>=LED_UPDATE_MS){lastMs=now;serviceSolenoidPulse(now);serviceSelfTest(now);mohawk_tick(now);}
}

// ============================================================
// JSON BUILDERS
// ============================================================
static String buildTelemetryJson(){
  const uint32_t now=millis();
  const uint32_t cdRem=(now<rt.cooldownUntilMs)?(rt.cooldownUntilMs-now):0;
  const uint32_t cntDur=(uint32_t)cfg.countdownSeconds*1000UL;
  const uint32_t cntEl=rt.countdownActive?(now-rt.countdownStartMs):0;
  const uint32_t cntRem=(rt.countdownActive&&cntDur>cntEl)?(cntDur-cntEl):0;
  const uint32_t chgEl=rt.chargeActive?(now-rt.chargeStartMs):0;
  const bool decayAct=(rt.partialDecayStartCount>0);
  const uint32_t decayTotal=decayAct?((uint32_t)rt.partialDecayStartCount*SHOT_PARTIAL_DECAY_STEP_MS):0;
  const uint32_t decayEl=decayAct?min(now-rt.partialDecayStartMs,decayTotal):0;
  const uint32_t decayRem=decayAct?(decayTotal-decayEl):0;
  String s; s.reserve(700);
  s+=F("{\"t\":"); s+=now;
  s+=F(",\"standalone\":"); s+=MOHAWK_STANDALONE_TEST_MODE?F("true"):F("false");
  s+=F(",\"physLeds\":"); s+=MOHAWK_PHYSICAL_LEDS_ENABLED?F("true"):F("false");
  s+=F(",\"conn\":"); s+=rt.connected?F("true"):F("false");
  s+=F(",\"lowBat\":"); s+=rt.lowBattery?F("true"):F("false");
  s+=F(",\"team\":"); s+=(int)cfg.team;
  s+=F(",\"discMode\":"); s+=(int)cfg.disconnectedMode;
  s+=F(",\"idleMode\":"); s+=(int)cfg.teamIdleMode;
  s+=F(",\"chgAct\":"); s+=rt.chargeActive?F("true"):F("false");
  s+=F(",\"chgElMs\":"); s+=chgEl;
  s+=F(",\"chgMode\":"); s+=(int)cfg.chargeMode;
  s+=F(",\"fireAct\":"); s+=rt.fireActive?F("true"):F("false");
  s+=F(",\"fireMode\":"); s+=(int)cfg.fireMode;
  s+=F(",\"fireSeq\":"); s+=rt.fireEventSeq;
  s+=F(",\"burst\":"); s+=(int)rt.burstShotCount;
  s+=F(",\"maxShots\":"); s+=(int)cfg.maxBurstShots;
  s+=F(",\"decayAct\":"); s+=decayAct?F("true"):F("false");
  s+=F(",\"decayCount\":"); s+=(int)rt.partialDecayStartCount;
  s+=F(",\"decayRem\":"); s+=decayRem;
  s+=F(",\"decayStepMs\":"); s+=(int)SHOT_PARTIAL_DECAY_STEP_MS;
  s+=F(",\"cdRem\":"); s+=cdRem;
  s+=F(",\"cdMs\":"); s+=(int)cfg.cooldownMs;
  s+=F(",\"cdMode\":"); s+=(int)cfg.cooldownMode;
  s+=F(",\"cntAct\":"); s+=rt.countdownActive?F("true"):F("false");
  s+=F(",\"cntRem\":"); s+=cntRem;
  s+=F(",\"cntSec\":"); s+=(int)cfg.countdownSeconds;
  s+=F(",\"totalShots\":"); s+=rt.totalShotsFired;
  s+=F(",\"selfTest\":"); s+=(gSTStep!=ST_IDLE)?F("true"):F("false");
  s+=F(",\"selfTestStep\":"); s+=(int)gSTStep;
  s+=F(",\"solArmed\":"); s+=solArmed?F("true"):F("false");
  s+=F(",\"identifyLed\":"); s+=rt.identifyLed;
  s+=F(",\"ver\":200}");
  return s;
}

static String buildConfigJson(){
  String s; s.reserve(1200);
  s+=F("{\"globalBrightness\":"); s+=cfg.globalBrightness;
  s+=F(",\"disconnectedMode\":"); s+=(int)cfg.disconnectedMode;
  s+=F(",\"disconnectedSpeed\":"); s+=cfg.disconnectedSpeed;
  s+=F(",\"disconnectedBrightness\":"); s+=cfg.disconnectedBrightness;
  s+=F(",\"team\":"); s+=(int)cfg.team;
  s+=F(",\"teamBlue\":\""); s+=hexStr(cfg.teamBlue); s+='"';
  s+=F(",\"teamOrange\":\""); s+=hexStr(cfg.teamOrange); s+='"';
  s+=F(",\"teamIdleMode\":"); s+=(int)cfg.teamIdleMode;
  s+=F(",\"teamBreathSpeed\":"); s+=cfg.teamBreathSpeed;
  s+=F(",\"teamBreathMinBrightness\":"); s+=cfg.teamBreathMinBrightness;
  s+=F(",\"teamBreathMaxBrightness\":"); s+=cfg.teamBreathMaxBrightness;
  s+=F(",\"teamBreathDiagonal\":"); s+=cfg.teamBreathDiagonal?F("true"):F("false");
  s+=F(",\"teamBreathRotate\":"); s+=cfg.teamBreathRotate?F("true"):F("false");
  s+=F(",\"teamBreathAngle\":"); s+=cfg.teamBreathAngle;
  s+=F(",\"teamBreathRotationSpeed\":"); s+=cfg.teamBreathRotationSpeed;
  s+=F(",\"chargeMode\":"); s+=(int)cfg.chargeMode;
  s+=F(",\"chargeSpeed\":"); s+=cfg.chargeSpeed;
  s+=F(",\"chargePixels\":"); s+=cfg.chargePixels;
  s+=F(",\"chargeBrightness\":"); s+=cfg.chargeBrightness;
  s+=F(",\"chargeColor\":\""); s+=hexStr(cfg.chargeColor); s+='"';
  s+=F(",\"fireMode\":"); s+=(int)cfg.fireMode;
  s+=F(",\"fireSpeed\":"); s+=cfg.fireSpeed;
  s+=F(",\"firePixels\":"); s+=cfg.firePixels;
  s+=F(",\"fireFade\":"); s+=cfg.fireFade;
  s+=F(",\"fireBrightness\":"); s+=cfg.fireBrightness;
  s+=F(",\"fireColor\":\""); s+=hexStr(cfg.fireColor); s+='"';
  s+=F(",\"shotMeterMode\":"); s+=(int)cfg.shotMeterMode;
  s+=F(",\"maxBurstShots\":"); s+=cfg.maxBurstShots;
  s+=F(",\"burstWindowMs\":"); s+=cfg.burstWindowMs;
  s+=F(",\"cooldownMs\":"); s+=cfg.cooldownMs;
  s+=F(",\"cooldownMode\":"); s+=(int)cfg.cooldownMode;
  s+=F(",\"cooldownBrightness\":"); s+=cfg.cooldownBrightness;
  s+=F(",\"shotPairColors\":[");
  for(int i=0;i<5;i++){if(i)s+=',';s+='"';s+=hexStr(cfg.shotPairColors[i]);s+='"';}
  s+=F("],\"countdownMode\":"); s+=(int)cfg.countdownMode;
  s+=F(",\"countdownSeconds\":"); s+=cfg.countdownSeconds;
  s+=F(",\"countdownSpeed\":"); s+=cfg.countdownSpeed;
  s+=F(",\"countdownBrightness\":"); s+=cfg.countdownBrightness;
  s+=F(",\"countdownColorA\":\""); s+=hexStr(cfg.countdownColorA); s+='"';
  s+=F(",\"countdownColorB\":\""); s+=hexStr(cfg.countdownColorB); s+='"';
  s+=F(",\"lowBatteryMode\":"); s+=(int)cfg.lowBatteryMode;
  s+=F(",\"lowBatteryBrightness\":"); s+=cfg.lowBatteryBrightness;
  s+='}';
  return s;
}

static String buildDiagnosticsJson(){
  String s; s.reserve(800);
  s+=F("{\"uptime\":"); s+=millis();
  s+=F(",\"freeHeap\":"); s+=ESP.getFreeHeap();
  s+=F(",\"minFreeHeap\":"); s+=ESP.getMinFreeHeap();
  s+=F(",\"physLeds\":"); s+=MOHAWK_PHYSICAL_LEDS_ENABLED?F("true"):F("false");
  s+=F(",\"standalone\":"); s+=MOHAWK_STANDALONE_TEST_MODE?F("true"):F("false");
  s+=F(",\"fmsMode\":"); s+=MOHAWK_FMS_INTEGRATED_MODE?F("true"):F("false");
  s+=F(",\"solPin\":"); s+=(int)SOLENOID_TEST_PIN;
  s+=F(",\"solArmed\":"); s+=solArmed?F("true"):F("false");
  s+=F(",\"selfTestStep\":"); s+=(int)gSTStep;
#if MOHAWK_STANDALONE_TEST_MODE
  s+=F(",\"apIp\":\""); s+=WiFi.softAPIP().toString(); s+='"';
  s+=F(",\"ssid\":\""); s+=MOHAWK_AP_SSID; s+='"';
#endif
  s+=F(",\"fw\":\"2.0.0\"");
  s+=F(",\"evtLog\":[");
  int cnt=min(evtCount,20);
  int start=(evtHead-cnt+EVT_LOG_SIZE)%EVT_LOG_SIZE;
  for(int i=0;i<cnt;i++){
    if(i)s+=',';
    int idx=(start+i)%EVT_LOG_SIZE;
    s+=F("{\"ms\":"); s+=evtLog[idx].ms;
    s+=F(",\"sev\":"); s+=(int)evtLog[idx].sev;
    s+=F(",\"msg\":\""); s+=evtLog[idx].msg; s+=F("\"}");
  }
  s+=F("]}");
  return s;
}

static String buildHardwareLedsJson(){
  String s; s.reserve(300);
  s+='[';
  for(int i=0;i<MOHAWK_LED_COUNT;i++){
    if(i)s+=',';
    char buf[8]; snprintf(buf,sizeof(buf),"\"#%02X%02X%02X\"",leds[i].r,leds[i].g,leds[i].b);
    s+=buf;
  }
  s+=']';
  return s;
}

static String buildRecipeText(){
  String r; r.reserve(1800);
  r+="// Mohawk LED Controller v2.2 Recipe\n";
  r+="// Generated at uptime "; r+=millis(); r+="ms\n\n";
  r+="cfg.globalBrightness="; r+=cfg.globalBrightness; r+=";\n";
  r+="cfg.disconnectedMode="; r+=(int)cfg.disconnectedMode; r+=";\n";
  r+="cfg.disconnectedSpeed="; r+=cfg.disconnectedSpeed; r+=";\n";
  r+="cfg.disconnectedBrightness="; r+=cfg.disconnectedBrightness; r+=";\n";
  r+="cfg.team="; r+=(int)cfg.team; r+=";\n";
  r+="cfg.teamBlue="; r+=hexCode(cfg.teamBlue); r+=";\n";
  r+="cfg.teamOrange="; r+=hexCode(cfg.teamOrange); r+=";\n";
  r+="cfg.teamIdleMode="; r+=(int)cfg.teamIdleMode; r+=";\n";
  r+="cfg.teamBreathSpeed="; r+=cfg.teamBreathSpeed; r+=";\n";
  r+="cfg.teamBreathMinBrightness="; r+=cfg.teamBreathMinBrightness; r+=";\n";
  r+="cfg.teamBreathMaxBrightness="; r+=cfg.teamBreathMaxBrightness; r+=";\n";
  r+="cfg.teamBreathDiagonal="; r+=cfg.teamBreathDiagonal?1:0; r+=";\n";
  r+="cfg.teamBreathRotate="; r+=cfg.teamBreathRotate?1:0; r+=";\n";
  r+="cfg.teamBreathAngle="; r+=cfg.teamBreathAngle; r+=";\n";
  r+="cfg.teamBreathRotationSpeed="; r+=cfg.teamBreathRotationSpeed; r+=";\n";
  r+="cfg.chargeMode="; r+=(int)cfg.chargeMode; r+=";\n";
  r+="cfg.chargeSpeed="; r+=cfg.chargeSpeed; r+=";\n";
  r+="cfg.chargePixels="; r+=cfg.chargePixels; r+=";\n";
  r+="cfg.chargeBrightness="; r+=cfg.chargeBrightness; r+=";\n";
  r+="cfg.chargeColor="; r+=hexCode(cfg.chargeColor); r+=";\n";
  r+="cfg.fireMode="; r+=(int)cfg.fireMode; r+=";\n";
  r+="cfg.fireSpeed="; r+=cfg.fireSpeed; r+=";\n";
  r+="cfg.firePixels="; r+=cfg.firePixels; r+=";\n";
  r+="cfg.fireFade="; r+=cfg.fireFade; r+=";\n";
  r+="cfg.fireBrightness="; r+=cfg.fireBrightness; r+=";\n";
  r+="cfg.fireColor="; r+=hexCode(cfg.fireColor); r+=";\n";
  r+="cfg.shotMeterMode="; r+=(int)cfg.shotMeterMode; r+=";\n";
  r+="cfg.maxBurstShots="; r+=cfg.maxBurstShots; r+=";\n";
  r+="cfg.burstWindowMs="; r+=cfg.burstWindowMs; r+=";\n";
  r+="cfg.cooldownMs="; r+=cfg.cooldownMs; r+=";\n";
  r+="cfg.cooldownMode="; r+=(int)cfg.cooldownMode; r+=";\n";
  r+="cfg.cooldownBrightness="; r+=cfg.cooldownBrightness; r+=";\n";
  for(int i=0;i<5;i++){r+="cfg.shotPairColors["; r+=i; r+="]="; r+=hexCode(cfg.shotPairColors[i]); r+=";\n";}
  r+="cfg.countdownMode="; r+=(int)cfg.countdownMode; r+=";\n";
  r+="cfg.countdownSeconds="; r+=cfg.countdownSeconds; r+=";\n";
  r+="cfg.countdownSpeed="; r+=cfg.countdownSpeed; r+=";\n";
  r+="cfg.countdownBrightness="; r+=cfg.countdownBrightness; r+=";\n";
  r+="cfg.countdownColorA="; r+=hexCode(cfg.countdownColorA); r+=";\n";
  r+="cfg.countdownColorB="; r+=hexCode(cfg.countdownColorB); r+=";\n";
  r+="cfg.lowBatteryMode="; r+=(int)cfg.lowBatteryMode; r+=";\n";
  r+="cfg.lowBatteryBrightness="; r+=cfg.lowBatteryBrightness; r+=";\n";
  return r;
}

// ============================================================
// HTTP HANDLERS
// ============================================================
// Forward declaration — MOHAWK_HTML[] is defined below after the HTTP handlers
extern const char MOHAWK_HTML[] PROGMEM;
static void handleRoot(){if(_srv)_srv->send_P(200,PSTR("text/html"),MOHAWK_HTML);}
static void handleTelemetry(){if(_srv)_srv->send(200,F("application/json"),buildTelemetryJson());}
static void handleConfig(){if(_srv)_srv->send(200,F("application/json"),buildConfigJson());}
static void handleDiagnostics(){if(_srv)_srv->send(200,F("application/json"),buildDiagnosticsJson());}
static void handleHardwareLeds(){if(_srv)_srv->send(200,F("application/json"),buildHardwareLedsJson());}
static void handleSave(){saveSettings();if(_srv)_srv->send(200,F("text/plain"),F("saved"));}
static void handleFactory(){factoryReset();initRuntime();if(_srv)_srv->send(200,F("text/plain"),F("reset"));}
static void handleRecipe(){if(_srv)_srv->send(200,F("text/plain"),buildRecipeText());}

static void handleConfigApply(){
  if(!_srv)return;
  for(uint8_t i=0;i<_srv->args();i++){
    String n=_srv->argName(i), v=_srv->arg(i);
    if(n==F("globalBrightness"))cfg.globalBrightness=clampU8(v.toInt(),1,255);
    else if(n==F("disconnectedMode"))cfg.disconnectedMode=clampU8(v.toInt(),0,DISC_MODE_COUNT-1);
    else if(n==F("disconnectedSpeed"))cfg.disconnectedSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("disconnectedBrightness"))cfg.disconnectedBrightness=clampU8(v.toInt(),1,255);
    else if(n==F("team"))cfg.team=clampU8(v.toInt(),0,1);
    else if(n==F("teamBlue"))cfg.teamBlue=parseCol(v);
    else if(n==F("teamOrange"))cfg.teamOrange=parseCol(v);
    else if(n==F("teamIdleMode"))cfg.teamIdleMode=clampU8(v.toInt(),0,IDLE_MODE_COUNT-1);
    else if(n==F("teamBreathSpeed"))cfg.teamBreathSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("teamBreathMinBrightness")){cfg.teamBreathMinBrightness=clampU8(v.toInt(),0,254);if(cfg.teamBreathMinBrightness>=cfg.teamBreathMaxBrightness)cfg.teamBreathMaxBrightness=min(255,cfg.teamBreathMinBrightness+1);}
    else if(n==F("teamBreathMaxBrightness")){cfg.teamBreathMaxBrightness=clampU8(v.toInt(),1,255);if(cfg.teamBreathMaxBrightness<=cfg.teamBreathMinBrightness)cfg.teamBreathMinBrightness=max(0,cfg.teamBreathMaxBrightness-1);}
    else if(n==F("teamBreathDiagonal"))cfg.teamBreathDiagonal=v.toInt()!=0;
    else if(n==F("teamBreathRotate"))cfg.teamBreathRotate=v.toInt()!=0;
    else if(n==F("teamBreathAngle"))cfg.teamBreathAngle=clampU8(v.toInt(),0,255);
    else if(n==F("teamBreathRotationSpeed"))cfg.teamBreathRotationSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("chargeMode"))cfg.chargeMode=clampU8(v.toInt(),0,CHARGE_MODE_COUNT-1);
    else if(n==F("chargeSpeed"))cfg.chargeSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("chargePixels"))cfg.chargePixels=clampU8(v.toInt(),1,MOHAWK_LED_COUNT);
    else if(n==F("chargeBrightness"))cfg.chargeBrightness=clampU8(v.toInt(),1,255);
    else if(n==F("chargeColor"))cfg.chargeColor=parseCol(v);
    else if(n==F("fireMode"))cfg.fireMode=clampU8(v.toInt(),0,FIRE_MODE_COUNT-1);
    else if(n==F("fireSpeed"))cfg.fireSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("firePixels"))cfg.firePixels=clampU8(v.toInt(),1,MOHAWK_LED_COUNT);
    else if(n==F("fireFade"))cfg.fireFade=clampU8(v.toInt(),0,255);
    else if(n==F("fireBrightness"))cfg.fireBrightness=clampU8(v.toInt(),1,255);
    else if(n==F("fireColor"))cfg.fireColor=parseCol(v);
    else if(n==F("shotMeterMode"))cfg.shotMeterMode=clampU8(v.toInt(),0,SHOT_MODE_COUNT-1);
    else if(n==F("maxBurstShots"))cfg.maxBurstShots=clampU8(v.toInt(),1,5);
    else if(n==F("burstWindowMs"))cfg.burstWindowMs=clampU16(v.toInt(),400,8000);
    else if(n==F("cooldownMs"))cfg.cooldownMs=clampU16(v.toInt(),200,8000);
    else if(n==F("cooldownMode"))cfg.cooldownMode=clampU8(v.toInt(),0,COOL_MODE_COUNT-1);
    else if(n==F("cooldownBrightness"))cfg.cooldownBrightness=clampU8(v.toInt(),1,255);
    else if(n.startsWith(F("shotPairColor"))){int idx=n.substring(13).toInt();if(idx>=0&&idx<5)cfg.shotPairColors[idx]=parseCol(v);}
    else if(n==F("countdownMode"))cfg.countdownMode=clampU8(v.toInt(),0,CDN_MODE_COUNT-1);
    else if(n==F("countdownSeconds"))cfg.countdownSeconds=clampU8(v.toInt(),3,30);
    else if(n==F("countdownSpeed"))cfg.countdownSpeed=clampU8(v.toInt(),1,100);
    else if(n==F("countdownBrightness"))cfg.countdownBrightness=clampU8(v.toInt(),1,255);
    else if(n==F("countdownColorA"))cfg.countdownColorA=parseCol(v);
    else if(n==F("countdownColorB"))cfg.countdownColorB=parseCol(v);
    else if(n==F("lowBatteryMode"))cfg.lowBatteryMode=clampU8(v.toInt(),0,LOWBAT_MODE_COUNT-1);
    else if(n==F("lowBatteryBrightness"))cfg.lowBatteryBrightness=clampU8(v.toInt(),1,255);
  }
  validateSettings();
  if(_srv)_srv->send(200,F("application/json"),F("{\"ok\":true}"));
}

static void handleTestCmd(){
  if(!_srv)return;
  String what=_srv->arg("what");
  if(what==F("connected"))mohawk_set_connected(true);
  else if(what==F("disconnected"))mohawk_set_connected(false);
  else if(what==F("lowBattery"))mohawk_set_low_battery(!rt.lowBattery);
  else if(what==F("lowBatOn"))mohawk_set_low_battery(true);
  else if(what==F("lowBatOff"))mohawk_set_low_battery(false);
  else if(what==F("chargeStart"))mohawk_start_solenoid_charge();
  else if(what==F("chargeStop"))mohawk_stop_solenoid_charge();
  else if(what==F("fire")){if(!rt.connected)mohawk_set_connected(true);mohawk_trigger_solenoid_animation();}
  else if(what==F("burst5")){
    if(!rt.connected)mohawk_set_connected(true);
    mohawk_reset_shot_meter();
    for(int i=0;i<5;i++)mohawk_trigger_solenoid_animation();
  }
  else if(what==F("resetShots"))mohawk_reset_shot_meter();
  else if(what==F("countdown"))mohawk_start_countdown(cfg.countdownSeconds);
  else if(what==F("countdownStop"))mohawk_stop_countdown();
  else if(what==F("cooldown")){rt.burstShotCount=cfg.maxBurstShots;rt.cooldownUntilMs=millis()+cfg.cooldownMs;rt.shotMeterHoldUntilMs=rt.cooldownUntilMs;}
  else if(what==F("identify")){int led=_srv->arg("led").toInt();if(led>=0&&led<MOHAWK_LED_COUNT){rt.identifyLed=led;rt.identifyUntilMs=millis()+1500;}}
  else if(what==F("selfTestStart"))startSelfTest();
  else if(what==F("selfTestCancel"))cancelSelfTest();
  else if(what==F("solArm"))armSolenoid();
  else if(what==F("solDisarm"))disarmSolenoid();
  else if(what==F("solPulse")){triggerSolenoidPulse();mohawk_trigger_solenoid_animation();}
  _srv->send(200,F("application/json"),F("{\"ok\":true}"));
}

// ============================================================
// WEB UI — PROGMEM HTML/CSS/JS
// ============================================================
const char MOHAWK_HTML[] PROGMEM = R"RAWHTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Mohawk LED v2.2</title>
<style>
:root{
  --bg:#040d1a;--panel:#081628;--panel2:#0c1e36;--line:#152d50;
  --text:#e6f2ff;--muted:#6a8ab8;--accent:#00e5ff;--accent2:#ffb300;
  --good:#00e87a;--bad:#ff3355;--warn:#ffbe00;--fire:#ff4400;
  --r:14px;--font:'SF Pro Display',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:var(--font);min-height:100vh;overflow-x:hidden}
header{position:sticky;top:0;z-index:30;background:rgba(4,13,26,.92);backdrop-filter:blur(16px);
  border-bottom:1px solid var(--line);padding:12px 18px;
  display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
.hdr-title{font-size:17px;font-weight:800;letter-spacing:-.2px}
.hdr-title span{color:var(--accent)}
.hdr-badges{display:flex;gap:7px;flex-wrap:wrap}
.badge{background:rgba(21,45,80,.7);border:1px solid var(--line);border-radius:99px;
  padding:5px 11px;font-size:11px;color:var(--muted)}
.badge strong{color:var(--text)}
.layout{display:grid;grid-template-columns:360px 1fr;gap:14px;max-width:1400px;margin:0 auto;padding:14px}
/* --- LED Canvas Panel --- */
.led-panel{display:flex;flex-direction:column;gap:12px}
.canvas-wrap{background:radial-gradient(circle at 50% 40%,#0d2a4a 0%,#040d1a 65%);
  border:1px solid var(--line);border-radius:var(--r);
  position:relative;padding:16px;display:flex;flex-direction:column;align-items:center;gap:10px}
.canvas-labels{display:flex;justify-content:space-between;width:320px;font-size:10px;color:var(--muted);padding:0 4px}
#ledCanvas{position:relative;width:320px;height:420px}
.led-dot{position:absolute;width:44px;height:44px;border-radius:50%;
  --led-color:#010810;--led-glow:rgba(0,0,0,0);
  background:radial-gradient(circle at 35% 30%,rgba(255,255,255,.18) 0%,var(--led-color) 50%,rgba(0,0,0,.85) 100%);
  box-shadow:0 0 8px var(--led-glow),0 0 20px var(--led-glow),0 0 44px var(--led-glow);
  display:flex;align-items:center;justify-content:center;
  cursor:pointer;user-select:none;border:1.5px solid rgba(255,255,255,.06);
  transition:transform .1s}
.led-dot:hover{transform:scale(1.15);z-index:20;border-color:var(--accent)}
.led-dot.bat-led{border-color:rgba(255,100,0,.3)}
.led-num{font-size:10px;font-weight:700;color:rgba(255,255,255,.45);pointer-events:none}
/* HUD strip */
.hud-strip{display:grid;grid-template-columns:repeat(4,1fr);gap:7px}
.hud-box{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:8px 10px}
.hud-k{font-size:9px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;margin-bottom:2px}
.hud-v{font-size:15px;font-weight:900;font-variant-numeric:tabular-nums}
.hud-v.ok{color:var(--good)}.hud-v.err{color:var(--bad)}.hud-v.warn{color:var(--warn)}.hud-v.dim{color:var(--muted)}
/* Tooltip */
#tooltip{position:fixed;background:#0a1e38;border:1px solid var(--line);border-radius:10px;
  padding:10px 13px;font-size:11px;color:var(--muted);pointer-events:none;z-index:100;
  min-width:160px;display:none;line-height:1.7}
#tooltip strong{color:var(--text);font-size:12px}
/* --- Right panel --- */
.ctrl-panel{display:flex;flex-direction:column;min-width:0}
.tabs{display:flex;gap:0;border-bottom:1px solid var(--line);overflow-x:auto;scrollbar-width:none;flex-shrink:0}
.tabs::-webkit-scrollbar{display:none}
.tab-btn{background:none;border:none;border-bottom:2px solid transparent;
  color:var(--muted);padding:10px 15px;cursor:pointer;font-size:12px;font-weight:700;
  white-space:nowrap;letter-spacing:.1px;transition:color .15s,border-color .15s;flex-shrink:0}
.tab-btn:hover{color:var(--text)}
.tab-btn.active{color:var(--accent);border-bottom-color:var(--accent)}
.tabpane{display:none;overflow-y:auto;max-height:calc(100vh - 120px);padding-bottom:20px}
.tabpane.active{display:block}
/* Cards / Sections */
.card{background:linear-gradient(160deg,rgba(12,30,54,.97),rgba(6,15,28,.97));
  border:1px solid var(--line);border-radius:var(--r);padding:16px;margin-bottom:12px}
.card-title{font-size:13px;font-weight:700;color:#b8d4f8;margin-bottom:12px;display:flex;align-items:center;gap:7px}
.dot{width:7px;height:7px;border-radius:50%;background:var(--accent);flex-shrink:0}
.dot.orange{background:var(--accent2)}.dot.red{background:var(--bad)}.dot.green{background:var(--good)}.dot.fire{background:var(--fire)}
/* Form controls */
.field{margin-bottom:10px}
.field-lbl{font-size:11px;color:var(--muted);margin-bottom:4px;display:flex;justify-content:space-between}
.field-val{color:var(--accent);font-variant-numeric:tabular-nums}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:9px}
.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
.g5{display:grid;grid-template-columns:repeat(5,1fr);gap:5px}
select,input[type=number]{width:100%;background:#060f1e;border:1px solid var(--line);border-radius:8px;
  color:var(--text);padding:8px 10px;font-size:12px;outline:none}
select:focus,input[type=number]:focus{border-color:var(--accent)}
input[type=range]{width:100%;accent-color:var(--accent);margin-top:3px;cursor:pointer}
input[type=color]{width:100%;height:36px;border:1px solid var(--line);border-radius:8px;background:#060f1e;padding:3px;cursor:pointer}
.tog{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--muted);margin:5px 0;cursor:pointer}
.tog input{width:15px;height:15px;accent-color:var(--accent)}
/* Buttons */
.btn{border:0;border-radius:9px;padding:8px 12px;font-size:12px;font-weight:700;cursor:pointer;
  letter-spacing:.1px;transition:opacity .1s,transform .08s;white-space:nowrap}
.btn:active{transform:translateY(1px);opacity:.8}
.btn-primary{background:linear-gradient(135deg,#00c8ef,#1e50ff);color:#fff;box-shadow:0 4px 14px rgba(0,200,239,.2)}
.btn-secondary{background:#0c1e36;color:#c0d8ff;border:1px solid var(--line)}
.btn-good{background:linear-gradient(135deg,#00cc70,#00a8e8);color:#fff}
.btn-warn{background:linear-gradient(135deg,#ffbe00,#ff6600);color:#000}
.btn-danger{background:linear-gradient(135deg,#ff3355,#a0001a);color:#fff}
.btn-fire{background:linear-gradient(135deg,#ff4400,#ff9900);color:#fff}
.brow{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px}
.note{font-size:11px;color:var(--muted);line-height:1.5;margin-top:5px}
/* FPS / telemetry */
.fps-badge{background:#0c1e36;border:1px solid var(--line);border-radius:7px;padding:4px 9px;font-size:11px;
  color:var(--muted);font-variant-numeric:tabular-nums;display:inline-block}
/* Shot pair colors */
.sp-cell{text-align:center}
.sp-lbl{font-size:9px;color:var(--muted);margin-bottom:3px}
/* Event log */
.evt-log{max-height:200px;overflow-y:auto;font-size:11px;font-family:'SF Mono',Menlo,monospace}
.evt-entry{padding:3px 0;border-bottom:1px solid rgba(21,45,80,.5);display:flex;gap:8px}
.evt-ms{color:var(--muted);min-width:70px;font-size:10px}
.evt-msg{color:var(--text)}.evt-info{color:var(--good)}.evt-warn{color:var(--warn)}.evt-err{color:var(--bad)}
/* Layer inspector */
.layer-row{display:flex;align-items:center;gap:8px;margin:4px 0;font-size:11px}
.layer-swatch{width:18px;height:18px;border-radius:4px;flex-shrink:0}
.layer-name{color:var(--muted);flex:1}
.layer-active{color:var(--good)}
.layer-inactive{color:var(--muted);opacity:.5}
/* Timeline */
#timeline{width:100%;height:6px;appearance:none;-webkit-appearance:none;background:var(--line);border-radius:3px;outline:none;cursor:pointer;accent-color:var(--accent)}
/* Responsive */
@media(max-width:860px){.layout{grid-template-columns:1fr}.tabpane{max-height:none}}
</style>
</head>
<body>
<header>
  <div class="hdr-title">Mohawk LED <span>v2.2</span></div>
  <div class="hdr-badges">
    <div class="badge">AP <strong>MiniBot_LED_UI</strong></div>
    <div class="badge">PW <strong>fifabot123</strong></div>
    <div class="badge">IP <strong>192.168.4.1</strong></div>
    <span class="fps-badge">FPS: <span id="fpsVal">--</span></span>
    <span class="fps-badge">Telem: <span id="telemAge">--</span>ms</span>
  </div>
</header>
<div class="layout">
  <!-- LEFT: LED Canvas always visible -->
  <div class="led-panel">
    <div class="canvas-wrap">
      <div class="canvas-labels"><span>Left Rail (0-7)</span><span>Right Rail (10-17)</span></div>
      <div id="ledCanvas"></div>
      <div class="canvas-labels" style="justify-content:center"><span>Bridge (8-9) — Battery LEDs</span></div>
    </div>
    <div class="hud-strip">
      <div class="hud-box"><div class="hud-k">State</div><div class="hud-v" id="hudState">--</div></div>
      <div class="hud-box"><div class="hud-k">Burst</div><div class="hud-v" id="hudBurst">0/5</div></div>
      <div class="hud-box"><div class="hud-k">Cooldown</div><div class="hud-v" id="hudCool">0.0s</div></div>
      <div class="hud-box"><div class="hud-k">Countdown</div><div class="hud-v" id="hudCnt">--</div></div>
    </div>
    <div class="hud-strip">
      <div class="hud-box"><div class="hud-k">Animation</div><div class="hud-v dim" style="font-size:11px" id="hudAnim">--</div></div>
      <div class="hud-box"><div class="hud-k">Shots</div><div class="hud-v dim" id="hudShots">0</div></div>
      <div class="hud-box"><div class="hud-k">Low Bat</div><div class="hud-v" id="hudBat">OFF</div></div>
      <div class="hud-box"><div class="hud-k">Sol Armed</div><div class="hud-v" id="hudArmed">NO</div></div>
    </div>
  </div>
  <!-- RIGHT: Tabbed controls -->
  <div class="ctrl-panel">
    <div class="tabs">
      <button class="tab-btn active" onclick="switchTab('state')">Robot State</button>
      <button class="tab-btn" onclick="switchTab('breath')">Team Breath</button>
      <button class="tab-btn" onclick="switchTab('solenoid')">Solenoid</button>
      <button class="tab-btn" onclick="switchTab('countdown')">Countdown</button>
      <button class="tab-btn" onclick="switchTab('lab')">Anim Lab</button>
      <button class="tab-btn" onclick="switchTab('diag')">Diagnostics</button>
      <button class="tab-btn" onclick="switchTab('settings')">Settings</button>
    </div>
    <!-- TAB: Robot State -->
    <div class="tabpane active" id="tab-state">
      <div class="card">
        <div class="card-title"><span class="dot green"></span>Connection</div>
        <div class="brow">
          <button class="btn btn-good" onclick="cmd('connected')">Set Connected</button>
          <button class="btn btn-warn" onclick="cmd('disconnected')">Set Disconnected</button>
        </div>
        <div class="g2" style="margin-top:8px">
          <div><div class="field-lbl">Disconnected animation</div>
            <select id="disconnectedMode" onchange="set('disconnectedMode',this.value)">
              <option value="0">Rainbow Wash</option><option value="1">Search Sweep</option>
              <option value="2">Wi-Fi Ping</option><option value="3">Lost Signal Static</option>
              <option value="4">Handshake Loop</option><option value="5">Standby Aurora</option>
              <option value="6">Radar Sweep</option><option value="7">Pair Breath Search</option>
              <option value="8">Waiting Comet</option><option value="9">Bottom Beacon</option>
              <option value="10">Signal Bars</option><option value="11">Diagonal Scanner</option>
            </select></div>
          <div><div class="field-lbl">Team</div>
            <select id="team" onchange="set('team',this.value)">
              <option value="0">Blue Team</option><option value="1">Orange Team</option>
            </select></div>
        </div>
        <div class="g2">
          <div class="field"><div class="field-lbl">Disc speed <span class="field-val" id="disconnectedSpeedVal"></span></div>
            <input type="range" id="disconnectedSpeed" min="1" max="100" oninput="rng(this,'disconnectedSpeed')"></div>
          <div class="field"><div class="field-lbl">Disc brightness <span class="field-val" id="disconnectedBrightnessVal"></span></div>
            <input type="range" id="disconnectedBrightness" min="1" max="255" oninput="rng(this,'disconnectedBrightness')"></div>
        </div>
        <div class="g2">
          <div class="field"><div class="field-lbl">Blue team color</div><input type="color" id="teamBlue" onchange="set('teamBlue',this.value)"></div>
          <div class="field"><div class="field-lbl">Orange team color</div><input type="color" id="teamOrange" onchange="set('teamOrange',this.value)"></div>
        </div>
        <div class="field"><div class="field-lbl">Global brightness <span class="field-val" id="globalBrightnessVal"></span></div>
          <input type="range" id="globalBrightness" min="1" max="255" oninput="rng(this,'globalBrightness')"></div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot red"></span>Low Battery</div>
        <div class="brow">
          <button class="btn btn-secondary" onclick="cmd('lowBatOn')">Low Bat ON</button>
          <button class="btn btn-secondary" onclick="cmd('lowBatOff')">Low Bat OFF</button>
          <button class="btn btn-secondary" onclick="cmd('lowBattery')">Toggle</button>
        </div>
        <div class="g2" style="margin-top:8px">
          <div><div class="field-lbl">Low battery mode</div>
            <select id="lowBatteryMode" onchange="set('lowBatteryMode',this.value)">
              <option value="0">Red Flash</option><option value="1">Double Blink</option>
              <option value="2">Heartbeat</option><option value="3">Ember</option>
              <option value="4">Voltage Tier</option><option value="5">Service Mode</option>
              <option value="6">Silent</option>
            </select></div>
          <div class="field"><div class="field-lbl">Low bat brightness <span class="field-val" id="lowBatteryBrightnessVal"></span></div>
            <input type="range" id="lowBatteryBrightness" min="1" max="255" oninput="rng(this,'lowBatteryBrightness')"></div>
        </div>
        <p class="note">LEDs 8 + 9 (bottom bridge). Low battery overlay is ALWAYS the last render layer — it overrides every other animation.</p>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot orange"></span>Self-Test &amp; Identify</div>
        <div class="brow">
          <button class="btn btn-primary" onclick="cmd('selfTestStart')">Run Full Self-Test</button>
          <button class="btn btn-danger" onclick="cmd('selfTestCancel')">Cancel Self-Test</button>
        </div>
        <div class="brow">
          <button class="btn btn-secondary" onclick="identifyAll()">Chase All LEDs</button>
          <button class="btn btn-secondary" onclick="cmd('identify','&led=8')">Identify LED 8</button>
          <button class="btn btn-secondary" onclick="cmd('identify','&led=9')">Identify LED 9</button>
        </div>
        <p class="note">Self-test runs: disconnected → connected → countdown → charge → fire → burst → cooldown → low battery → LED chase.</p>
      </div>
    </div>
    <!-- TAB: Team Breath -->
    <div class="tabpane" id="tab-breath">
      <div class="card">
        <div class="card-title"><span class="dot green"></span>Connected Idle Animation</div>
        <div class="field"><div class="field-lbl">Animation mode</div>
          <select id="teamIdleMode" onchange="set('teamIdleMode',this.value)">
            <option value="0">Flat Team Breath</option><option value="1">Diagonal Breath</option>
            <option value="2">Rotating Diagonal</option><option value="3">Comet Orbit</option>
            <option value="4">Team Sparkle</option><option value="5">Power Core</option>
            <option value="6">Rail Glow</option><option value="7">Shield</option>
            <option value="8">Tactical Stealth</option><option value="9">Pulse Rings</option>
            <option value="10">Energy Equalizer</option><option value="11">Stadium Wave</option>
          </select></div>
        <div class="g2">
          <div class="field"><div class="field-lbl">Speed <span class="field-val" id="teamBreathSpeedVal"></span></div>
            <input type="range" id="teamBreathSpeed" min="1" max="100" oninput="rng(this,'teamBreathSpeed')"></div>
          <div class="field"><div class="field-lbl">Min brightness <span class="field-val" id="teamBreathMinBrightnessVal"></span></div>
            <input type="range" id="teamBreathMinBrightness" min="0" max="254" oninput="rng(this,'teamBreathMinBrightness')"></div>
        </div>
        <div class="field"><div class="field-lbl">Max brightness <span class="field-val" id="teamBreathMaxBrightnessVal"></span></div>
          <input type="range" id="teamBreathMaxBrightness" min="1" max="255" oninput="rng(this,'teamBreathMaxBrightness')"></div>
        <label class="tog"><input type="checkbox" id="teamBreathDiagonal" onchange="set('teamBreathDiagonal',this.checked?1:0)">Use diagonal wave</label>
        <label class="tog"><input type="checkbox" id="teamBreathRotate" onchange="set('teamBreathRotate',this.checked?1:0)">Rotate diagonal (clock-hand)</label>
        <div class="g2">
          <div class="field"><div class="field-lbl">Static angle <span class="field-val" id="teamBreathAngleVal"></span></div>
            <input type="range" id="teamBreathAngle" min="0" max="255" oninput="rng(this,'teamBreathAngle')"></div>
          <div class="field"><div class="field-lbl">Rotation speed <span class="field-val" id="teamBreathRotationSpeedVal"></span></div>
            <input type="range" id="teamBreathRotationSpeed" min="1" max="100" oninput="rng(this,'teamBreathRotationSpeed')"></div>
        </div>
        <div class="brow"><button class="btn btn-good" onclick="cmd('connected')">Test Team Breath</button></div>
        <p class="note">Rotation center is the midpoint between LED 4 and LED 13. Diagonal/rotate only affect modes 1+2.</p>
      </div>
    </div>
    <!-- TAB: Solenoid -->
    <div class="tabpane" id="tab-solenoid">
      <div class="card">
        <div class="card-title"><span class="dot"></span>Pre-Fire / Armed Glow <span class="pill">optional</span></div>
        <div class="g2">
          <div><div class="field-lbl">Armed glow mode</div>
            <select id="chargeMode" onchange="set('chargeMode',this.value)">
              <option value="0">Bottom Core</option><option value="1">Rail Gather</option>
              <option value="2">Capacitor Fill</option><option value="3">Scanner Arm</option>
              <option value="4">Plasma Core</option><option value="5">Voltage Ramp</option>
              <option value="6">Magnetic Pair Pulse</option><option value="7">Lock-On Build</option>
              <option value="8">Danger Hold</option><option value="9">Coil Wind-Up</option>
              <option value="10">Pulse Compress</option><option value="11">Ready Snap</option>
            </select></div>
          <div><div class="field-lbl">Armed glow color</div><input type="color" id="chargeColor" onchange="set('chargeColor',this.value)"></div>
        </div>
        <div class="g3">
          <div class="field"><div class="field-lbl">Speed <span class="field-val" id="chargeSpeedVal"></span></div>
            <input type="range" id="chargeSpeed" min="1" max="100" oninput="rng(this,'chargeSpeed')"></div>
          <div class="field"><div class="field-lbl">Pixels <span class="field-val" id="chargePixelsVal"></span></div>
            <input type="range" id="chargePixels" min="1" max="18" oninput="rng(this,'chargePixels')"></div>
          <div class="field"><div class="field-lbl">Brightness <span class="field-val" id="chargeBrightnessVal"></span></div>
            <input type="range" id="chargeBrightness" min="1" max="255" oninput="rng(this,'chargeBrightness')"></div>
        </div>
        <div class="brow">
          <button class="btn btn-primary" onclick="cmd('chargeStart')">Start Armed Glow</button>
          <button class="btn btn-secondary" onclick="cmd('chargeStop')">Stop Armed Glow</button>
        </div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot warn" style="background:var(--warn)"></span>5-Shot Lockout Meter</div>
        <div class="g5">
          <div class="sp-cell"><div class="sp-lbl">Shot 1<br>0+17</div><input type="color" id="shotPairColor0" onchange="set('shotPairColor0',this.value)"></div>
          <div class="sp-cell"><div class="sp-lbl">Shot 2<br>1+16</div><input type="color" id="shotPairColor1" onchange="set('shotPairColor1',this.value)"></div>
          <div class="sp-cell"><div class="sp-lbl">Shot 3<br>2+15</div><input type="color" id="shotPairColor2" onchange="set('shotPairColor2',this.value)"></div>
          <div class="sp-cell"><div class="sp-lbl">Shot 4<br>3+14</div><input type="color" id="shotPairColor3" onchange="set('shotPairColor3',this.value)"></div>
          <div class="sp-cell"><div class="sp-lbl">Shot 5<br>4+13</div><input type="color" id="shotPairColor4" onchange="set('shotPairColor4',this.value)"></div>
        </div>
        <div class="g3" style="margin-top:10px">
          <div class="field"><div class="field-lbl">Max burst shots <span class="field-val" id="maxBurstShotsVal"></span></div>
            <input type="range" id="maxBurstShots" min="1" max="5" oninput="rng(this,'maxBurstShots')"></div>
          <div class="field"><div class="field-lbl">Burst window ms <span class="field-val" id="burstWindowMsVal"></span></div>
            <input type="range" id="burstWindowMs" min="400" max="8000" step="100" oninput="rng(this,'burstWindowMs')"></div>
          <div class="field"><div class="field-lbl">Cooldown ms <span class="field-val" id="cooldownMsVal"></span></div>
            <input type="range" id="cooldownMs" min="200" max="8000" step="100" oninput="rng(this,'cooldownMs')"></div>
        </div>
        <div class="g2">
          <div><div class="field-lbl">Cooldown mode</div>
            <select id="cooldownMode" onchange="set('cooldownMode',this.value)">
              <option value="0">Reverse Pair Clear</option><option value="1">Heat Dissolve</option>
              <option value="2">Team Reclaim</option><option value="3">Cooling Breath</option>
              <option value="4">Minimal Fade</option><option value="5">Safety Flash</option>
              <option value="6">Lockout Warning</option><option value="7">Smooth Return</option>
            </select></div>
          <div class="field"><div class="field-lbl">Cooldown brightness <span class="field-val" id="cooldownBrightnessVal"></span></div>
            <input type="range" id="cooldownBrightness" min="1" max="255" oninput="rng(this,'cooldownBrightness')"></div>
        </div>
        <div class="brow">
          <button class="btn btn-fire" onclick="doFire()">Fire One Shot</button>
          <button class="btn btn-warn" onclick="doBurst5()">5-Shot Burst</button>
          <button class="btn btn-secondary" onclick="cmd('cooldown')">Trigger Cooldown</button>
          <button class="btn btn-danger" onclick="cmd('resetShots')">Reset Shots</button>
        </div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot fire"></span>Fire Animation</div>
        <div class="g2">
          <div><div class="field-lbl">Fire mode</div>
            <select id="fireMode" onchange="set('fireMode',this.value)">
              <option value="0">Dual Rail Shot</option><option value="1">Forward Wave</option>
              <option value="2">Muzzle Burst</option><option value="3">Recoil Shock</option>
              <option value="4">Plasma Lance</option><option value="5">Lightning Fork</option>
              <option value="6">Sonic Boom</option><option value="7">Sparkle Blast</option>
              <option value="8">Team Impact</option><option value="9">Clean Flash</option>
              <option value="10">Hyperdrive Streak</option><option value="11">Energy Kick</option>
            </select></div>
          <div><div class="field-lbl">Fire color</div><input type="color" id="fireColor" onchange="set('fireColor',this.value)"></div>
        </div>
        <div class="g3">
          <div class="field"><div class="field-lbl">Speed <span class="field-val" id="fireSpeedVal"></span></div>
            <input type="range" id="fireSpeed" min="1" max="100" oninput="rng(this,'fireSpeed')"></div>
          <div class="field"><div class="field-lbl">Pixels <span class="field-val" id="firePixelsVal"></span></div>
            <input type="range" id="firePixels" min="1" max="18" oninput="rng(this,'firePixels')"></div>
          <div class="field"><div class="field-lbl">Fade <span class="field-val" id="fireFadeVal"></span></div>
            <input type="range" id="fireFade" min="0" max="255" oninput="rng(this,'fireFade')"></div>
        </div>
        <div class="field"><div class="field-lbl">Brightness <span class="field-val" id="fireBrightnessVal"></span></div>
          <input type="range" id="fireBrightness" min="1" max="255" oninput="rng(this,'fireBrightness')"></div>
        <div class="brow"><button class="btn btn-fire" style="width:100%" onclick="doFire()">Test Fire (+ 1 shot to burst)</button></div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot red"></span>Physical Solenoid</div>
        <div class="brow">
          <button class="btn btn-danger" onclick="cmd('solArm')">ARM Solenoid</button>
          <button class="btn btn-secondary" onclick="cmd('solDisarm')">Disarm</button>
          <button class="btn btn-warn" onclick="cmd('solPulse')" id="solPulseBtn">Pulse + Fire LEDs</button>
        </div>
        <p class="note" id="solNote">Physical solenoid disabled (SOLENOID_TEST_PIN = -1). Set pin in sketch to enable. Arm timeout: 15 seconds.</p>
      </div>
    </div>
    <!-- TAB: Countdown -->
    <div class="tabpane" id="tab-countdown">
      <div class="card">
        <div class="card-title"><span class="dot warn" style="background:var(--warn)"></span>Game Countdown</div>
        <div class="g2">
          <div><div class="field-lbl">Countdown mode</div>
            <select id="countdownMode" onchange="set('countdownMode',this.value)">
              <option value="0">U Progress Drain</option><option value="1">Traffic Build</option>
              <option value="2">Stadium Pulse</option><option value="3">Comet Tick</option>
              <option value="4">Side Launch</option><option value="5">Gate Opening</option>
              <option value="6">Final Green Blast</option><option value="7">Referee Signal</option>
              <option value="8">Team Split</option><option value="9">Energy Build</option>
              <option value="10">Pulse Stack</option><option value="11">Calm Pro</option>
            </select></div>
          <div class="field"><div class="field-lbl">Seconds <span class="field-val" id="countdownSecondsVal"></span></div>
            <input type="range" id="countdownSeconds" min="3" max="30" oninput="rng(this,'countdownSeconds')"></div>
        </div>
        <div class="g2">
          <div><div class="field-lbl">Color A</div><input type="color" id="countdownColorA" onchange="set('countdownColorA',this.value)"></div>
          <div><div class="field-lbl">Color B (final seconds)</div><input type="color" id="countdownColorB" onchange="set('countdownColorB',this.value)"></div>
        </div>
        <div class="g2">
          <div class="field"><div class="field-lbl">Speed <span class="field-val" id="countdownSpeedVal"></span></div>
            <input type="range" id="countdownSpeed" min="1" max="100" oninput="rng(this,'countdownSpeed')"></div>
          <div class="field"><div class="field-lbl">Brightness <span class="field-val" id="countdownBrightnessVal"></span></div>
            <input type="range" id="countdownBrightness" min="1" max="255" oninput="rng(this,'countdownBrightness')"></div>
        </div>
        <div class="brow">
          <button class="btn btn-primary" onclick="cmd('countdown')">Start Countdown</button>
          <button class="btn btn-secondary" onclick="cmd('countdownStop')">Stop</button>
        </div>
      </div>
    </div>
    <!-- TAB: Animation Lab -->
    <div class="tabpane" id="tab-lab">
      <div class="card">
        <div class="card-title"><span class="dot"></span>Timeline Preview</div>
        <div class="g2" style="margin-bottom:10px">
          <div><div class="field-lbl">Preview mode</div>
            <select id="labMode" onchange="labSetMode(this.value)">
              <option value="sim">Browser Simulation</option>
              <option value="hw">Hardware Live</option>
              <option value="manual">Manual Preview</option>
            </select></div>
          <div><div class="field-lbl">Animation override</div>
            <select id="labAnim" onchange="labSetAnim(this.value)">
              <option value="">-- Live Composite --</option>
              <option value="disc-rainbow">Disc: Rainbow Wash</option>
              <option value="disc-sweep">Disc: Search Sweep</option>
              <option value="disc-ping">Disc: Wi-Fi Ping</option>
              <option value="disc-aurora">Disc: Aurora</option>
              <option value="disc-comet">Disc: Comet</option>
              <option value="team-flat">Team: Flat Breath</option>
              <option value="team-diag">Team: Diagonal</option>
              <option value="team-rotate">Team: Rotating</option>
              <option value="team-orbit">Team: Comet Orbit</option>
              <option value="team-power">Team: Power Core</option>
            </select></div>
        </div>
        <div class="field"><div class="field-lbl">Timeline scrub <span class="field-val" id="labTimeVal">0.0s</span></div>
          <input type="range" id="labScrub" min="0" max="10000" value="0" oninput="labScrub(this.value)"></div>
        <div class="brow">
          <button class="btn btn-primary" onclick="labPlay()">Play</button>
          <button class="btn btn-secondary" onclick="labPause()">Pause</button>
          <button class="btn btn-secondary" onclick="labRestart()">Restart</button>
          <button class="btn btn-secondary" id="labLoopBtn" onclick="labToggleLoop()">Loop: ON</button>
        </div>
        <div class="brow" style="margin-top:6px">
          <button class="btn btn-secondary" onclick="labSetSpeed(0.25)">0.25x</button>
          <button class="btn btn-secondary" onclick="labSetSpeed(1)">1x</button>
          <button class="btn btn-secondary" onclick="labSetSpeed(2)">2x</button>
        </div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot orange"></span>Layer Inspector</div>
        <div id="layerInspector"></div>
        <p class="note">Layers are rendered in order. Low Battery is always the final layer and cannot be hidden.</p>
      </div>
    </div>
    <!-- TAB: Diagnostics -->
    <div class="tabpane" id="tab-diag">
      <div class="card">
        <div class="card-title"><span class="dot green"></span>System Status</div>
        <div id="diagInfo" style="font-size:12px;color:var(--muted);line-height:2"></div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot"></span>Event Log</div>
        <div class="evt-log" id="evtLog"><div style="color:var(--muted);font-size:11px">Loading...</div></div>
        <div class="brow" style="margin-top:8px"><button class="btn btn-secondary" onclick="clearEvtLog()">Clear Browser Log</button></div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot orange"></span>Hardware LED Buffer</div>
        <p class="note">Actual rendered colors from ESP32. Polled on demand — not real-time.</p>
        <div class="brow"><button class="btn btn-secondary" onclick="fetchHwLeds()">Fetch Hardware LEDs</button></div>
        <div id="hwLedOut" style="font-size:10px;color:var(--muted);margin-top:8px;font-family:monospace;word-break:break-all"></div>
      </div>
    </div>
    <!-- TAB: Settings -->
    <div class="tabpane" id="tab-settings">
      <div class="card">
        <div class="card-title"><span class="dot green"></span>Persistence</div>
        <p class="note" style="margin-bottom:10px">Save writes all settings to ESP32 NVS flash (namespace mohawkv22). They survive power cycles. Factory reset clears NVS and reverts to firmware defaults.</p>
        <div class="brow">
          <button class="btn btn-good" onclick="doSave()">Save to ESP32</button>
          <button class="btn btn-danger" onclick="doFactory()">Factory Reset</button>
        </div>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot orange"></span>Recipe Generator</div>
        <div class="brow"><button class="btn btn-secondary" onclick="doRecipe()">Generate Recipe</button>
          <button class="btn btn-secondary" onclick="copyRecipe()">Copy to Clipboard</button></div>
        <textarea id="recipeOut" style="width:100%;min-height:220px;background:#030810;color:#6eff9a;border:1px solid var(--line);border-radius:8px;padding:10px;font-family:monospace;font-size:11px;margin-top:10px;resize:vertical" placeholder="Click Generate Recipe after tuning."></textarea>
      </div>
      <div class="card">
        <div class="card-title"><span class="dot"></span>FMS Integration Notes</div>
        <p class="note">To integrate with FMS: set MOHAWK_STANDALONE_TEST_MODE=0 and MOHAWK_FMS_INTEGRATED_MODE=1. Then from FMS: call setup_mohawk() in setup(), registerMohawkRoutes(server) after WiFi is up, and mohawk_service() in loop(). All routes are under /api/mohawk/. See header comment for full details.</p>
      </div>
    </div>
  </div>
</div>
<div id="tooltip"></div>

<script>
// ============================================================
// LED DATA
// ============================================================
const N = 18;
const LED_XY = [
  {x:0,y:7},{x:0,y:6},{x:0,y:5},{x:0,y:4},{x:0,y:3},{x:0,y:2},{x:0,y:1},{x:0,y:0},
  {x:0.8,y:0},{x:1.2,y:0},
  {x:2,y:0},{x:2,y:1},{x:2,y:2},{x:2,y:3},{x:2,y:4},{x:2,y:5},{x:2,y:6},{x:2,y:7}
];
// [cx, cy] pixel centers within 320x420 canvas
const LED_PX = [
  [40,22],[40,70],[40,118],[40,166],[40,214],[40,262],[40,310],[40,358],
  [120,395],[200,395],
  [280,358],[280,310],[280,262],[280,214],[280,166],[280,118],[280,70],[280,22]
];
const SHOT_PAIRS = [[0,17],[1,16],[2,15],[3,14],[4,13]];
const LED_ROLES = [
  'Left rail top','Left rail','Left rail','Left rail','Left rail (shot center)','Left rail','Left rail','Left rail bottom',
  'Bottom bridge (battery A)','Bottom bridge (battery B)',
  'Right rail bottom','Right rail','Right rail','Right rail (shot center)','Right rail','Right rail','Right rail','Right rail top'
];
const DISC_NAMES=['Rainbow Wash','Search Sweep','Wi-Fi Ping','Lost Signal','Handshake Loop','Standby Aurora','Radar Sweep','Pair Breath','Waiting Comet','Bottom Beacon','Signal Bars','Diag Scanner'];
const IDLE_NAMES=['Flat Breath','Diagonal Breath','Rotating Diagonal','Comet Orbit','Team Sparkle','Power Core','Rail Glow','Shield','Stealth','Pulse Rings','Equalizer','Stadium Wave'];

// ============================================================
// STATE & CONFIG
// ============================================================
let state = {
  connected:false, lowBattery:false, team:0,
  discMode:0, idleMode:2,
  chargeActive:false, chargeStartMs:0, chargeMode:0,
  fireActive:false, fireStartMs:0, fireMode:10,
  fireEventSeq:0, prevFireSeq:0,
  burstShots:0, maxShots:5,
  partialDecayStartCount:0, partialDecayStartMs:0, partialDecayUntilMs:0, partialDecayStepMs:320,
  cooldownUntilMs:0, cooldownMs:2000, cooldownMode:0,
  countdownActive:false, countdownStartMs:0,
  countdownDurationMs:10000, countdownSeconds:10,
  identifyLed:-1, identifyUntilMs:0,
  selfTestActive:false, selfTestStep:0,
  solArmed:false, totalShots:0
};
let cfg = {
  globalBrightness:255, disconnectedMode:0, disconnectedSpeed:100, disconnectedBrightness:244,
  team:0, teamBlue:'#0050FF', teamOrange:'#FF5000',
  teamIdleMode:2, teamBreathSpeed:28, teamBreathMinBrightness:5, teamBreathMaxBrightness:255,
  teamBreathDiagonal:true, teamBreathRotate:true, teamBreathAngle:10, teamBreathRotationSpeed:100,
  chargeMode:0, chargeSpeed:48, chargePixels:6, chargeBrightness:255, chargeColor:'#00FFAA',
  fireMode:10, fireSpeed:100, firePixels:3, fireFade:255, fireBrightness:107, fireColor:'#FFFFFF',
  shotMeterMode:0, maxBurstShots:5, burstWindowMs:2000, cooldownMs:2000,
  cooldownMode:0, cooldownBrightness:255,
  shotPairColors:['#FF0202','#FF0606','#FF0000','#FE0000','#FF0000'],
  countdownMode:1, countdownSeconds:10, countdownSpeed:50, countdownBrightness:240,
  countdownColorA:'#FFBE00', countdownColorB:'#00FF00',
  lowBatteryMode:0, lowBatteryBrightness:255
};
// UI tracking
let fpsCount=0, fps=0, lastFpsMs=performance.now(), lastTelemMs=0;
let labTimeMs=0, labPlaying=true, labSpeed=1.0, labLooping=true, labAnimKey='', labManual=false, labHwMode=false;
let hwLedColors=null;
let layerVis={base:true,charge:true,shot:true,fire:true,countdown:true,identify:true,lowbat:true};
let browserEvtLog=[];
let simTimeMs=0, lastFrameMs=performance.now();
let ledEls=[], ledColors=new Array(N).fill(null).map(()=>({r:0,g:0,b:0}));
let configFetched=false;

// ============================================================
// MATH HELPERS
// ============================================================
const lerp=(a,b,t)=>a+(b-a)*t;
const clamp=(v,lo,hi)=>Math.max(lo,Math.min(hi,v));
const smoothstep=(t)=>{t=clamp(t,0,1);return t*t*(3-2*t);};
function hsvToRgb(h,s,v){
  h=((h%256)+256)%256;
  const hi=Math.floor(h/43), f=(h%43)/43;
  const p=v*(255-s)/255|0, q=v*(255-f*s)/255|0, t2=v*(255-(1-f)*s)/255|0;
  v=v|0;
  switch(hi%6){case 0:return{r:v,g:t2,b:p};case 1:return{r:q,g:v,b:p};case 2:return{r:p,g:v,b:t2};
    case 3:return{r:p,g:q,b:v};case 4:return{r:t2,g:p,b:v};default:return{r:v,g:p,b:q};}
}
function hexToRgb(h){const v=parseInt(h.slice(1),16);return{r:(v>>16)&255,g:(v>>8)&255,b:v&255};}
function rgbToHex(c){return '#'+[c.r,c.g,c.b].map(v=>clamp(v|0,0,255).toString(16).padStart(2,'0')).join('');}
function scaleColor(c,b){const s=b/255;return{r:c.r*s|0,g:c.g*s|0,b:c.b*s|0};}
function blendColor(a,b,t){return{r:lerp(a.r,b.r,t)|0,g:lerp(a.g,b.g,t)|0,b:lerp(a.b,b.b,t)|0};}
function blendLedC(cols,i,c,amt){if(i<0||i>=N)return;const t=amt/255;cols[i]=blendColor(cols[i],c,t);}
function scale8(v,s){return v*s/255|0;}
function sin8(x){return Math.sin((x/255)*Math.PI*2)*127.5+127.5|0;}
function beatsin8(bpm,lo,hi,tMs,phase=0){
  const t=((tMs*bpm/60000)+(phase/255))%1;
  return lo+(hi-lo)*(Math.sin(t*Math.PI*2)*0.5+0.5)|0;
}
function spdRate(s,lo,hi){return lo+(hi-lo)*clamp(s,1,100)/100;}
function mapR(v,i0,i1,o0,o1){return o0+(o1-o0)*((v-i0)/(i1-i0));}

// Precomputed spatial positions
const SPATIAL_POS=Array.from({length:N},(_,i)=>{
  const {x,y}=LED_XY[i];
  if(x<0.4)return clamp((7-y)/17,0,1);
  if(x<1.0)return 8/17;
  if(x<1.6)return 9/17;
  return clamp((10+y)/17,0,1);
});
function ledAtStep(step){
  if(step>=N)step=N-1;
  const t=step/17; let best=0,bestE=999;
  for(let i=0;i<N;i++){const e=Math.abs(SPATIAL_POS[i]-t);if(e<bestE){bestE=e;best=i;}}
  return best;
}
const LED_AT_STEP=Array.from({length:N},(_,i)=>ledAtStep(i));
function getTeamRgb(){return hexToRgb(cfg.team===0?cfg.teamBlue:cfg.teamOrange);}

// ============================================================
// JS ANIMATION RENDERERS — matching C++ implementations
// ============================================================
function jsRainbowWash(cols,tMs){
  const rate=spdRate(cfg.disconnectedSpeed,0.000025,0.00035);
  const hBase=(tMs*rate*255)%256;
  for(let i=0;i<N;i++) cols[i]=scaleColor(hsvToRgb((hBase+SPATIAL_POS[i]*120)%256,255,1),cfg.disconnectedBrightness);
}
function jsSearchSweep(cols,tMs){
  const rate=spdRate(cfg.disconnectedSpeed,0.0003,0.004);
  const t=(tMs*rate)%2, pos=(t<1)?t:2-t;
  for(let i=0;i<N;i++){const d=Math.abs(SPATIAL_POS[i]-pos);
    if(d<0.12)cols[i]={r:0,g:cfg.disconnectedBrightness,b:cfg.disconnectedBrightness};
    else if(d<0.26){const v=(1-(d-0.12)/0.14)*cfg.disconnectedBrightness*0.5|0;cols[i]={r:0,g:v,b:v};}
    else cols[i]={r:0,g:0,b:0};}
}
function jsWifiPing(cols,tMs){
  const rate=spdRate(cfg.disconnectedSpeed,0.0003,0.003);
  const t=(tMs*rate)%1, ring=t*9;
  for(let i=0;i<N;i++){cols[i]={r:0,g:0,b:0};
    const dx=LED_XY[i].x-1,dy=LED_XY[i].y,dist=Math.sqrt(dx*dx+dy*dy),d=Math.abs(dist-ring);
    if(d<0.7){const v=(1-d/0.7)*(1-t)*cfg.disconnectedBrightness|0;cols[i]={r:0,g:v>>1,b:v};}}
  const core=beatsin8(12,60,cfg.disconnectedBrightness,tMs);
  cols[8]={r:0,g:core>>1,b:core}; cols[9]={r:0,g:core>>1,b:core};
}
function jsAurora(cols,tMs){
  const t=tMs*0.0001;
  for(let i=0;i<N;i++){const sp=SPATIAL_POS[i];
    const w1=Math.sin(sp*6.28+t)*0.5+0.5, w2=Math.sin(sp*3.14-t*0.7)*0.5+0.5;
    cols[i]=scaleColor(hsvToRgb((w1*60+w2*30+130)%256,210,1),(w1*0.6+0.4)*cfg.disconnectedBrightness|0);}
}
function jsComet(cols,tMs){
  const rate=spdRate(cfg.disconnectedSpeed,0.0006,0.006);
  const pos=(tMs*rate)%1, hBase=pos*255;
  for(let i=0;i<N;i++){let d=Math.abs(SPATIAL_POS[i]-pos);if(d>0.5)d=1-d;
    if(d<0.28) cols[i]=scaleColor(hsvToRgb((hBase+d*50)%256,255,1),(1-d/0.28)*cfg.disconnectedBrightness|0);
    else cols[i]={r:0,g:0,b:0};}
}
function jsDisconnectedBase(cols,tMs){
  const dm=state.discMode!==undefined?state.discMode:cfg.disconnectedMode;
  switch(dm){
    case 0:jsRainbowWash(cols,tMs);break;case 1:jsSearchSweep(cols,tMs);break;
    case 2:jsWifiPing(cols,tMs);break;case 5:jsAurora(cols,tMs);break;
    case 8:jsComet(cols,tMs);break;default:jsRainbowWash(cols,tMs);break;
  }
}
function jsFlatBreath(cols,tMs){
  const bpm=8+(cfg.teamBreathSpeed/6)|0;
  const mn=Math.min(cfg.teamBreathMinBrightness,cfg.teamBreathMaxBrightness-1);
  const mx=Math.max(mn+1,cfg.teamBreathMaxBrightness);
  const wave=beatsin8(bpm,mn,mx,tMs);
  const tc=scaleColor(getTeamRgb(),wave);
  for(let i=0;i<N;i++) cols[i]={...tc};
}
function jsDiagonalBreath(cols,tMs,rotate){
  const bpm=8+(cfg.teamBreathSpeed/6)|0;
  const mn=Math.min(cfg.teamBreathMinBrightness,cfg.teamBreathMaxBrightness-1);
  const mx=Math.max(mn+1,cfg.teamBreathMaxBrightness);
  const tc=getTeamRgb();
  const cx=(LED_XY[4].x+LED_XY[13].x)*0.5, cy=(LED_XY[4].y+LED_XY[13].y)*0.5;
  const angle=rotate?((tMs*(0.00015+cfg.teamBreathRotationSpeed*0.000006))%(Math.PI*2))
                     :(cfg.teamBreathAngle/255*Math.PI*2);
  const cs=Math.cos(angle),sn=Math.sin(angle);
  for(let i=0;i<N;i++){
    const proj=(LED_XY[i].x-cx)*cs+(LED_XY[i].y-cy)*sn;
    const wave=beatsin8(bpm,mn,mx,tMs,(proj*92)&255);
    cols[i]=scaleColor(tc,wave);
  }
}
function jsCometOrbit(cols,tMs){
  const tc=getTeamRgb();
  const mn=Math.min(cfg.teamBreathMinBrightness,cfg.teamBreathMaxBrightness-1);
  const rate=spdRate(cfg.teamBreathSpeed,0.0003,0.003);
  const pos=(tMs*rate)%1;
  for(let i=0;i<N;i++){let d=Math.abs(SPATIAL_POS[i]-pos);if(d>0.5)d=1-d;
    const v=d<0.3?Math.max(mn,(1-d/0.3)*cfg.teamBreathMaxBrightness|0):mn;
    cols[i]=scaleColor(tc,v);}
}
function jsPowerCore(cols,tMs){
  const tc=getTeamRgb();
  const bpm=8+(cfg.teamBreathSpeed/6)|0;
  const mn=Math.min(cfg.teamBreathMinBrightness,cfg.teamBreathMaxBrightness-1);
  const wave=beatsin8(bpm,mn,cfg.teamBreathMaxBrightness,tMs);
  for(let i=0;i<N;i++){
    const dist=Math.abs(SPATIAL_POS[i]-0.47)*2, fade=Math.max(0.3,1-dist*0.7);
    cols[i]=scaleColor(tc,Math.max(mn,wave*fade|0));
  }
}
function jsRailGlow(cols,tMs){
  const tc=getTeamRgb();
  const bpm=6+(cfg.teamBreathSpeed/8)|0;
  const mn=Math.min(cfg.teamBreathMinBrightness,cfg.teamBreathMaxBrightness-1);
  const mx=Math.max(mn+1,cfg.teamBreathMaxBrightness);
  const wL=beatsin8(bpm,mn,mx,tMs,0), wR=beatsin8(bpm,mn,mx,tMs,128);
  for(let i=0;i<N;i++) cols[i]=scaleColor(tc,LED_XY[i].x<0.5?wL:LED_XY[i].x>1.5?wR:mn);
}
function jsTeamIdleBase(cols,tMs){
  const m=state.idleMode!==undefined?state.idleMode:cfg.teamIdleMode;
  switch(m){
    case 0:jsFlatBreath(cols,tMs);break;case 1:jsDiagonalBreath(cols,tMs,false);break;
    case 2:jsDiagonalBreath(cols,tMs,true);break;case 3:jsCometOrbit(cols,tMs);break;
    case 5:jsPowerCore(cols,tMs);break;case 6:jsRailGlow(cols,tMs);break;
    default:jsFlatBreath(cols,tMs);break;
  }
}
function jsChargeOverlay(cols,tMs){
  if(!state.chargeActive)return;
  if(state.cooldownUntilMs>0&&tMs<state.cooldownUntilMs)return;
  const el=tMs-state.chargeStartMs;
  const c=scaleColor(hexToRgb(cfg.chargeColor),cfg.chargeBrightness);
  switch(state.chargeMode!==undefined?state.chargeMode:cfg.chargeMode){
    case 0:{const p=beatsin8(18+(cfg.chargeSpeed/4)|0,70,255,tMs);
      blendLedC(cols,8,c,p);blendLedC(cols,9,c,p);blendLedC(cols,7,c,scale8(p,120));blendLedC(cols,10,c,scale8(p,120));blendLedC(cols,6,c,scale8(p,70));blendLedC(cols,11,c,scale8(p,70));break;}
    case 1:{const rate=spdRate(cfg.chargeSpeed,0.00018,0.00220);
      const phase=(el*rate)%1, headY=7*(1-phase), railW=0.3+cfg.chargePixels/18*5.5;
      for(let i=0;i<N;i++){if(i===8||i===9){blendLedC(cols,i,c,beatsin8(20,120,255,tMs));continue;}
        const d=Math.abs(LED_XY[i].y-headY);if(d<railW)blendLedC(cols,i,c,(1-d/railW)*255|0);}break;}
    case 2:{const rate=spdRate(cfg.chargeSpeed,0.00018,0.00220);
      const phase=(el*rate)%1, width=clamp(cfg.chargePixels/N,0.05,0.75);
      for(let i=0;i<N;i++){const pos=SPATIAL_POS[i];
        if(pos<=phase)blendLedC(cols,i,c,210);else if(pos<phase+width)blendLedC(cols,i,c,(1-(pos-phase)/width)*180|0);}break;}
    case 3:{const rate=spdRate(cfg.chargeSpeed,0.00018,0.00220);
      const phase=(el*rate)%1, angle=phase*Math.PI*2, cs=Math.cos(angle),sn=Math.sin(angle);
      const lw=0.18+cfg.chargePixels/18*1.6;
      for(let i=0;i<N;i++){const dx=LED_XY[i].x-1,dy=LED_XY[i].y-3,dist=Math.abs(dx*(-sn)+dy*cs);
        if(dist<lw)blendLedC(cols,i,c,(1-dist/lw)*220|0);}break;}
    default:{const p=beatsin8(28+(cfg.chargeSpeed/3)|0,90,255,tMs);
      blendLedC(cols,8,c,p);blendLedC(cols,9,c,p);blendLedC(cols,7,c,scale8(p,130));blendLedC(cols,10,c,scale8(p,130));break;}
  }
}
function jsCooldownLayer(cols,tMs){
  const cdMs=Math.max(1,state.cooldownMs||cfg.cooldownMs);
  const cdStart=state.cooldownUntilMs-cdMs;
  const el=Math.max(0,tMs-cdStart);
  const prog=clamp(el/cdMs,0,1);

  // Chirp blink matching C++: phi(t) = fSlow*t + (fFast-fSlow)*t^2/(2*T)
  const fSlow=0.9, fFast=16.0;
  const tSec=el/1000, TSec=cdMs/1000;
  const bPhase=fSlow*tSec+(fFast-fSlow)*tSec*tSec/(2*Math.max(TSec,0.001));
  const sinVal=Math.sin(bPhase*Math.PI*2)*0.5+0.5;
  const brt=cfg.cooldownBrightness;
  const blinkBrt=Math.max(brt>>3, sinVal*brt|0);

  const total=Math.min(5,state.maxShots||cfg.maxBurstShots);
  const red=scaleColor({r:255,g:0,b:0},blinkBrt);
  const tc=getTeamRgb();
  const cmode=state.cooldownMode!==undefined?state.cooldownMode:cfg.cooldownMode;
  if(cmode===0){
    const ret=Math.floor(prog*(total+0.001)), rem=total-Math.min(total,ret);
    for(let p=0;p<rem;p++){cols[SHOT_PAIRS[p][0]]={...red};cols[SHOT_PAIRS[p][1]]={...red};}
  } else {
    const mixed=blendColor({r:255,g:0,b:0},tc,prog);
    const cv=scaleColor(mixed,blinkBrt);
    for(let p=0;p<total;p++){cols[SHOT_PAIRS[p][0]]={...cv};cols[SHOT_PAIRS[p][1]]={...cv};}
  }
}
function jsPartialDecayActive(tMs){
  if(!state.partialDecayStartCount)return false;
  if(tMs>=state.partialDecayUntilMs){
    state.partialDecayStartCount=0;
    state.partialDecayStartMs=0;
    state.partialDecayUntilMs=0;
    state.burstShots=0;
    return false;
  }
  return true;
}
function jsRenderPartialDecay(cols,tMs){
  if(!jsPartialDecayActive(tMs))return;
  const elapsed=tMs-state.partialDecayStartMs;
  const stepsCleared=Math.min(state.partialDecayStartCount,Math.floor(elapsed/state.partialDecayStepMs));
  const visible=state.partialDecayStartCount-stepsCleared;
  if(visible<=0)return;
  const local=elapsed%state.partialDecayStepMs;
  const fadeOut=mapR(local,0,state.partialDecayStepMs-1,255,40)|0;
  const newest=visible-1;
  for(let p=0;p<visible;p++){
    const sc2=scaleColor(hexToRgb(cfg.shotPairColors[p]),cfg.chargeBrightness);
    const a=p===newest?fadeOut:235;
    blendLedC(cols,SHOT_PAIRS[p][0],sc2,a);
    blendLedC(cols,SHOT_PAIRS[p][1],sc2,a);
  }
}
function jsShotMeter(cols,tMs){
  if(state.cooldownUntilMs>0&&tMs>=state.cooldownUntilMs){state.cooldownUntilMs=0;state.burstShots=0;state.partialDecayStartCount=0;return;}
  const inCd=state.cooldownUntilMs>0&&tMs<state.cooldownUntilMs;
  if(inCd){jsCooldownLayer(cols,tMs);return;}
  if(jsPartialDecayActive(tMs)){jsRenderPartialDecay(cols,tMs);return;}
  if(state.burstShots===0)return;
  const filled=Math.min(5,state.burstShots);
  const lp=beatsin8(14,175,255,tMs);
  for(let p=0;p<filled;p++){
    const sc2=scaleColor(hexToRgb(cfg.shotPairColors[p]),cfg.chargeBrightness);
    const a=p===filled-1?lp:245;
    blendLedC(cols,SHOT_PAIRS[p][0],sc2,a);blendLedC(cols,SHOT_PAIRS[p][1],sc2,a);
  }
}
function jsFireOverlay(cols,tMs){
  if(!state.fireActive)return;
  if(state.cooldownUntilMs>0&&tMs<state.cooldownUntilMs){state.fireActive=false;return;}
  const dur=mapR(cfg.fireSpeed,1,100,1200,180);
  const el=tMs-state.fireStartMs;
  if(el>=dur){state.fireActive=false;return;}
  const prog=el/dur, width=cfg.firePixels/N;
  const c=scaleColor(hexToRgb(cfg.fireColor),cfg.fireBrightness);
  const fm=state.fireMode!==undefined?state.fireMode:cfg.fireMode;
  switch(fm){
    case 0:for(let i=0;i<N;i++){const sp=clamp(LED_XY[i].y/7,0,1),d=Math.abs(sp-prog);if(d<width)blendLedC(cols,i,c,255);else if(d<width+0.18)blendLedC(cols,i,c,(1-(d-width)/0.18)*cfg.fireFade|0);}break;
    case 1:for(let i=0;i<N;i++){const d=Math.abs(SPATIAL_POS[i]-prog);if(d<width)blendLedC(cols,i,c,255);else if(d<width+0.20)blendLedC(cols,i,c,(1-(d-width)/0.20)*cfg.fireFade|0);}break;
    case 2:{const a=(1-prog)*255|0;for(let i=0;i<N;i++)blendLedC(cols,i,c,scale8(a,70));blendLedC(cols,0,c,a);blendLedC(cols,17,c,a);blendLedC(cols,1,c,scale8(a,220));blendLedC(cols,16,c,scale8(a,220));break;}
    case 3:{const head=1-prog;for(let i=0;i<N;i++){const d=Math.abs(SPATIAL_POS[i]-head);if(d<width)blendLedC(cols,i,c,255);else if(d<width+0.20)blendLedC(cols,i,c,(1-(d-width)/0.20)*cfg.fireFade|0);}break;}
    case 7:{const dec=(1-prog)*255|0,thr=mapR(dec,0,255,15,170)|0;for(let i=0;i<N;i++)if(Math.random()*255<thr)blendLedC(cols,i,c,80+Math.random()*175|0);break;}
    case 8:{const mix=blendColor(c,getTeamRgb(),prog);const bv=(1-prog*0.8)*255|0;for(let i=0;i<N;i++)blendLedC(cols,i,scaleColor(mix,bv),240);break;}
    case 9:{const a=(1-prog)*(1-prog)*255|0;for(let i=0;i<N;i++)blendLedC(cols,i,scaleColor({r:255,g:255,b:255},scale8(a,cfg.fireBrightness)),a);break;}
    default:for(let i=0;i<N;i++){const d=Math.abs(SPATIAL_POS[i]-prog);if(d<width)blendLedC(cols,i,c,255);}break;
  }
}
function jsCountdownOverlay(cols,tMs){
  if(!state.countdownActive)return;
  const dur=state.countdownDurationMs, el=tMs-state.countdownStartMs;
  if(el>=dur){state.countdownActive=false;
    const b=scaleColor(hexToRgb(cfg.countdownColorB),cfg.countdownBrightness);for(let i=0;i<N;i++)cols[i]={...b};return;}
  const prog=el/dur, secLeft=Math.ceil((dur-el)/1000);
  const cA=scaleColor(hexToRgb(cfg.countdownColorA),cfg.countdownBrightness);
  const cB=scaleColor(hexToRgb(cfg.countdownColorB),cfg.countdownBrightness);
  const tickP=mapR(cfg.countdownSpeed,1,100,1400,300);
  const tickPulse=sin8((el%tickP)/tickP*255|0);
  const cm=cfg.countdownMode;
  switch(cm){
    case 0:default:{const dim=scaleColor(getTeamRgb(),18);for(let i=0;i<N;i++)cols[i]={...dim};
      const cnt=(1-prog)*N|0;for(let s=0;s<cnt;s++)blendLedC(cols,LED_AT_STEP[s],cA,220);break;}
    case 1:{for(let i=0;i<N;i++)cols[i]={r:0,g:0,b:0};
      const tc=secLeft<=3?cB:secLeft<=6?cA:scaleColor({r:255,g:0,b:0},cfg.countdownBrightness);
      const fillN=mapR(cfg.countdownSeconds-secLeft+1,0,cfg.countdownSeconds,1,N)|0;
      for(let s=0;s<fillN&&s<N;s++)blendLedC(cols,LED_AT_STEP[s],tc,240);
      blendLedC(cols,8,scaleColor({r:255,g:255,b:255},cfg.countdownBrightness),scale8(tickPulse,160));
      blendLedC(cols,9,scaleColor({r:255,g:255,b:255},cfg.countdownBrightness),scale8(tickPulse,160));break;}
    case 2:{const pc=secLeft<=1?cB:cA;const bv=mapR(tickPulse,0,255,45,cfg.countdownBrightness)|0;
      const fc=scaleColor(pc,bv);for(let i=0;i<N;i++)cols[i]={...fc};break;}
    case 3:{const dim=scaleColor(getTeamRgb(),18);for(let i=0;i<N;i++)cols[i]={...dim};
      const head=(el%1000)/1000*N|0;
      for(let o=-3;o<=3;o++){const s=(head+o+N)%N;blendLedC(cols,LED_AT_STEP[s],cA,255-Math.min(220,Math.abs(o)*58));}break;}
    case 4:{const dim=scaleColor(getTeamRgb(),22);for(let i=0;i<N;i++)cols[i]={...dim};
      const p=(el%1000)/1000, lc=secLeft<=2?cB:cA;
      for(let i=0;i<N;i++){const sp=SPATIAL_POS[i];if(Math.abs(sp-p)<0.10||Math.abs(sp-(1-p))<0.10)blendLedC(cols,i,lc,240);}break;}
    case 7:{const lOn=(secLeft%2)===0; const bv=scale8(tickPulse,cfg.countdownBrightness);
      for(let i=0;i<N;i++)cols[i]=(LED_XY[i].x<0.5)===lOn?scaleColor(cA,bv):scaleColor(cB,bv>>1);break;}
  }
}
function jsIdentifyOverlay(cols,tMs){
  if(state.identifyLed<0)return;
  if(tMs>=state.identifyUntilMs){state.identifyLed=-1;return;}
  const on=((tMs/80)|0)%2===0;
  cols[state.identifyLed]=on?{r:255,g:255,b:255}:{r:0,g:0,b:0};
}
function jsLowBatteryOverlay(cols,tMs){
  if(!state.lowBattery)return;
  let w={r:0,g:0,b:0};
  switch(cfg.lowBatteryMode){
    case 0:{const on=((tMs/220)|0)%2===0;w=on?scaleColor({r:255,g:0,b:0},cfg.lowBatteryBrightness):{r:22,g:0,b:0};break;}
    case 1:{const t=tMs%800,on=t<80||(t>170&&t<260);w=on?scaleColor({r:255,g:0,b:0},cfg.lowBatteryBrightness):{r:8,g:0,b:0};break;}
    case 2:{const t=tMs%1200;let pk;
      if(t<100)pk=t/100;else if(t<200)pk=1-(t-100)/100;else if(t<320)pk=(t-200)/120*0.6;
      else if(t<430)pk=0.6-(t-320)/110*0.6;else pk=0;
      const v=pk*cfg.lowBatteryBrightness|0;w={r:v,g:0,b:0};break;}
    case 3:{const e=beatsin8(4,40,cfg.lowBatteryBrightness,tMs);w={r:e,g:0,b:0};break;}
    default:{const v=cfg.lowBatteryBrightness;w={r:v,g:0,b:0};break;}
  }
  cols[8]={...w}; cols[9]={...w};
}

// Main frame compositor
function renderMohawkFrame(tMs){
  const cols=new Array(N).fill(null).map(()=>({r:0,g:0,b:0}));
  if(!state.connected) jsDisconnectedBase(cols,tMs); else jsTeamIdleBase(cols,tMs);
  if(layerVis.charge) jsChargeOverlay(cols,tMs);
  if(layerVis.shot)   jsShotMeter(cols,tMs);
  if(layerVis.fire)   jsFireOverlay(cols,tMs);
  if(layerVis.countdown) jsCountdownOverlay(cols,tMs);
  if(layerVis.identify)  jsIdentifyOverlay(cols,tMs);
  if(layerVis.lowbat)    jsLowBatteryOverlay(cols,tMs); // ALWAYS LAST
  // Scale by global brightness
  const gb=cfg.globalBrightness/255;
  for(let i=0;i<N;i++){cols[i].r=cols[i].r*gb|0;cols[i].g=cols[i].g*gb|0;cols[i].b=cols[i].b*gb|0;}
  return cols;
}

// ============================================================
// DOM — LED elements (built once)
// ============================================================
function buildLedElements(){
  const canvas=document.getElementById('ledCanvas');
  canvas.innerHTML='';
  ledEls=[];
  for(let i=0;i<N;i++){
    const [cx,cy]=LED_PX[i];
    const el=document.createElement('div');
    el.className='led-dot'+(i===8||i===9?' bat-led':'');
    el.id='led'+i;
    el.style.left=(cx-22)+'px'; el.style.top=(cy-22)+'px';
    const num=document.createElement('span'); num.className='led-num'; num.textContent=i;
    el.appendChild(num);
    el.addEventListener('mouseenter',()=>showTooltip(i,el));
    el.addEventListener('mouseleave',hideTooltip);
    el.addEventListener('click',()=>{fetch('/api/mohawk/test?what=identify&led='+i);});
    canvas.appendChild(el); ledEls.push(el);
  }
}

function updateLedElements(cols){
  for(let i=0;i<N;i++){
    const {r,g,b}=cols[i];
    const hex=rgbToHex({r,g,b});
    const lum=(r*299+g*587+b*114)/1000;
    const glow=Math.min(0.85,lum/180).toFixed(2);
    ledEls[i].style.setProperty('--led-color',hex);
    ledEls[i].style.setProperty('--led-glow','rgba('+r+','+g+','+b+','+glow+')');
    ledColors[i]={r,g,b};
  }
}

// ============================================================
// TOOLTIP
// ============================================================
function showTooltip(i,el){
  const tt=document.getElementById('tooltip');
  const {r,g,b}=ledColors[i];
  const sp=SPATIAL_POS[i].toFixed(3);
  const pairIdx=SHOT_PAIRS.findIndex(p=>p[0]===i||p[1]===i);
  let html='<strong>LED '+i+'</strong><br>';
  html+=LED_ROLES[i]+'<br>';
  html+='XY: ('+LED_XY[i].x.toFixed(1)+', '+LED_XY[i].y.toFixed(1)+')<br>';
  html+='Path pos: '+sp+'<br>';
  if(pairIdx>=0)html+='Shot pair: '+(pairIdx+1)+'<br>';
  if(i===8||i===9)html+='<span style="color:var(--warn)">Battery warning LED</span><br>';
  html+='Color: '+rgbToHex({r,g,b});
  tt.innerHTML=html;
  const rect=el.getBoundingClientRect();
  tt.style.left=(rect.right+8)+'px'; tt.style.top=(rect.top-10)+'px';
  tt.style.display='block';
}
function hideTooltip(){document.getElementById('tooltip').style.display='none';}

// ============================================================
// ANIMATION LOOP — 60+ FPS via requestAnimationFrame
// ============================================================
function animationLoop(timestamp){
  const dt=timestamp-lastFrameMs; lastFrameMs=timestamp;
  simTimeMs+=dt;
  // FPS tracking
  fpsCount++;
  if(timestamp-lastFpsMs>=1000){fps=fpsCount;fpsCount=0;lastFpsMs=timestamp;document.getElementById('fpsVal').textContent=fps;}
  // Telemetry age
  if(lastTelemMs>0){const age=(timestamp-lastTelemMs)|0;document.getElementById('telemAge').textContent=age;}
  // Lab time advance
  if(labManual&&labPlaying) labTimeMs+=dt*labSpeed;
  let tMs=labManual?labTimeMs:simTimeMs;
  // Render
  let cols;
  if(labHwMode&&hwLedColors) cols=hwLedColors.map(h=>hexToRgb(h));
  else if(labAnimKey) cols=renderLabOnly(labAnimKey,tMs);
  else cols=renderMohawkFrame(tMs);
  updateLedElements(cols);
  updateLayerInspector(tMs);
  requestAnimationFrame(animationLoop);
}

function renderLabOnly(key,tMs){
  const cols=new Array(N).fill(null).map(()=>({r:0,g:0,b:0}));
  switch(key){
    case 'disc-rainbow':jsRainbowWash(cols,tMs);break;
    case 'disc-sweep':jsSearchSweep(cols,tMs);break;
    case 'disc-ping':jsWifiPing(cols,tMs);break;
    case 'disc-aurora':jsAurora(cols,tMs);break;
    case 'disc-comet':jsComet(cols,tMs);break;
    case 'team-flat':jsFlatBreath(cols,tMs);break;
    case 'team-diag':jsDiagonalBreath(cols,tMs,false);break;
    case 'team-rotate':jsDiagonalBreath(cols,tMs,true);break;
    case 'team-orbit':jsCometOrbit(cols,tMs);break;
    case 'team-power':jsPowerCore(cols,tMs);break;
    default:jsRainbowWash(cols,tMs);break;
  }
  const gb=cfg.globalBrightness/255;
  for(let i=0;i<N;i++){cols[i].r=cols[i].r*gb|0;cols[i].g=cols[i].g*gb|0;cols[i].b=cols[i].b*gb|0;}
  return cols;
}

// ============================================================
// TELEMETRY (100ms poll — state sync only)
// ============================================================
async function fetchTelemetry(){
  try{
    const r=await fetch('/api/mohawk/telemetry'); const d=await r.json();
    const now=performance.now(); lastTelemMs=now;
    // State sync
    state.connected=d.conn; state.lowBattery=d.lowBat; state.team=d.team;
    state.discMode=d.discMode; state.idleMode=d.idleMode;
    state.burstShots=d.burst; state.maxShots=d.maxShots;
    state.partialDecayStepMs=d.decayStepMs||320;
    if(d.decayAct){
      const total=(d.decayCount||0)*state.partialDecayStepMs;
      const elapsed=Math.max(0,total-(d.decayRem||0));
      state.partialDecayStartCount=d.decayCount||0;
      state.partialDecayStartMs=now-elapsed;
      state.partialDecayUntilMs=now+(d.decayRem||0);
    } else {
      state.partialDecayStartCount=0; state.partialDecayStartMs=0; state.partialDecayUntilMs=0;
    }
    state.totalShots=d.totalShots; state.solArmed=d.solArmed;
    state.selfTestActive=d.selfTest; state.selfTestStep=d.selfTestStep;
    state.cooldownMs=d.cdMs; state.cooldownMode=d.cdMode;
    state.chargeMode=d.chgMode; state.fireMode=d.fireMode;
    // Cooldown sync
    if(d.cdRem>0) state.cooldownUntilMs=now+d.cdRem; else state.cooldownUntilMs=0;
    // Charge sync
    if(d.chgAct){if(!state.chargeActive)state.chargeStartMs=now-d.chgElMs;state.chargeActive=true;}
    else state.chargeActive=false;
    // Fire sequence
    if(d.fireSeq!==state.fireEventSeq){state.fireEventSeq=d.fireSeq;state.fireStartMs=now;state.fireActive=true;state.fireMode=d.fireMode;}
    // Countdown sync
    if(d.cntAct&&!state.countdownActive){
      state.countdownActive=true; state.countdownDurationMs=d.cntSec*1000;
      state.countdownStartMs=now-Math.max(0,(state.countdownDurationMs-d.cntRem));
    }else if(!d.cntAct) state.countdownActive=false;
    state.countdownSeconds=d.cntSec;
    // Identify
    if(d.identifyLed>=0){state.identifyLed=d.identifyLed;state.identifyUntilMs=now+600;}
    updateHud();
    if(!configFetched)fetchConfig();
  }catch(e){}
}

async function fetchConfig(){
  try{
    const r=await fetch('/api/mohawk/config'); const d=await r.json();
    Object.assign(cfg,d);
    if(Array.isArray(d.shotPairColors))cfg.shotPairColors=d.shotPairColors;
    applyConfigToUI(); configFetched=true;
  }catch(e){}
}

function applyConfigToUI(){
  const ids=['globalBrightness','disconnectedMode','disconnectedSpeed','disconnectedBrightness',
    'team','teamIdleMode','teamBreathSpeed','teamBreathMinBrightness','teamBreathMaxBrightness',
    'teamBreathAngle','teamBreathRotationSpeed','chargeMode','chargeSpeed','chargePixels','chargeBrightness',
    'fireMode','fireSpeed','firePixels','fireFade','fireBrightness',
    'shotMeterMode','maxBurstShots','burstWindowMs','cooldownMs','cooldownMode','cooldownBrightness',
    'countdownMode','countdownSeconds','countdownSpeed','countdownBrightness',
    'lowBatteryMode','lowBatteryBrightness'];
  for(const id of ids){const el=document.getElementById(id);if(el&&cfg[id]!==undefined){el.value=cfg[id];const v=document.getElementById(id+'Val');if(v)v.textContent=cfg[id];}}
  const colorIds={teamBlue:'teamBlue',teamOrange:'teamOrange',chargeColor:'chargeColor',fireColor:'fireColor',countdownColorA:'countdownColorA',countdownColorB:'countdownColorB'};
  for(const[id,k]of Object.entries(colorIds)){const el=document.getElementById(id);if(el&&cfg[k])el.value=cfg[k];}
  for(let i=0;i<5;i++){const el=document.getElementById('shotPairColor'+i);if(el&&cfg.shotPairColors[i])el.value=cfg.shotPairColors[i];}
  const diag=document.getElementById('teamBreathDiagonal');if(diag)diag.checked=!!cfg.teamBreathDiagonal;
  const rot=document.getElementById('teamBreathRotate');if(rot)rot.checked=!!cfg.teamBreathRotate;
}

// ============================================================
// HUD UPDATE
// ============================================================
function updateHud(){
  const stEl=document.getElementById('hudState');
  if(stEl){stEl.textContent=state.connected?'Connected':'Disconnected';stEl.className='hud-v '+(state.connected?'ok':'err');}
  const burstEl=document.getElementById('hudBurst');if(burstEl)burstEl.textContent=state.burstShots+'/'+(state.maxShots||5);
  const coolEl=document.getElementById('hudCool');
  if(coolEl){const rem=Math.max(0,state.cooldownUntilMs-performance.now());coolEl.textContent=(rem/1000).toFixed(1)+'s';coolEl.className='hud-v '+(rem>0?'err':'dim');}
  const cntEl=document.getElementById('hudCnt');
  if(cntEl){if(state.countdownActive){const rem=Math.max(0,state.countdownDurationMs-(performance.now()-state.countdownStartMs));cntEl.textContent=(rem/1000).toFixed(1)+'s';cntEl.className='hud-v warn';}else{cntEl.textContent='--';cntEl.className='hud-v dim';}}
  const animEl=document.getElementById('hudAnim');
  if(animEl) animEl.textContent=state.connected?(IDLE_NAMES[state.idleMode]||'?'):(DISC_NAMES[state.discMode]||'?');
  const shotsEl=document.getElementById('hudShots');if(shotsEl)shotsEl.textContent=state.totalShots;
  const batEl=document.getElementById('hudBat');if(batEl){batEl.textContent=state.lowBattery?'ON':'OFF';batEl.className='hud-v '+(state.lowBattery?'err':'dim');}
  const armedEl=document.getElementById('hudArmed');if(armedEl){armedEl.textContent=state.solArmed?'YES':'NO';armedEl.className='hud-v '+(state.solArmed?'warn':'dim');}
}

// ============================================================
// LAYER INSPECTOR
// ============================================================
function updateLayerInspector(){
  const el=document.getElementById('layerInspector');if(!el)return;
  const layers=[
    {key:'base',name:'Base (Disconnected/Idle)',color:'#2be4ff',always:false},
    {key:'charge',name:'Charge Overlay',color:'#00ffaa',always:false},
    {key:'shot',name:'Shot Meter + Cooldown',color:'#ffbe00',always:false},
    {key:'fire',name:'Fire Overlay',color:'#ff4400',always:false},
    {key:'countdown',name:'Countdown',color:'#ffbe00',always:false},
    {key:'identify',name:'Identify',color:'#ffffff',always:false},
    {key:'lowbat',name:'Low Battery (ALWAYS ON)',color:'#ff0033',always:true}
  ];
  el.innerHTML=layers.map(l=>{
    const active=layerVis[l.key];
    return '<div class="layer-row">'
      +'<div class="layer-swatch" style="background:'+l.color+';opacity:'+(active?1:0.25)+'"></div>'
      +'<span class="layer-name '+(active?'layer-active':'layer-inactive')+'">'+l.name+'</span>'
      +(l.always?'':`<button style="font-size:10px;background:#0c1e36;border:1px solid var(--line);border-radius:5px;color:var(--muted);cursor:pointer;padding:2px 7px" onclick="toggleLayer('`+l.key+`')">`+(active?'Hide':'Show')+'</button>')
      +'</div>';
  }).join('');
}
function toggleLayer(k){if(k!=='lowbat'){layerVis[k]=!layerVis[k];updateLayerInspector();}}

// ============================================================
// DIAGNOSTICS
// ============================================================
async function fetchDiag(){
  try{const r=await fetch('/api/mohawk/diagnostics');const d=await r.json();
    const el=document.getElementById('diagInfo');if(!el)return;
    el.innerHTML=[
      'Uptime: '+(d.uptime/1000).toFixed(1)+'s',
      'Free heap: '+d.freeHeap+' bytes',
      'Min heap: '+d.minFreeHeap+' bytes',
      'Physical LEDs: '+(d.physLeds?'YES':'NO'),
      'Standalone: '+(d.standalone?'YES':'NO'),
      'FMS mode: '+(d.fmsMode?'YES':'NO'),
      'Solenoid pin: '+d.solPin,
      'Solenoid armed: '+(d.solArmed?'YES':'NO'),
      'AP IP: '+(d.apIp||'N/A'),
      'SSID: '+(d.ssid||'N/A'),
      'Firmware: '+(d.fw||'?'),
      'Browser FPS: '+fps,
      'Self-test step: '+d.selfTestStep
    ].map(t=>'<div>'+t+'</div>').join('');
    // Update event log from ESP32
    if(d.evtLog){
      d.evtLog.forEach(e=>{
        if(!browserEvtLog.find(b=>b.ms===e.ms&&b.msg===e.msg)){
          browserEvtLog.unshift({ms:e.ms,sev:e.sev,msg:e.msg});
        }
      });
      if(browserEvtLog.length>80)browserEvtLog.splice(80);
      renderEvtLog();
    }
  }catch(e){}
}
function renderEvtLog(){
  const el=document.getElementById('evtLog');if(!el)return;
  if(browserEvtLog.length===0){el.innerHTML='<div style="color:var(--muted)">No events yet.</div>';return;}
  el.innerHTML=browserEvtLog.slice(0,50).map(e=>{
    const cls=e.sev===0?'evt-info':e.sev===1?'evt-warn':'evt-err';
    const ts=(e.ms/1000).toFixed(1)+'s';
    return '<div class="evt-entry"><span class="evt-ms">'+ts+'</span><span class="'+cls+'">'+e.msg+'</span></div>';
  }).join('');
}
function clearEvtLog(){browserEvtLog=[];renderEvtLog();}
function addEvt(sev,msg){browserEvtLog.unshift({ms:performance.now(),sev,msg});if(browserEvtLog.length>80)browserEvtLog.pop();renderEvtLog();}

async function fetchHwLeds(){
  try{const r=await fetch('/api/mohawk/hardware-leds');const d=await r.json();
    hwLedColors=d;
    document.getElementById('hwLedOut').textContent=d.join(' ');
    if(labHwMode)updateLedElements(d.map(h=>hexToRgb(h)));
  }catch(e){document.getElementById('hwLedOut').textContent='Error fetching hardware LEDs';}
}

// ============================================================
// UI CONTROL HELPERS
// ============================================================
function qs(id){return document.getElementById(id);}
function switchTab(name){
  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
  document.querySelectorAll('.tabpane').forEach(p=>p.classList.remove('active'));
  const names=['state','breath','solenoid','countdown','lab','diag','settings'];
  const idx=names.indexOf(name);
  if(idx>=0) document.querySelectorAll('.tab-btn')[idx].classList.add('active');
  const tab=document.getElementById('tab-'+name);if(tab)tab.classList.add('active');
  if(name==='diag')fetchDiag();
}
function rng(el,k){const v=document.getElementById(k+'Val');if(v)v.textContent=el.value;set(k,el.value);}
async function set(k,v){
  try{await fetch('/api/mohawk/config/apply?'+encodeURIComponent(k)+'='+encodeURIComponent(v));
    if(cfg.hasOwnProperty(k)){
      if(typeof cfg[k]==='boolean') cfg[k]=parseInt(v)!==0;
      else if(typeof cfg[k]==='number') cfg[k]=parseFloat(v);
      else cfg[k]=v;
    }
  }catch(e){}
}
async function cmd(what,extra=''){
  try{await fetch('/api/mohawk/test?what='+what+extra);}catch(e){}
}
async function doFire(){
  if(!state.connected)await cmd('connected');
  await cmd('fire');
  state.fireStartMs=performance.now(); state.fireActive=true;
}
async function doBurst5(){
  if(!state.connected)await cmd('connected');
  await cmd('burst5');
  state.burstShots=5;
  state.cooldownUntilMs=performance.now()+cfg.cooldownMs;
}
async function doSave(){await cmd('save');addEvt(0,'Settings saved to ESP32');}
async function doFactory(){if(confirm('Factory reset all settings?')){await cmd('factory');await fetchConfig();addEvt(1,'Factory reset');}}
async function doRecipe(){try{const r=await fetch('/api/mohawk/recipe');qs('recipeOut').value=await r.text();}catch(e){}}
function copyRecipe(){const el=qs('recipeOut');el.select();document.execCommand('copy');}
async function identifyAll(){for(let i=0;i<N;i++){await cmd('identify','&led='+i);await new Promise(r=>setTimeout(r,200));}}

// Animation lab
function labSetMode(v){labManual=(v==='manual');labHwMode=(v==='hw');}
function labSetAnim(v){labAnimKey=v;}
function labPlay(){labPlaying=true;}
function labPause(){labPlaying=false;}
function labRestart(){labTimeMs=0;qs('labScrub').value=0;}
function labToggleLoop(){labLooping=!labLooping;qs('labLoopBtn').textContent='Loop: '+(labLooping?'ON':'OFF');}
function labSetSpeed(s){labSpeed=s;}
function labScrub(v){labTimeMs=parseFloat(v);qs('labTimeVal').textContent=(v/1000).toFixed(1)+'s';}

// ============================================================
// INIT
// ============================================================
document.addEventListener('DOMContentLoaded',()=>{
  buildLedElements();
  requestAnimationFrame(animationLoop);
  setInterval(fetchTelemetry, 100);
  setInterval(fetchDiag, 3000);
  addEvt(0,'Web UI loaded');
});
</script>
</body>
</html>
)RAWHTML";

// ============================================================
// ROUTE REGISTRATION
// ============================================================
void registerMohawkRoutes(WebServer &svr) {
  _srv = &svr;
  svr.on("/",                         HTTP_GET, handleRoot);
  svr.on("/api/mohawk/telemetry",     HTTP_GET, handleTelemetry);
  svr.on("/api/mohawk/state",         HTTP_GET, handleTelemetry);
  svr.on("/api/mohawk/config",        HTTP_GET, handleConfig);
  svr.on("/api/mohawk/config/apply",  HTTP_GET, handleConfigApply);
  svr.on("/api/mohawk/test",          HTTP_GET, handleTestCmd);
  svr.on("/api/mohawk/save",          HTTP_GET, handleSave);
  svr.on("/api/mohawk/factory",       HTTP_GET, handleFactory);
  svr.on("/api/mohawk/recipe",        HTTP_GET, handleRecipe);
  svr.on("/api/mohawk/diagnostics",   HTTP_GET, handleDiagnostics);
  svr.on("/api/mohawk/hardware-leds", HTTP_GET, handleHardwareLeds);
  logEvent(0, "Routes registered under /api/mohawk/");
}

// ============================================================
// SETUP_MOHAWK — call from setup() in FMS or standalone
// ============================================================
void setup_mohawk() {
  buildXYMap();
  loadSettings();
  initRuntime();

#if MOHAWK_PHYSICAL_LEDS_ENABLED
  pinMode(MOHAWK_LED_PIN, OUTPUT);
  FastLED.addLeds<LED_CHIPSET, MOHAWK_LED_PIN, LED_COLOR_ORDER>(leds, MOHAWK_LED_COUNT);
  FastLED.setBrightness(cfg.globalBrightness);
  FastLED.clear(true);
#endif

#if SOLENOID_TEST_PIN >= 0
  pinMode(SOLENOID_TEST_PIN, OUTPUT);
  digitalWrite(SOLENOID_TEST_PIN, SOLENOID_TEST_ACTIVE_HIGH ? LOW : HIGH);
#endif

  logEvent(0, "Mohawk LED Controller v2.2 ready");
  Serial.println(F("[Mohawk] LED Controller v2.2 init OK"));
}

// ============================================================
// STANDALONE SETUP + LOOP
// (In FMS mode, comment these out and use FMS setup/loop)
// ============================================================
#if MOHAWK_STANDALONE_TEST_MODE

void setup() {
  Serial.begin(115200);
  delay(200);
  random16_add_entropy((uint16_t)analogRead(0));

  setup_mohawk();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(MOHAWK_AP_SSID, MOHAWK_AP_PASS);
  delay(200);

  registerMohawkRoutes(_standaloneServer);
  _standaloneServer.begin();

  logEvent(0, "Standalone AP started");
  Serial.print(F("[Mohawk] AP SSID: "));
  Serial.println(MOHAWK_AP_SSID);
  Serial.print(F("[Mohawk] UI:  http://"));
  Serial.println(WiFi.softAPIP());
  Serial.println(F("[Mohawk] Ready."));
}

void loop() {
  _standaloneServer.handleClient();
  mohawk_service();
}

#endif // MOHAWK_STANDALONE_TEST_MODE

/*
  ============================================================
  FMS INTEGRATION TEMPLATE (if MOHAWK_FMS_INTEGRATED_MODE 1):

  In your FMS setup():
    setup_mohawk();
    // ... your WiFi connect code ...
    registerMohawkRoutes(yourWebServer);
    yourWebServer.begin();

  In your FMS loop():
    yourWebServer.handleClient();
    mohawk_service();

  Available public functions:
    mohawk_set_connected(bool)           - robot on/off field
    mohawk_set_low_battery(bool)         - show low battery on LEDs 8+9
    mohawk_start_solenoid_charge()       - start charge animation
    mohawk_stop_solenoid_charge()        - stop charge animation
    mohawk_trigger_solenoid_animation()  - fire! (tracks burst count + cooldown)
    mohawk_reset_shot_meter()            - clear burst counter
    mohawk_start_countdown(uint8_t sec)  - start countdown animation
    mohawk_stop_countdown()              - stop countdown
    armSolenoid()                        - arm physical solenoid (if pin set)
    disarmSolenoid()                     - disarm physical solenoid
    triggerSolenoidPulse()               - fire physical solenoid pulse
  ============================================================
*/
