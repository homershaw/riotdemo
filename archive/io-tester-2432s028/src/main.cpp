#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

static constexpr const char* FW="Riot-IOTEST-CYD-v24";
static constexpr uint8_t MODBUS_DEFAULT_SLAVE=1;
static constexpr uint32_t MODBUS_BAUD=9600;
static constexpr int RS485_TX=22;
static constexpr int RS485_RX=35;
static constexpr int TOUCH_IRQ=36;
static constexpr int TOUCH_MISO=39;
static constexpr int TOUCH_MOSI=32;
static constexpr int TOUCH_CLK=25;
static constexpr int TOUCH_CS=33;
static constexpr int UI_W=320;
static constexpr int UI_H=240;
static constexpr int UI_LEFT=6;
static constexpr int UI_RIGHT=314;
static constexpr int FOOTER_Y=208;

TFT_eSPI tft;
TFT_eSPI& canvas=tft;
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS,TOUCH_IRQ);
HardwareSerial modbus(2);
WebServer server(80);
WiFiManager wm;

bool diState[4]={};
bool doState[4]={};
float ai_mA[8]={};
float ao_mA[4]={};
float aoForcePct[4]={};
bool modbusOnline=false;
uint8_t modbusSlave=MODBUS_DEFAULT_SLAVE;
uint32_t modbusGood=0,modbusBad=0,modbusTimeouts=0,modbusCrcFails=0;
uint32_t lastPoll=0,lastScreen=0;
uint8_t pollPhase=0;
String lastTx,lastRx,lastError="BOOT";

enum Page:uint8_t{PAGE_AI=0,PAGE_DI=1,PAGE_DO=2,PAGE_AO=3,PAGE_COMMS=4};
Page page=PAGE_AI;

uint16_t crc16(const uint8_t* p,size_t n){uint16_t c=0xFFFF;for(size_t i=0;i<n;i++){c^=p[i];for(uint8_t b=0;b<8;b++)c=(c&1)?((c>>1)^0xA001):(c>>1);}return c;}
String hexFrame(const uint8_t* p,size_t n){String s;char b[4];for(size_t i=0;i<n;i++){snprintf(b,sizeof(b),"%02X",p[i]);if(i)s+=' ';s+=b;}return s;}

bool transact(uint8_t* req,size_t reqLen,uint8_t* rsp,size_t& count,size_t minCount,uint32_t timeout=150){while(modbus.available())modbus.read();uint16_t c=crc16(req,reqLen-2);req[reqLen-2]=c&0xFF;req[reqLen-1]=c>>8;lastTx=hexFrame(req,reqLen);modbus.write(req,reqLen);modbus.flush();count=0;uint32_t st=millis();while(millis()-st<timeout){while(modbus.available()&&count<64)rsp[count++]=(uint8_t)modbus.read();if(count>=minCount){delay(4);while(modbus.available()&&count<64)rsp[count++]=(uint8_t)modbus.read();break;}delay(1);}if(!count){modbusTimeouts++;lastError="TIMEOUT";lastRx="<NONE>";return false;}lastRx=hexFrame(rsp,count);if(count<4){lastError="SHORT FRAME";return false;}uint16_t got=(uint16_t)rsp[count-2]|((uint16_t)rsp[count-1]<<8);if(got!=crc16(rsp,count-2)){modbusCrcFails++;lastError="CRC";return false;}return true;}
bool readBits(uint8_t fc,bool* dst){uint8_t req[8]={modbusSlave,fc,0,0,0,4,0,0},rsp[64];size_t n=0;if(!transact(req,8,rsp,n,6))return false;if(rsp[0]!=modbusSlave||rsp[1]!=fc||rsp[2]<1)return false;for(int i=0;i<4;i++)dst[i]=(rsp[3]&(1U<<i))!=0;return true;}
bool readAIBlock(uint16_t startReg,uint8_t offset){uint8_t req[8]={modbusSlave,0x04,(uint8_t)(startReg>>8),(uint8_t)startReg,0,4,0,0},rsp[64];size_t n=0;if(!transact(req,8,rsp,n,13))return false;if(rsp[0]!=modbusSlave||rsp[1]!=0x04||rsp[2]<8)return false;for(int i=0;i<4;i++){uint16_t raw=((uint16_t)rsp[3+i*2]<<8)|rsp[4+i*2];ai_mA[offset+i]=raw/1000.0f;}return true;}
bool readAO(){uint8_t req[8]={modbusSlave,0x03,0,0,0,4,0,0},rsp[64];size_t n=0;if(!transact(req,8,rsp,n,13))return false;if(rsp[0]!=modbusSlave||rsp[1]!=0x03||rsp[2]<8)return false;for(int i=0;i<4;i++){uint16_t raw=((uint16_t)rsp[3+i*2]<<8)|rsp[4+i*2];ao_mA[i]=raw/1000.0f;}return true;}
bool writeDO(uint8_t ch,bool on){if(ch>=4)return false;uint16_t v=on?0xFF00:0;uint8_t req[8]={modbusSlave,0x05,0,ch,(uint8_t)(v>>8),(uint8_t)v,0,0},rsp[64];size_t n=0;if(!transact(req,8,rsp,n,8))return false;bool ok=n>=8&&memcmp(req,rsp,6)==0;if(ok)doState[ch]=on;return ok;}
bool writeAO(uint8_t ch,float pct){if(ch>=4)return false;pct=constrain(pct,0.0f,100.0f);float ma=4.0f+pct*0.16f;uint16_t raw=(uint16_t)lroundf(ma*1000.0f);uint8_t req[8]={modbusSlave,0x06,0,ch,(uint8_t)(raw>>8),(uint8_t)raw,0,0},rsp[64];size_t n=0;if(!transact(req,8,rsp,n,8))return false;bool ok=n>=8&&memcmp(req,rsp,6)==0;if(ok){aoForcePct[ch]=pct;ao_mA[ch]=ma;}return ok;}
void pollModbus(){if(millis()-lastPoll<300)return;lastPoll=millis();bool ok=false;switch(pollPhase++%5){case 0:ok=readBits(0x02,diState);break;case 1:ok=readBits(0x01,doState);break;case 2:ok=readAIBlock(0,0);break;case 3:ok=readAIBlock(4,4);break;default:ok=readAO();break;}if(ok){modbusGood++;modbusOnline=true;lastError="OK";}else{modbusBad++;if(modbusBad>2&&modbusBad>modbusGood+2)modbusOnline=false;}}
void setModbusSlave(uint8_t address){modbusSlave=address;modbusOnline=false;modbusGood=0;modbusBad=0;modbusTimeouts=0;modbusCrcFails=0;pollPhase=0;lastPoll=0;lastTx="";lastRx="";lastError="ADDRESS CHANGED";}

void present(){}
void header(const char* title){canvas.fillScreen(TFT_BLACK);canvas.setTextDatum(TL_DATUM);canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("RIOT I/O TESTER",UI_LEFT+2,5,2);canvas.setTextColor(modbusOnline?TFT_GREEN:TFT_RED,TFT_BLACK);canvas.drawRightString(modbusOnline?"M31 ONLINE":"M31 OFFLINE",UI_RIGHT-2,5,2);canvas.setTextColor(TFT_CYAN,TFT_BLACK);canvas.drawString(title,UI_LEFT+2,28,4);canvas.drawFastHLine(UI_LEFT,58,UI_RIGHT-UI_LEFT,TFT_DARKGREY);}
void footer(){const char* names[]={"AI","DI","DO","AO","MB"};const int x0=8,usable=304,tabW=usable/5;for(int i=0;i<5;i++){int x=x0+i*tabW;int w=(i==4)?(x0+usable-x):tabW;uint16_t bg=(page==i)?TFT_DARKCYAN:TFT_DARKGREY;canvas.fillRect(x,FOOTER_Y,w-2,31,bg);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,bg);canvas.drawString(names[i],x+(w-2)/2,FOOTER_Y+15,2);}canvas.setTextDatum(TL_DATUM);}
void drawAI(){header("ANALOG INPUTS");for(int i=0;i<8;i++){int col=i/4,row=i%4,x=col?166:12,y=68+row*33;int right=col?306:152;canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("AI"+String(i+1),x,y,2);canvas.setTextColor(modbusOnline?TFT_GREEN:TFT_RED,TFT_BLACK);canvas.drawRightString(String(ai_mA[i],3)+" mA",right,y,2);}footer();}
void drawDI(){header("DIGITAL INPUTS");for(int i=0;i<4;i++){int y=70+i*32;canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("DI"+String(i+1),14,y,4);canvas.fillRoundRect(190,y-2,104,27,5,diState[i]?TFT_GREEN:TFT_DARKGREY);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,diState[i]?TFT_GREEN:TFT_DARKGREY);canvas.drawString(diState[i]?"ON":"OFF",242,y+11,2);canvas.setTextDatum(TL_DATUM);}footer();}
void drawDO(){header("DIGITAL OUTPUTS");for(int i=0;i<4;i++){int y=67+i*34;canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("DO"+String(i+1),14,y+4,4);canvas.fillRoundRect(155,y,140,29,5,doState[i]?TFT_GREEN:TFT_MAROON);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,doState[i]?TFT_GREEN:TFT_MAROON);canvas.drawString(doState[i]?"TURN OFF":"TURN ON",225,y+14,2);canvas.setTextDatum(TL_DATUM);}footer();}
void drawAO(){header("ANALOG OUTPUTS");for(int i=0;i<4;i++){int y=63+i*35;canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("AO"+String(i+1),10,y+7,2);canvas.setTextColor(TFT_GREEN,TFT_BLACK);canvas.drawString(String(ao_mA[i],3)+"mA",50,y+7,2);canvas.fillRoundRect(144,y,43,28,4,TFT_DARKGREY);canvas.fillRoundRect(192,y,43,28,4,TFT_DARKGREY);canvas.fillRoundRect(240,y,65,28,4,TFT_DARKCYAN);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,TFT_DARKGREY);canvas.drawString("-10",165,y+14,2);canvas.drawString("+10",213,y+14,2);canvas.setTextColor(TFT_WHITE,TFT_DARKCYAN);canvas.drawString(String(aoForcePct[i],0)+"%",272,y+14,2);canvas.setTextDatum(TL_DATUM);}footer();}
void drawComms(){header("MODBUS / WIFI");canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("Slave",12,67,2);canvas.drawRightString(String(modbusSlave),306,67,2);canvas.drawString("RS485",12,87,2);canvas.drawRightString("9600 8N1 TX22 RX35",306,87,2);canvas.drawString("Good / Bad",12,107,2);canvas.drawRightString(String(modbusGood)+" / "+String(modbusBad),306,107,2);canvas.drawString("Timeout / CRC",12,127,2);canvas.drawRightString(String(modbusTimeouts)+" / "+String(modbusCrcFails),306,127,2);canvas.drawString("WiFi IP",12,147,2);canvas.setTextColor(TFT_GREEN,TFT_BLACK);canvas.drawRightString(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"OFFLINE",306,147,2);canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString("Last",12,167,2);canvas.drawRightString(lastError,306,167,2);canvas.setTextColor(TFT_LIGHTGREY,TFT_BLACK);canvas.drawString(FW,12,190,1);footer();}
void redraw(){switch(page){case PAGE_AI:drawAI();break;case PAGE_DI:drawDI();break;case PAGE_DO:drawDO();break;case PAGE_AO:drawAO();break;default:drawComms();break;}present();}

void refreshHeaderStatus(){static bool valid=false;static bool last=false;if(valid&&last==modbusOnline)return;last=modbusOnline;valid=true;canvas.fillRect(188,2,124,19,TFT_BLACK);canvas.setTextDatum(TL_DATUM);canvas.setTextColor(modbusOnline?TFT_GREEN:TFT_RED,TFT_BLACK);canvas.drawRightString(modbusOnline?"M31 ONLINE":"M31 OFFLINE",UI_RIGHT-2,5,2);}
void refreshLive(){
  static Page cachedPage=(Page)255;
  static float lastAI[8]={};
  static float lastAO[4]={};
  static float lastPct[4]={};
  static bool lastDI[4]={};
  static bool lastDO[4]={};
  static uint8_t lastSlave=0;
  static String lastErr="";
  static String lastIp="";
  static uint32_t lastMbPaint=0;
  refreshHeaderStatus();
  if(cachedPage!=page){cachedPage=page;for(int i=0;i<8;i++)lastAI[i]=ai_mA[i];for(int i=0;i<4;i++){lastAO[i]=ao_mA[i];lastPct[i]=aoForcePct[i];lastDI[i]=diState[i];lastDO[i]=doState[i];}lastSlave=modbusSlave;lastErr=lastError;lastIp=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("OFFLINE");lastMbPaint=millis();return;}
  if(page==PAGE_AI){
    for(int i=0;i<8;i++){if(fabsf(ai_mA[i]-lastAI[i])<0.01f)continue;lastAI[i]=ai_mA[i];int col=i/4,row=i%4,y=68+row*33;int x=col?213:59;int right=col?306:152;canvas.fillRect(x,y,94,18,TFT_BLACK);canvas.setTextColor(modbusOnline?TFT_GREEN:TFT_RED,TFT_BLACK);canvas.drawRightString(String(ai_mA[i],3)+" mA",right,y,2);}
  }else if(page==PAGE_DI){
    for(int i=0;i<4;i++){if(diState[i]==lastDI[i])continue;lastDI[i]=diState[i];int y=70+i*32;uint16_t bg=diState[i]?TFT_GREEN:TFT_DARKGREY;canvas.fillRoundRect(190,y-2,104,27,5,bg);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,bg);canvas.drawString(diState[i]?"ON":"OFF",242,y+11,2);}canvas.setTextDatum(TL_DATUM);
  }else if(page==PAGE_DO){
    for(int i=0;i<4;i++){if(doState[i]==lastDO[i])continue;lastDO[i]=doState[i];int y=67+i*34;uint16_t bg=doState[i]?TFT_GREEN:TFT_MAROON;canvas.fillRoundRect(155,y,140,29,5,bg);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,bg);canvas.drawString(doState[i]?"TURN OFF":"TURN ON",225,y+14,2);}canvas.setTextDatum(TL_DATUM);
  }else if(page==PAGE_AO){
    for(int i=0;i<4;i++){int y=63+i*35;if(fabsf(ao_mA[i]-lastAO[i])>=0.01f){lastAO[i]=ao_mA[i];canvas.fillRect(48,y+5,90,19,TFT_BLACK);canvas.setTextDatum(TL_DATUM);canvas.setTextColor(TFT_GREEN,TFT_BLACK);canvas.drawString(String(ao_mA[i],3)+"mA",50,y+7,2);}if(fabsf(aoForcePct[i]-lastPct[i])>=0.5f){lastPct[i]=aoForcePct[i];canvas.fillRoundRect(240,y,65,28,4,TFT_DARKCYAN);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(TFT_WHITE,TFT_DARKCYAN);canvas.drawString(String(aoForcePct[i],0)+"%",272,y+14,2);}}canvas.setTextDatum(TL_DATUM);
  }else{
    String ip=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("OFFLINE");
    bool immediate=(modbusSlave!=lastSlave)||(lastError!=lastErr)||(ip!=lastIp);
    if(!immediate&&millis()-lastMbPaint<2000)return;
    lastMbPaint=millis();lastSlave=modbusSlave;lastErr=lastError;lastIp=ip;
    canvas.setTextDatum(TL_DATUM);canvas.setTextColor(TFT_WHITE,TFT_BLACK);
    canvas.fillRect(260,64,48,19,TFT_BLACK);canvas.drawRightString(String(modbusSlave),306,67,2);
    canvas.fillRect(168,104,140,19,TFT_BLACK);canvas.drawRightString(String(modbusGood)+" / "+String(modbusBad),306,107,2);
    canvas.fillRect(168,124,140,19,TFT_BLACK);canvas.drawRightString(String(modbusTimeouts)+" / "+String(modbusCrcFails),306,127,2);
    canvas.fillRect(168,144,140,19,TFT_BLACK);canvas.setTextColor(TFT_GREEN,TFT_BLACK);canvas.drawRightString(ip,306,147,2);
    canvas.fillRect(168,164,140,19,TFT_BLACK);canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawRightString(lastError,306,167,2);
  }
}

bool readTouch(int& x,int& y){if(!touch.tirqTouched()&&!touch.touched())return false;TS_Point p=touch.getPoint();if(p.x<100||p.y<100)return false;x=map(constrain(p.x,250,3425),3425,250,0,319);y=map(constrain(p.y,490,3540),3540,490,0,239);x=constrain(x,0,319);y=constrain(y,0,239);Serial.printf("TOUCH raw=%d,%d mapped=%d,%d\n",p.x,p.y,x,y);return true;}
void serviceTouch(){static uint32_t last=0;if(millis()-last<180)return;int x,y;if(!readTouch(x,y))return;last=millis();if(y>=204){if(x>=8&&x<312){page=(Page)constrain((x-8)/60,0,4);redraw();}return;}if(page==PAGE_DO){int ch=(y-65)/34;if(ch>=0&&ch<4){writeDO(ch,!doState[ch]);refreshLive();}}else if(page==PAGE_AO){int ch=(y-62)/35;if(ch>=0&&ch<4){float p=aoForcePct[ch];if(x>=142&&x<189)p-=10;else if(x>=190&&x<237)p+=10;else return;writeAO(ch,p);refreshLive();}}}

String stateJson(){String j="{\"firmware\":\""+String(FW)+"\",\"modbus\":\""+(modbusOnline?"ONLINE":"OFFLINE")+"\",\"slave\":"+String(modbusSlave)+",\"ip\":\""+(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("OFFLINE"))+"\"";j+=",\"good\":"+String(modbusGood)+",\"bad\":"+String(modbusBad)+",\"timeouts\":"+String(modbusTimeouts)+",\"crcFails\":"+String(modbusCrcFails);j+=",\"di\":[";for(int i=0;i<4;i++){if(i)j+=',';j+=diState[i]?"true":"false";}j+="]";j+=",\"do\":[";for(int i=0;i<4;i++){if(i)j+=',';j+=doState[i]?"true":"false";}j+="]";j+=",\"ai_mA\":[";for(int i=0;i<8;i++){if(i)j+=',';j+=String(ai_mA[i],3);}j+="]";j+=",\"ao_mA\":[";for(int i=0;i<4;i++){if(i)j+=',';j+=String(ao_mA[i],3);}j+="]";j+=",\"lastError\":\""+lastError+"\",\"lastTx\":\""+lastTx+"\",\"lastRx\":\""+lastRx+"\"}";return j;}

void beginWeb(){server.on("/api/state",HTTP_GET,[](){server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",stateJson());});server.on("/api/do",HTTP_POST,[](){int ch=server.arg("ch").toInt()-1;bool ok=writeDO(ch,server.arg("on")=="1");server.send(ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");});server.on("/api/ao",HTTP_POST,[](){int ch=server.arg("ch").toInt()-1;bool ok=writeAO(ch,server.arg("pct").toFloat());server.send(ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");});server.on("/api/modbus-address",HTTP_POST,[](){int a=server.arg("address").toInt();if(a<1||a>247){server.send(400,"application/json","{\"ok\":false,\"error\":\"address must be 1..247\"}");return;}setModbusSlave((uint8_t)a);if(page==PAGE_COMMS)redraw();else refreshHeaderStatus();server.send(200,"application/json",stateJson());});server.on("/",HTTP_GET,[](){static const char PAGE[] PROGMEM=R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>Riot I/O Tester</title><style>*{box-sizing:border-box}body{font-family:Arial;background:#101214;color:#eee;margin:16px;max-width:950px}.top,.card{background:#191d20;border:1px solid #3a4146;border-radius:10px;padding:13px;margin:10px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}.p{background:#111619;border:1px solid #334047;border-radius:8px;padding:10px}.ok{color:#6ee7a8}.bad{color:#ff7474}button,input{padding:9px;background:#262c30;color:#fff;border:1px solid #59646b;border-radius:6px}pre{white-space:pre-wrap;font-size:12px;color:#aeb8bd}</style></head><body><div class=top><h2>Riot I/O Tester</h2><b id=mb>Loading...</b> <span id=ip></span></div><div class=card><h3>Modbus Slave Address</h3><div class=p>Active slave: <b id=slaveNow>1</b>&nbsp;&nbsp;<input id=slave type=number min=1 max=247 value=1 style="width:85px"> <button onclick="setSlave()">Apply</button><br><small>Runtime only; reboot returns to address 1.</small></div></div><div class=card><h3>Analog Inputs</h3><div id=ai class=grid></div></div><div class=card><h3>Digital Inputs</h3><div id=di class=grid></div></div><div class=card><h3>Digital Outputs</h3><div id=dout class=grid></div></div><div class=card><h3>Analog Outputs</h3><div id=ao class=grid></div></div><div class=card><h3>Modbus Diagnostics</h3><pre id=diag></pre></div><script>const e=x=>document.getElementById(x);async function post(u,p){let r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(p)});setTimeout(load,150);return r}async function setSlave(){let v=Number(e('slave').value);if(v<1||v>247){alert('Modbus address must be 1 through 247');return}let r=await post('/api/modbus-address',{address:v});if(!r.ok)alert('Could not change Modbus address')}function f(x){return Number(x||0).toFixed(3)}async function load(){let s=await fetch('/api/state?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());mb.textContent='M31 '+s.modbus;mb.className=s.modbus==='ONLINE'?'ok':'bad';ip.textContent=' | '+s.ip;slaveNow.textContent=s.slave;if(document.activeElement!==e('slave'))e('slave').value=s.slave;ai.innerHTML=s.ai_mA.map((v,i)=>'<div class=p><b>AI'+(i+1)+'</b><br>'+f(v)+' mA</div>').join('');di.innerHTML=s.di.map((v,i)=>'<div class=p><b>DI'+(i+1)+'</b><br>'+(v?'ON':'OFF')+'</div>').join('');dout.innerHTML=s.do.map((v,i)=>'<div class=p><b>DO'+(i+1)+'</b><br>'+(v?'ON':'OFF')+'<br><button onclick="post(\'/api/do\',{ch:'+(i+1)+',on:'+(v?0:1)+'})">'+(v?'TURN OFF':'TURN ON')+'</button></div>').join('');ao.innerHTML=s.ao_mA.map((v,i)=>'<div class=p><b>AO'+(i+1)+'</b><br>'+f(v)+' mA<br><input id=a'+i+' type=number min=0 max=100 value=50 style="width:70px"> % <button onclick="post(\'/api/ao\',{ch:'+(i+1)+',pct:e(\'a'+i+'\').value})">Force</button></div>').join('');diag.textContent='Slave '+s.slave+' | Good '+s.good+'  Bad '+s.bad+'  Timeouts '+s.timeouts+'  CRC '+s.crcFails+'\nLast: '+s.lastError+'\nTX: '+s.lastTx+'\nRX: '+s.lastRx}load();setInterval(load,1000)</script></body></html>)HTML";server.send(200,"text/html",PAGE);});server.begin();}

void drawSplash(const String& a,const String& b,uint16_t c){canvas.fillScreen(TFT_BLACK);canvas.setTextDatum(MC_DATUM);canvas.setTextColor(c,TFT_BLACK);canvas.drawString(a,160,90,4);canvas.setTextColor(TFT_WHITE,TFT_BLACK);canvas.drawString(b,160,130,2);canvas.setTextDatum(TL_DATUM);present();}

void setup(){Serial.begin(115200);delay(200);pinMode(TFT_BL,OUTPUT);digitalWrite(TFT_BL,HIGH);tft.init();tft.setRotation(3);tft.fillScreen(TFT_BLACK);Serial.printf("LCD ST7789 direct r3 change-driven %dx%d\n",tft.width(),tft.height());drawSplash("RIOT I/O TESTER","Starting WiFi...",TFT_CYAN);touchSPI.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);touch.begin(touchSPI);modbus.begin(MODBUS_BAUD,SERIAL_8N1,RS485_RX,RS485_TX);wm.setConfigPortalTimeout(180);wm.setConnectTimeout(15);bool wifi=wm.autoConnect("Riot-IO-Tester");Serial.printf("WiFi: %s IP=%s\n",wifi?"OK":"OFFLINE",wifi?WiFi.localIP().toString().c_str():"-");drawSplash(wifi?"WIFI CONNECTED":"WIFI OFFLINE",wifi?WiFi.localIP().toString():String("Riot-IO-Tester"),wifi?TFT_GREEN:TFT_RED);delay(1800);beginWeb();redraw();}
void loop(){server.handleClient();pollModbus();serviceTouch();if(millis()-lastScreen>250){lastScreen=millis();refreshLive();}delay(2);}
