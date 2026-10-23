#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_idf_version.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <vector>
#include <SPI.h>
#include <SPIFFS.h>
#include <time.h>
#include <TFT_eSPI.h>
#include "aga_engine.h"
#include "RiotHistoryStore.h"
#include "RiotFlowHistoryStore.h"
#include "RiotDailyFlowHistoryStore.h"
#include "RiotTimeService.h"
#include "qrcode.h"

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

// RIOT-FLOW FIRMWARE VERSION V2
static constexpr const char* FW = "Riot-Flow-v2-A5C-A6C2";

// RIOT REMOTE OTA MANIFEST CHECK GATE A6C1 V1
// RIOT A8B PROTECTED AUDIT LOG V1
static bool riotFsMounted=false;

// RIOT A10A VIRGIN SPIFFS AUTOPROVISION V1
// A brand-new controller may have a completely erased SPIFFS partition.
// Only an ALL-0xFF partition is considered demonstrably virgin and eligible
// for one-time automatic formatting. Any non-erased byte means "do not
// format": preserve the partition and report a protected storage fault.
static bool riotSpiffsPartitionFullyErased(const esp_partition_t* part,
                                           size_t& checkedBytes){
  checkedBytes=0;
  if(!part || part->type!=ESP_PARTITION_TYPE_DATA ||
     part->subtype!=ESP_PARTITION_SUBTYPE_DATA_SPIFFS)return false;

  static uint8_t scanBuf[512];
  for(size_t offset=0;offset<part->size;offset+=sizeof(scanBuf)){
    size_t n=part->size-offset;
    if(n>sizeof(scanBuf))n=sizeof(scanBuf);
    esp_err_t err=esp_partition_read(part,offset,scanBuf,n);
    if(err!=ESP_OK){
      Serial.printf("SPIFFS virgin scan read failed offset=%u err=%d\n",
        (unsigned)offset,(int)err);
      return false;
    }
    checkedBytes+=n;
    for(size_t i=0;i<n;i++){
      if(scanBuf[i]!=0xFF){
        Serial.printf("SPIFFS virgin scan: NON-ERASED byte offset=%u value=0x%02X\n",
          (unsigned)(offset+i),(unsigned)scanBuf[i]);
        return false;
      }
    }
  }
  return checkedBytes==part->size;
}

static bool riotProvisionVirginSpiffs(){
  const esp_partition_t* part=esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,nullptr);
  if(!part){
    Serial.println("SPIFFS virgin provision: partition not found");
    return false;
  }

  size_t checked=0;
  const bool erased=riotSpiffsPartitionFullyErased(part,checked);
  Serial.printf(
    "SPIFFS virgin scan: label=%s address=0x%08lX size=%u checked=%u erased=%s\n",
    part->label,
    (unsigned long)part->address,
    (unsigned)part->size,
    (unsigned)checked,
    erased?"YES":"NO");

  if(!erased){
    Serial.println("SPIFFS protected: automatic format REFUSED (partition is not virgin)");
    return false;
  }

  Serial.println("SPIFFS virgin partition confirmed; performing ONE-TIME format");
  if(!SPIFFS.format()){
    Serial.println("SPIFFS virgin provision: FORMAT FAILED");
    return false;
  }

  if(!SPIFFS.begin(false)){
    Serial.println("SPIFFS virgin provision: mount after format FAILED");
    return false;
  }

  Serial.println("SPIFFS virgin provision: FORMAT + MOUNT OK");
  return true;
}

static constexpr const char* RIOT_OTA_PRODUCT = "Riot-Flow";
static constexpr const char* RIOT_OTA_PLATFORM = "TRMNL_XIAO_EPAPER";
static constexpr const char* RIOT_OTA_CHANNEL = "stable";
static constexpr const char* RIOT_OTA_VERSION = "2.0.0-a6c2";
static constexpr const char* RIOT_OTA_BUILD = "2026.09.13-A6C2";
static constexpr const char* RIOT_OTA_MANIFEST_URL =
  "https://raw.githubusercontent.com/homershaw/MutliFlowOTA/main/riot-flow/manifest.json";

// RIOT OTA PARTITION INVENTORY GATE A6A V1
static String riotPartitionJson(const esp_partition_t* p){
  if(!p)return "null";
  char address[16],size[16];
  snprintf(address,sizeof(address),"0x%08lX",(unsigned long)p->address);
  snprintf(size,sizeof(size),"0x%08lX",(unsigned long)p->size);
  String j="{\"label\":\""+String(p->label)+"\"";
  j+=",\"address\":\""+String(address)+"\"";
  j+=",\"sizeBytes\":"+String((unsigned long)p->size);
  j+=",\"sizeHex\":\""+String(size)+"\"";
  j+=",\"type\":"+String((unsigned)p->type);
  j+=",\"subtype\":"+String((unsigned)p->subtype);
  j+="}";
  return j;
}

static String riotOtaPartitionsJson(){
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* boot=esp_ota_get_boot_partition();
  const esp_partition_t* next=esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* ota0=esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
  const esp_partition_t* ota1=esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
  const esp_partition_t* factory=esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,nullptr);
  const esp_partition_t* otadata=esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);

  bool dualOta=(ota0!=nullptr && ota1!=nullptr);
  bool nextUsable=(next!=nullptr && running!=nullptr && next->address!=running->address);

  String j="{\"ok\":true";
  j+=",\"gate\":\"A6a\"";
  j+=",\"firmware\":\""+String(FW)+"\"";
  j+=",\"flashBytes\":"+String((unsigned long)ESP.getFlashChipSize());
  j+=",\"running\":"+riotPartitionJson(running);
  j+=",\"boot\":"+riotPartitionJson(boot);
  j+=",\"nextUpdate\":"+riotPartitionJson(next);
  j+=",\"ota0\":"+riotPartitionJson(ota0);
  j+=",\"ota1\":"+riotPartitionJson(ota1);
  j+=",\"factory\":"+riotPartitionJson(factory);
  j+=",\"otaData\":"+riotPartitionJson(otadata);
  j+=",\"dualOta\":"+String(dualOta?"true":"false");
  j+=",\"nextUpdateUsable\":"+String(nextUsable?"true":"false");
  j+="}";
  return j;
}

struct RiotRemoteOtaManifest {
  bool valid=false;
  String product;
  String platform;
  String channel;
  String version;
  String firmwareUrl;
  String sha256;
  size_t size=0;
  String notes;
  String error;
};

static String riotOtaJsonStringField(const String& json,const char* key){
  String needle=String("\"")+key+"\"";
  int p=json.indexOf(needle);
  if(p<0)return "";
  p=json.indexOf(':',p+needle.length());
  if(p<0)return "";
  p++;
  while(p<(int)json.length() && isspace((unsigned char)json[p]))p++;
  if(p>=(int)json.length() || json[p]!='\"')return "";
  p++;
  String out;
  bool esc=false;
  for(;p<(int)json.length();p++){
    char ch=json[p];
    if(esc){
      if(ch=='n')out+='\n';
      else if(ch=='r')out+='\r';
      else if(ch=='t')out+='\t';
      else out+=ch;
      esc=false;
    }else if(ch=='\\'){
      esc=true;
    }else if(ch=='\"'){
      return out;
    }else out+=ch;
  }
  return "";
}

static uint32_t riotOtaJsonUIntField(const String& json,const char* key){
  String needle=String("\"")+key+"\"";
  int p=json.indexOf(needle);
  if(p<0)return 0;
  p=json.indexOf(':',p+needle.length());
  if(p<0)return 0;
  p++;
  while(p<(int)json.length() && isspace((unsigned char)json[p]))p++;
  uint32_t v=0; bool any=false;
  while(p<(int)json.length() && isdigit((unsigned char)json[p])){
    any=true;
    v=v*10UL+(uint32_t)(json[p]-'0');
    p++;
  }
  return any?v:0;
}

static bool riotOtaSha256LooksValid(const String& s){
  if(s.length()!=64)return false;
  for(size_t i=0;i<s.length();i++){
    if(!isxdigit((unsigned char)s[i]))return false;
  }
  return true;
}

static bool riotFetchRemoteOtaManifest(RiotRemoteOtaManifest& m){
  m=RiotRemoteOtaManifest();
  if(WiFi.status()!=WL_CONNECTED){
    m.error="Wi-Fi is not connected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // A6c1 transport check; A6c2 retains SHA-256 binary verification.
  HTTPClient http;
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if(!http.begin(client,RIOT_OTA_MANIFEST_URL)){
    m.error="Could not open manifest URL";
    return false;
  }

  int code=http.GET();
  if(code!=HTTP_CODE_OK){
    m.error="Manifest HTTP "+String(code);
    http.end();
    return false;
  }

  String body=http.getString();
  http.end();

  uint32_t schema=riotOtaJsonUIntField(body,"schema");
  m.product=riotOtaJsonStringField(body,"product");
  m.platform=riotOtaJsonStringField(body,"platform");
  m.channel=riotOtaJsonStringField(body,"channel");
  m.version=riotOtaJsonStringField(body,"version");
  m.firmwareUrl=riotOtaJsonStringField(body,"firmware_url");
  m.sha256=riotOtaJsonStringField(body,"sha256");
  m.sha256.toLowerCase();
  m.size=(size_t)riotOtaJsonUIntField(body,"size");
  m.notes=riotOtaJsonStringField(body,"notes");

  if(schema!=1){m.error="Unsupported manifest schema";return false;}
  if(m.product!=RIOT_OTA_PRODUCT){m.error="Manifest product mismatch";return false;}
  if(m.platform!=RIOT_OTA_PLATFORM){m.error="Manifest platform mismatch";return false;}
  if(m.channel!=RIOT_OTA_CHANNEL){m.error="Manifest channel mismatch";return false;}
  if(!m.version.length()){m.error="Manifest version missing";return false;}
  if(!m.firmwareUrl.startsWith("https://")){m.error="Firmware URL must use HTTPS";return false;}
  if(!riotOtaSha256LooksValid(m.sha256)){m.error="Manifest SHA-256 missing or invalid";return false;}
  if(m.size<1024){m.error="Manifest firmware size is invalid";return false;}

  const esp_partition_t* next=esp_ota_get_next_update_partition(nullptr);
  if(!next){m.error="No inactive OTA partition available";return false;}
  if(m.size>next->size){m.error="Firmware is larger than inactive OTA slot";return false;}

  m.valid=true;
  return true;
}

static String riotRemoteOtaCheckJson(){
  RiotRemoteOtaManifest m;
  bool ok=riotFetchRemoteOtaManifest(m);
  String j="{\"ok\":"+String(ok?"true":"false");
  j+=",\"gate\":\"A6c2\"";
  j+=",\"product\":\""+String(RIOT_OTA_PRODUCT)+"\"";
  j+=",\"platform\":\""+String(RIOT_OTA_PLATFORM)+"\"";
  j+=",\"channel\":\""+String(RIOT_OTA_CHANNEL)+"\"";
  j+=",\"currentVersion\":\""+String(RIOT_OTA_VERSION)+"\"";
  j+=",\"build\":\""+String(RIOT_OTA_BUILD)+"\"";
  j+=",\"manifestUrl\":\""+String(RIOT_OTA_MANIFEST_URL)+"\"";
  if(ok){
    String notes=m.notes; notes.replace("\\","\\\\"); notes.replace("\"","\\\"");
    j+=",\"remoteVersion\":\""+m.version+"\"";
    j+=",\"size\":"+String((unsigned long)m.size);
    j+=",\"sha256\":\""+m.sha256+"\"";
    j+=",\"firmwareUrl\":\""+m.firmwareUrl+"\"";
    j+=",\"notes\":\""+notes+"\"";
    j+=",\"updateAvailable\":"+String(m.version!=RIOT_OTA_VERSION?"true":"false");
    j+=",\"reinstallSupported\":true";
  }else{
    String err=m.error; err.replace("\\","\\\\"); err.replace("\"","\\\"");
    j+=",\"error\":\""+err+"\"";
  }
  j+="}";
  return j;
}

// RIOT VERIFIED REMOTE OTA INSTALL GATE A6C2 V1
static bool riotRemoteOtaActive=false;
static size_t riotRemoteOtaBytes=0;
static String riotRemoteOtaVersion="";
static String riotRemoteOtaMessage="";

static String riotOtaHexLower(const uint8_t* data,size_t len){
  static const char* hex="0123456789abcdef";
  String out;
  out.reserve(len*2);
  for(size_t i=0;i<len;i++){
    out+=hex[(data[i]>>4)&0x0F];
    out+=hex[data[i]&0x0F];
  }
  return out;
}

static bool riotInstallRemoteOta(const RiotRemoteOtaManifest& m,String& message){
  if(!m.valid){
    message="Manifest is not valid";
    return false;
  }
  if(riotRemoteOtaActive){
    message="Another remote OTA operation is already active";
    return false;
  }

  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* target=esp_ota_get_next_update_partition(nullptr);
  if(!running || !target || target->address==running->address){
    message="No inactive OTA partition available";
    return false;
  }
  if(m.size>target->size){
    message="Firmware is larger than inactive OTA slot";
    return false;
  }

  riotRemoteOtaActive=true;
  riotRemoteOtaBytes=0;
  riotRemoteOtaVersion=m.version;
  riotRemoteOtaMessage="Downloading remote firmware";

  WiFiClientSecure client;
  client.setInsecure(); // Binary integrity is enforced by manifest SHA-256 in A6c2.
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if(!http.begin(client,m.firmwareUrl)){
    riotRemoteOtaActive=false;
    message="Could not open firmware URL";
    return false;
  }

  int code=http.GET();
  if(code!=HTTP_CODE_OK){
    message="Firmware HTTP "+String(code);
    http.end();
    riotRemoteOtaActive=false;
    return false;
  }

  int contentLength=http.getSize();
  if(contentLength>0 && (size_t)contentLength!=m.size){
    message="Firmware size does not match manifest";
    http.end();
    riotRemoteOtaActive=false;
    return false;
  }

  esp_ota_handle_t handle=0;
  esp_err_t err=esp_ota_begin(target,m.size,&handle);
  if(err!=ESP_OK){
    message="esp_ota_begin failed: "+String((int)err);
    http.end();
    riotRemoteOtaActive=false;
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if(mbedtls_sha256_starts_ret(&sha,0)!=0){
    esp_ota_abort(handle);
    http.end();
    mbedtls_sha256_free(&sha);
    riotRemoteOtaActive=false;
    message="Could not start SHA-256";
    return false;
  }

  WiFiClient* stream=http.getStreamPtr();

  // RIOT A6C2 HEAP BUFFER REPAIR V2
  // Keep the 4 KiB download buffer off the WebServer/loop task stack.
  // The first A6c2 runtime attempt double-faulted with a corrupted backtrace
  // immediately after entering the synchronous remote-install path.
  static constexpr size_t RIOT_REMOTE_OTA_BUF_SIZE=4096;
  uint8_t* buf=(uint8_t*)heap_caps_malloc(
    RIOT_REMOTE_OTA_BUF_SIZE,
    MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!buf){
    buf=(uint8_t*)heap_caps_malloc(RIOT_REMOTE_OTA_BUF_SIZE,MALLOC_CAP_8BIT);
  }
  if(!buf){
    esp_ota_abort(handle);
    http.end();
    mbedtls_sha256_free(&sha);
    riotRemoteOtaActive=false;
    message="Could not allocate remote OTA download buffer";
    return false;
  }

  Serial.printf("OTA A6C2 DOWNLOAD START version=%s bytes=%lu target=%s freeHeap=%lu freePSRAM=%lu stackHWM=%u\n",
    m.version.c_str(),
    (unsigned long)m.size,
    target->label,
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getFreePsram(),
    (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  size_t total=0;
  uint32_t lastData=millis();
  bool failed=false;
  bool magicChecked=false;

  while(total<m.size){
    int avail=stream->available();
    if(avail<=0){
      if(!http.connected() && total<m.size){
        message="Firmware download ended early";
        failed=true;
        break;
      }
      if(millis()-lastData>20000UL){
        message="Firmware download timed out";
        failed=true;
        break;
      }
      delay(1);
      continue;
    }

    size_t want=min((size_t)avail,min(RIOT_REMOTE_OTA_BUF_SIZE,m.size-total));
    int got=stream->readBytes(buf,want);
    if(got<=0){
      if(millis()-lastData>20000UL){
        message="Firmware download read timed out";
        failed=true;
        break;
      }
      delay(1);
      continue;
    }

    lastData=millis();

    if(!magicChecked){
      magicChecked=true;
      if(buf[0]!=0xE9){
        message="Remote firmware has invalid ESP32 image magic";
        failed=true;
        break;
      }
    }

    if(mbedtls_sha256_update_ret(&sha,buf,(size_t)got)!=0){
      message="SHA-256 update failed";
      failed=true;
      break;
    }

    err=esp_ota_write(handle,buf,(size_t)got);
    if(err!=ESP_OK){
      message="Flash write failed: "+String((int)err);
      failed=true;
      break;
    }

    total+=(size_t)got;
    riotRemoteOtaBytes=total;
    delay(0);
  }

  uint8_t digest[32];
  bool hashOK=false;
  if(!failed && total==m.size && mbedtls_sha256_finish_ret(&sha,digest)==0){
    String actual=riotOtaHexLower(digest,sizeof(digest));
    hashOK=actual.equalsIgnoreCase(m.sha256);
    if(!hashOK)message="SHA-256 verification failed";
  }else if(!failed){
    message="Could not finish SHA-256 verification";
    failed=true;
  }

  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
  buf=nullptr;
  http.end();

  Serial.printf("OTA A6C2 DOWNLOAD END bytes=%lu expected=%lu hashOK=%u failed=%u stackHWM=%u\n",
    (unsigned long)total,
    (unsigned long)m.size,
    hashOK?1:0,
    failed?1:0,
    (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  if(failed || !hashOK || total!=m.size){
    esp_ota_abort(handle);
    riotRemoteOtaActive=false;
    riotRemoteOtaMessage=message;
    return false;
  }

  Serial.println("OTA A6C2 SHA256 VERIFIED; finalizing image");
  err=esp_ota_end(handle);
  if(err!=ESP_OK){
    riotRemoteOtaActive=false;
    message="esp_ota_end validation failed: "+String((int)err);
    riotRemoteOtaMessage=message;
    return false;
  }

  Serial.printf("OTA A6C2 IMAGE VALID; selecting boot partition %s\n",target->label);
  err=esp_ota_set_boot_partition(target);
  if(err!=ESP_OK){
    riotRemoteOtaActive=false;
    message="Could not select verified OTA partition: "+String((int)err);
    riotRemoteOtaMessage=message;
    return false;
  }

  Serial.printf("OTA A6C2 BOOT PARTITION SET %s; HTTP response then delayed reboot\n",target->label);
  riotRemoteOtaActive=false;
  riotRemoteOtaMessage="Verified "+m.version+" SHA-256 and installed "+String((unsigned long)total)+" bytes";
  message=riotRemoteOtaMessage+" to "+String(target->label)+". Reboot pending.";
  return true;
}

// RIOT LOCAL WEB OTA UPLOAD GATE A6B V1
static esp_ota_handle_t riotOtaHandle=0;
static const esp_partition_t* riotOtaTarget=nullptr;
static bool riotOtaActive=false;
static bool riotOtaComplete=false;
static bool riotOtaFailed=false;
static size_t riotOtaBytes=0;
static esp_err_t riotOtaLastError=ESP_OK;
static String riotOtaFilename="";
static uint32_t riotOtaRebootAtMs=0;

// RIOT A7B2 FORWARD DECL REPAIR V1
static String riotOtaUploadStatusJson();

// RIOT BROWSER MEDIATED OTA GATE A7B2 V1
static bool riotBrowserOtaShaActive=false;
static mbedtls_sha256_context riotBrowserOtaSha;
static String riotBrowserOtaExpectedSha="";
static String riotBrowserOtaActualSha="";
static String riotBrowserOtaVersion="";
static size_t riotBrowserOtaExpectedSize=0;

static void riotBrowserOtaResetVerify(){
  if(riotBrowserOtaShaActive){
    mbedtls_sha256_free(&riotBrowserOtaSha);
    riotBrowserOtaShaActive=false;
  }
  riotBrowserOtaExpectedSha="";
  riotBrowserOtaActualSha="";
  riotBrowserOtaVersion="";
  riotBrowserOtaExpectedSize=0;
}

static String riotBrowserOtaStatusJson(){
  String j=riotOtaUploadStatusJson();
  if(j.endsWith("}"))j.remove(j.length()-1);
  j+=",\"gateBrowser\":\"A7b2\"";
  j+=",\"expectedVersion\":\""+riotBrowserOtaVersion+"\"";
  j+=",\"expectedSize\":"+String((unsigned long)riotBrowserOtaExpectedSize);
  j+=",\"expectedSha256\":\""+riotBrowserOtaExpectedSha+"\"";
  j+=",\"actualSha256\":\""+riotBrowserOtaActualSha+"\"";
  j+="}";
  return j;
}

static void riotOtaResetUploadState(){
  riotOtaHandle=0;
  riotOtaTarget=nullptr;
  riotOtaActive=false;
  riotOtaComplete=false;
  riotOtaFailed=false;
  riotOtaBytes=0;
  riotOtaLastError=ESP_OK;
  riotOtaFilename="";
}

static void riotOtaAbortUpload(esp_err_t err){
  if(riotOtaActive){
    esp_ota_abort(riotOtaHandle);
    riotOtaActive=false;
  }
  riotOtaFailed=true;
  riotOtaComplete=false;
  riotOtaLastError=err;
}

static String riotOtaUploadStatusJson(){
  const esp_partition_t* running=esp_ota_get_running_partition();
  String j="{\"ok\":"+String(!riotOtaFailed?"true":"false");
  j+=",\"gate\":\"A6b\"";
  j+=",\"active\":"+String(riotOtaActive?"true":"false");
  j+=",\"complete\":"+String(riotOtaComplete?"true":"false");
  j+=",\"failed\":"+String(riotOtaFailed?"true":"false");
  j+=",\"bytes\":"+String((unsigned long)riotOtaBytes);
  j+=",\"error\":"+String((int)riotOtaLastError);
  j+=",\"filename\":\""+riotOtaFilename+"\"";
  j+=",\"running\":"+(running?riotPartitionJson(running):String("null"));
  j+=",\"target\":"+(riotOtaTarget?riotPartitionJson(riotOtaTarget):String("null"));
  j+=",\"rebootPending\":"+String(riotOtaRebootAtMs?"true":"false");
  j+="}";
  return j;
}

static constexpr int MODBUS_TX_PIN = 43; // XIAO D6
static constexpr int MODBUS_RX_PIN = 8;  // XIAO D9 - GPIO44/D7 is ePaper CS on TRMNL EE04
// EP32 M31 EDITABLE UNIT BAUD GATE4H V1
static uint32_t m31RtuBaud = 9600;
static uint8_t m31UnitId = 1;

// EE04 / TRMNL battery telemetry mapping. Keep these physical pin choices
// isolated so future processors can remap hardware without changing power logic.
static constexpr int BATTERY_ADC_PIN = 1;         // XIAO D0/A0, EE04 BAT_ADC
static constexpr int BATTERY_ADC_ENABLE_PIN = 6;  // XIAO D5/A5, EE04 ADC_EN
static constexpr bool BATTERY_TELEMETRY_AVAILABLE = true;
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
static constexpr float BATTERY_ADC_CALIBRATION = 0.9947f;
static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 5000UL;
static constexpr uint8_t BATTERY_SAMPLE_COUNT = 20;

// Main-power sensing hardware abstraction.
// GPIO42 / D15 is reserved for POWER_GOOD on EE04/XIAO Plus. Physical sensing
// remains disabled until the external 15k/22k VBUS divider is actually wired.
static constexpr int POWER_GOOD_PIN = 42;
static constexpr bool POWER_GOOD_SENSE_ENABLED = false;
static constexpr uint32_t POWER_GOOD_DEBOUNCE_MS = 1000UL;

enum PowerGoodOverride : uint8_t {
  POWER_OVERRIDE_AUTO = 0,
  POWER_OVERRIDE_FORCE_ON = 1,
  POWER_OVERRIDE_FORCE_OFF = 2
};

PowerGoodOverride powerGoodOverride = POWER_OVERRIDE_AUTO;
bool powerGood = true;
bool powerOutageActive = false;
bool powerInputCandidate = true;
uint32_t powerInputCandidateSinceMs = 0;
uint32_t powerOutageStartMs = 0;
uint32_t powerLastOutageDurationSec = 0;
uint32_t powerOutageCount = 0;
bool powerEventPending = false;
String powerPendingEvent = "";
uint32_t powerPendingDurationSec = 0;

float batteryVoltageV = NAN;
uint16_t batteryAdcRaw = 0;
uint32_t batterySampleCounter = 0;
uint32_t batteryLastSampleMs = 0;

void beginBatteryTelemetry(){
  pinMode(BATTERY_ADC_ENABLE_PIN,OUTPUT);
  digitalWrite(BATTERY_ADC_ENABLE_PIN,LOW);
  pinMode(BATTERY_ADC_PIN,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN,ADC_11db);
}

void sampleBatteryTelemetry(bool force=false){
  uint32_t now=millis();
  if(!force && batterySampleCounter &&
     now-batteryLastSampleMs<BATTERY_SAMPLE_INTERVAL_MS)return;

  batteryLastSampleMs=now;
  digitalWrite(BATTERY_ADC_ENABLE_PIN,HIGH);
  delay(10);

  uint32_t rawSum=0;
  uint32_t mvSum=0;
  for(uint8_t i=0;i<BATTERY_SAMPLE_COUNT;i++){
    rawSum+=analogRead(BATTERY_ADC_PIN);
    mvSum+=analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(100);
  }

  digitalWrite(BATTERY_ADC_ENABLE_PIN,LOW);

  batteryAdcRaw=(uint16_t)((rawSum+(BATTERY_SAMPLE_COUNT/2))/BATTERY_SAMPLE_COUNT);
  float adcMilliVolts=(float)mvSum/(float)BATTERY_SAMPLE_COUNT;
  batteryVoltageV=(adcMilliVolts/1000.0f)*BATTERY_DIVIDER_RATIO*BATTERY_ADC_CALIBRATION;
  batterySampleCounter++;
}

bool batteryTelemetryValid(){
  if(!BATTERY_TELEMETRY_AVAILABLE)return false;
  if(!batterySampleCounter || !isfinite(batteryVoltageV))return false;

  // A connected single-cell Li battery should remain comfortably above this
  // even when deeply discharged. Near-zero ADC readings mean no usable source
  // is connected and must never be treated as a real 0.000 V process value.
  if(batteryAdcRaw<100 || batteryVoltageV<2.0f || batteryVoltageV>5.0f)return false;
  return true;
}

HardwareSerial modbusSerial(1);
WiFiClient m31TcpClient;
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

struct NVSStatsSnapshot {
  size_t used=0;
  size_t free=0;
  size_t total=0;
  bool valid=false;
};

NVSStatsSnapshot getNVSStatsSnapshot(){
  nvs_stats_t st={};
  NVSStatsSnapshot r;
  if(nvs_get_stats(nullptr,&st)==ESP_OK){
    r.used=st.used_entries;
    r.free=st.free_entries;
    r.total=st.total_entries;
    r.valid=true;
  }
  return r;
}

void printNVSStats(const char* label){
  NVSStatsSnapshot st=getNVSStatsSnapshot();
  if(st.valid){
    Serial.printf("NVS %s: used=%u free=%u total=%u\n",
      label,
      (unsigned)st.used,
      (unsigned)st.free,
      (unsigned)st.total);
  }else{
    Serial.printf("NVS %s: stats unavailable\n",label);
  }
}

WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);

struct MQTTConfig {
  bool enabled=false;
  String host="";
  uint16_t port=1883;
  String user="";
  String pass="";
  String baseTopic="multiflow/controller";
  uint32_t liveIntervalSec=5;
  uint32_t totalizerIntervalSec=60;
  uint32_t statusIntervalSec=60;
  bool retain=false;
};

MQTTConfig mqttCfg;
bool saveMQTTConfig();
// EP32 MQTT FLASH PROOF GATE5F V1
static bool mqttFlashBootPrimaryOK=false;
static bool mqttFlashBootBackupOK=false;
static bool mqttFlashBootLoadOK=false;
static bool mqttFlashLastSaveOK=false;
static uint32_t mqttFlashSaveCount=0;

uint32_t mqttLastConnectAttempt=0;
uint32_t mqttLastLivePublish=0;
uint32_t mqttLastTotalizerPublish=0;
uint32_t mqttLastStatusPublish=0;
uint32_t mqttConnectCount=0;
uint32_t mqttPublishCount=0;
uint32_t mqttPublishFailCount=0;
uint64_t mqttPayloadBytes=0;
uint64_t mqttTopicBytes=0;
uint32_t mqttTrafficStartMs=0;

struct MQTTDailyUsage {
  uint32_t date=0;
  uint32_t publishes=0;
  uint64_t payloadBytes=0;
  uint64_t topicBytes=0;
};

MQTTDailyUsage mqttDailyUsage;

struct MQTTDailyUsageRecord {
  uint32_t date=0;
  uint32_t publishes=0;
  uint64_t payloadBytes=0;
  uint64_t topicBytes=0;
};

static constexpr uint8_t MQTT_DAILY_USAGE_QUEUE_SIZE=8;
MQTTDailyUsageRecord mqttDailyUsageQueue[MQTT_DAILY_USAGE_QUEUE_SIZE];
uint8_t mqttDailyUsageCount=0;
uint32_t mqttDailyNvsWriteFailures=0;
uint32_t mqttDailyNvsRetryAfterMs=0;
static constexpr uint32_t MQTT_DAILY_NVS_RETRY_BACKOFF_MS=60000UL;

void saveMQTTDailyUsage();
void rolloverMQTTDailyUsage(uint32_t newDate);

String wifiStaSsid="";
String wifiStaPass="";

// RIOT DEVICE IDENTITY + UNIQUE FACTORY AP GATE A4A V1
// Factory fallback AP identity is deterministic per controller.
// A future commissioning-page override may replace the AP name, but firmware
// upgrades must never overwrite an explicitly saved operator choice.
String riotFallbackApSsid="";

String riotDeviceMac(){
  String mac=WiFi.macAddress();
  mac.toUpperCase();
  return mac;
}

String riotDeviceId(){
  String mac=riotDeviceMac();
  mac.replace(":","");
  mac.replace("-","");
  if(mac.length()>=4)return mac.substring(mac.length()-4);
  return "0000";
}

String riotDefaultApSsid(){
  return String("Riot-")+riotDeviceId();
}

String siteName="MultiFlow Site";
String siteLSD="";
bool captivePortalActive=false;
uint32_t lastReconnectAttempt=0;
uint32_t wifiConnectStart=0;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS=15000;

bool timeServiceStarted=false;

void startTimeService(){
  if(timeServiceStarted || WiFi.status()!=WL_CONNECTED)return;
  // Alberta / Mountain Time with DST.
  configTzTime("MST7MDT,M3.2.0,M11.1.0","pool.ntp.org","time.nist.gov");
  timeServiceStarted=true;
}

String displayDateTime(){
  struct tm t;
  if(!getLocalTime(&t,5))return "--/--/----  --:--";
  char b[32];
  strftime(b,sizeof(b),"%Y-%m-%d  %H:%M",&t);
  return String(b);
}

bool diRawState[4] = {false,false,false,false};
bool diState[4] = {false,false,false,false}; // effective DI seen by logic
bool diForceActive[4] = {false,false,false,false};
bool diForceValue[4] = {false,false,false,false};
bool doState[4] = {false,false,false,false}; // physical/readback DO
bool doForceActive[4] = {false,false,false,false};
bool doForceValue[4] = {false,false,false,false};

// EP32 ACTIVE M31 RACK PROFILE GATE4C V1
// EP32 M31 MODULAR RACK PROFILE GATE A1 V3
//
// Gate A1 adds configurable host + two expansion slots.
// Existing proven polling behavior is deliberately preserved.
//
// Runtime polling in A1:
//   Host: AI1-4 / DI1-4 / DO1-4
//   Slot 1 GXAXA0404: AI5-8 / AO1-4
//   Slot 2: configuration/diagnostic metadata only.
//
// Persistence remains legacy V1 in this gate.

enum Ep32M31ExpansionProfile : uint8_t {
  EP32_M31_EXP_NONE=0,
  EP32_M31_EXP_GXAXA0404=1
};

enum Ep32M31HostModel : uint8_t {
  EP32_M31_HOST_AFAX4440G_U=0,
  EP32_M31_HOST_AAAX4440G=1
};

enum Ep32M31ModuleType : uint8_t {
  EP32_M31_MODULE_NONE=0,
  EP32_M31_MODULE_GXAXA0404=1,
  EP32_M31_MODULE_GXAXX0800=2
};

static Ep32M31ExpansionProfile ep32M31ExpansionProfile=EP32_M31_EXP_NONE;
static Ep32M31HostModel ep32M31Host=EP32_M31_HOST_AFAX4440G_U;
static Ep32M31ModuleType ep32M31Slot1=EP32_M31_MODULE_NONE;
static Ep32M31ModuleType ep32M31Slot2=EP32_M31_MODULE_NONE;

static const char* ep32M31HostModel(){
  switch(ep32M31Host){
    case EP32_M31_HOST_AAAX4440G:
      return "M31-AAAX4440G";

    case EP32_M31_HOST_AFAX4440G_U:
    default:
      return "M31-AFAX4440G-U";
  }
}

static const char* ep32M31ModuleModel(Ep32M31ModuleType m){
  switch(m){
    case EP32_M31_MODULE_GXAXA0404:
      return "M31-GXAXA0404";

    case EP32_M31_MODULE_GXAXX0800:
      return "M31-GXAXX0800";

    case EP32_M31_MODULE_NONE:
    default:
      return "NONE";
  }
}

static uint8_t ep32M31ModuleAiCount(Ep32M31ModuleType m){
  switch(m){
    case EP32_M31_MODULE_GXAXA0404:
      return 4;

    case EP32_M31_MODULE_GXAXX0800:
      return 8;

    default:
      return 0;
  }
}

static uint8_t ep32M31ModuleAoCount(Ep32M31ModuleType m){
  return m==EP32_M31_MODULE_GXAXA0404 ? 4 : 0;
}

static uint8_t ep32M31ConfiguredAiCount(){
  return 4
    + ep32M31ModuleAiCount(ep32M31Slot1)
    + ep32M31ModuleAiCount(ep32M31Slot2);
}

static uint8_t ep32M31ConfiguredAoCount(){
  return ep32M31ModuleAoCount(ep32M31Slot1)
    + ep32M31ModuleAoCount(ep32M31Slot2);
}

static bool ep32M31ExpansionPresent(){
  return ep32M31ExpansionProfile==EP32_M31_EXP_GXAXA0404;
}

// EP32 M31 AI16 ACQUISITION GATE A2A V2
// EP32 M31 AI16 DIAGNOSTICS GATE A2A1 V2
static bool ep32M31Slot2Ai8Present(){
  return ep32M31Slot2==EP32_M31_MODULE_GXAXX0800;
}

static uint8_t ep32M31ActiveAiCount(){
  if(ep32M31Slot2Ai8Present())return 16;
  return ep32M31ExpansionPresent()?8:4;
}

static uint8_t ep32M31ActiveAoCount(){
  return ep32M31ExpansionPresent()?4:0;
}

static const char* ep32M31ExpansionModel(){
  return ep32M31ExpansionPresent()?"M31-GXAXA0404":"NONE";
}

float ai_mA[16] = {
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0
};

struct AnalogEUConfig {
  char name[24];
  char units[12];
  float euLow;
  float euHigh;
};

AnalogEUConfig aiCfg[16] = {
  {"AGA DP","kPa",0.0f,100.0f},
  {"Static Pressure","kPa",0.0f,100.0f},
  {"Temperature","C",-40.0f,80.0f},
  {"Line Pressure","kPa",0.0f,100.0f},
  {"AI5","%",0.0f,100.0f},
  {"AI6","%",0.0f,100.0f},
  {"AI7","%",0.0f,100.0f},
  {"AI8","%",0.0f,100.0f},
  {"AI9","%",0.0f,100.0f},
  {"AI10","%",0.0f,100.0f},
  {"AI11","%",0.0f,100.0f},
  {"AI12","%",0.0f,100.0f},
  {"AI13","%",0.0f,100.0f},
  {"AI14","%",0.0f,100.0f},
  {"AI15","%",0.0f,100.0f},
  {"AI16","%",0.0f,100.0f}
};

bool aiManual[16]={
  false,false,true,false,false,false,false,false,
  false,false,false,false,false,false,false,false
};
float aiManualEU[16]={
  0,0,20,0,0,0,0,0,
  0,0,0,0,0,0,0,0
};
float aiEU[16] = {
  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0
};
float ao_mA[4] = {0,0,0,0};
bool aoForceActive[4] = {false,false,false,false};

// EP32 IO MQTT GATE6B1 SELECTION PERSISTENCE UI V1
// Per-channel publication selection is kept in its own flash file so the
// validated operator_config_v1.bin layout does not change.
bool ioMqttAI[8]={false,false,false,false,false,false,false,false};
bool ioMqttDI[4]={false,false,false,false};
bool ioMqttDO[4]={false,false,false,false};
bool ioMqttAO[4]={false,false,false,false};

static uint32_t ioMqttPublishCount=0;
static uint32_t ioMqttChangePublishCount=0;
static uint32_t ioMqttLastPublishMs=0;
static bool ioMqttDigitalSeen[8]={false,false,false,false,false,false,false,false};
static bool ioMqttDigitalLast[8]={false,false,false,false,false,false,false,false};

static constexpr const char* IO_MQTT_CFG_PATH="/io_mqtt_v1.bin";
static constexpr const char* IO_MQTT_CFG_TMP="/io_mqtt_v1.tmp";
static constexpr const char* IO_MQTT_CFG_BAK="/io_mqtt_v1.bak";
static constexpr uint32_t IO_MQTT_CFG_MAGIC=0x494F4D31UL; // IOM1
static constexpr uint16_t IO_MQTT_CFG_VERSION=1;

struct IOMQTTFlashConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t ai[8];
  uint8_t di[4];
  uint8_t dout[4];
  uint8_t ao[4];
  uint8_t reserved[8];
  uint32_t checksum;
};

static bool ioMqttFlashLoaded=false;
static bool ioMqttFlashLastSaveOK=false;
static String ioMqttFlashStatus="NOT LOADED";

static uint32_t ioMqttFlashChecksum(const IOMQTTFlashConfigV1& c){
  const uint8_t* p=(const uint8_t*)&c;
  size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static bool ioMqttFlashValid(const IOMQTTFlashConfigV1& c){
  if(c.magic!=IO_MQTT_CFG_MAGIC || c.version!=IO_MQTT_CFG_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=ioMqttFlashChecksum(c))return false;
  for(uint8_t i=0;i<8;i++)if(c.ai[i]>1)return false;
  for(uint8_t i=0;i<4;i++)if(c.di[i]>1 || c.dout[i]>1 || c.ao[i]>1)return false;
  return true;
}

static bool ioMqttFlashRead(const char* path,IOMQTTFlashConfigV1& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && ioMqttFlashValid(c);
}

static void ioMqttFlashCapture(IOMQTTFlashConfigV1& c){
  memset(&c,0,sizeof(c));
  c.magic=IO_MQTT_CFG_MAGIC;c.version=IO_MQTT_CFG_VERSION;c.size=sizeof(c);
  for(uint8_t i=0;i<8;i++)c.ai[i]=ioMqttAI[i]?1:0;
  for(uint8_t i=0;i<4;i++){c.di[i]=ioMqttDI[i]?1:0;c.dout[i]=ioMqttDO[i]?1:0;c.ao[i]=ioMqttAO[i]?1:0;}
  c.checksum=ioMqttFlashChecksum(c);
}

static void ioMqttFlashApply(const IOMQTTFlashConfigV1& c){
  for(uint8_t i=0;i<8;i++)ioMqttAI[i]=c.ai[i]!=0;
  for(uint8_t i=0;i<4;i++){ioMqttDI[i]=c.di[i]!=0;ioMqttDO[i]=c.dout[i]!=0;ioMqttAO[i]=c.ao[i]!=0;}
}

bool saveIOMQTTConfig(){
  IOMQTTFlashConfigV1 c;ioMqttFlashCapture(c);
  if(SPIFFS.exists(IO_MQTT_CFG_TMP))SPIFFS.remove(IO_MQTT_CFG_TMP);
  File f=SPIFFS.open(IO_MQTT_CFG_TMP,FILE_WRITE);
  if(!f){ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE OPEN FAIL";return false;}
  size_t wrote=f.write((const uint8_t*)&c,sizeof(c));f.flush();f.close();
  if(wrote!=sizeof(c)){SPIFFS.remove(IO_MQTT_CFG_TMP);ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE WRITE FAIL";return false;}
  IOMQTTFlashConfigV1 verify;
  if(!ioMqttFlashRead(IO_MQTT_CFG_TMP,verify)){SPIFFS.remove(IO_MQTT_CFG_TMP);ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE VERIFY FAIL";return false;}
  if(SPIFFS.exists(IO_MQTT_CFG_BAK))SPIFFS.remove(IO_MQTT_CFG_BAK);
  if(SPIFFS.exists(IO_MQTT_CFG_PATH) && !SPIFFS.rename(IO_MQTT_CFG_PATH,IO_MQTT_CFG_BAK)){
    SPIFFS.remove(IO_MQTT_CFG_TMP);ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE BACKUP FAIL";return false;
  }
  if(!SPIFFS.rename(IO_MQTT_CFG_TMP,IO_MQTT_CFG_PATH)){
    if(SPIFFS.exists(IO_MQTT_CFG_BAK))SPIFFS.rename(IO_MQTT_CFG_BAK,IO_MQTT_CFG_PATH);
    ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE COMMIT FAIL";return false;
  }
  if(!ioMqttFlashRead(IO_MQTT_CFG_PATH,verify)){
    SPIFFS.remove(IO_MQTT_CFG_PATH);
    if(SPIFFS.exists(IO_MQTT_CFG_BAK))SPIFFS.rename(IO_MQTT_CFG_BAK,IO_MQTT_CFG_PATH);
    ioMqttFlashLastSaveOK=false;ioMqttFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }
  if(SPIFFS.exists(IO_MQTT_CFG_BAK))SPIFFS.remove(IO_MQTT_CFG_BAK);
  ioMqttFlashLoaded=true;ioMqttFlashLastSaveOK=true;ioMqttFlashStatus="SAVED";
  return true;
}

void loadIOMQTTConfig(){
  IOMQTTFlashConfigV1 c;
  if(ioMqttFlashRead(IO_MQTT_CFG_PATH,c)){
    ioMqttFlashApply(c);ioMqttFlashLoaded=true;ioMqttFlashLastSaveOK=true;ioMqttFlashStatus="LOADED";
    Serial.printf("IOMQTT LOAD OK bytes=%u\n",(unsigned)sizeof(c));
    return;
  }
  for(uint8_t i=0;i<8;i++)ioMqttAI[i]=false;
  for(uint8_t i=0;i<4;i++){ioMqttDI[i]=false;ioMqttDO[i]=false;ioMqttAO[i]=false;}
  ioMqttFlashLoaded=false;ioMqttFlashLastSaveOK=false;
  ioMqttFlashStatus=SPIFFS.exists(IO_MQTT_CFG_PATH)?"INVALID - DEFAULTS":"MISSING - DEFAULTS";
  saveIOMQTTConfig();
}
float aoForcePercent[4] = {0,0,0,0};
uint8_t aoForceNext = 0;
uint32_t aoForceLastRefreshMs = 0;


enum EpaperThemeMode:uint8_t { EPAPER_THEME_LIGHT=0, EPAPER_THEME_DARK=1, EPAPER_THEME_AUTO=2 };

// RIOT PLATFORM DISPLAY MODE GATE A4B V1
enum RiotDisplayMode:uint8_t { RIOT_DISPLAY_AUTO=0, RIOT_DISPLAY_EPAPER=1, RIOT_DISPLAY_HEADLESS=2 };

struct TrmnlDisplayConfig {
  uint8_t displayMode=RIOT_DISPLAY_AUTO;
  uint8_t flowDecimals=3;
  bool showRunStatus=true;
  bool showRunInputs=true;
  bool showTotalFlow=true;
  bool showWiFi=true;
  bool showM31=true;
  bool showActiveAlarms=true;
  uint8_t themeMode=EPAPER_THEME_LIGHT;
  uint8_t lightHour=7;
  uint8_t darkHour=19;
};

TrmnlDisplayConfig displayCfg;
bool epaperRuntimeEnabled=false;

const char* riotDisplayModeName(uint8_t mode){
  if(mode==RIOT_DISPLAY_EPAPER)return "EPAPER";
  if(mode==RIOT_DISPLAY_HEADLESS)return "HEADLESS";
  return "AUTO";
}

bool riotDisplayModeWantsEpaper(){
#ifdef EPAPER_ENABLE
  return displayCfg.displayMode!=RIOT_DISPLAY_HEADLESS;
#else
  return false;
#endif
}

// RIOT EPAPER PASSIVE PRESENCE PROBE GATE A4C1 V1
// EE04 wiring: BUSY is GPIO4. This probe NEVER calls epaper.begin() and does
// not clock SPI. It only samples BUSY briefly as plain input, weak pull-up,
// and weak pull-down so connected and disconnected electrical signatures can
// be characterized before AUTO makes any boot decision.
static constexpr int RIOT_EPAPER_BUSY_PIN=4;
static constexpr uint8_t RIOT_EPAPER_PROBE_SAMPLES=24;

struct RiotEpaperProbeResult {
  uint8_t inputHigh=0;
  uint8_t pullupHigh=0;
  uint8_t pulldownHigh=0;
  String signature="UNKNOWN";
};

static uint8_t riotSampleBusyHigh(uint8_t mode){
  pinMode(RIOT_EPAPER_BUSY_PIN,mode);
  delay(2);
  uint8_t high=0;
  for(uint8_t i=0;i<RIOT_EPAPER_PROBE_SAMPLES;i++){
    if(digitalRead(RIOT_EPAPER_BUSY_PIN)==HIGH)high++;
    delayMicroseconds(250);
  }
  return high;
}

RiotEpaperProbeResult riotProbeEpaperBusyPassive(){
  RiotEpaperProbeResult r;
  r.inputHigh=riotSampleBusyHigh(INPUT);
  r.pullupHigh=riotSampleBusyHigh(INPUT_PULLUP);
  r.pulldownHigh=riotSampleBusyHigh(INPUT_PULLDOWN);
  pinMode(RIOT_EPAPER_BUSY_PIN,INPUT);

  const uint8_t n=RIOT_EPAPER_PROBE_SAMPLES;
  const bool upHigh=r.pullupHigh>=(uint8_t)(n-2);
  const bool downLow=r.pulldownHigh<=2;
  const bool bothHigh=r.pullupHigh>=(uint8_t)(n-2) && r.pulldownHigh>=(uint8_t)(n-2);
  const bool bothLow=r.pullupHigh<=2 && r.pulldownHigh<=2;

  if(upHigh && downLow)r.signature="FLOATING";
  else if(bothHigh)r.signature="DRIVEN_HIGH";
  else if(bothLow)r.signature="DRIVEN_LOW";
  else r.signature="MIXED";

  Serial.printf("EPAPER PASSIVE PROBE busyPin=%d inputHigh=%u/%u pullupHigh=%u/%u pulldownHigh=%u/%u signature=%s\n",
    RIOT_EPAPER_BUSY_PIN,
    (unsigned)r.inputHigh,(unsigned)n,
    (unsigned)r.pullupHigh,(unsigned)n,
    (unsigned)r.pulldownHigh,(unsigned)n,
    r.signature.c_str());
  return r;
}

String riotEpaperProbeJson(const RiotEpaperProbeResult& r){
  String j="{\"ok\":true";
  j+=",\"gate\":\"A4c1\"";
  j+=",\"passiveOnly\":true";
  j+=",\"busyPin\":"+String(RIOT_EPAPER_BUSY_PIN);
  j+=",\"samplesPerMode\":"+String(RIOT_EPAPER_PROBE_SAMPLES);
  j+=",\"inputHigh\":"+String(r.inputHigh);
  j+=",\"pullupHigh\":"+String(r.pullupHigh);
  j+=",\"pulldownHigh\":"+String(r.pulldownHigh);
  j+=",\"signature\":\""+r.signature+"\"";
  j+=",\"displayMode\":\""+String(riotDisplayModeName(displayCfg.displayMode))+"\"";
  j+=",\"epaperRuntimeEnabled\":"+String(epaperRuntimeEnabled?"true":"false");
  j+="}";
  return j;
}

// RIOT EPAPER AUTO PRESENCE RESOLUTION GATE A4C2 V1
String epaperAutoProbeSignature="NOT_RUN";
bool epaperAutoPresenceDetected=false;

bool riotResolveEpaperRuntime(){
#ifdef EPAPER_ENABLE
  if(displayCfg.displayMode==RIOT_DISPLAY_HEADLESS){
    epaperAutoProbeSignature="BYPASS_HEADLESS";
    epaperAutoPresenceDetected=false;
    return false;
  }
  if(displayCfg.displayMode==RIOT_DISPLAY_EPAPER){
    epaperAutoProbeSignature="BYPASS_FORCED_EPAPER";
    epaperAutoPresenceDetected=true;
    return true;
  }

  RiotEpaperProbeResult r=riotProbeEpaperBusyPassive();
  epaperAutoProbeSignature=r.signature;
  epaperAutoPresenceDetected=(r.signature=="DRIVEN_HIGH" || r.signature=="DRIVEN_LOW");

  Serial.printf("DISPLAY AUTO PROBE signature=%s present=%s -> %s\n",
    epaperAutoProbeSignature.c_str(),
    epaperAutoPresenceDetected?"YES":"NO",
    epaperAutoPresenceDetected?"EPAPER":"HEADLESS");

  return epaperAutoPresenceDetected;
#else
  epaperAutoProbeSignature="NO_EPAPER_BUILD";
  epaperAutoPresenceDetected=false;
  return false;
#endif
}

static constexpr const char* DISPLAY_LOGO_PATH="/display_logo.bin";
static constexpr const char* SITE_IDENTITY_PATH="/site_identity.cfg";
static constexpr uint16_t DISPLAY_LOGO_MAX_W=180;
static constexpr uint16_t DISPLAY_LOGO_MAX_H=56;
File logoUploadFile;
bool logoUploadOK=false;
bool epaperLogoInvert=false;

bool displayLogoExists(){
  if(!SPIFFS.exists(DISPLAY_LOGO_PATH))return false;
  File f=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_READ);
  if(!f)return false;
  size_t n=f.size();
  f.close();
  return n>=5 && n<=4+((DISPLAY_LOGO_MAX_W*DISPLAY_LOGO_MAX_H+7)/8);
}

// EP32 DISPLAY FLASH CONFIG GATE5B V1
static constexpr const char* DISPLAY_CFG_PATH="/display_config_v1.bin";
static constexpr const char* DISPLAY_CFG_TMP="/display_config_v1.tmp";
static constexpr const char* DISPLAY_CFG_BAK="/display_config_v1.bak";
static constexpr uint32_t DISPLAY_CFG_MAGIC=0x44434631UL; // DCF1
static constexpr uint16_t DISPLAY_CFG_VERSION=1;

struct DisplayFlashConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t flowDecimals;
  uint8_t showRunStatus;
  uint8_t showRunInputs;
  uint8_t showTotalFlow;
  uint8_t showWiFi;
  uint8_t showM31;
  uint8_t showActiveAlarms;
  uint8_t themeMode;
  uint8_t lightHour;
  uint8_t darkHour;
  uint8_t logoInvert;
  uint8_t reserved[5];
  uint32_t checksum;
};

static bool displayFlashLoaded=false;
static bool displayFlashLastSaveOK=false;
static String displayFlashStatus="NOT LOADED";

static uint32_t displayFlashChecksum(const DisplayFlashConfigV1& c){
  const uint8_t* p=(const uint8_t*)&c;
  const size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static bool displayFlashValid(const DisplayFlashConfigV1& c){
  if(c.magic!=DISPLAY_CFG_MAGIC || c.version!=DISPLAY_CFG_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=displayFlashChecksum(c))return false;
  if(c.flowDecimals>5 || c.themeMode>2 || c.lightHour>23 || c.darkHour>23)return false;
  if(c.reserved[0]>RIOT_DISPLAY_HEADLESS)return false;
  if(c.showRunStatus>1 || c.showRunInputs>1 || c.showTotalFlow>1 ||
     c.showWiFi>1 || c.showM31>1 || c.showActiveAlarms>1 || c.logoInvert>1)return false;
  return true;
}

static bool displayFlashRead(const char* path,DisplayFlashConfigV1& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && displayFlashValid(c);
}

static void displayFlashCapture(DisplayFlashConfigV1& c){
  memset(&c,0,sizeof(c));
  c.magic=DISPLAY_CFG_MAGIC;
  c.version=DISPLAY_CFG_VERSION;
  c.size=sizeof(c);
  c.flowDecimals=displayCfg.flowDecimals;
  c.showRunStatus=displayCfg.showRunStatus?1:0;
  c.showRunInputs=displayCfg.showRunInputs?1:0;
  c.showTotalFlow=displayCfg.showTotalFlow?1:0;
  c.showWiFi=displayCfg.showWiFi?1:0;
  c.showM31=displayCfg.showM31?1:0;
  c.showActiveAlarms=displayCfg.showActiveAlarms?1:0;
  c.themeMode=displayCfg.themeMode;
  c.lightHour=displayCfg.lightHour;
  c.darkHour=displayCfg.darkHour;
  c.logoInvert=epaperLogoInvert?1:0;
  c.reserved[0]=displayCfg.displayMode;
  c.checksum=displayFlashChecksum(c);
}

static void displayFlashApply(const DisplayFlashConfigV1& c){
  displayCfg.displayMode=(uint8_t)constrain((int)c.reserved[0],0,2);
  displayCfg.flowDecimals=c.flowDecimals;
  displayCfg.showRunStatus=c.showRunStatus!=0;
  displayCfg.showRunInputs=c.showRunInputs!=0;
  displayCfg.showTotalFlow=c.showTotalFlow!=0;
  displayCfg.showWiFi=c.showWiFi!=0;
  displayCfg.showM31=c.showM31!=0;
  displayCfg.showActiveAlarms=c.showActiveAlarms!=0;
  displayCfg.themeMode=c.themeMode;
  displayCfg.lightHour=c.lightHour;
  displayCfg.darkHour=c.darkHour;
  epaperLogoInvert=c.logoInvert!=0;
}

bool saveDisplayConfig(){
  DisplayFlashConfigV1 c;
  displayFlashCapture(c);

  if(SPIFFS.exists(DISPLAY_CFG_TMP))SPIFFS.remove(DISPLAY_CFG_TMP);
  File out=SPIFFS.open(DISPLAY_CFG_TMP,FILE_WRITE);
  if(!out){displayFlashLastSaveOK=false;displayFlashStatus="SAVE OPEN FAIL";return false;}
  size_t wrote=out.write((const uint8_t*)&c,sizeof(c));
  out.flush();out.close();
  if(wrote!=sizeof(c)){
    SPIFFS.remove(DISPLAY_CFG_TMP);
    displayFlashLastSaveOK=false;displayFlashStatus="SAVE WRITE FAIL";return false;
  }

  DisplayFlashConfigV1 verify;
  if(!displayFlashRead(DISPLAY_CFG_TMP,verify)){
    SPIFFS.remove(DISPLAY_CFG_TMP);
    displayFlashLastSaveOK=false;displayFlashStatus="SAVE VERIFY TMP FAIL";return false;
  }

  if(SPIFFS.exists(DISPLAY_CFG_BAK))SPIFFS.remove(DISPLAY_CFG_BAK);
  if(SPIFFS.exists(DISPLAY_CFG_PATH) && !SPIFFS.rename(DISPLAY_CFG_PATH,DISPLAY_CFG_BAK)){
    SPIFFS.remove(DISPLAY_CFG_TMP);
    displayFlashLastSaveOK=false;displayFlashStatus="SAVE BACKUP RENAME FAIL";return false;
  }
  if(!SPIFFS.rename(DISPLAY_CFG_TMP,DISPLAY_CFG_PATH)){
    if(SPIFFS.exists(DISPLAY_CFG_BAK))SPIFFS.rename(DISPLAY_CFG_BAK,DISPLAY_CFG_PATH);
    displayFlashLastSaveOK=false;displayFlashStatus="SAVE COMMIT RENAME FAIL";return false;
  }
  if(!displayFlashRead(DISPLAY_CFG_PATH,verify)){
    SPIFFS.remove(DISPLAY_CFG_PATH);
    if(SPIFFS.exists(DISPLAY_CFG_BAK))SPIFFS.rename(DISPLAY_CFG_BAK,DISPLAY_CFG_PATH);
    displayFlashLastSaveOK=false;displayFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }

  if(SPIFFS.exists(DISPLAY_CFG_BAK))SPIFFS.remove(DISPLAY_CFG_BAK);
  displayFlashLoaded=true;
  displayFlashLastSaveOK=true;
  displayFlashStatus="SAVED";
  Serial.printf("DISPLAYCFG SAVE OK bytes=%u theme=%u light=%u dark=%u invert=%u\n",
    (unsigned)sizeof(c),(unsigned)displayCfg.themeMode,
    (unsigned)displayCfg.lightHour,(unsigned)displayCfg.darkHour,
    epaperLogoInvert?1:0);
  return true;
}

void loadDisplayConfig(){
  DisplayFlashConfigV1 c;
  if(displayFlashRead(DISPLAY_CFG_PATH,c)){
    displayFlashApply(c);
    displayFlashLoaded=true;
    displayFlashLastSaveOK=true;
    displayFlashStatus="LOADED";
    Serial.printf("DISPLAYCFG LOAD OK bytes=%u theme=%u light=%u dark=%u invert=%u\n",
      (unsigned)sizeof(c),(unsigned)displayCfg.themeMode,
      (unsigned)displayCfg.lightHour,(unsigned)displayCfg.darkHour,
      epaperLogoInvert?1:0);
    return;
  }

  // One-time migration from the existing display NVS namespace. If it has no
  // keys, the existing compiled defaults remain unchanged.
  prefs.begin("display",true);
  bool legacy=prefs.isKey("dec") || prefs.isKey("theme") || prefs.isKey("runstat") ||
              prefs.isKey("runin") || prefs.isKey("total") || prefs.isKey("wifi") ||
              prefs.isKey("m31") || prefs.isKey("alarms") || prefs.isKey("lightHr") ||
              prefs.isKey("darkHr") || prefs.isKey("logoInv");
  if(legacy){
    displayCfg.flowDecimals=(uint8_t)constrain((int)prefs.getUChar("dec",displayCfg.flowDecimals),0,5);
    displayCfg.showRunStatus=prefs.getBool("runstat",displayCfg.showRunStatus);
    displayCfg.showRunInputs=prefs.getBool("runin",displayCfg.showRunInputs);
    displayCfg.showTotalFlow=prefs.getBool("total",displayCfg.showTotalFlow);
    displayCfg.showWiFi=prefs.getBool("wifi",displayCfg.showWiFi);
    displayCfg.showM31=prefs.getBool("m31",displayCfg.showM31);
    displayCfg.showActiveAlarms=prefs.getBool("alarms",displayCfg.showActiveAlarms);
    displayCfg.themeMode=(uint8_t)constrain((int)prefs.getUChar("theme",displayCfg.themeMode),0,2);
    displayCfg.lightHour=(uint8_t)constrain((int)prefs.getUChar("lightHr",displayCfg.lightHour),0,23);
    displayCfg.darkHour=(uint8_t)constrain((int)prefs.getUChar("darkHr",displayCfg.darkHour),0,23);
    epaperLogoInvert=prefs.getBool("logoInv",epaperLogoInvert);
  }
  prefs.end();

  displayFlashLoaded=false;
  displayFlashLastSaveOK=false;
  displayFlashStatus=legacy?"MIGRATING NVS":"MISSING - DEFAULTS";
  if(saveDisplayConfig()){
    displayFlashStatus=legacy?"MIGRATED NVS":"CREATED DEFAULTS";
  }
}



String displayConfigJson(){
  String j="{\"displayMode\":"+String(displayCfg.displayMode);
  j+=",\"displayModeName\":\""+String(riotDisplayModeName(displayCfg.displayMode))+"\"";
  j+=",\"epaperRuntimeEnabled\":"+String(epaperRuntimeEnabled?"true":"false");
  j+=",\"epaperPresenceDetected\":"+String(epaperAutoPresenceDetected?"true":"false");
  j+=",\"epaperProbeSignature\":\""+epaperAutoProbeSignature+"\"";
  j+=",\"rebootRequiredForModeChange\":true";
  j+=",\"flowDecimals\":"+String(displayCfg.flowDecimals);
  j+=",\"showRunStatus\":"+String(displayCfg.showRunStatus?"true":"false");
  j+=",\"showRunInputs\":"+String(displayCfg.showRunInputs?"true":"false");
  j+=",\"showTotalFlow\":"+String(displayCfg.showTotalFlow?"true":"false");
  j+=",\"showWiFi\":"+String(displayCfg.showWiFi?"true":"false");
  j+=",\"showM31\":"+String(displayCfg.showM31?"true":"false");
  j+=",\"showActiveAlarms\":"+String(displayCfg.showActiveAlarms?"true":"false");
  j+=",\"themeMode\":"+String(displayCfg.themeMode);
  j+=",\"lightHour\":"+String(displayCfg.lightHour);
  j+=",\"darkHour\":"+String(displayCfg.darkHour);
  j+=",\"siteName\":\""+siteName+"\"";
  j+=",\"siteLSD\":\""+siteLSD+"\"";
  j+=",\"logoStored\":"+String(displayLogoExists()?"true":"false");
  j+=",\"epaperLogoInvert\":"+String(epaperLogoInvert?"true":"false");
  j+="}";
  return j;
}

enum FlowInputRole:uint8_t { FLOW_ROLE_DP=0,FLOW_ROLE_STATIC=1,FLOW_ROLE_TEMP=2 };
enum FlowSourceType:uint8_t { FLOW_SRC_M31=1 };
enum FlowPointQuality:uint8_t { FLOW_QUALITY_BAD=0,FLOW_QUALITY_GOOD=1,FLOW_QUALITY_MANUAL=2 };

struct FlowInputBinding {
  uint8_t sourceType;
  uint8_t sourceIndex;
  bool manual;
  float manualValue;
};

struct FlowResolvedPoint {
  float value;
  FlowPointQuality quality;
  String sourceName;
};

extern bool modbusOnline;

static constexpr uint8_t FLOW_RUN_COUNT=6;
extern bool flowRunValid[FLOW_RUN_COUNT];
extern float flowRunFlow[FLOW_RUN_COUNT];
FlowResolvedPoint resolveFlowRunInput(uint8_t run,uint8_t role);

FlowInputBinding run1Bind[3]={
  {FLOW_SRC_M31,0,false,0.0f},
  {FLOW_SRC_M31,1,false,0.0f},
  {FLOW_SRC_M31,2,false,20.0f}
};

FlowInputBinding flowRunBind[FLOW_RUN_COUNT][3];
bool flowRunEnabled[FLOW_RUN_COUNT]={true,false,false,false,false,false};
char flowRunName[FLOW_RUN_COUNT][25]={{0}};
char flowRunLSD[FLOW_RUN_COUNT][25]={{0}};

enum ControlOutputType:uint8_t {
  CONTROL_OUTPUT_NONE=0,
  CONTROL_OUTPUT_AO=1,
  CONTROL_OUTPUT_DO=2
};

struct ManualControlOutput {
  ControlOutputType type=CONTROL_OUTPUT_NONE;
  uint8_t channel=0;       // zero-based AO/DO channel
  float aoPercent=0.0f;    // 0-100% => 4-20 mA
  bool doOn=false;
};

ManualControlOutput manualControlOutput[FLOW_RUN_COUNT];

enum FlowControlMode:uint8_t {
  FLOW_CTRL_DISABLED=0,
  FLOW_CTRL_MANUAL=1,
  FLOW_CTRL_AUTO=2
};

struct FlowPIDConfigV1 {
  uint8_t mode=FLOW_CTRL_DISABLED;
  uint8_t aoChannel=0;
  float flowSP=0.0f;
  float p=0.5f;
  float i=0.0f;
  float d=0.0f;
  float manualOut=0.0f;
  float outMin=0.0f;
  float outMax=100.0f;
};

struct FlowPIDConfig {
  uint8_t mode=FLOW_CTRL_DISABLED;
  uint8_t aoChannel=0;       // zero-based AO1-AO4
  float flowSP=0.0f;         // E3 m3/day
  float p=0.5f;
  float i=0.0f;
  float d=0.0f;
  float manualOut=0.0f;     // %
  float outMin=0.0f;        // %
  float outMax=100.0f;      // %

  // High static-pressure override. Higher AO % opens the valve, so the
  // override is a low-selector: final OUT = min(flow OUT, pressure OUT).
  bool pressureOverrideEnabled=false;
  float pressureSP=0.0f;     // same engineering units as run STATIC input
  float pressureP=1.0f;
  float pressureI=0.0f;
  float pressureD=0.0f;
};

struct FlowPIDRuntime {
  float output=0.0f;         // final selected output
  float flowOutput=0.0f;
  float integral=0.0f;
  float lastPV=0.0f;
  uint32_t lastCalcMs=0;
  uint8_t lastMode=FLOW_CTRL_DISABLED;
  bool initialized=false;
  bool inputGood=false;

  float pressureOutput=100.0f;
  float pressureIntegral=0.0f;
  float lastPressurePV=0.0f;
  uint32_t lastPressureCalcMs=0;
  bool pressureInitialized=false;
  bool pressureGood=false;
  bool pressureOverrideActive=false;
};

FlowPIDConfig flowPIDCfg[FLOW_RUN_COUNT];
FlowPIDRuntime flowPIDRt[FLOW_RUN_COUNT];

static constexpr uint32_t CONTROL_FILE_MAGIC=0x43545231UL; // CTR1

struct ControlFileV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  FlowPIDConfigV1 cfg[FLOW_RUN_COUNT];
  uint32_t checksum;
};

struct ControlFileV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  FlowPIDConfig cfg[FLOW_RUN_COUNT];
  uint32_t checksum;
};

// EP32 PID FLASH HARDENING GATE5E V1
static constexpr const char* CONTROL_FILE_PATH="/control.cfg";
static constexpr const char* CONTROL_FILE_TMP="/control.tmp";
static constexpr const char* CONTROL_FILE_BAK="/control.bak";
// EP32 GATE5E BOOT LOAD PROOF V1
static bool pidFlashLoaded=false;
static bool pidFlashLastSaveOK=false;
static bool pidFlashBootLoadOK=false;
static uint32_t pidFlashSaveCount=0;
static String pidFlashStatus="NOT LOADED";

template<typename T>
uint32_t controlFileChecksumT(const T& in){
  const uint8_t* p=(const uint8_t*)&in;
  size_t n=sizeof(T)-sizeof(uint32_t);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

void sanitizeFlowPIDConfig(uint8_t r){
  FlowPIDConfig &c=flowPIDCfg[r];
  if(c.mode>FLOW_CTRL_AUTO)c.mode=FLOW_CTRL_DISABLED;
  if(c.aoChannel>3)c.aoChannel=0;
  c.outMin=constrain(c.outMin,0.0f,100.0f);
  c.outMax=constrain(c.outMax,0.0f,100.0f);
  if(c.outMax<c.outMin)c.outMax=c.outMin;
  c.manualOut=constrain(c.manualOut,c.outMin,c.outMax);
}

bool saveFlowPIDConfig(){
  ControlFileV2 f={};
  f.magic=CONTROL_FILE_MAGIC;
  f.version=2;
  f.size=sizeof(f);
  for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
    sanitizeFlowPIDConfig(r);
    f.cfg[r]=flowPIDCfg[r];
  }
  f.checksum=controlFileChecksumT(f);

  if(SPIFFS.exists(CONTROL_FILE_TMP))SPIFFS.remove(CONTROL_FILE_TMP);
  File out=SPIFFS.open(CONTROL_FILE_TMP,FILE_WRITE);
  if(!out){pidFlashLastSaveOK=false;pidFlashStatus="SAVE OPEN FAIL";return false;}
  size_t n=out.write((const uint8_t*)&f,sizeof(f));
  out.flush();
  out.close();
  if(n!=sizeof(f)){
    SPIFFS.remove(CONTROL_FILE_TMP);
    pidFlashLastSaveOK=false;pidFlashStatus="SAVE WRITE FAIL";return false;
  }

  File verify=SPIFFS.open(CONTROL_FILE_TMP,FILE_READ);
  ControlFileV2 v={};
  bool verified=verify && verify.size()==sizeof(v) &&
    verify.read((uint8_t*)&v,sizeof(v))==sizeof(v) &&
    v.magic==CONTROL_FILE_MAGIC && v.version==2 && v.size==sizeof(v) &&
    v.checksum==controlFileChecksumT(v);
  if(verify)verify.close();
  if(!verified){
    SPIFFS.remove(CONTROL_FILE_TMP);
    pidFlashLastSaveOK=false;pidFlashStatus="SAVE VERIFY TMP FAIL";return false;
  }

  if(SPIFFS.exists(CONTROL_FILE_BAK))SPIFFS.remove(CONTROL_FILE_BAK);
  if(SPIFFS.exists(CONTROL_FILE_PATH) && !SPIFFS.rename(CONTROL_FILE_PATH,CONTROL_FILE_BAK)){
    SPIFFS.remove(CONTROL_FILE_TMP);
    pidFlashLastSaveOK=false;pidFlashStatus="SAVE BACKUP RENAME FAIL";return false;
  }
  if(!SPIFFS.rename(CONTROL_FILE_TMP,CONTROL_FILE_PATH)){
    if(SPIFFS.exists(CONTROL_FILE_BAK))SPIFFS.rename(CONTROL_FILE_BAK,CONTROL_FILE_PATH);
    pidFlashLastSaveOK=false;pidFlashStatus="SAVE COMMIT RENAME FAIL";return false;
  }

  verify=SPIFFS.open(CONTROL_FILE_PATH,FILE_READ);
  memset(&v,0,sizeof(v));
  verified=verify && verify.size()==sizeof(v) &&
    verify.read((uint8_t*)&v,sizeof(v))==sizeof(v) &&
    v.magic==CONTROL_FILE_MAGIC && v.version==2 && v.size==sizeof(v) &&
    v.checksum==controlFileChecksumT(v);
  if(verify)verify.close();
  if(!verified){
    SPIFFS.remove(CONTROL_FILE_PATH);
    if(SPIFFS.exists(CONTROL_FILE_BAK))SPIFFS.rename(CONTROL_FILE_BAK,CONTROL_FILE_PATH);
    pidFlashLastSaveOK=false;pidFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }

  if(SPIFFS.exists(CONTROL_FILE_BAK))SPIFFS.remove(CONTROL_FILE_BAK);
  pidFlashLoaded=true;
  pidFlashLastSaveOK=true;
  pidFlashSaveCount++;
  pidFlashStatus="SAVED";
  Serial.printf("PIDCFG SAVE OK bytes=%u\n",(unsigned)sizeof(f));
  return true;
}

void loadFlowPIDConfig(){
  for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
    flowPIDCfg[r]=FlowPIDConfig();
    flowPIDRt[r]=FlowPIDRuntime();
  }

  pidFlashLoaded=false;
  pidFlashLastSaveOK=false;
  pidFlashBootLoadOK=false;
  pidFlashStatus="MISSING - DEFAULTS";
  if(!SPIFFS.exists(CONTROL_FILE_PATH))return;

  File in=SPIFFS.open(CONTROL_FILE_PATH,FILE_READ);
  if(!in){pidFlashStatus="LOAD OPEN FAIL";return;}

  uint32_t magic=0;
  uint16_t version=0,size=0;
  if(in.read((uint8_t*)&magic,sizeof(magic))!=sizeof(magic) ||
     in.read((uint8_t*)&version,sizeof(version))!=sizeof(version) ||
     in.read((uint8_t*)&size,sizeof(size))!=sizeof(size)){
    in.close();
    pidFlashStatus="LOAD HEADER FAIL";
    return;
  }
  in.seek(0);

  if(magic==CONTROL_FILE_MAGIC && version==2 && size==sizeof(ControlFileV2)){
    ControlFileV2 f={};
    size_t n=in.read((uint8_t*)&f,sizeof(f));
    in.close();
    if(n!=sizeof(f) || f.checksum!=controlFileChecksumT(f)){
      pidFlashStatus="LOAD VERIFY FAIL";
      return;
    }
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
      flowPIDCfg[r]=f.cfg[r];
      sanitizeFlowPIDConfig(r);
    }
    pidFlashLoaded=true;
    pidFlashLastSaveOK=true;
    pidFlashBootLoadOK=true;
    pidFlashStatus="LOADED";
    Serial.printf("PIDCFG LOAD OK version=2 bytes=%u\n",(unsigned)sizeof(f));
    return;
  }

  if(magic==CONTROL_FILE_MAGIC && version==1 && size==sizeof(ControlFileV1)){
    ControlFileV1 f={};
    size_t n=in.read((uint8_t*)&f,sizeof(f));
    in.close();
    if(n!=sizeof(f) || f.checksum!=controlFileChecksumT(f)){
      pidFlashStatus="V1 VERIFY FAIL";
      return;
    }
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
      flowPIDCfg[r].mode=f.cfg[r].mode;
      flowPIDCfg[r].aoChannel=f.cfg[r].aoChannel;
      flowPIDCfg[r].flowSP=f.cfg[r].flowSP;
      flowPIDCfg[r].p=f.cfg[r].p;
      flowPIDCfg[r].i=f.cfg[r].i;
      flowPIDCfg[r].d=f.cfg[r].d;
      flowPIDCfg[r].manualOut=f.cfg[r].manualOut;
      flowPIDCfg[r].outMin=f.cfg[r].outMin;
      flowPIDCfg[r].outMax=f.cfg[r].outMax;
      sanitizeFlowPIDConfig(r);
    }
    if(saveFlowPIDConfig()){
      pidFlashStatus="MIGRATED V1";
      Serial.println("PIDCFG MIGRATED V1 -> V2");
    }else{
      pidFlashStatus="V1 MIGRATION SAVE FAIL";
    }
    return;
  }

  in.close();
  pidFlashStatus="UNSUPPORTED FORMAT";
}

bool flowPIDChannelConflict(uint8_t run,uint8_t channel){
  for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
    if(r==run)continue;
    if(flowPIDCfg[r].mode!=FLOW_CTRL_DISABLED && flowPIDCfg[r].aoChannel==channel)return true;
  }
  return false;
}

const char* flowControlModeName(uint8_t mode){
  if(mode==FLOW_CTRL_MANUAL)return "MANUAL";
  if(mode==FLOW_CTRL_AUTO)return "AUTO";
  return "DISABLED";
}

void initializePIDBumpless(uint8_t run,float pv){
  FlowPIDConfig &c=flowPIDCfg[run];
  FlowPIDRuntime &rt=flowPIDRt[run];
  float current=constrain((ao_mA[c.aoChannel]-4.0f)/0.16f,c.outMin,c.outMax);
  if(!isfinite(current))current=constrain(c.manualOut,c.outMin,c.outMax);
  rt.output=current;
  rt.integral=current-c.p*(c.flowSP-pv);
  rt.lastPV=pv;
  rt.lastCalcMs=millis();
  rt.initialized=true;
}

void serviceFlowPID(){
  static uint8_t nextRun=0;
  uint32_t now=millis();
  static uint32_t lastService=0;
  if(now-lastService<250)return;
  lastService=now;

  uint8_t run=nextRun;
  nextRun=(nextRun+1)%FLOW_RUN_COUNT;
  FlowPIDConfig &c=flowPIDCfg[run];
  FlowPIDRuntime &rt=flowPIDRt[run];

  if(!flowRunEnabled[run] || c.mode==FLOW_CTRL_DISABLED){
    rt.inputGood=false;
    rt.pressureGood=false;
    rt.pressureOverrideActive=false;
    rt.lastMode=FLOW_CTRL_DISABLED;
    rt.initialized=false;
    rt.pressureInitialized=false;
    return;
  }

  if(c.mode==FLOW_CTRL_MANUAL){
    rt.flowOutput=constrain(c.manualOut,c.outMin,c.outMax);
    rt.output=rt.flowOutput;
    rt.inputGood=true;
    rt.pressureGood=false;
    rt.pressureOverrideActive=false;
    rt.lastMode=FLOW_CTRL_MANUAL;
    rt.initialized=true;
    rt.pressureInitialized=false;
    rt.lastCalcMs=now;
    return;
  }

  bool good=flowRunValid[run] && isfinite(flowRunFlow[run]);
  rt.inputGood=good;
  if(!good)return;

  float pv=flowRunFlow[run];
  if(!rt.initialized || rt.lastMode!=FLOW_CTRL_AUTO){
    initializePIDBumpless(run,pv);
    rt.flowOutput=rt.output;
    rt.lastMode=FLOW_CTRL_AUTO;
  }else{
    float dt=(now-rt.lastCalcMs)/1000.0f;
    if(dt<=0.0f || dt>5.0f)dt=0.25f;
    float error=c.flowSP-pv;
    float dPV=(pv-rt.lastPV)/dt;

    rt.integral += c.i*error*dt;
    rt.integral=constrain(rt.integral,c.outMin,c.outMax);

    float out=c.p*error + rt.integral - c.d*dPV;
    out=constrain(out,c.outMin,c.outMax);

    if((out>=c.outMax && error>0.0f) || (out<=c.outMin && error<0.0f)){
      rt.integral -= c.i*error*dt;
      rt.integral=constrain(rt.integral,c.outMin,c.outMax);
      out=constrain(c.p*error + rt.integral - c.d*dPV,c.outMin,c.outMax);
    }

    rt.flowOutput=out;
    rt.lastPV=pv;
    rt.lastCalcMs=now;
  }

  rt.output=rt.flowOutput;
  rt.pressureGood=false;
  rt.pressureOverrideActive=false;

  if(c.pressureOverrideEnabled){
    FlowResolvedPoint pp=resolveFlowRunInput(run,FLOW_ROLE_STATIC);
    rt.pressureGood=isfinite(pp.value) && pp.quality!=FLOW_QUALITY_BAD;

    if(rt.pressureGood){
      float ppv=pp.value;
      float perror=c.pressureSP-ppv;

      // Below the high-pressure limit, track the flow controller output.
      // This makes takeover and release bumpless.
      if(ppv<c.pressureSP){
        rt.pressureOutput=rt.flowOutput;
        rt.pressureIntegral=rt.flowOutput-c.pressureP*perror;
        rt.lastPressurePV=ppv;
        rt.lastPressureCalcMs=now;
        rt.pressureInitialized=true;
      }else{
        if(!rt.pressureInitialized){
          rt.pressureOutput=rt.flowOutput;
          rt.pressureIntegral=rt.flowOutput-c.pressureP*perror;
          rt.lastPressurePV=ppv;
          rt.lastPressureCalcMs=now;
          rt.pressureInitialized=true;
        }

        float pdt=(now-rt.lastPressureCalcMs)/1000.0f;
        if(pdt<=0.0f || pdt>5.0f)pdt=0.25f;
        float dPPV=(ppv-rt.lastPressurePV)/pdt;

        rt.pressureIntegral += c.pressureI*perror*pdt;
        rt.pressureIntegral=constrain(rt.pressureIntegral,c.outMin,c.outMax);
        float pout=c.pressureP*perror + rt.pressureIntegral - c.pressureD*dPPV;
        pout=constrain(pout,c.outMin,c.outMax);

        if((pout>=c.outMax && perror>0.0f) || (pout<=c.outMin && perror<0.0f)){
          rt.pressureIntegral -= c.pressureI*perror*pdt;
          rt.pressureIntegral=constrain(rt.pressureIntegral,c.outMin,c.outMax);
          pout=constrain(c.pressureP*perror + rt.pressureIntegral - c.pressureD*dPPV,c.outMin,c.outMax);
        }

        rt.pressureOutput=pout;
        rt.lastPressurePV=ppv;
        rt.lastPressureCalcMs=now;
      }

      // High-pressure override can only close farther than the flow loop.
      rt.output=min(rt.flowOutput,rt.pressureOutput);
      rt.pressureOverrideActive=(ppv>=c.pressureSP && rt.pressureOutput<rt.flowOutput-0.05f);
    }
  }

  rt.output=constrain(rt.output,c.outMin,c.outMax);
  rt.lastMode=FLOW_CTRL_AUTO;
}


// EP32 GATE5C FORWARD DECL FIX V1
void initFlowRunDefaults();

const char* controlOutputTypeName(ControlOutputType t){
  switch(t){
    case CONTROL_OUTPUT_AO:return "AO";
    case CONTROL_OUTPUT_DO:return "DO";
    default:return "NONE";
  }
}

bool controlOutputConflict(uint8_t run,ControlOutputType type,uint8_t channel){
  if(type==CONTROL_OUTPUT_NONE)return false;
  for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
    if(r==run)continue;
    if(manualControlOutput[r].type==type &&
       manualControlOutput[r].channel==channel)return true;
  }
  return false;
}

// EP32 FLOW RUN FLASH CONFIG GATE5C V1
static constexpr const char* FLOWRUN_CFG_PATH="/flow_runs_v1.bin";
static constexpr const char* FLOWRUN_CFG_TMP="/flow_runs_v1.tmp";
static constexpr const char* FLOWRUN_CFG_BAK="/flow_runs_v1.bak";
static constexpr uint32_t FLOWRUN_CFG_MAGIC=0x46525631UL; // FRV1
static constexpr uint16_t FLOWRUN_CFG_VERSION=1;

struct FlowRunBindingFileV1 {
  uint8_t sourceType;
  uint8_t sourceIndex;
  uint8_t manual;
  uint8_t reserved;
  float manualValue;
};

struct FlowRunFileV1 {
  uint8_t enabled;
  char name[25];
  char lsd[25];
  FlowRunBindingFileV1 input[3];
};

struct FlowRunsFlashConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  FlowRunFileV1 run[FLOW_RUN_COUNT];
  uint32_t checksum;
};

static bool flowRunFlashLoaded=false;
static bool flowRunFlashLastSaveOK=false;
static String flowRunFlashStatus="NOT LOADED";

static uint32_t flowRunFlashChecksum(const FlowRunsFlashConfigV1& c){
  const uint8_t* p=(const uint8_t*)&c;
  const size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static bool flowRunFlashValid(FlowRunsFlashConfigV1& c){
  if(c.magic!=FLOWRUN_CFG_MAGIC || c.version!=FLOWRUN_CFG_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=flowRunFlashChecksum(c))return false;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    c.run[run].name[sizeof(c.run[run].name)-1]='\0';
    c.run[run].lsd[sizeof(c.run[run].lsd)-1]='\0';
    if(c.run[run].enabled>1)return false;
    for(uint8_t role=0;role<3;role++){
      FlowRunBindingFileV1& b=c.run[run].input[role];
      if(b.sourceType!=FLOW_SRC_M31 || b.sourceIndex>=8 || b.manual>1 || !isfinite(b.manualValue))return false;
    }
  }
  return true;
}

static bool flowRunFlashRead(const char* path,FlowRunsFlashConfigV1& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && flowRunFlashValid(c);
}

static void flowRunFlashCapture(FlowRunsFlashConfigV1& c){
  memset(&c,0,sizeof(c));
  c.magic=FLOWRUN_CFG_MAGIC;
  c.version=FLOWRUN_CFG_VERSION;
  c.size=sizeof(c);
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    c.run[run].enabled=flowRunEnabled[run]?1:0;
    strlcpy(c.run[run].name,flowRunName[run],sizeof(c.run[run].name));
    strlcpy(c.run[run].lsd,flowRunLSD[run],sizeof(c.run[run].lsd));
    for(uint8_t role=0;role<3;role++){
      FlowInputBinding& b=(run==0)?run1Bind[role]:flowRunBind[run][role];
      c.run[run].input[role].sourceType=b.sourceType;
      c.run[run].input[role].sourceIndex=b.sourceIndex;
      c.run[run].input[role].manual=b.manual?1:0;
      c.run[run].input[role].manualValue=b.manualValue;
    }
  }
  c.run[0].enabled=1;
  c.checksum=flowRunFlashChecksum(c);
}

static void flowRunFlashApply(const FlowRunsFlashConfigV1& c){
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    flowRunEnabled[run]=(run==0)?true:(c.run[run].enabled!=0);
    strlcpy(flowRunName[run],c.run[run].name,sizeof(flowRunName[run]));
    strlcpy(flowRunLSD[run],c.run[run].lsd,sizeof(flowRunLSD[run]));
    for(uint8_t role=0;role<3;role++){
      FlowInputBinding& b=(run==0)?run1Bind[role]:flowRunBind[run][role];
      b.sourceType=FLOW_SRC_M31;
      b.sourceIndex=c.run[run].input[role].sourceIndex;
      b.manual=c.run[run].input[role].manual!=0;
      b.manualValue=c.run[run].input[role].manualValue;
    }
  }
  for(uint8_t role=0;role<3;role++)flowRunBind[0][role]=run1Bind[role];
}

bool saveFlowRunsFlashConfig(){
  FlowRunsFlashConfigV1 c;
  flowRunFlashCapture(c);

  if(SPIFFS.exists(FLOWRUN_CFG_TMP))SPIFFS.remove(FLOWRUN_CFG_TMP);
  File out=SPIFFS.open(FLOWRUN_CFG_TMP,FILE_WRITE);
  if(!out){flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE OPEN FAIL";return false;}
  size_t wrote=out.write((const uint8_t*)&c,sizeof(c));
  out.flush();out.close();
  if(wrote!=sizeof(c)){
    SPIFFS.remove(FLOWRUN_CFG_TMP);
    flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE WRITE FAIL";return false;
  }

  FlowRunsFlashConfigV1 verify;
  if(!flowRunFlashRead(FLOWRUN_CFG_TMP,verify)){
    SPIFFS.remove(FLOWRUN_CFG_TMP);
    flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE VERIFY TMP FAIL";return false;
  }

  if(SPIFFS.exists(FLOWRUN_CFG_BAK))SPIFFS.remove(FLOWRUN_CFG_BAK);
  if(SPIFFS.exists(FLOWRUN_CFG_PATH) && !SPIFFS.rename(FLOWRUN_CFG_PATH,FLOWRUN_CFG_BAK)){
    SPIFFS.remove(FLOWRUN_CFG_TMP);
    flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE BACKUP RENAME FAIL";return false;
  }
  if(!SPIFFS.rename(FLOWRUN_CFG_TMP,FLOWRUN_CFG_PATH)){
    if(SPIFFS.exists(FLOWRUN_CFG_BAK))SPIFFS.rename(FLOWRUN_CFG_BAK,FLOWRUN_CFG_PATH);
    flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE COMMIT RENAME FAIL";return false;
  }
  if(!flowRunFlashRead(FLOWRUN_CFG_PATH,verify)){
    SPIFFS.remove(FLOWRUN_CFG_PATH);
    if(SPIFFS.exists(FLOWRUN_CFG_BAK))SPIFFS.rename(FLOWRUN_CFG_BAK,FLOWRUN_CFG_PATH);
    flowRunFlashLastSaveOK=false;flowRunFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }

  if(SPIFFS.exists(FLOWRUN_CFG_BAK))SPIFFS.remove(FLOWRUN_CFG_BAK);
  flowRunFlashLoaded=true;
  flowRunFlashLastSaveOK=true;
  flowRunFlashStatus="SAVED";
  Serial.printf("FLOWRUNCFG SAVE OK bytes=%u enabled=[%u,%u,%u,%u,%u,%u]\n",
    (unsigned)sizeof(c),
    flowRunEnabled[0],flowRunEnabled[1],flowRunEnabled[2],
    flowRunEnabled[3],flowRunEnabled[4],flowRunEnabled[5]);
  return true;
}

void loadFlowRunsFlashConfig(){
  initFlowRunDefaults();

  FlowRunsFlashConfigV1 c;
  if(flowRunFlashRead(FLOWRUN_CFG_PATH,c)){
    flowRunFlashApply(c);
    flowRunFlashLoaded=true;
    flowRunFlashLastSaveOK=true;
    flowRunFlashStatus="LOADED";
    Serial.printf("FLOWRUNCFG LOAD OK bytes=%u enabled=[%u,%u,%u,%u,%u,%u]\n",
      (unsigned)sizeof(c),
      flowRunEnabled[0],flowRunEnabled[1],flowRunEnabled[2],
      flowRunEnabled[3],flowRunEnabled[4],flowRunEnabled[5]);
    return;
  }

  // One-time migration of the existing six-run metadata and input bindings.
  bool legacy=false;
  prefs.begin("flowruns",true);
  legacy=prefs.isKey("v4mask") || prefs.isKey("r10i") || prefs.isKey("r10m") || prefs.isKey("r10v");
  uint16_t mask=prefs.getUShort("v4mask",1U);

  for(uint8_t role=0;role<3;role++){
    String k="r1"+String(role);
    run1Bind[role].sourceType=FLOW_SRC_M31;
    run1Bind[role].sourceIndex=prefs.getUChar((k+"i").c_str(),run1Bind[role].sourceIndex);
    run1Bind[role].manual=prefs.getBool((k+"m").c_str(),run1Bind[role].manual);
    run1Bind[role].manualValue=prefs.getFloat((k+"v").c_str(),run1Bind[role].manualValue);
    if(run1Bind[role].sourceIndex>=8)run1Bind[role].sourceIndex=role;
  }

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    flowRunEnabled[run]=(run==0)?true:((mask&(1U<<run))!=0);
    if(run==0)continue;
    for(uint8_t role=0;role<3;role++){
      String k="v4r"+String(run)+String(role);
      if(prefs.isKey((k+"i").c_str()) || prefs.isKey((k+"m").c_str()) || prefs.isKey((k+"v").c_str()))legacy=true;
      flowRunBind[run][role].sourceType=FLOW_SRC_M31;
      flowRunBind[run][role].sourceIndex=prefs.getUChar((k+"i").c_str(),flowRunBind[run][role].sourceIndex);
      flowRunBind[run][role].manual=prefs.getBool((k+"m").c_str(),flowRunBind[run][role].manual);
      flowRunBind[run][role].manualValue=prefs.getFloat((k+"v").c_str(),flowRunBind[run][role].manualValue);
      if(flowRunBind[run][role].sourceIndex>=8)flowRunBind[run][role].sourceIndex=role;
    }
  }
  prefs.end();

  prefs.begin("flowmeta",true);
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    String nk="n"+String(run),lk="l"+String(run);
    if(prefs.isKey(nk.c_str()) || prefs.isKey(lk.c_str()))legacy=true;
    String n=prefs.getString(nk.c_str(),"");
    String l=prefs.getString(lk.c_str(),"");
    n.toCharArray(flowRunName[run],sizeof(flowRunName[run]));
    l.toCharArray(flowRunLSD[run],sizeof(flowRunLSD[run]));
  }
  prefs.end();

  for(uint8_t role=0;role<3;role++)flowRunBind[0][role]=run1Bind[role];

  flowRunFlashLoaded=false;
  flowRunFlashLastSaveOK=false;
  flowRunFlashStatus=legacy?"MIGRATING NVS":"MISSING - DEFAULTS";
  if(saveFlowRunsFlashConfig()){
    flowRunFlashStatus=legacy?"MIGRATED NVS":"CREATED DEFAULTS";
  }
}

void initFlowRunDefaults(){
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    for(uint8_t role=0;role<3;role++){
      flowRunBind[run][role]=run1Bind[role];
    }
  }
}

void loadFlowRunFoundation(){
  Serial.println("FLOWRUNCFG: legacy flow foundation NVS loader retired");
}

bool saveFlowRunFoundation(uint8_t run){
  if(run>=FLOW_RUN_COUNT)return false;
  return saveFlowRunsFlashConfig();
}

String flowRunDisplayName(uint8_t run){
  if(run>=FLOW_RUN_COUNT)return "FLOW";
  String x;
  if(flowRunName[run][0])x=flowRunName[run];
  if(flowRunLSD[run][0]){
    if(x.length())x+=" - ";
    x+=flowRunLSD[run];
  }
  if(!x.length())x="FLOW RUN "+String(run+1);
  return x;
}

FlowResolvedPoint resolveFlowRunInput(uint8_t run,uint8_t role){
  FlowResolvedPoint r={NAN,FLOW_QUALITY_BAD,""};
  if(run>=FLOW_RUN_COUNT||role>2)return r;
  FlowInputBinding &b=(run==0)?run1Bind[role]:flowRunBind[run][role];
  if(b.manual){
    r.value=b.manualValue;
    r.quality=isfinite(r.value)?FLOW_QUALITY_MANUAL:FLOW_QUALITY_BAD;
    r.sourceName="Manual";
    return r;
  }
  if(b.sourceIndex<8){
    r.value=aiEU[b.sourceIndex];
    if(aiManual[b.sourceIndex]){
      r.quality=isfinite(r.value)?FLOW_QUALITY_MANUAL:FLOW_QUALITY_BAD;
      r.sourceName="M31 AI"+String(b.sourceIndex+1)+" analog override";
    }else{
      r.quality=(modbusOnline&&isfinite(r.value))?FLOW_QUALITY_GOOD:FLOW_QUALITY_BAD;
      r.sourceName="M31 AI"+String(b.sourceIndex+1);
    }
  }
  return r;
}

const char* flowQualityName(FlowPointQuality q){
  if(q==FLOW_QUALITY_GOOD)return "GOOD";
  if(q==FLOW_QUALITY_MANUAL)return "MANUAL";
  return "BAD";
}

FlowResolvedPoint resolveRun1Input(uint8_t role){
  FlowResolvedPoint r={NAN,FLOW_QUALITY_BAD,""};
  if(role>2)return r;
  FlowInputBinding& b=run1Bind[role];

  if(b.manual){
    r.value=b.manualValue;
    r.quality=isfinite(r.value)?FLOW_QUALITY_MANUAL:FLOW_QUALITY_BAD;
    r.sourceName="Manual";
    return r;
  }

  if(b.sourceType==FLOW_SRC_M31 && b.sourceIndex<8){
    r.value=aiEU[b.sourceIndex];
    if(aiManual[b.sourceIndex]){
      r.quality=isfinite(r.value)?FLOW_QUALITY_MANUAL:FLOW_QUALITY_BAD;
      r.sourceName="M31 AI"+String(b.sourceIndex+1)+" analog override";
    }else{
      r.quality=(modbusOnline&&isfinite(r.value))?FLOW_QUALITY_GOOD:FLOW_QUALITY_BAD;
      r.sourceName="M31 AI"+String(b.sourceIndex+1);
    }
  }
  return r;
}

bool run1InputsReady(){
  for(uint8_t role=0;role<3;role++){
    FlowResolvedPoint p=resolveRun1Input(role);
    if(!isfinite(p.value)||p.quality==FLOW_QUALITY_BAD)return false;
  }
  return true;
}

bool run1DependsOnLiveM31(){
  for(uint8_t role=0;role<3;role++){
    if(!run1Bind[role].manual)return true;
  }
  return false;
}

void loadRun1Bindings(){
  Serial.println("FLOWRUNCFG: legacy Run1 binding NVS loader retired");
}

void saveRun1Bindings(){
  (void)saveFlowRunsFlashConfig();
}

struct AGASetupConfig {
  float pipeID_mm;
  float orifice_mm;
  float basePressure_kPaAbs;
  float baseTemperature_C;
  float atmosphericPressure_kPa;
  bool staticPressureIsAbsolute;
  uint8_t tapType;
};

AGASetupConfig agaRunCfg[FLOW_RUN_COUNT];
AGASetupConfig &agaCfg=agaRunCfg[0];

void setDefaultAGAConfig(AGASetupConfig &c){
  c.pipeID_mm=100.0f;
  c.orifice_mm=50.0f;
  c.basePressure_kPaAbs=101.325f;
  c.baseTemperature_C=15.0f;
  c.atmosphericPressure_kPa=101.325f;
  c.staticPressureIsAbsolute=false;
  c.tapType=0;
}

static constexpr int AGA_COMPONENTS=21;
float agaRunMolPct[FLOW_RUN_COUNT][AGA_COMPONENTS]={{0}};
float (&agaMolPct)[AGA_COMPONENTS]=agaRunMolPct[0];

void setDefaultGas(float* gas){
  for(uint8_t i=0;i<AGA_COMPONENTS;i++)gas[i]=0.0f;
  gas[0]=100.0f;
}

extern bool displayDirty;

static constexpr uint8_t GAS_LIBRARY_SLOTS=6;
char gasLibraryName[GAS_LIBRARY_SLOTS][25]={{0}};
bool gasLibraryUsed[GAS_LIBRARY_SLOTS]={false,false,false,false,false,false};
uint8_t gasLibraryStandard=255;

// EP32 AGA FLASH CONFIG GATE5D V1
static constexpr const char* AGA_CFG_PATH="/aga_config_v1.bin";
static constexpr const char* AGA_CFG_TMP="/aga_config_v1.tmp";
static constexpr const char* AGA_CFG_BAK="/aga_config_v1.bak";
static constexpr uint32_t AGA_CFG_MAGIC=0x41474631UL; // AGF1
static constexpr uint16_t AGA_CFG_VERSION=1;

struct AGAMeterFileV1 {
  float pipeID_mm;
  float orifice_mm;
  float basePressure_kPaAbs;
  float baseTemperature_C;
  float atmosphericPressure_kPa;
  uint8_t staticPressureIsAbsolute;
  uint8_t tapType;
  uint8_t reserved[2];
};

struct AGALibrarySlotFileV1 {
  uint8_t used;
  char name[25];
  uint8_t reserved[2];
  float gas[AGA_COMPONENTS];
};

struct AGAFlashConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  AGAMeterFileV1 meter[FLOW_RUN_COUNT];
  float runGas[FLOW_RUN_COUNT][AGA_COMPONENTS];
  uint8_t libraryStandard;
  uint8_t reserved[3];
  AGALibrarySlotFileV1 library[GAS_LIBRARY_SLOTS];
  uint32_t checksum;
};

float gasLibraryMolPct[GAS_LIBRARY_SLOTS][AGA_COMPONENTS]={{0}};
static bool agaFlashLoaded=false;
static bool agaFlashLastSaveOK=false;
static String agaFlashStatus="NOT LOADED";

static uint32_t agaFlashChecksum(const AGAFlashConfigV1& c){
  const uint8_t* p=(const uint8_t*)&c;
  const size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static bool agaMeterValid(const AGAMeterFileV1& m){
  return isfinite(m.pipeID_mm) && m.pipeID_mm>0.0f &&
         isfinite(m.orifice_mm) && m.orifice_mm>0.0f &&
         m.orifice_mm<m.pipeID_mm &&
         isfinite(m.basePressure_kPaAbs) && m.basePressure_kPaAbs>0.0f &&
         isfinite(m.baseTemperature_C) &&
         isfinite(m.atmosphericPressure_kPa) && m.atmosphericPressure_kPa>=0.0f &&
         m.staticPressureIsAbsolute<=1 && m.tapType<=2;
}

static bool agaGasValid(const float* gas){
  float total=0.0f;
  for(uint8_t i=0;i<AGA_COMPONENTS;i++){
    if(!isfinite(gas[i]) || gas[i]<0.0f || gas[i]>100.0f)return false;
    total+=gas[i];
  }
  return fabsf(total-100.0f)<=0.05f;
}

static bool agaFlashValid(AGAFlashConfigV1& c){
  if(c.magic!=AGA_CFG_MAGIC || c.version!=AGA_CFG_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=agaFlashChecksum(c))return false;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    if(!agaMeterValid(c.meter[run]) || !agaGasValid(c.runGas[run]))return false;
  }
  if(c.libraryStandard!=255 && c.libraryStandard>=GAS_LIBRARY_SLOTS)return false;
  for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
    c.library[slot].name[sizeof(c.library[slot].name)-1]='\0';
    if(c.library[slot].used>1)return false;
    if(c.library[slot].used && !agaGasValid(c.library[slot].gas))return false;
  }
  return true;
}

static bool agaFlashRead(const char* path,AGAFlashConfigV1& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && agaFlashValid(c);
}

static void agaFlashCapture(AGAFlashConfigV1& c){
  memset(&c,0,sizeof(c));
  c.magic=AGA_CFG_MAGIC;
  c.version=AGA_CFG_VERSION;
  c.size=sizeof(c);
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    const AGASetupConfig& a=agaRunCfg[run];
    AGAMeterFileV1& m=c.meter[run];
    m.pipeID_mm=a.pipeID_mm;
    m.orifice_mm=a.orifice_mm;
    m.basePressure_kPaAbs=a.basePressure_kPaAbs;
    m.baseTemperature_C=a.baseTemperature_C;
    m.atmosphericPressure_kPa=a.atmosphericPressure_kPa;
    m.staticPressureIsAbsolute=a.staticPressureIsAbsolute?1:0;
    m.tapType=a.tapType;
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)c.runGas[run][i]=agaRunMolPct[run][i];
  }
  c.libraryStandard=gasLibraryStandard;
  for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
    c.library[slot].used=gasLibraryUsed[slot]?1:0;
    strlcpy(c.library[slot].name,gasLibraryName[slot],sizeof(c.library[slot].name));
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)c.library[slot].gas[i]=gasLibraryMolPct[slot][i];
  }
  c.checksum=agaFlashChecksum(c);
}

static void agaFlashApply(const AGAFlashConfigV1& c){
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    const AGAMeterFileV1& m=c.meter[run];
    AGASetupConfig& a=agaRunCfg[run];
    a.pipeID_mm=m.pipeID_mm;
    a.orifice_mm=m.orifice_mm;
    a.basePressure_kPaAbs=m.basePressure_kPaAbs;
    a.baseTemperature_C=m.baseTemperature_C;
    a.atmosphericPressure_kPa=m.atmosphericPressure_kPa;
    a.staticPressureIsAbsolute=m.staticPressureIsAbsolute!=0;
    a.tapType=m.tapType;
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)agaRunMolPct[run][i]=c.runGas[run][i];
  }
  gasLibraryStandard=c.libraryStandard;
  for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
    gasLibraryUsed[slot]=c.library[slot].used!=0;
    strlcpy(gasLibraryName[slot],c.library[slot].name,sizeof(gasLibraryName[slot]));
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)gasLibraryMolPct[slot][i]=c.library[slot].gas[i];
  }
}

bool saveAGAFlashConfig(){
  AGAFlashConfigV1 c;
  agaFlashCapture(c);
  if(!agaFlashValid(c)){
    agaFlashLastSaveOK=false;
    agaFlashStatus="SAVE VALIDATION FAIL";
    return false;
  }

  if(SPIFFS.exists(AGA_CFG_TMP))SPIFFS.remove(AGA_CFG_TMP);
  File out=SPIFFS.open(AGA_CFG_TMP,FILE_WRITE);
  if(!out){agaFlashLastSaveOK=false;agaFlashStatus="SAVE OPEN FAIL";return false;}
  size_t wrote=out.write((const uint8_t*)&c,sizeof(c));
  out.flush();out.close();
  if(wrote!=sizeof(c)){
    SPIFFS.remove(AGA_CFG_TMP);
    agaFlashLastSaveOK=false;agaFlashStatus="SAVE WRITE FAIL";return false;
  }

  AGAFlashConfigV1 verify;
  if(!agaFlashRead(AGA_CFG_TMP,verify)){
    SPIFFS.remove(AGA_CFG_TMP);
    agaFlashLastSaveOK=false;agaFlashStatus="SAVE VERIFY TMP FAIL";return false;
  }

  if(SPIFFS.exists(AGA_CFG_BAK))SPIFFS.remove(AGA_CFG_BAK);
  if(SPIFFS.exists(AGA_CFG_PATH) && !SPIFFS.rename(AGA_CFG_PATH,AGA_CFG_BAK)){
    SPIFFS.remove(AGA_CFG_TMP);
    agaFlashLastSaveOK=false;agaFlashStatus="SAVE BACKUP RENAME FAIL";return false;
  }
  if(!SPIFFS.rename(AGA_CFG_TMP,AGA_CFG_PATH)){
    if(SPIFFS.exists(AGA_CFG_BAK))SPIFFS.rename(AGA_CFG_BAK,AGA_CFG_PATH);
    agaFlashLastSaveOK=false;agaFlashStatus="SAVE COMMIT RENAME FAIL";return false;
  }
  if(!agaFlashRead(AGA_CFG_PATH,verify)){
    SPIFFS.remove(AGA_CFG_PATH);
    if(SPIFFS.exists(AGA_CFG_BAK))SPIFFS.rename(AGA_CFG_BAK,AGA_CFG_PATH);
    agaFlashLastSaveOK=false;agaFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }

  if(SPIFFS.exists(AGA_CFG_BAK))SPIFFS.remove(AGA_CFG_BAK);
  agaFlashLoaded=true;
  agaFlashLastSaveOK=true;
  agaFlashStatus="SAVED";
  Serial.printf("AGACFG SAVE OK bytes=%u gaslibStd=%d\n",(unsigned)sizeof(c),
    gasLibraryStandard<GAS_LIBRARY_SLOTS?(int)gasLibraryStandard:-1);
  return true;
}

void loadAGAFlashConfig(){
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    setDefaultAGAConfig(agaRunCfg[run]);
    setDefaultGas(agaRunMolPct[run]);
  }
  memset(gasLibraryName,0,sizeof(gasLibraryName));
  memset(gasLibraryUsed,0,sizeof(gasLibraryUsed));
  memset(gasLibraryMolPct,0,sizeof(gasLibraryMolPct));
  gasLibraryStandard=255;

  AGAFlashConfigV1 c;
  if(agaFlashRead(AGA_CFG_PATH,c)){
    agaFlashApply(c);
    agaFlashLoaded=true;
    agaFlashLastSaveOK=true;
    agaFlashStatus="LOADED";
    Serial.printf("AGACFG LOAD OK bytes=%u gaslibStd=%d\n",(unsigned)sizeof(c),
      gasLibraryStandard<GAS_LIBRARY_SLOTS?(int)gasLibraryStandard:-1);
    return;
  }

  bool legacy=false;

  // Run 1 meter + gas.
  prefs.begin("aga",true);
  legacy=prefs.isKey("pipeID") || prefs.isKey("orifice") || prefs.isKey("c0");
  agaCfg.pipeID_mm=prefs.getFloat("pipeID",agaCfg.pipeID_mm);
  agaCfg.orifice_mm=prefs.getFloat("orifice",agaCfg.orifice_mm);
  agaCfg.basePressure_kPaAbs=prefs.getFloat("baseP",agaCfg.basePressure_kPaAbs);
  agaCfg.baseTemperature_C=prefs.getFloat("baseT",agaCfg.baseTemperature_C);
  agaCfg.atmosphericPressure_kPa=prefs.getFloat("atmP",agaCfg.atmosphericPressure_kPa);
  agaCfg.staticPressureIsAbsolute=prefs.getBool("pabs",agaCfg.staticPressureIsAbsolute);
  agaCfg.tapType=prefs.getUChar("tap",agaCfg.tapType);
  for(uint8_t i=0;i<AGA_COMPONENTS;i++){
    String k="c"+String(i);
    agaMolPct[i]=prefs.getFloat(k.c_str(),agaMolPct[i]);
  }
  prefs.end();

  for(uint8_t run=1;run<FLOW_RUN_COUNT;run++){
    agaRunCfg[run]=agaCfg;
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)agaRunMolPct[run][i]=agaRunMolPct[0][i];
  }

  // Per-run meter overrides.
  prefs.begin("agaruns",true);
  for(uint8_t run=1;run<FLOW_RUN_COUNT;run++){
    String p="r"+String(run+1);
    if(prefs.isKey((p+"pi").c_str()) || prefs.isKey((p+"or").c_str()))legacy=true;
    agaRunCfg[run].pipeID_mm=prefs.getFloat((p+"pi").c_str(),agaRunCfg[run].pipeID_mm);
    agaRunCfg[run].orifice_mm=prefs.getFloat((p+"or").c_str(),agaRunCfg[run].orifice_mm);
    agaRunCfg[run].basePressure_kPaAbs=prefs.getFloat((p+"bp").c_str(),agaRunCfg[run].basePressure_kPaAbs);
    agaRunCfg[run].baseTemperature_C=prefs.getFloat((p+"bt").c_str(),agaRunCfg[run].baseTemperature_C);
    agaRunCfg[run].atmosphericPressure_kPa=prefs.getFloat((p+"ap").c_str(),agaRunCfg[run].atmosphericPressure_kPa);
    agaRunCfg[run].staticPressureIsAbsolute=prefs.getBool((p+"ab").c_str(),agaRunCfg[run].staticPressureIsAbsolute);
    agaRunCfg[run].tapType=prefs.getUChar((p+"tp").c_str(),agaRunCfg[run].tapType);
  }
  prefs.end();

  // Per-run gas overrides.
  prefs.begin("agagas",true);
  for(uint8_t run=1;run<FLOW_RUN_COUNT;run++){
    String p="r"+String(run+1);
    if(!prefs.isKey((p+"c0").c_str()))continue;
    legacy=true;
    for(uint8_t i=0;i<AGA_COMPONENTS;i++){
      String k=p+"c"+String(i);
      agaRunMolPct[run][i]=prefs.getFloat(k.c_str(),agaRunMolPct[run][i]);
    }
  }
  prefs.end();

  // Named gas analysis library, including compositions.
  prefs.begin("gaslib",true);
  if(prefs.isKey("std"))legacy=true;
  gasLibraryStandard=prefs.getUChar("std",255);
  if(gasLibraryStandard>=GAS_LIBRARY_SLOTS)gasLibraryStandard=255;
  for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
    String nk="n"+String(slot);
    String n=prefs.getString(nk.c_str(),"");
    if(n.length())legacy=true;
    n.toCharArray(gasLibraryName[slot],sizeof(gasLibraryName[slot]));
    gasLibraryUsed[slot]=n.length()>0;
    if(gasLibraryUsed[slot]){
      for(uint8_t i=0;i<AGA_COMPONENTS;i++){
        String k="s"+String(slot)+"c"+String(i);
        gasLibraryMolPct[slot][i]=prefs.getFloat(k.c_str(),0.0f);
      }
    }
  }
  prefs.end();

  // If a legacy library slot is marked used but its gas is absent/corrupt,
  // leave migration uncommitted rather than silently writing a bad analysis.
  for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
    if(gasLibraryUsed[slot] && !agaGasValid(gasLibraryMolPct[slot])){
      agaFlashLoaded=false;
      agaFlashLastSaveOK=false;
      agaFlashStatus="MIGRATION VALIDATION FAIL";
      Serial.printf("AGACFG MIGRATION ABORT gas library slot=%u invalid\n",(unsigned)slot);
      return;
    }
  }

  agaFlashLoaded=false;
  agaFlashLastSaveOK=false;
  agaFlashStatus=legacy?"MIGRATING NVS":"MISSING - DEFAULTS";
  if(saveAGAFlashConfig()){
    agaFlashStatus=legacy?"MIGRATED NVS":"CREATED DEFAULTS";
  }
}

void saveAGAConfig();

void loadGasLibraryMeta(){
  Serial.println("AGACFG: legacy gaslib NVS loader retired");
}

bool saveGasLibrarySlot(uint8_t slot,const String& name,uint8_t sourceRun){
  if(slot>=GAS_LIBRARY_SLOTS || sourceRun>=FLOW_RUN_COUNT)return false;
  String clean=name;clean.trim();
  if(!clean.length())clean="Analysis "+String(slot+1);
  if(clean.length()>24)clean=clean.substring(0,24);
  clean.toCharArray(gasLibraryName[slot],sizeof(gasLibraryName[slot]));
  gasLibraryUsed[slot]=true;
  for(uint8_t i=0;i<AGA_COMPONENTS;i++)gasLibraryMolPct[slot][i]=agaRunMolPct[sourceRun][i];
  return saveAGAFlashConfig();
}

void saveAGARunGas(uint8_t run);

bool loadGasLibrarySlot(uint8_t slot,uint8_t targetRun){
  if(slot>=GAS_LIBRARY_SLOTS || !gasLibraryUsed[slot] || targetRun>=FLOW_RUN_COUNT)return false;
  for(uint8_t i=0;i<AGA_COMPONENTS;i++)agaRunMolPct[targetRun][i]=gasLibraryMolPct[slot][i];
  if(!saveAGAFlashConfig())return false;
  displayDirty=true;
  return true;
}

bool copyGasRun(uint8_t sourceRun,uint8_t targetRun){
  if(sourceRun>=FLOW_RUN_COUNT || targetRun>=FLOW_RUN_COUNT || sourceRun==targetRun)return false;
  for(uint8_t i=0;i<AGA_COMPONENTS;i++)agaRunMolPct[targetRun][i]=agaRunMolPct[sourceRun][i];
  saveAGARunGas(targetRun);
  displayDirty=true;
  return true;
}

void setGasLibraryStandard(uint8_t slot){
  if(slot>=GAS_LIBRARY_SLOTS || !gasLibraryUsed[slot])return;
  gasLibraryStandard=slot;
  (void)saveAGAFlashConfig();
}

AGAEngineResult agaResult={};
AGAEngineResult flowRunResult[FLOW_RUN_COUNT]={};
bool flowRunValid[FLOW_RUN_COUNT]={false};
float flowRunFlow[FLOW_RUN_COUNT]={0};

struct FlowDayTotal {
  double volumeE3m3=0.0;
  double flowHours=0.0;
};

FlowDayTotal flowDay[FLOW_RUN_COUNT];

struct DailyRolloverRecord {
  uint32_t date=0;
  uint8_t run=0;
  double volumeE3m3=0.0;
  double flowHours=0.0;
};

static constexpr uint8_t DAILY_ROLLOVER_QUEUE_SIZE=32;
DailyRolloverRecord dailyRolloverQueue[DAILY_ROLLOVER_QUEUE_SIZE];
uint8_t dailyRolloverCount=0;

struct FlowHourAccumulator {
  double volumeE3m3=0.0;
  double flowHours=0.0;
  double dpTimeSum=0.0;
  double staticTimeSum=0.0;
  double tempTimeSum=0.0;
  double validSeconds=0.0;
};

FlowHourAccumulator flowHour[FLOW_RUN_COUNT];

struct HourlyHistoryRecord {
  uint32_t hourKey=0; // YYYYMMDDHH local time
  uint8_t run=0;
  double volumeE3m3=0.0;
  double flowHours=0.0;
  float avgDP=0.0f;
  float avgStatic=0.0f;
  float avgTemp=0.0f;
};

static constexpr uint8_t HOURLY_HISTORY_QUEUE_SIZE=96;
HourlyHistoryRecord hourlyHistoryQueue[HOURLY_HISTORY_QUEUE_SIZE];
uint8_t hourlyHistoryCount=0;
uint32_t flowTotalizerHour=0;

uint32_t flowTotalizerDate=0;
uint32_t flowTotalizerLastMs=0;
uint32_t flowTotalizerLastSaveMs=0;
bool flowTotalizerClockValid=false;
bool flowTotalizerClockBackstepActive=false;
uint32_t flowTotalizerClockBackstepCount=0;
static constexpr uint8_t FLOW_TIME_HISTORY_SOURCE_INDEX=254;

uint32_t localDateKey(){
  struct tm t;
  if(!getLocalTime(&t,5))return 0;
  return (uint32_t)(t.tm_year+1900)*10000UL+(uint32_t)(t.tm_mon+1)*100UL+(uint32_t)t.tm_mday;
}

uint32_t localHourKey(){
  struct tm t;
  if(!getLocalTime(&t,5))return 0;
  return
    (uint32_t)(t.tm_year+1900)*1000000UL+
    (uint32_t)(t.tm_mon+1)*10000UL+
    (uint32_t)t.tm_mday*100UL+
    (uint32_t)t.tm_hour;
}

void logFlowTimeBoundaryEvent(const char* label,uint32_t previousHour,uint32_t currentHour,uint32_t previousDate,uint32_t currentDate,float value){
  RiotHistoryStore::Entry rec={};
  rec.sequence=(uint32_t)RiotHistoryStore::count(SPIFFS)+1U;
  time_t nowTime=time(nullptr);
  rec.epoch=(nowTime>1000000000)?(uint32_t)nowTime:0U;
  rec.type=RiotHistoryStore::ENTRY_EVENT;
  rec.sourceIndex=FLOW_TIME_HISTORY_SOURCE_INDEX;
  rec.value=value;
  rec.threshold=0.0f;
  snprintf(rec.label,sizeof(rec.label),"%s",label);
  snprintf(rec.message,sizeof(rec.message),"H%lu>%lu D%lu>%lu",
    (unsigned long)previousHour,(unsigned long)currentHour,
    (unsigned long)previousDate,(unsigned long)currentDate);
  if(!RiotHistoryStore::append(SPIFFS,rec)){
    Serial.println("FLOW TIME boundary history append failed");
  }
  Serial.printf("FLOW TIME EVENT %s H%lu>%lu D%lu>%lu\n",label,
    (unsigned long)previousHour,(unsigned long)currentHour,
    (unsigned long)previousDate,(unsigned long)currentDate);
}

void saveDailyRolloverQueue(){
  // Intentionally RAM-only. Large history blobs do not belong in the shared
  // NVS config partition; finalized records are sent through MQTT.
}

void loadDailyRolloverQueue(){
  memset(dailyRolloverQueue,0,sizeof(dailyRolloverQueue));
  dailyRolloverCount=0;
}

bool queueDailyRollover(uint8_t run,uint32_t date,double volume,double hours){
  if(run>=FLOW_RUN_COUNT || !date)return false;

  if(dailyRolloverCount>=DAILY_ROLLOVER_QUEUE_SIZE){
    // Preserve the newest finalized records if the broker has been offline
    // longer than the queue capacity.
    memmove(
      &dailyRolloverQueue[0],
      &dailyRolloverQueue[1],
      sizeof(DailyRolloverRecord)*(DAILY_ROLLOVER_QUEUE_SIZE-1)
    );
    dailyRolloverCount=DAILY_ROLLOVER_QUEUE_SIZE-1;
  }

  DailyRolloverRecord &r=dailyRolloverQueue[dailyRolloverCount++];
  r.date=date;
  r.run=run;
  r.volumeE3m3=volume;
  r.flowHours=hours;

  RiotDailyFlowHistoryStore::Entry local={};
  local.date=r.date;
  local.run=r.run;
  local.volumeE3m3=(float)r.volumeE3m3;
  local.flowHours=(float)r.flowHours;
  if(!RiotDailyFlowHistoryStore::append(SPIFFS,local)){
    Serial.println("DAILY FLOW HISTORY local append failed");
  }

  saveDailyRolloverQueue();
  return true;
}

void removeDailyRolloverFront(){
  if(!dailyRolloverCount)return;
  if(dailyRolloverCount>1){
    memmove(
      &dailyRolloverQueue[0],
      &dailyRolloverQueue[1],
      sizeof(DailyRolloverRecord)*(dailyRolloverCount-1)
    );
  }
  dailyRolloverCount--;
  memset(&dailyRolloverQueue[dailyRolloverCount],0,sizeof(DailyRolloverRecord));
  saveDailyRolloverQueue();
}

void saveHourlyHistoryQueue(){
  // Intentionally RAM-only. Keep NVS reserved for configuration and small
  // checkpoint state so broker/network settings remain reliable.
}

void loadHourlyHistoryQueue(){
  memset(hourlyHistoryQueue,0,sizeof(hourlyHistoryQueue));
  hourlyHistoryCount=0;
}

void queueHourlyHistory(uint8_t run,uint32_t hourKey,const FlowHourAccumulator& a){
  if(run>=FLOW_RUN_COUNT || !hourKey)return;

  if(hourlyHistoryCount>=HOURLY_HISTORY_QUEUE_SIZE){
    memmove(
      &hourlyHistoryQueue[0],
      &hourlyHistoryQueue[1],
      sizeof(HourlyHistoryRecord)*(HOURLY_HISTORY_QUEUE_SIZE-1)
    );
    hourlyHistoryCount=HOURLY_HISTORY_QUEUE_SIZE-1;
  }

  HourlyHistoryRecord &r=hourlyHistoryQueue[hourlyHistoryCount++];
  r.hourKey=hourKey;
  r.run=run;
  r.volumeE3m3=a.volumeE3m3;
  r.flowHours=a.flowHours;
  if(a.validSeconds>0.0){
    r.avgDP=(float)(a.dpTimeSum/a.validSeconds);
    r.avgStatic=(float)(a.staticTimeSum/a.validSeconds);
    r.avgTemp=(float)(a.tempTimeSum/a.validSeconds);
  }else{
    r.avgDP=0.0f;
    r.avgStatic=0.0f;
    r.avgTemp=0.0f;
  }

  RiotFlowHistoryStore::Entry local={};
  local.hourKey=r.hourKey;
  local.run=r.run;
  local.volumeE3m3=(float)r.volumeE3m3;
  local.flowHours=(float)r.flowHours;
  local.avgDP=r.avgDP;
  local.avgStatic=r.avgStatic;
  local.avgTemp=r.avgTemp;
  if(!RiotFlowHistoryStore::append(SPIFFS,local)){
    Serial.println("FLOW HISTORY local append failed");
  }
}

void finalizeHourlyHistory(uint32_t newHourKey){
  uint32_t closingHour=flowTotalizerHour;
  if(closingHour && closingHour!=newHourKey){
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      if(!flowRunEnabled[run] &&
         flowHour[run].volumeE3m3<=0.0 &&
         flowHour[run].flowHours<=0.0 &&
         flowHour[run].validSeconds<=0.0)continue;
      queueHourlyHistory(run,closingHour,flowHour[run]);
    }
    saveHourlyHistoryQueue();
  }

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++)flowHour[run]=FlowHourAccumulator{};
  flowTotalizerHour=newHourKey;
}

void removeHourlyHistoryFront(){
  if(!hourlyHistoryCount)return;
  if(hourlyHistoryCount>1){
    memmove(
      &hourlyHistoryQueue[0],
      &hourlyHistoryQueue[1],
      sizeof(HourlyHistoryRecord)*(hourlyHistoryCount-1)
    );
  }
  hourlyHistoryCount--;
  memset(&hourlyHistoryQueue[hourlyHistoryCount],0,sizeof(HourlyHistoryRecord));
  saveHourlyHistoryQueue();
}

static constexpr uint32_t FLOWTOT_STATE_MAGIC=0x46545432UL; // "FTT2"
static constexpr uint16_t FLOWTOT_STATE_VERSION=2;
static constexpr const char* FLOWTOT_STATE_PATH="/flowtot_state.bin";
static constexpr const char* FLOWTOT_STATE_TMP_PATH="/flowtot_state.tmp";

struct FlowTotalizerStateV2 {
  uint32_t magic=FLOWTOT_STATE_MAGIC;
  uint16_t version=FLOWTOT_STATE_VERSION;
  uint16_t runCount=FLOW_RUN_COUNT;
  uint32_t date=0;
  uint32_t hour=0;
  FlowDayTotal day[FLOW_RUN_COUNT];
  FlowHourAccumulator hourState[FLOW_RUN_COUNT];
  uint32_t checksum=0;
};

uint32_t flowTotalizerStateChecksum(const FlowTotalizerStateV2& in){
  const uint8_t* p=(const uint8_t*)&in;
  size_t n=offsetof(FlowTotalizerStateV2,checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){
    h^=p[i];
    h*=16777619UL;
  }
  return h;
}

bool writeFlowTotalizerStateFile(){
  FlowTotalizerStateV2 s;
  s.date=flowTotalizerDate;
  s.hour=flowTotalizerHour;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    s.day[run]=flowDay[run];
    s.hourState[run]=flowHour[run];
  }
  s.checksum=flowTotalizerStateChecksum(s);

  if(SPIFFS.exists(FLOWTOT_STATE_TMP_PATH))SPIFFS.remove(FLOWTOT_STATE_TMP_PATH);
  File out=SPIFFS.open(FLOWTOT_STATE_TMP_PATH,FILE_WRITE);
  if(!out){
    Serial.println("FLOWTOT SPIFFS checkpoint open failed");
    return false;
  }
  size_t written=out.write((const uint8_t*)&s,sizeof(s));
  out.flush();
  out.close();
  if(written!=sizeof(s)){
    if(SPIFFS.exists(FLOWTOT_STATE_TMP_PATH))SPIFFS.remove(FLOWTOT_STATE_TMP_PATH);
    Serial.printf("FLOWTOT SPIFFS checkpoint short write: %u/%u bytes\n",
      (unsigned)written,(unsigned)sizeof(s));
    return false;
  }

  if(SPIFFS.exists(FLOWTOT_STATE_PATH))SPIFFS.remove(FLOWTOT_STATE_PATH);
  if(!SPIFFS.rename(FLOWTOT_STATE_TMP_PATH,FLOWTOT_STATE_PATH)){
    if(SPIFFS.exists(FLOWTOT_STATE_TMP_PATH))SPIFFS.remove(FLOWTOT_STATE_TMP_PATH);
    Serial.println("FLOWTOT SPIFFS checkpoint rename failed");
    return false;
  }
  return true;
}

void saveFlowTotalizers(){
  if(!writeFlowTotalizerStateFile()){
    Serial.println("FLOWTOT checkpoint save failed");
  }
}

bool loadFlowTotalizerStateFile(){
  File in=SPIFFS.open(FLOWTOT_STATE_PATH,FILE_READ);
  if(!in)return false;
  if(in.size()!=sizeof(FlowTotalizerStateV2)){
    in.close();
    return false;
  }

  FlowTotalizerStateV2 s;
  size_t got=in.read((uint8_t*)&s,sizeof(s));
  in.close();
  if(got!=sizeof(s) ||
     s.magic!=FLOWTOT_STATE_MAGIC ||
     s.version!=FLOWTOT_STATE_VERSION ||
     s.runCount!=FLOW_RUN_COUNT ||
     s.checksum!=flowTotalizerStateChecksum(s)){
    return false;
  }

  flowTotalizerDate=s.date;
  flowTotalizerHour=s.hour;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    flowDay[run]=s.day[run];
    flowHour[run]=s.hourState[run];
  }
  return true;
}

void loadFlowTotalizers(){
  bool fileLoaded=loadFlowTotalizerStateFile();
  bool legacyFound=false;

  if(!fileLoaded){
    // One-time migration from the old NVS checkpoint if it still exists.
    prefs.begin("flowtot",true);
    legacyFound=
      prefs.isKey("date") || prefs.isKey("hour") ||
      prefs.isKey("v0") || prefs.isKey("q0v");

    flowTotalizerDate=prefs.getUInt("date",0);
    flowTotalizerHour=prefs.getUInt("hour",0);
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      String vk="v"+String(run);
      String hk="h"+String(run);
      flowDay[run].volumeE3m3=prefs.getDouble(vk.c_str(),0.0);
      flowDay[run].flowHours=prefs.getDouble(hk.c_str(),0.0);

      String p="q"+String(run);
      flowHour[run].volumeE3m3=prefs.getDouble((p+"v").c_str(),0.0);
      flowHour[run].flowHours=prefs.getDouble((p+"h").c_str(),0.0);
      flowHour[run].dpTimeSum=prefs.getDouble((p+"d").c_str(),0.0);
      flowHour[run].staticTimeSum=prefs.getDouble((p+"p").c_str(),0.0);
      flowHour[run].tempTimeSum=prefs.getDouble((p+"t").c_str(),0.0);
      flowHour[run].validSeconds=prefs.getDouble((p+"s").c_str(),0.0);
    }
    prefs.end();

    if(writeFlowTotalizerStateFile()){
      if(legacyFound){
        prefs.begin("flowtot",false);
        prefs.clear();
        prefs.end();
      }
      Serial.printf("FLOWTOT migrated to SPIFFS V2 (%u bytes, legacy=%s)\n",
        (unsigned)sizeof(FlowTotalizerStateV2),legacyFound?"YES":"NO");
    }
  }else{
    Serial.printf("FLOWTOT SPIFFS V2 loaded (%u bytes)\n",
      (unsigned)sizeof(FlowTotalizerStateV2));
  }

  flowTotalizerLastMs=millis();
  flowTotalizerLastSaveMs=millis();
}

void resetFlowToday(uint32_t dateKey){
  uint32_t closingDate=flowTotalizerDate;
  rolloverMQTTDailyUsage(dateKey);

  // Finalize the closing contract day before clearing today's accumulators.
  // A zero closingDate means first initialization/migration, not a real day.
  if(closingDate && closingDate!=dateKey){
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      if(!flowRunEnabled[run] && flowDay[run].volumeE3m3<=0.0 && flowDay[run].flowHours<=0.0)continue;
      queueDailyRollover(
        run,
        closingDate,
        flowDay[run].volumeE3m3,
        flowDay[run].flowHours
      );
    }
  }

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    flowDay[run].volumeE3m3=0.0;
    flowDay[run].flowHours=0.0;
  }
  flowTotalizerDate=dateKey;
  saveFlowTotalizers();
  displayDirty=true;
}

void serviceFlowTotalizers(){
  uint32_t now=millis();
  uint32_t dateKey=localDateKey();
  uint32_t hourKey=localHourKey();

  if(!dateKey || !hourKey){
    // Never roll or integrate against an unknown calendar time.
    flowTotalizerLastMs=now;
    return;
  }

  const RiotTimeService::BoundaryRelation dateRelation=
    RiotTimeService::compareDateKey(flowTotalizerDate,dateKey);
  const RiotTimeService::BoundaryRelation hourRelation=
    RiotTimeService::compareHourKey(flowTotalizerHour,hourKey);
  const bool calendarBackstep=
    dateRelation==RiotTimeService::BOUNDARY_BACKWARD ||
    hourRelation==RiotTimeService::BOUNDARY_BACKWARD;

  // A backward clock correction must never close a second copy of an hour/day.
  // Keep the persisted period keys as high-water marks and suspend integration
  // until wall-clock time catches back up. Reset lastMs while held so elapsed
  // time cannot accumulate into a later catch-up sample.
  if(calendarBackstep){
    if(!flowTotalizerClockBackstepActive){
      flowTotalizerClockBackstepActive=true;
      flowTotalizerClockBackstepCount++;
      logFlowTimeBoundaryEvent("TIME_STEP_BACKWARD",
        flowTotalizerHour,hourKey,flowTotalizerDate,dateKey,-1.0f);
    }
    flowTotalizerClockValid=true;
    flowTotalizerLastMs=now;
    return;
  }

  if(flowTotalizerClockBackstepActive){
    flowTotalizerClockBackstepActive=false;
    logFlowTimeBoundaryEvent("TIME_STEP_RECOVERED",
      flowTotalizerHour,hourKey,flowTotalizerDate,dateKey,0.0f);
    flowTotalizerLastMs=now;
  }

  if(!flowTotalizerClockValid){
    flowTotalizerClockValid=true;

    rolloverMQTTDailyUsage(dateKey);

    // If rebooted after an hour boundary, finalize the persisted previous
    // hour before beginning the current hour.
    if(flowTotalizerHour!=hourKey)finalizeHourlyHistory(hourKey);
    if(flowTotalizerDate!=dateKey)resetFlowToday(dateKey);

    flowTotalizerLastMs=now;
    saveFlowTotalizers();
    return;
  }

  if(flowTotalizerHour!=hourKey){
    finalizeHourlyHistory(hourKey);
    saveFlowTotalizers();
  }

  if(flowTotalizerDate!=dateKey){
    resetFlowToday(dateKey);
    flowTotalizerLastMs=now;
    return;
  }

  uint32_t elapsedMs=now-flowTotalizerLastMs;
  if(elapsedMs<1000)return;
  flowTotalizerLastMs=now;

  double elapsedDays=(double)elapsedMs/86400000.0;
  double elapsedHours=(double)elapsedMs/3600000.0;
  double elapsedSeconds=(double)elapsedMs/1000.0;
  bool changed=false;

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    if(!flowRunEnabled[run])continue;

    if(flowRunValid[run]){
      float q=flowRunFlow[run];
      if(isfinite(q) && q>0.0f){
        double dv=(double)q*elapsedDays;
        flowDay[run].volumeE3m3+=dv;
        flowDay[run].flowHours+=elapsedHours;
        flowHour[run].volumeE3m3+=dv;
        flowHour[run].flowHours+=elapsedHours;
        changed=true;
      }
    }

    FlowResolvedPoint dp=resolveFlowRunInput(run,FLOW_ROLE_DP);
    FlowResolvedPoint sp=resolveFlowRunInput(run,FLOW_ROLE_STATIC);
    FlowResolvedPoint tp=resolveFlowRunInput(run,FLOW_ROLE_TEMP);
    if(dp.quality!=FLOW_QUALITY_BAD &&
       sp.quality!=FLOW_QUALITY_BAD &&
       tp.quality!=FLOW_QUALITY_BAD &&
       isfinite(dp.value) && isfinite(sp.value) && isfinite(tp.value)){
      flowHour[run].dpTimeSum+=(double)dp.value*elapsedSeconds;
      flowHour[run].staticTimeSum+=(double)sp.value*elapsedSeconds;
      flowHour[run].tempTimeSum+=(double)tp.value*elapsedSeconds;
      flowHour[run].validSeconds+=elapsedSeconds;
    }
  }

  if(changed)displayDirty=true;

  // Periodic NVS checkpoint. Five minutes limits flash wear while preserving
  // current day and current hour state across normal reboots/power interruptions.
  if(now-flowTotalizerLastSaveMs>=300000UL){
    flowTotalizerLastSaveMs=now;
    saveFlowTotalizers();
    saveMQTTDailyUsage();
  }
}

uint32_t flowRunCalcCount[FLOW_RUN_COUNT]={0};
uint32_t flowRunCalcMs[FLOW_RUN_COUNT]={0};

float totalFlowPreview = 0.0f;
bool flowPreviewValid = false;
bool agaCoreReady = false;

TaskHandle_t agaTaskHandle=nullptr;
portMUX_TYPE agaMux=portMUX_INITIALIZER_UNLOCKED;
volatile bool agaRequestPending=false;
volatile bool agaTaskBusy=false;
uint32_t agaRequestCount=0;
uint32_t agaCompleteCount=0;
uint32_t agaTaskLastMs=0;
float agaReqDp=0.0f;
float agaReqStatic=0.0f;
float agaReqTemp=0.0f;
volatile uint8_t agaReqRun=0;

float scale4to20(float mA,float lo,float hi){
  float f=(mA-4.0f)/16.0f;
  if(f<0.0f)f=0.0f;
  if(f>1.0f)f=1.0f;
  return lo+f*(hi-lo);
}

void updateEngineeringUnits(){
  for(int i=0;i<16;i++){
    aiEU[i]=scale4to20(ai_mA[i],aiCfg[i].euLow,aiCfg[i].euHigh);
    if(aiManual[i])aiEU[i]=aiManualEU[i];
  }
}

uint32_t modbusGood=0;
uint32_t modbusBad=0;
uint32_t consecutiveFails=0;
bool modbusOnline=false;
bool modbusCommsEnabled=true;
// EP32 GATE5 FORWARD DECL FIX V1
bool saveOperatorFlashConfig();
uint32_t lastPoll=0;

// EP32 LEGACY IO NVS RETIREMENT GATE5A V1
void loadModbusCommsConfig(){
  Serial.println("OPCFG: legacy modbus NVS loader retired; flash config is authoritative");
}

void saveModbusCommsConfig(){
  (void)saveOperatorFlashConfig();
}
uint8_t pollPhase=0;

uint32_t rawRxFrames=0;
uint32_t rawRxBytes=0;
uint32_t rawTimeouts=0;
uint32_t rawCrcFails=0;
String lastRawTx="";
String lastRawRx="";
String lastWriteTx="";
String lastWriteRx="";


// EP32 RULE ENGINE GATE5G1 FLASH16 V1
// EP32 RULE ENGINE GATE5G5A DUAL ACTION SCHEMA V5
// EP32 RULE ENGINE GATE5G5B DUAL ACTION EXEC UI V4
// EP32 RULE ENGINE GATE5G5A STACK RECOVERY V2
static constexpr uint8_t RIOT_RULE_COUNT=16;
static constexpr uint8_t RIOT_RULE_LEGACY_COUNT=8;
enum RiotRuleCompare:uint8_t { RIOT_RULE_GT=0,RIOT_RULE_LT=1,RIOT_RULE_GTE=2,RIOT_RULE_LTE=3,RIOT_RULE_EQ=4,RIOT_RULE_NEQ=5 };
enum RiotRuleAction:uint8_t { RIOT_ACT_ALARM=0,RIOT_ACT_EVENT=1,RIOT_ACT_DO_OFF=2,RIOT_ACT_DO_ON=3,RIOT_ACT_PID_MANUAL=4,RIOT_ACT_NONE=255 };

struct RiotRuleConfig {
  bool enabled=false;
  char name[25]="Rule";
  uint8_t tagIndex=0;
  uint8_t compare=RIOT_RULE_GT;
  float threshold=0.0f;
  uint32_t delayMs=0;
  bool latch=false;
  uint8_t action=RIOT_ACT_ALARM;
  uint8_t target=1;
  uint8_t action2=RIOT_ACT_NONE;
  uint8_t target2=1;
  uint8_t elseAction=RIOT_ACT_NONE;
  uint8_t elseTarget=1;
  uint8_t elseAction2=RIOT_ACT_NONE;
  uint8_t elseTarget2=1;
  char message[49]="";
};
struct RiotRuleRuntime {
  bool condition=false;
  bool timing=false;
  bool active=false;
  bool latched=false;
  bool alarm=false;
  bool acknowledged=false;
  bool firedThisCondition=false;
  uint32_t trueSinceMs=0;
  uint32_t fireCount=0;
  uint32_t lastFireMs=0;
  float fireValue=0.0f;
  char firedAt[24]="";
  char status[32]="IDLE";
};
RiotRuleConfig riotRules[RIOT_RULE_COUNT];
RiotRuleRuntime riotRuleRt[RIOT_RULE_COUNT];
bool riotRulesMasterEnabled=false;
uint32_t riotRuleDOWriteGood=0;
uint32_t riotRuleDOWriteBad=0;

void resetRiotRuleRuntime(uint8_t i){
  if(i>=RIOT_RULE_COUNT)return;
  riotRuleRt[i]=RiotRuleRuntime();
}
struct RiotRuleEnableFile {
  uint32_t magic=0x52454E31UL; // REN1
  uint8_t version=1;
  uint8_t mask=0;
  uint16_t reserved=0;
  uint32_t checksum=0;
};
static constexpr const char* RIOT_RULE_ENABLE_PATH="/rule_enable.bin";
static constexpr const char* RIOT_RULE_ENABLE_TMP="/rule_enable.tmp";

uint32_t riotRuleEnableChecksum(const RiotRuleEnableFile& f){
  const uint8_t* p=(const uint8_t*)&f;
  uint32_t h=2166136261UL;
  for(size_t i=0;i<offsetof(RiotRuleEnableFile,checksum);i++){h^=p[i];h*=16777619UL;}
  return h;
}

bool loadRiotRuleEnableMask(uint8_t& mask){
  File in=SPIFFS.open(RIOT_RULE_ENABLE_PATH,FILE_READ);
  if(!in || in.size()!=sizeof(RiotRuleEnableFile)){if(in)in.close();return false;}
  RiotRuleEnableFile f;
  size_t n=in.read((uint8_t*)&f,sizeof(f));
  in.close();
  if(n!=sizeof(f) || f.magic!=0x52454E31UL || f.version!=1 || f.checksum!=riotRuleEnableChecksum(f))return false;
  mask=f.mask;
  return true;
}

bool saveRiotRuleEnableMask(){
  RiotRuleEnableFile f;
  for(uint8_t i=0;i<RIOT_RULE_LEGACY_COUNT;i++)if(riotRules[i].enabled)f.mask|=(uint8_t)(1U<<i);
  f.checksum=riotRuleEnableChecksum(f);

  if(SPIFFS.exists(RIOT_RULE_ENABLE_TMP))SPIFFS.remove(RIOT_RULE_ENABLE_TMP);
  File out=SPIFFS.open(RIOT_RULE_ENABLE_TMP,FILE_WRITE);
  if(!out)return false;
  size_t n=out.write((const uint8_t*)&f,sizeof(f));
  out.flush();out.close();
  if(n!=sizeof(f)){SPIFFS.remove(RIOT_RULE_ENABLE_TMP);return false;}
  if(SPIFFS.exists(RIOT_RULE_ENABLE_PATH))SPIFFS.remove(RIOT_RULE_ENABLE_PATH);
  if(!SPIFFS.rename(RIOT_RULE_ENABLE_TMP,RIOT_RULE_ENABLE_PATH)){
    SPIFFS.remove(RIOT_RULE_ENABLE_TMP);return false;
  }
  Serial.printf("RULE ENABLE FILE saved mask=0x%02X\n",(unsigned)f.mask);
  return true;
}


static constexpr uint32_t RIOT_RULE_FILE_MAGIC=0x52554C31UL; // RUL1
static constexpr const char* RIOT_RULE_FILE_PATH="/rules_config_v1.bin";
static constexpr const char* RIOT_RULE_FILE_TMP="/rules_config_v1.tmp";
static constexpr const char* RIOT_RULE_FILE_BAK="/rules_config_v1.bak";

struct RiotRuleConfigV1Legacy {
  bool enabled;
  char name[25];
  uint8_t tagIndex;
  uint8_t compare;
  float threshold;
  uint32_t delayMs;
  bool latch;
  uint8_t action;
  uint8_t target;
  uint8_t elseAction;
  uint8_t elseTarget;
  char message[49];
};

struct RiotRuleFileV1Legacy {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t masterEnabled;
  uint8_t reserved[3];
  RiotRuleConfigV1Legacy rules[RIOT_RULE_COUNT];
  uint32_t checksum;
};

struct RiotRuleFileV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t masterEnabled;
  uint8_t reserved[3];
  RiotRuleConfig rules[RIOT_RULE_COUNT];
  uint32_t checksum;
};

static bool riotRuleFlashLoaded=false;
static bool riotRuleFlashLastSaveOK=false;
static bool riotRuleFlashBootLoadOK=false;
static uint32_t riotRuleFlashSaveCount=0;
static String riotRuleFlashStatus="NOT LOADED";

template<typename T>
uint32_t riotRuleFileChecksumT(const T& in){
  const uint8_t* p=(const uint8_t*)&in;
  size_t n=sizeof(in)-sizeof(uint32_t);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

void riotRuleDefaults16(){
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
    riotRules[i]=RiotRuleConfig();
    snprintf(riotRules[i].name,sizeof(riotRules[i].name),"Rule %u",(unsigned)(i+1));
    riotRules[i].enabled=false;
    riotRules[i].action=RIOT_ACT_NONE;
    riotRules[i].target=1;
    riotRules[i].action2=RIOT_ACT_NONE;
    riotRules[i].target2=1;
    riotRules[i].elseAction=RIOT_ACT_NONE;
    riotRules[i].elseTarget=1;
    riotRules[i].elseAction2=RIOT_ACT_NONE;
    riotRules[i].elseTarget2=1;
    resetRiotRuleRuntime(i);
  }
}

void sanitizeRiotRuleConfig(uint8_t i){
  if(i>=RIOT_RULE_COUNT)return;
  RiotRuleConfig &r=riotRules[i];
  if(r.compare>RIOT_RULE_NEQ)r.compare=RIOT_RULE_GT;
  if(r.action!=RIOT_ACT_NONE && r.action>RIOT_ACT_PID_MANUAL)r.action=RIOT_ACT_NONE;
  if(r.action2!=RIOT_ACT_NONE && r.action2>RIOT_ACT_PID_MANUAL)r.action2=RIOT_ACT_NONE;
  if(r.elseAction==RIOT_ACT_ALARM || (r.elseAction!=RIOT_ACT_NONE && r.elseAction>RIOT_ACT_PID_MANUAL))r.elseAction=RIOT_ACT_NONE;
  if(r.elseAction2==RIOT_ACT_ALARM || (r.elseAction2!=RIOT_ACT_NONE && r.elseAction2>RIOT_ACT_PID_MANUAL))r.elseAction2=RIOT_ACT_NONE;
  if(r.target<1||r.target>6)r.target=1;
  if(r.target2<1||r.target2>6)r.target2=1;
  if(r.elseTarget<1||r.elseTarget>6)r.elseTarget=1;
  if(r.elseTarget2<1||r.elseTarget2>6)r.elseTarget2=1;
  if(r.tagIndex>46)r.tagIndex=0;
  if(r.delayMs>86400000UL)r.delayMs=86400000UL;
  if(!r.name[0])snprintf(r.name,sizeof(r.name),"Rule %u",(unsigned)(i+1));
}

bool readRiotRuleFlashFileV2(const char* path,RiotRuleFileV2& f){
  File in=SPIFFS.open(path,FILE_READ);
  if(!in)return false;
  bool ok=in.size()==sizeof(f) && in.read((uint8_t*)&f,sizeof(f))==sizeof(f);
  in.close();
  if(!ok)return false;
  return f.magic==RIOT_RULE_FILE_MAGIC && f.version==2 && f.size==sizeof(f) &&
         f.checksum==riotRuleFileChecksumT(f);
}

bool readRiotRuleFlashFileV1Legacy(const char* path,RiotRuleFileV1Legacy& f){
  File in=SPIFFS.open(path,FILE_READ);
  if(!in)return false;
  bool ok=in.size()==sizeof(f) && in.read((uint8_t*)&f,sizeof(f))==sizeof(f);
  in.close();
  if(!ok)return false;
  return f.magic==RIOT_RULE_FILE_MAGIC && f.version==1 && f.size==sizeof(f) &&
         f.checksum==riotRuleFileChecksumT(f);
}

void copyLegacyRuleV1(const RiotRuleConfigV1Legacy& s,RiotRuleConfig& d){
  d=RiotRuleConfig();
  d.enabled=s.enabled;
  memcpy(d.name,s.name,sizeof(s.name));
  d.name[sizeof(d.name)-1]=0;
  d.tagIndex=s.tagIndex;
  d.compare=s.compare;
  d.threshold=s.threshold;
  d.delayMs=s.delayMs;
  d.latch=s.latch;
  d.action=s.action;
  d.target=s.target;
  d.action2=RIOT_ACT_NONE;
  d.target2=1;
  d.elseAction=s.elseAction;
  d.elseTarget=s.elseTarget;
  d.elseAction2=RIOT_ACT_NONE;
  d.elseTarget2=1;
  memcpy(d.message,s.message,sizeof(s.message));
  d.message[sizeof(d.message)-1]=0;
}

bool saveRiotRulesFlash(){
  static RiotRuleFileV2 f;
  memset(&f,0,sizeof(f));
  f.magic=RIOT_RULE_FILE_MAGIC;
  f.version=2;
  f.size=sizeof(f);
  f.masterEnabled=riotRulesMasterEnabled?1:0;
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
    sanitizeRiotRuleConfig(i);
    f.rules[i]=riotRules[i];
  }
  f.checksum=riotRuleFileChecksumT(f);

  if(SPIFFS.exists(RIOT_RULE_FILE_TMP))SPIFFS.remove(RIOT_RULE_FILE_TMP);
  File out=SPIFFS.open(RIOT_RULE_FILE_TMP,FILE_WRITE);
  if(!out){riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE OPEN FAIL";return false;}
  size_t n=out.write((const uint8_t*)&f,sizeof(f));
  out.flush();out.close();
  if(n!=sizeof(f)){SPIFFS.remove(RIOT_RULE_FILE_TMP);riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE WRITE FAIL";return false;}

  static RiotRuleFileV2 verify;
  memset(&verify,0,sizeof(verify));
  if(!readRiotRuleFlashFileV2(RIOT_RULE_FILE_TMP,verify)){
    SPIFFS.remove(RIOT_RULE_FILE_TMP);
    riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE VERIFY TMP FAIL";return false;
  }

  if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.remove(RIOT_RULE_FILE_BAK);
  if(SPIFFS.exists(RIOT_RULE_FILE_PATH) && !SPIFFS.rename(RIOT_RULE_FILE_PATH,RIOT_RULE_FILE_BAK)){
    SPIFFS.remove(RIOT_RULE_FILE_TMP);
    riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE BACKUP RENAME FAIL";return false;
  }
  if(!SPIFFS.rename(RIOT_RULE_FILE_TMP,RIOT_RULE_FILE_PATH)){
    if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.rename(RIOT_RULE_FILE_BAK,RIOT_RULE_FILE_PATH);
    riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE COMMIT RENAME FAIL";return false;
  }
  if(!readRiotRuleFlashFileV2(RIOT_RULE_FILE_PATH,verify)){
    SPIFFS.remove(RIOT_RULE_FILE_PATH);
    if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.rename(RIOT_RULE_FILE_BAK,RIOT_RULE_FILE_PATH);
    riotRuleFlashLastSaveOK=false;riotRuleFlashStatus="SAVE FINAL VERIFY FAIL";return false;
  }

  if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.remove(RIOT_RULE_FILE_BAK);
  riotRuleFlashLoaded=true;
  riotRuleFlashLastSaveOK=true;
  riotRuleFlashSaveCount++;
  riotRuleFlashStatus="SAVED";
  Serial.printf("RULECFG SAVE OK version=2 rules=%u bytes=%u\n",(unsigned)RIOT_RULE_COUNT,(unsigned)sizeof(f));
  return true;
}

void loadRiotRules(){
  riotRuleDefaults16();
  riotRulesMasterEnabled=false;
  riotRuleFlashLoaded=false;
  riotRuleFlashLastSaveOK=false;
  riotRuleFlashBootLoadOK=false;
  riotRuleFlashStatus="MISSING - DEFAULTS";

  static RiotRuleFileV2 f2;
  memset(&f2,0,sizeof(f2));
  if(readRiotRuleFlashFileV2(RIOT_RULE_FILE_PATH,f2)){
    riotRulesMasterEnabled=f2.masterEnabled!=0;
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      riotRules[i]=f2.rules[i];
      sanitizeRiotRuleConfig(i);
    }
    riotRuleFlashLoaded=true;
    riotRuleFlashLastSaveOK=true;
    riotRuleFlashBootLoadOK=true;
    riotRuleFlashStatus="LOADED V2";
    Serial.printf("RULECFG LOAD OK version=2 rules=%u bytes=%u\n",(unsigned)RIOT_RULE_COUNT,(unsigned)sizeof(f2));
    return;
  }

  static RiotRuleFileV1Legacy f1;
  memset(&f1,0,sizeof(f1));
  if(readRiotRuleFlashFileV1Legacy(RIOT_RULE_FILE_PATH,f1)){
    riotRulesMasterEnabled=f1.masterEnabled!=0;
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      copyLegacyRuleV1(f1.rules[i],riotRules[i]);
      sanitizeRiotRuleConfig(i);
    }
    if(saveRiotRulesFlash()){
      riotRuleFlashBootLoadOK=false;
      riotRuleFlashStatus="MIGRATED V1 TO V2";
      Serial.println("RULECFG MIGRATED flash V1 -> V2 dual-action schema");
    }else{
      riotRuleFlashStatus="V1 TO V2 SAVE FAIL";
    }
    return;
  }

  // One-time migration from the legacy 8-rule NVS + enable-mask file.
  uint8_t fileEnableMask=0;
  const bool haveFileEnableMask=loadRiotRuleEnableMask(fileEnableMask);
  prefs.begin("rules",true);
  riotRulesMasterEnabled=prefs.getBool("master",false);
  const bool haveEnableMask=prefs.isKey("enmask");
  const uint8_t enableMask=haveEnableMask?prefs.getUChar("enmask",0):0;
  for(uint8_t i=0;i<RIOT_RULE_LEGACY_COUNT;i++){
    String p="r"+String(i);
    const bool haveLegacyEnable=prefs.isKey((p+"en").c_str());
    const bool legacyEnable=haveLegacyEnable?prefs.getBool((p+"en").c_str(),false):false;
    const bool maskEnable=haveEnableMask?((enableMask&(1U<<i))!=0):false;
    const bool fileEnable=haveFileEnableMask?((fileEnableMask&(1U<<i))!=0):false;
    riotRules[i].enabled=haveFileEnableMask?fileEnable:(haveLegacyEnable?legacyEnable:maskEnable);
    String nm=prefs.getString((p+"nm").c_str(),"Rule "+String(i+1));
    nm.toCharArray(riotRules[i].name,sizeof(riotRules[i].name));
    riotRules[i].tagIndex=prefs.getUChar((p+"tg").c_str(),0);
    riotRules[i].compare=prefs.getUChar((p+"cp").c_str(),RIOT_RULE_GT);
    riotRules[i].threshold=prefs.getFloat((p+"th").c_str(),0.0f);
    riotRules[i].delayMs=prefs.getULong((p+"dl").c_str(),0);
    riotRules[i].latch=prefs.getBool((p+"la").c_str(),false);
    riotRules[i].action=prefs.getUChar((p+"ac").c_str(),RIOT_ACT_NONE);
    riotRules[i].target=prefs.getUChar((p+"tr").c_str(),1);
    riotRules[i].action2=RIOT_ACT_NONE;
    riotRules[i].target2=1;
    riotRules[i].elseAction=RIOT_ACT_NONE;
    riotRules[i].elseTarget=1;
    riotRules[i].elseAction2=RIOT_ACT_NONE;
    riotRules[i].elseTarget2=1;
    String msg=prefs.getString((p+"ms").c_str(),"");
    msg.toCharArray(riotRules[i].message,sizeof(riotRules[i].message));
    sanitizeRiotRuleConfig(i);
  }
  prefs.end();

  if(saveRiotRulesFlash()){
    riotRuleFlashStatus="MIGRATED LEGACY";
    Serial.println("RULECFG MIGRATED legacy 8 rules -> flash 16 rules");
  }else{
    riotRuleFlashStatus="LEGACY MIGRATION SAVE FAIL";
  }
}
void saveRiotRulesMaster(){
  if(!saveRiotRulesFlash())Serial.println("RULECFG master save failed");
}
void saveRiotRule(uint8_t i){
  if(i>=RIOT_RULE_COUNT)return;
  sanitizeRiotRuleConfig(i);
  if(!saveRiotRulesFlash())Serial.printf("RULECFG save failed index=%u\n",(unsigned)(i+1));
}

struct ModbusMonitorRecord {
  uint32_t ms=0;
  char dir[4]={0};
  char text[196]={0};
};
static constexpr uint8_t MODBUS_MONITOR_COUNT=48;
ModbusMonitorRecord modbusMonitor[MODBUS_MONITOR_COUNT];
uint8_t modbusMonitorHead=0;
uint8_t modbusMonitorUsed=0;
uint32_t modbusMonitorSequence=0;

void addModbusMonitor(const char* dir,const String& text){
  ModbusMonitorRecord &r=modbusMonitor[modbusMonitorHead];
  r.ms=millis();
  snprintf(r.dir,sizeof(r.dir),"%s",dir?dir:"");
  text.toCharArray(r.text,sizeof(r.text));
  modbusMonitorHead=(modbusMonitorHead+1)%MODBUS_MONITOR_COUNT;
  if(modbusMonitorUsed<MODBUS_MONITOR_COUNT)modbusMonitorUsed++;
  modbusMonitorSequence++;
}

uint32_t lastDisplay=0;
bool displayDirty=true;
bool epaperForceFullRefresh=false;

// RIOT A9B KEY1 DEMO QR SCREEN V1
static constexpr uint8_t RIOT_KEY1_PIN=2;   // EE04 KEY1 / GPIO2 / active-low
static bool riotDemoQrActive=false;
static bool riotKey1LastRaw=HIGH;
static bool riotKey1Stable=HIGH;
static uint32_t riotKey1ChangedMs=0;
static constexpr uint32_t RIOT_KEY_DEBOUNCE_MS=45UL;
uint16_t epaperPartialRefreshes=0;
uint16_t epaperFlowPartialRefreshes=0;
static constexpr uint16_t EPAPER_FULL_CLEANUP_EVERY=60;
static constexpr uint16_t EPAPER_FLOW_CLEANUP_EVERY=48;
// Dark mode makes the panel-wide cleanup waveform very conspicuous. Keep
// periodic cleanup, but allow a much longer partial-refresh run before it.
static constexpr uint16_t EPAPER_DARK_FULL_CLEANUP_EVERY=720;
static constexpr uint16_t EPAPER_DARK_FLOW_CLEANUP_EVERY=720;

enum EpaperRegionMask : uint8_t {
  EPAPER_REGION_NONE   = 0,
  EPAPER_REGION_HEADER = 1 << 0,
  EPAPER_REGION_FLOW   = 1 << 1,
  EPAPER_REGION_IO     = 1 << 2,
  EPAPER_REGION_FOOTER = 1 << 3,
  EPAPER_REGION_ALARM  = 1 << 4
};
uint8_t epaperPartialRegionMask=EPAPER_REGION_FLOW;
uint8_t epaperPendingRegionMask=EPAPER_REGION_NONE;
// EP32 WEB RELIABILITY GATE5B EPAPER FLOW CADENCE V1
static constexpr uint32_t EPAPER_FLOW_MIN_UPDATE_MS=30000UL;
static constexpr uint16_t EPAPER_FLOW_LOCAL_SCRUB_EVERY=6;
uint32_t epaperLastFlowUpdateMs=0;

void requestFullEPaperRefresh(){
  epaperForceFullRefresh=true;
  displayDirty=true;
  lastDisplay=0;
}

uint32_t fpMix(uint32_t h,uint32_t v){
  h^=v;
  h*=16777619UL;
  return h;
}

uint32_t displayHeaderFingerprint(){
  struct tm t;
  uint32_t minuteKey=0;
  if(getLocalTime(&t,5)){
    minuteKey=(uint32_t)(t.tm_yday+1)*1440UL+
              (uint32_t)t.tm_hour*60UL+
              (uint32_t)t.tm_min;
  }
  return minuteKey;
}

uint32_t displayFlowFingerprint(){
  uint32_t h=2166136261UL;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    h=fpMix(h,flowRunEnabled[run]?1U:0U);
    h=fpMix(h,flowRunValid[run]?1U:0U);
    if(!flowRunEnabled[run])continue;
    h=fpMix(h,(uint32_t)lroundf((flowRunValid[run]?flowRunFlow[run]:0.0f)*1000.0f));
    FlowResolvedPoint dp=resolveFlowRunInput(run,FLOW_ROLE_DP);
    FlowResolvedPoint sp=resolveFlowRunInput(run,FLOW_ROLE_STATIC);
    FlowResolvedPoint tp=resolveFlowRunInput(run,FLOW_ROLE_TEMP);
    h=fpMix(h,(uint32_t)lroundf((isfinite(dp.value)?dp.value:0.0f)*10.0f));
    h=fpMix(h,(uint32_t)lroundf((isfinite(sp.value)?sp.value:0.0f)*10.0f));
    h=fpMix(h,(uint32_t)lroundf((isfinite(tp.value)?tp.value:0.0f)*10.0f));
    h=fpMix(h,(uint32_t)lround(flowDay[run].volumeE3m3*100.0));
    h=fpMix(h,(uint32_t)lround(flowDay[run].flowHours*10.0));
    h=fpMix(h,(uint32_t)lroundf(flowPIDRt[run].output));
    h=fpMix(h,(uint32_t)flowPIDCfg[run].mode);
    h=fpMix(h,flowPIDRt[run].pressureOverrideActive?1U:0U);
  }
  return h;
}

bool riotRuleTagValue(uint8_t index,float& value,String& quality);
uint32_t displayIOFingerprint(){
  uint32_t h=2166136261UL;
  for(uint8_t i=0;i<4;i++){
    h=fpMix(h,diState[i]?1U:0U);
    h=fpMix(h,doState[i]?1U:0U);
  }
  return h;
}

uint32_t displayAlarmFingerprint(){
  uint32_t h=2166136261UL;
  h=fpMix(h,displayCfg.showActiveAlarms?1U:0U);
  if(!displayCfg.showActiveAlarms)return h;
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
    RiotRuleRuntime &rt=riotRuleRt[i];
    if(!(rt.alarm||rt.latched))continue;
    h=fpMix(h,(uint32_t)i+1U);
    float v=0.0f;String q;
    riotRuleTagValue(riotRules[i].tagIndex,v,q);
    h=fpMix(h,(uint32_t)lroundf((isfinite(v)?v:0.0f)*1000.0f));
    h=fpMix(h,rt.lastFireMs);
    break;
  }
  return h;
}

uint32_t displayFooterFingerprint(){
  uint32_t h=2166136261UL;
  h=fpMix(h,modbusCommsEnabled?1U:0U);
  h=fpMix(h,modbusOnline?1U:0U);
  h=fpMix(h,WiFi.status()==WL_CONNECTED?1U:0U);
  h=fpMix(h,powerOutageActive?1U:0U);
  // Battery ADC noise should not cause repeated footer refreshes. Quantize the
  // display fingerprint to 50 mV steps; the measured voltage itself remains
  // full-resolution for logging/status.
  h=fpMix(h,(uint32_t)lroundf((isfinite(batteryVoltageV)?batteryVoltageV:0.0f)*20.0f));
  return h;
}

uint16_t modbusCRC(const uint8_t* data,size_t len){
  uint16_t crc=0xFFFF;
  for(size_t pos=0;pos<len;pos++){
    crc^=data[pos];
    for(int i=0;i<8;i++){
      if(crc&1){crc>>=1;crc^=0xA001;}
      else crc>>=1;
    }
  }
  return crc;
}

bool transactRtu(const uint8_t* req,size_t reqLen,uint8_t* response,size_t& count,size_t minCount,uint32_t timeoutMs=120){
  while(modbusSerial.available())modbusSerial.read();

  lastRawTx="";
  Serial.print("MB TX:");
  char txh[4];
  for(size_t i=0;i<reqLen;i++){
    Serial.printf(" %02X",req[i]);
    snprintf(txh,sizeof(txh),"%02X",req[i]);
    if(i)lastRawTx+=" ";
    lastRawTx+=txh;
  }
  Serial.println();

  const bool isWrite=(reqLen>=2 && (req[1]==0x05 || req[1]==0x06 || req[1]==0x0F || req[1]==0x10));
  if(isWrite)lastWriteTx=lastRawTx;
  addModbusMonitor("TX",lastRawTx);

  modbusSerial.write(req,reqLen);
  modbusSerial.flush();

  count=0;
  uint32_t started=millis();
  while(millis()-started<timeoutMs){
    server.handleClient();
    while(modbusSerial.available() && count<64){
      response[count++]=(uint8_t)modbusSerial.read();
    }

    if(count>=minCount){
      // allow a short inter-byte gap so the complete frame is captured
      delay(4);
      while(modbusSerial.available() && count<64){
        response[count++]=(uint8_t)modbusSerial.read();
      }
      break;
    }
    delay(1);
  }

  lastRawRx="";
  if(count==0){
    if(isWrite)lastWriteRx="<NONE>";
    rawTimeouts++;
    Serial.println("MB RX: <NONE>");
    addModbusMonitor("ERR","TIMEOUT / NO RESPONSE");
    return false;
  }

  rawRxFrames++;
  rawRxBytes+=count;
  Serial.print("MB RX:");
  char h[4];
  for(size_t i=0;i<count;i++){
    Serial.printf(" %02X",response[i]);
    snprintf(h,sizeof(h),"%02X",response[i]);
    if(i)lastRawRx+=" ";
    lastRawRx+=h;
  }
  Serial.println();
  if(isWrite)lastWriteRx=lastRawRx;
  addModbusMonitor("RX",lastRawRx);

  if(count<4){
    Serial.println("MB RX ERROR: short frame");
    addModbusMonitor("ERR","SHORT FRAME");
    return false;
  }

  uint16_t rxCRC=(uint16_t)response[count-2] | ((uint16_t)response[count-1]<<8);
  uint16_t calcCRC=modbusCRC(response,count-2);
  if(rxCRC!=calcCRC){
    rawCrcFails++;
    Serial.printf("MB RX ERROR: CRC got=%04X calc=%04X\n",rxCRC,calcCRC);
    addModbusMonitor("ERR",String("CRC ERROR got=")+String(rxCRC,HEX)+" calc="+String(calcCRC,HEX));
    return false;
  }

  return count>=minCount;
}

// EP32 M31 TRANSPORT ABSTRACTION GATE1 V1
enum M31TransportMode : uint8_t { M31_TRANSPORT_RTU=0, M31_TRANSPORT_TCP=1 };
static M31TransportMode m31TransportMode=M31_TRANSPORT_RTU;

// EP32 M31 TCP TRANSPORT GATE2 V1
static String m31TcpHost="192.168.1.138";
static uint16_t m31TcpPort=502;
static uint16_t m31TcpTransactionId=0;

// EP32 FLASH CONFIG FOUNDATION GATE5 V1
// EP32 OPERATOR CONFIG V2 RACK PERSISTENCE GATE A3 V2
// Operator configuration source of truth. V2 persists the modular M31 rack
// profile and all 16 analog-input operator settings. A valid V1 file is
// migrated once on boot and retained as a rollback artifact.
static constexpr const char* OPERATOR_CFG_PATH="/operator_config_v2.bin";
static constexpr const char* OPERATOR_CFG_TMP="/operator_config_v2.tmp";
static constexpr const char* OPERATOR_CFG_BAK="/operator_config_v2.bak";
static constexpr const char* OPERATOR_CFG_V1_PATH="/operator_config_v1.bin";
static constexpr uint32_t OPERATOR_CFG_MAGIC=0x4D464332UL; // MFC2
static constexpr uint16_t OPERATOR_CFG_VERSION=2;
static constexpr uint32_t OPERATOR_CFG_V1_MAGIC=0x4D464331UL; // MFC1
static constexpr uint16_t OPERATOR_CFG_V1_VERSION=1;

struct OperatorAIConfigV1 {
  char name[24];
  char units[12];
  float euLow;
  float euHigh;
  uint8_t manual;
  float manualEU;
};

struct OperatorFlashConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t m31Enabled;
  uint8_t transport;
  uint8_t unitId;
  uint8_t expansion;
  uint32_t rtuBaud;
  uint16_t tcpPort;
  char tcpHost[40];
  OperatorAIConfigV1 ai[8];
  uint32_t checksum;
};

struct OperatorFlashConfigV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t m31Enabled;
  uint8_t transport;
  uint8_t unitId;
  uint8_t hostModel;
  uint8_t slot1;
  uint8_t slot2;
  uint8_t reserved0;
  uint32_t rtuBaud;
  uint16_t tcpPort;
  char tcpHost[40];
  OperatorAIConfigV1 ai[16];
  uint32_t checksum;
};

static bool operatorFlashLoaded=false;
static bool operatorFlashLastSaveOK=false;
static String operatorFlashStatus="NOT LOADED";

static uint32_t operatorFlashChecksumV1(const OperatorFlashConfigV1& c){
  const uint8_t* p=(const uint8_t*)&c;
  const size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static uint32_t operatorFlashChecksum(const OperatorFlashConfigV2& c){
  const uint8_t* p=(const uint8_t*)&c;
  const size_t n=sizeof(c)-sizeof(c.checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static bool operatorValidBaud(uint32_t b){
  return b==1200 || b==2400 || b==4800 || b==9600 ||
         b==19200 || b==38400 || b==57600 || b==115200;
}

static bool operatorAIConfigValid(OperatorAIConfigV1& a){
  a.name[sizeof(a.name)-1]='\0';
  a.units[sizeof(a.units)-1]='\0';
  return isfinite(a.euLow) && isfinite(a.euHigh) &&
         fabsf(a.euHigh-a.euLow)>=0.000001f &&
         isfinite(a.manualEU) && a.manual<=1;
}

static bool operatorFlashConfigV1Valid(OperatorFlashConfigV1& c){
  if(c.magic!=OPERATOR_CFG_V1_MAGIC || c.version!=OPERATOR_CFG_V1_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=operatorFlashChecksumV1(c))return false;
  if(c.transport>M31_TRANSPORT_TCP)return false;
  if(c.unitId<1 || c.unitId>247)return false;
  if(!operatorValidBaud(c.rtuBaud) || c.tcpPort==0)return false;
  c.tcpHost[sizeof(c.tcpHost)-1]='\0';
  if(c.transport==M31_TRANSPORT_TCP && !c.tcpHost[0])return false;
  if(c.expansion>EP32_M31_EXP_GXAXA0404)return false;
  for(uint8_t i=0;i<8;i++)if(!operatorAIConfigValid(c.ai[i]))return false;
  return true;
}

static bool operatorFlashConfigValid(OperatorFlashConfigV2& c){
  if(c.magic!=OPERATOR_CFG_MAGIC || c.version!=OPERATOR_CFG_VERSION || c.size!=sizeof(c))return false;
  if(c.checksum!=operatorFlashChecksum(c))return false;
  if(c.transport>M31_TRANSPORT_TCP)return false;
  if(c.unitId<1 || c.unitId>247)return false;
  if(!operatorValidBaud(c.rtuBaud) || c.tcpPort==0)return false;
  c.tcpHost[sizeof(c.tcpHost)-1]='\0';
  if(c.transport==M31_TRANSPORT_TCP && !c.tcpHost[0])return false;
  if(c.hostModel>EP32_M31_HOST_AAAX4440G)return false;
  if(c.slot1>EP32_M31_MODULE_GXAXX0800 || c.slot2>EP32_M31_MODULE_GXAXX0800)return false;
  for(uint8_t i=0;i<16;i++)if(!operatorAIConfigValid(c.ai[i]))return false;
  return true;
}

static void operatorCaptureConfig(OperatorFlashConfigV2& c){
  memset(&c,0,sizeof(c));
  c.magic=OPERATOR_CFG_MAGIC;
  c.version=OPERATOR_CFG_VERSION;
  c.size=sizeof(c);
  c.m31Enabled=modbusCommsEnabled?1:0;
  c.transport=(uint8_t)m31TransportMode;
  c.unitId=m31UnitId;
  c.hostModel=(uint8_t)ep32M31Host;
  c.slot1=(uint8_t)ep32M31Slot1;
  c.slot2=(uint8_t)ep32M31Slot2;
  c.rtuBaud=m31RtuBaud;
  c.tcpPort=m31TcpPort;
  m31TcpHost.toCharArray(c.tcpHost,sizeof(c.tcpHost));
  for(uint8_t i=0;i<16;i++){
    strlcpy(c.ai[i].name,aiCfg[i].name,sizeof(c.ai[i].name));
    strlcpy(c.ai[i].units,aiCfg[i].units,sizeof(c.ai[i].units));
    c.ai[i].euLow=aiCfg[i].euLow;
    c.ai[i].euHigh=aiCfg[i].euHigh;
    c.ai[i].manual=aiManual[i]?1:0;
    c.ai[i].manualEU=aiManualEU[i];
  }
  c.checksum=operatorFlashChecksum(c);
}

static void operatorApplyConfig(const OperatorFlashConfigV2& c){
  modbusCommsEnabled=c.m31Enabled!=0;
  m31TransportMode=(M31TransportMode)c.transport;
  m31UnitId=c.unitId;
  ep32M31Host=(Ep32M31HostModel)c.hostModel;
  ep32M31Slot1=(Ep32M31ModuleType)c.slot1;
  ep32M31Slot2=(Ep32M31ModuleType)c.slot2;
  ep32M31ExpansionProfile=
    (ep32M31Slot1==EP32_M31_MODULE_GXAXA0404)
    ? EP32_M31_EXP_GXAXA0404
    : EP32_M31_EXP_NONE;
  m31RtuBaud=c.rtuBaud;
  m31TcpPort=c.tcpPort;
  m31TcpHost=String(c.tcpHost);
  for(uint8_t i=0;i<16;i++){
    strlcpy(aiCfg[i].name,c.ai[i].name,sizeof(aiCfg[i].name));
    strlcpy(aiCfg[i].units,c.ai[i].units,sizeof(aiCfg[i].units));
    aiCfg[i].euLow=c.ai[i].euLow;
    aiCfg[i].euHigh=c.ai[i].euHigh;
    aiManual[i]=c.ai[i].manual!=0;
    aiManualEU[i]=c.ai[i].manualEU;
  }
}

static void operatorApplyLegacyV1(const OperatorFlashConfigV1& c){
  modbusCommsEnabled=c.m31Enabled!=0;
  m31TransportMode=(M31TransportMode)c.transport;
  m31UnitId=c.unitId;
  ep32M31ExpansionProfile=(Ep32M31ExpansionProfile)c.expansion;
  ep32M31Host=EP32_M31_HOST_AFAX4440G_U;
  ep32M31Slot1=
    (ep32M31ExpansionProfile==EP32_M31_EXP_GXAXA0404)
    ? EP32_M31_MODULE_GXAXA0404
    : EP32_M31_MODULE_NONE;
  ep32M31Slot2=EP32_M31_MODULE_NONE;
  m31RtuBaud=c.rtuBaud;
  m31TcpPort=c.tcpPort;
  m31TcpHost=String(c.tcpHost);
  for(uint8_t i=0;i<8;i++){
    strlcpy(aiCfg[i].name,c.ai[i].name,sizeof(aiCfg[i].name));
    strlcpy(aiCfg[i].units,c.ai[i].units,sizeof(aiCfg[i].units));
    aiCfg[i].euLow=c.ai[i].euLow;
    aiCfg[i].euHigh=c.ai[i].euHigh;
    aiManual[i]=c.ai[i].manual!=0;
    aiManualEU[i]=c.ai[i].manualEU;
  }
  for(uint8_t i=8;i<16;i++)aiManual[i]=false;
}

static bool operatorReadFile(const char* path,OperatorFlashConfigV2& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && operatorFlashConfigValid(c);
}

static bool operatorReadLegacyV1(const char* path,OperatorFlashConfigV1& c){
  File f=SPIFFS.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()!=sizeof(c)){f.close();return false;}
  size_t got=f.read((uint8_t*)&c,sizeof(c));
  f.close();
  return got==sizeof(c) && operatorFlashConfigV1Valid(c);
}

bool saveOperatorFlashConfig(){
  OperatorFlashConfigV2 c;
  operatorCaptureConfig(c);

  if(SPIFFS.exists(OPERATOR_CFG_TMP))SPIFFS.remove(OPERATOR_CFG_TMP);
  File out=SPIFFS.open(OPERATOR_CFG_TMP,FILE_WRITE);
  if(!out){
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="SAVE OPEN FAIL";
    return false;
  }
  size_t wrote=out.write((const uint8_t*)&c,sizeof(c));
  out.flush();
  out.close();
  if(wrote!=sizeof(c)){
    SPIFFS.remove(OPERATOR_CFG_TMP);
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="SAVE WRITE FAIL";
    return false;
  }

  OperatorFlashConfigV2 verifyTmp;
  if(!operatorReadFile(OPERATOR_CFG_TMP,verifyTmp)){
    SPIFFS.remove(OPERATOR_CFG_TMP);
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="SAVE VERIFY TMP FAIL";
    return false;
  }

  if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.remove(OPERATOR_CFG_BAK);
  if(SPIFFS.exists(OPERATOR_CFG_PATH)){
    if(!SPIFFS.rename(OPERATOR_CFG_PATH,OPERATOR_CFG_BAK)){
      SPIFFS.remove(OPERATOR_CFG_TMP);
      operatorFlashLastSaveOK=false;
      operatorFlashStatus="SAVE BACKUP RENAME FAIL";
      return false;
    }
  }

  if(!SPIFFS.rename(OPERATOR_CFG_TMP,OPERATOR_CFG_PATH)){
    if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.rename(OPERATOR_CFG_BAK,OPERATOR_CFG_PATH);
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="SAVE COMMIT RENAME FAIL";
    return false;
  }

  OperatorFlashConfigV2 verifyFinal;
  if(!operatorReadFile(OPERATOR_CFG_PATH,verifyFinal)){
    SPIFFS.remove(OPERATOR_CFG_PATH);
    if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.rename(OPERATOR_CFG_BAK,OPERATOR_CFG_PATH);
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="SAVE FINAL VERIFY FAIL";
    return false;
  }

  if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.remove(OPERATOR_CFG_BAK);
  operatorFlashLoaded=true;
  operatorFlashLastSaveOK=true;
  operatorFlashStatus="SAVED V2";
  Serial.printf("OPCFG V2 SAVE OK bytes=%u rack=%s/%s/%s transport=%s enabled=%u unit=%u host=%s port=%u baud=%lu\n",
    (unsigned)sizeof(c),
    ep32M31HostModel(),
    ep32M31ModuleModel(ep32M31Slot1),
    ep32M31ModuleModel(ep32M31Slot2),
    m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU",
    modbusCommsEnabled?1:0,
    (unsigned)m31UnitId,
    m31TcpHost.c_str(),
    (unsigned)m31TcpPort,
    (unsigned long)m31RtuBaud);
  return true;
}

bool loadOperatorFlashConfig(){
  OperatorFlashConfigV2 c;
  if(operatorReadFile(OPERATOR_CFG_PATH,c)){
    operatorApplyConfig(c);
    operatorFlashLoaded=true;
    operatorFlashLastSaveOK=true;
    operatorFlashStatus="LOADED V2";
    Serial.printf("OPCFG V2 LOAD OK bytes=%u rack=%s/%s/%s transport=%s enabled=%u unit=%u host=%s port=%u baud=%lu\n",
      (unsigned)sizeof(c),
      ep32M31HostModel(),
      ep32M31ModuleModel(ep32M31Slot1),
      ep32M31ModuleModel(ep32M31Slot2),
      m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU",
      modbusCommsEnabled?1:0,
      (unsigned)m31UnitId,
      m31TcpHost.c_str(),
      (unsigned)m31TcpPort,
      (unsigned long)m31RtuBaud);
    return true;
  }

  OperatorFlashConfigV1 legacy;
  if(operatorReadLegacyV1(OPERATOR_CFG_V1_PATH,legacy)){
    operatorApplyLegacyV1(legacy);
    operatorFlashStatus="MIGRATING V1";
    if(saveOperatorFlashConfig()){
      operatorFlashStatus="MIGRATED V1->V2";
      Serial.println("OPCFG migrated V1 to V2; legacy V1 retained");
      return true;
    }
    operatorFlashLoaded=false;
    operatorFlashLastSaveOK=false;
    operatorFlashStatus="V1 LOAD OK - V2 MIGRATION SAVE FAILED";
    return false;
  }

  for(uint8_t i=0;i<16;i++)aiManual[i]=false;
  operatorFlashLoaded=false;
  operatorFlashLastSaveOK=false;
  operatorFlashStatus=SPIFFS.exists(OPERATOR_CFG_PATH)?"INVALID V2 - DEFAULTS":"MISSING V2 - DEFAULTS";
  Serial.printf("OPCFG %s; using defaults (AI1-16 LIVE)\n",operatorFlashStatus.c_str());
  return false;
}

// EP32 M31 TCP RESILIENCE GATE A2C V1
bool transactTcp(const uint8_t* req,size_t reqLen,uint8_t* response,size_t& count,size_t minCount,uint32_t timeoutMs=120){
  count=0;
  if(reqLen<4 || reqLen>58)return false;
  if(WiFi.status()!=WL_CONNECTED){
    lastRawRx="<WIFI OFFLINE>";
    rawTimeouts++;
    addModbusMonitor("ERR","TCP WIFI OFFLINE");
    return false;
  }

  const size_t pduLen=reqLen-3;
  const uint16_t tid=++m31TcpTransactionId;
  const uint16_t mbapLen=(uint16_t)(1+pduLen);
  uint8_t tx[64];
  const size_t txLen=7+pduLen;
  tx[0]=(uint8_t)(tid>>8); tx[1]=(uint8_t)tid;
  tx[2]=0; tx[3]=0;
  tx[4]=(uint8_t)(mbapLen>>8); tx[5]=(uint8_t)mbapLen;
  tx[6]=req[0];
  memcpy(tx+7,req+1,pduLen);

  lastRawTx="";
  char h[4];
  for(size_t i=0;i<reqLen;i++){
    snprintf(h,sizeof(h),"%02X",req[i]);
    if(i)lastRawTx+=" ";
    lastRawTx+=h;
  }
  const bool isWrite=(reqLen>=2 && (req[1]==0x05 || req[1]==0x06 || req[1]==0x0F || req[1]==0x10));
  if(isWrite)lastWriteTx=lastRawTx;
  Serial.printf("MBTCP TX tid=%u target=%s:%u RTU-SHAPE=%s\n",(unsigned)tid,m31TcpHost.c_str(),(unsigned)m31TcpPort,lastRawTx.c_str());
  addModbusMonitor("TX",String("TCP ")+m31TcpHost+":"+String(m31TcpPort)+" "+lastRawTx);

  if(m31TcpClient.connected())m31TcpClient.stop();
  m31TcpClient.setTimeout(timeoutMs);
  if(!m31TcpClient.connect(m31TcpHost.c_str(),m31TcpPort,(int32_t)timeoutMs)){
    rawTimeouts++;
    lastRawRx="<TCP CONNECT FAIL>";
    if(isWrite)lastWriteRx=lastRawRx;
    Serial.println("MBTCP RX: <CONNECT FAIL>");
    addModbusMonitor("ERR","TCP CONNECT FAIL");
    return false;
  }

  if(m31TcpClient.write(tx,txLen)!=txLen){
    m31TcpClient.stop();
    lastRawRx="<TCP WRITE FAIL>";
    if(isWrite)lastWriteRx=lastRawRx;
    addModbusMonitor("ERR","TCP WRITE FAIL");
    return false;
  }
  m31TcpClient.flush();

  uint8_t hdr[7];
  size_t hdrCount=0;
  uint32_t started=millis();
  while(hdrCount<sizeof(hdr) && millis()-started<timeoutMs){
    server.handleClient();
    while(m31TcpClient.available() && hdrCount<sizeof(hdr))hdr[hdrCount++]=(uint8_t)m31TcpClient.read();
    if(hdrCount<sizeof(hdr))delay(1);
  }
  if(hdrCount<sizeof(hdr)){
    m31TcpClient.stop();
    rawTimeouts++;
    lastRawRx="<TCP HEADER TIMEOUT>";
    if(isWrite)lastWriteRx=lastRawRx;
    Serial.println("MBTCP RX: <HEADER TIMEOUT>");
    addModbusMonitor("ERR","TCP HEADER TIMEOUT");
    return false;
  }

  const uint16_t rxTid=((uint16_t)hdr[0]<<8)|hdr[1];
  const uint16_t protocol=((uint16_t)hdr[2]<<8)|hdr[3];
  const uint16_t rxLen=((uint16_t)hdr[4]<<8)|hdr[5];
  if(rxTid!=tid || protocol!=0 || rxLen<2 || rxLen>62){
    m31TcpClient.stop();
    lastRawRx="<TCP MBAP INVALID>";
    if(isWrite)lastWriteRx=lastRawRx;
    addModbusMonitor("ERR","TCP MBAP INVALID");
    return false;
  }

  const size_t rxPduLen=rxLen-1;
  if(1+rxPduLen+2>64){
    m31TcpClient.stop();
    return false;
  }
  response[0]=hdr[6];
  size_t got=0;
  started=millis();
  while(got<rxPduLen && millis()-started<timeoutMs){
    server.handleClient();
    while(m31TcpClient.available() && got<rxPduLen)response[1+got++]=(uint8_t)m31TcpClient.read();
    if(got<rxPduLen)delay(1);
  }
  m31TcpClient.stop();
  if(got!=rxPduLen){
    rawTimeouts++;
    lastRawRx="<TCP PDU TIMEOUT>";
    if(isWrite)lastWriteRx=lastRawRx;
    Serial.println("MBTCP RX: <PDU TIMEOUT>");
    addModbusMonitor("ERR","TCP PDU TIMEOUT");
    return false;
  }

  count=1+rxPduLen;
  uint16_t crc=modbusCRC(response,count);
  response[count++]=(uint8_t)(crc&0xFF);
  response[count++]=(uint8_t)(crc>>8);

  lastRawRx="";
  for(size_t i=0;i<count;i++){
    snprintf(h,sizeof(h),"%02X",response[i]);
    if(i)lastRawRx+=" ";
    lastRawRx+=h;
  }
  if(isWrite)lastWriteRx=lastRawRx;
  rawRxFrames++;
  rawRxBytes+=count;
  Serial.printf("MBTCP RX tid=%u RTU-SHAPE=%s\n",(unsigned)tid,lastRawRx.c_str());
  addModbusMonitor("RX",String("TCP ")+lastRawRx);

  return count>=minCount;
}

bool transact(const uint8_t* req,size_t reqLen,uint8_t* response,size_t& count,size_t minCount,uint32_t timeoutMs=120){
  if(m31TransportMode==M31_TRANSPORT_TCP)return transactTcp(req,reqLen,response,count,minCount,timeoutMs);
  return transactRtu(req,reqLen,response,count,minCount,timeoutMs);
}


bool readBits(uint8_t fc,bool* dst){
  uint8_t req[8]={m31UnitId,fc,0,0,0,4,0,0};
  uint16_t crc=modbusCRC(req,6); req[6]=crc&0xFF; req[7]=crc>>8;
  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,6))return false;
  if(response[0]!=m31UnitId || response[1]!=fc || response[2]<1)return false;
  uint8_t bits=response[3];
  bool changed=false;
  for(int i=0;i<4;i++){
    bool next=(bits&(1U<<i))!=0;
    if(dst[i]!=next)changed=true;
    dst[i]=next;
  }
  if(changed){
    epaperPendingRegionMask|=EPAPER_REGION_IO;
    displayDirty=true;
  }
  return true;
}

bool readAIBlock(uint16_t startReg,int offset){
  uint8_t req[8]={
    m31UnitId,0x04,
    (uint8_t)(startReg>>8),(uint8_t)(startReg&0xFF),
    0,4,0,0
  };
  uint16_t crc=modbusCRC(req,6); req[6]=crc&0xFF; req[7]=crc>>8;
  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,13))return false;
  if(response[0]!=m31UnitId || response[1]!=0x04 || response[2]<8)return false;
  for(int i=0;i<4;i++){
    int b=3+i*2;
    uint16_t raw=((uint16_t)response[b]<<8)|response[b+1];
    ai_mA[offset+i]=raw/1000.0f;
  }
  return true;
}
bool readAO(){
  uint8_t req[8]={
    m31UnitId,0x03,
    0x00,0x00,
    0x00,0x04,
    0,0
  };
  uint16_t crc=modbusCRC(req,6); req[6]=crc&0xFF; req[7]=crc>>8;
  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,13))return false;
  if(count<13 || response[0]!=m31UnitId || response[1]!=0x03 || response[2]<8)return false;
  for(uint8_t i=0;i<4;i++){
    uint16_t raw=((uint16_t)response[3+i*2]<<8)|response[4+i*2];
    ao_mA[i]=raw/1000.0f;
  }
  return true;
}


bool writeM31CoilRaw(uint8_t channel,bool on){
  if(channel>=4)return false;
  uint16_t address=channel;
  uint16_t value=on?0xFF00:0x0000;
  uint8_t req[8]={
    m31UnitId,0x05,
    (uint8_t)(address>>8),(uint8_t)(address&0xFF),
    (uint8_t)(value>>8),(uint8_t)(value&0xFF),
    0,0
  };
  uint16_t crc=modbusCRC(req,6);
  req[6]=crc&0xFF; req[7]=crc>>8;

  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,8))return false;
  if(count<8 ||
     response[0]!=m31UnitId ||
     response[1]!=0x05 ||
     response[2]!=req[2] || response[3]!=req[3] ||
     response[4]!=req[4] || response[5]!=req[5])return false;

  doState[channel]=on;
  displayDirty=true;
  return true;
}

bool writeM31AORaw(uint8_t channel,float percent){
  if(channel>=4)return false;
  percent=constrain(percent,0.0f,100.0f);

  // Match the proven DIS0705H implementation exactly:
  // AO engineering integer register, mA x 1000, FC06 single-register write.
  float mA=4.0f+(percent*0.16f);
  uint16_t raw=(uint16_t)lroundf(mA*1000.0f);
  uint16_t address=channel;

  uint8_t req[8]={
    m31UnitId,0x06,
    (uint8_t)(address>>8),(uint8_t)(address&0xFF),
    (uint8_t)(raw>>8),(uint8_t)(raw&0xFF),
    0,0
  };
  uint16_t crc=modbusCRC(req,6);
  req[6]=crc&0xFF; req[7]=crc>>8;

  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,8))return false;
  if(count<8 ||
     response[0]!=m31UnitId ||
     response[1]!=0x06 ||
     response[2]!=req[2] || response[3]!=req[3] ||
     response[4]!=req[4] || response[5]!=req[5])return false;

  return true;
}

// EP32 I/O FORCE BACKEND GATE4A V1
static void updateEffectiveDI(){
  for(uint8_t i=0;i<4;i++)diState[i]=diForceActive[i]?diForceValue[i]:diRawState[i];
}

bool writeM31Coil(uint8_t channel,bool on){
  if(channel>=4)return false;
  // A forced DO owns the physical output. Normal rule/control writes are
  // acknowledged but must not override the commissioning force.
  if(doForceActive[channel])return true;
  return writeM31CoilRaw(channel,on);
}

bool writeM31AO(uint8_t channel,float percent){
  if(channel>=4)return false;
  if(!ep32M31ExpansionPresent())return false;
  // Existing AO FORCE owns the physical output until released.
  if(aoForceActive[channel])return true;
  return writeM31AORaw(channel,percent);
}



bool riotRuleTagValue(uint8_t index,float& value,String& quality){
  value=0.0f;quality="BAD";
  String mbQuality=!modbusCommsEnabled?"DISABLED":(modbusOnline?"GOOD":"BAD");
  if(index<8){
    value=aiEU[index];quality=aiManual[index]?"MANUAL":mbQuality;return aiManual[index]||modbusOnline;
  }
  index-=8;
  if(index<4){value=diState[index]?1.0f:0.0f;quality=mbQuality;return modbusOnline;}
  index-=4;
  if(index<4){value=doState[index]?1.0f:0.0f;quality=mbQuality;return modbusOnline;}
  index-=4;
  if(index<4){value=ao_mA[index];quality=mbQuality;return modbusOnline;}
  index-=4;
  if(index<24){
    uint8_t run=index/4,kind=index%4;
    if(run>=FLOW_RUN_COUNT)return false;
    if(kind==0){value=flowRunValid[run]?flowRunFlow[run]:0.0f;quality=!flowRunEnabled[run]?"DISABLED":(flowRunValid[run]?"GOOD":"BAD");return flowRunEnabled[run]&&flowRunValid[run];}
    if(kind==1){value=(float)flowDay[run].volumeE3m3;quality=flowRunEnabled[run]?"GOOD":"DISABLED";return flowRunEnabled[run];}
    if(kind==2){value=flowPIDCfg[run].flowSP;quality=flowPIDCfg[run].mode==FLOW_CTRL_DISABLED?"DISABLED":"GOOD";return flowPIDCfg[run].mode!=FLOW_CTRL_DISABLED;}
    value=flowPIDRt[run].output;quality=flowPIDCfg[run].mode==FLOW_CTRL_DISABLED?"DISABLED":"GOOD";return flowPIDCfg[run].mode!=FLOW_CTRL_DISABLED;
  }
  index-=24;
  if(index==0){
    if(!batteryTelemetryValid()){
      value=NAN;
      quality=!BATTERY_TELEMETRY_AVAILABLE?"UNAVAILABLE":"BAD";
      return false;
    }
    value=batteryVoltageV;
    quality="GOOD";
    return true;
  }
  if(index==1){value=WiFi.status()==WL_CONNECTED?1.0f:0.0f;quality=WiFi.status()==WL_CONNECTED?"GOOD":"BAD";return true;}
  if(index==2){value=mqttClient.connected()?1.0f:0.0f;quality=mqttCfg.enabled?(mqttClient.connected()?"GOOD":"BAD"):"DISABLED";return true;}
  return false;
}
String riotRuleTagUnits(uint8_t index){
  if(index<8)return String(aiCfg[index].units);
  if(index>=16 && index<20)return "mA";
  if(index>=20 && index<44){
    uint8_t kind=(index-20)%4;
    if(kind==0 || kind==2)return "E3 m3/day";
    if(kind==1)return "E3 m3";
    return "%";
  }
  if(index==44)return "V";
  return "";
}
bool riotRuleCompare(float v,uint8_t cmp,float t){
  switch(cmp){
    case RIOT_RULE_LT:return v<t;
    case RIOT_RULE_GTE:return v>=t;
    case RIOT_RULE_LTE:return v<=t;
    case RIOT_RULE_EQ:return fabsf(v-t)<0.0001f;
    case RIOT_RULE_NEQ:return fabsf(v-t)>=0.0001f;
    default:return v>t;
  }
}

// EP32 RULE ENGINE GATE5G5B DUAL ACTION EXEC UI V4
String runRiotRuleAction(uint8_t i,uint8_t action,uint8_t target,bool otherwise,bool secondary){
  if(i>=RIOT_RULE_COUNT)return "BAD RULE";
  RiotRuleConfig &r=riotRules[i];
  RiotRuleRuntime &rt=riotRuleRt[i];
  const char* side=otherwise?"OTHERWISE":"THEN";
  const char* slot=secondary?"2":"1";

  if(action==RIOT_ACT_NONE)return "NONE";

  float value=0.0f;String quality;
  riotRuleTagValue(r.tagIndex,value,quality);

  if(action==RIOT_ACT_ALARM){
    if(otherwise)return "ALARM BLOCKED";
    rt.alarm=true;
    rt.acknowledged=false;
    RiotHistoryStore::Entry rec={};
    rec.sequence=(uint32_t)RiotHistoryStore::count(SPIFFS)+1U;
    time_t now=time(nullptr);
    rec.epoch=(now>1000000000)?(uint32_t)now:0U;
    rec.type=RiotHistoryStore::ENTRY_ALARM;
    rec.sourceIndex=i;
    rec.value=isfinite(value)?value:0.0f;
    rec.threshold=r.threshold;
    snprintf(rec.label,sizeof(rec.label),"%s",r.name);
    snprintf(rec.message,sizeof(rec.message),"%s",strlen(r.message)?r.message:r.name);
    if(!RiotHistoryStore::append(SPIFFS,rec))Serial.println("RULE ALARM history append failed");
    Serial.printf("RULE %s ACTION%s ALARM: Rule %u\n",side,slot,(unsigned)(i+1));
    return "ALARM";
  }

  if(action==RIOT_ACT_EVENT){
    RiotHistoryStore::Entry rec={};
    rec.sequence=(uint32_t)RiotHistoryStore::count(SPIFFS)+1U;
    time_t now=time(nullptr);
    rec.epoch=(now>1000000000)?(uint32_t)now:0U;
    rec.type=RiotHistoryStore::ENTRY_EVENT;
    rec.sourceIndex=i;
    rec.value=isfinite(value)?value:0.0f;
    rec.threshold=r.threshold;
    snprintf(rec.label,sizeof(rec.label),"%s%s",r.name,otherwise?" ELSE":"");
    snprintf(rec.message,sizeof(rec.message),"%s",strlen(r.message)?r.message:(otherwise?"Otherwise action":r.name));
    if(!RiotHistoryStore::append(SPIFFS,rec))Serial.println("RULE EVENT history append failed");
    Serial.printf("RULE %s ACTION%s EVENT: Rule %u\n",side,slot,(unsigned)(i+1));
    return "EVENT";
  }

  if(action==RIOT_ACT_DO_OFF || action==RIOT_ACT_DO_ON){
    if(!modbusCommsEnabled || !modbusOnline || target<1 || target>4){
      riotRuleDOWriteBad++;
      Serial.printf("RULE %s ACTION%s DO BLOCKED: Rule %u target=%u\n",side,slot,(unsigned)(i+1),(unsigned)target);
      return "DO BLOCKED";
    }
    bool ok=writeM31Coil((uint8_t)(target-1),action==RIOT_ACT_DO_ON);
    if(ok)riotRuleDOWriteGood++; else riotRuleDOWriteBad++;
    Serial.printf("RULE %s ACTION%s DO: Rule %u DO%u=%s %s\n",
      side,slot,(unsigned)(i+1),(unsigned)target,action==RIOT_ACT_DO_ON?"ON":"OFF",ok?"OK":"FAIL");
    return ok?"DO APPLIED":"DO FAILED";
  }

  if(action==RIOT_ACT_PID_MANUAL){
    if(target<1 || target>FLOW_RUN_COUNT)return "PID BAD TARGET";
    uint8_t run=(uint8_t)(target-1);
    FlowPIDConfig before=flowPIDCfg[run];
    FlowPIDRuntime beforeRt=flowPIDRt[run];
    if(!flowRunEnabled[run] || before.mode==FLOW_CTRL_DISABLED)return "PID BLOCKED";

    float holdOut=isfinite(beforeRt.output)?beforeRt.output:before.manualOut;
    holdOut=constrain(holdOut,before.outMin,before.outMax);
    flowPIDCfg[run].manualOut=holdOut;
    flowPIDCfg[run].mode=FLOW_CTRL_MANUAL;
    flowPIDRt[run]=FlowPIDRuntime();
    flowPIDRt[run].output=holdOut;
    flowPIDRt[run].flowOutput=holdOut;
    flowPIDRt[run].inputGood=true;
    flowPIDRt[run].lastMode=FLOW_CTRL_MANUAL;
    flowPIDRt[run].initialized=true;
    flowPIDRt[run].lastCalcMs=millis();

    if(saveFlowPIDConfig()){
      Serial.printf("RULE %s ACTION%s PID MANUAL: Run %u hold %.2f%%\n",side,slot,(unsigned)(run+1),holdOut);
      return "PID MANUAL";
    }
    flowPIDCfg[run]=before;
    flowPIDRt[run]=beforeRt;
    return "PID SAVE FAIL";
  }

  return "NONE";
}

void fireRiotRule(uint8_t i){
  if(i>=RIOT_RULE_COUNT)return;
  RiotRuleConfig &r=riotRules[i];
  RiotRuleRuntime &rt=riotRuleRt[i];
  rt.active=true;
  rt.firedThisCondition=true;
  rt.fireCount++;
  rt.lastFireMs=millis();

  float firedValue=0.0f;String firedQuality;
  riotRuleTagValue(r.tagIndex,firedValue,firedQuality);
  rt.fireValue=isfinite(firedValue)?firedValue:0.0f;
  String stamp=displayDateTime();
  stamp.toCharArray(rt.firedAt,sizeof(rt.firedAt));
  if(r.latch)rt.latched=true;

  String s1=runRiotRuleAction(i,r.action,r.target,false,false);
  String s2=runRiotRuleAction(i,r.action2,r.target2,false,true);
  String st=s2=="NONE"?s1:(s1=="NONE"?s2:(s1+" + "+s2));
  if(st=="NONE")st="FIRED";
  st.toCharArray(rt.status,sizeof(rt.status));
}

// EP32 RULE ENGINE GATE5G2 OTHERWISE EXEC V1
void fireRiotRuleOtherwise(uint8_t i){
  if(i>=RIOT_RULE_COUNT)return;
  RiotRuleConfig &r=riotRules[i];
  RiotRuleRuntime &rt=riotRuleRt[i];

  String s1=runRiotRuleAction(i,r.elseAction,r.elseTarget,true,false);
  String s2=runRiotRuleAction(i,r.elseAction2,r.elseTarget2,true,true);
  String st=s2=="NONE"?s1:(s1=="NONE"?s2:(s1+" + "+s2));
  if(st=="NONE")st="IDLE";
  st.toCharArray(rt.status,sizeof(rt.status));
}

void serviceRiotRules(){
  static uint32_t last=0;uint32_t now=millis();if(now-last<250)return;last=now;
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
    RiotRuleConfig &r=riotRules[i];RiotRuleRuntime &rt=riotRuleRt[i];
    if(!riotRulesMasterEnabled || !r.enabled){
      if(!rt.latched)resetRiotRuleRuntime(i);
      continue;
    }

    // A latched rule remains asserted until the operator resets it.
    // OTHERWISE cannot execute while the rule is latched.
    if(rt.latched){rt.active=true;snprintf(rt.status,sizeof(rt.status),"LATCHED");continue;}

    float v=0.0f;String q;bool good=riotRuleTagValue(r.tagIndex,v,q);
    if(!good){
      // Bad/disabled/unavailable input never executes THEN or OTHERWISE.
      rt.condition=false;
      rt.timing=false;
      rt.trueSinceMs=0;
      rt.active=false;
      snprintf(rt.status,sizeof(rt.status),"SOURCE %s",q.c_str());
      continue;
    }

    bool cond=riotRuleCompare(v,r.compare,r.threshold);
    rt.condition=cond;

    if(!cond){
      // Edge-triggered OTHERWISE: execute once only after this rule's THEN
      // side actually completed its delay and fired. It does not run at boot
      // simply because the condition starts false.
      const bool runOtherwise=rt.firedThisCondition;
      rt.timing=false;
      rt.trueSinceMs=0;
      rt.active=false;
      rt.alarm=false;
      if(runOtherwise)fireRiotRuleOtherwise(i);
      rt.firedThisCondition=false;
      continue;
    }

    if(!rt.trueSinceMs)rt.trueSinceMs=now;
    uint32_t elapsed=now-rt.trueSinceMs;
    if(elapsed<r.delayMs){
      rt.timing=true;
      snprintf(rt.status,sizeof(rt.status),"TIMING");
      continue;
    }

    rt.timing=false;
    if(!rt.firedThisCondition)fireRiotRule(i);
    else rt.active=true;
  }
}

bool applyManualControlOutput(uint8_t run){
  if(run>=FLOW_RUN_COUNT || !flowRunEnabled[run] || !modbusOnline)return false;
  ManualControlOutput &o=manualControlOutput[run];
  if(o.type==CONTROL_OUTPUT_AO)return writeM31AO(o.channel,o.aoPercent);
  if(o.type==CONTROL_OUTPUT_DO)return writeM31Coil(o.channel,o.doOn);
  return false;
}

void notePoll(bool ok){
  if(ok){
    modbusGood++;
    consecutiveFails=0;
  }else{
    modbusBad++;
    if(consecutiveFails<1000000)consecutiveFails++;
  }
  bool was=modbusOnline;
  modbusOnline=consecutiveFails<5;
  if(was!=modbusOnline)displayDirty=true;
  if(ok)updateEngineeringUnits();
}


// EP32 M31 RACK NEGOTIATE GATE3 V2
// Ported from the proven P4 M31 commission/rack sequence, but routed through
// EP32 transact() so the same logic works over RTU or Modbus TCP.
struct Ep32M31RackStatus {
  bool valid=false;
  uint16_t protect=0;
  uint16_t command=0;
  uint16_t complete=0;
  uint16_t deviceError=0;
  uint16_t exceptionHi=0;
  uint16_t exceptionLo=0;
  uint16_t pending=0;
  String error="NOT READ";
};

static bool ep32M31ReadHolding(uint16_t start,uint16_t qty,uint16_t* out,String& error){
  if(qty<1 || qty>20 || out==nullptr){error="invalid holding request";return false;}
  uint8_t req[8]={m31UnitId,0x03,(uint8_t)(start>>8),(uint8_t)start,(uint8_t)(qty>>8),(uint8_t)qty,0,0};
  uint16_t crc=modbusCRC(req,6); req[6]=(uint8_t)(crc&0xFF); req[7]=(uint8_t)(crc>>8);
  uint8_t response[64]; size_t count=0;
  const size_t minCount=(size_t)5U+(size_t)qty*2U;
  if(!transact(req,sizeof(req),response,count,minCount,500)){error="holding transaction failed";return false;}
  if(count<5){error="holding response short";return false;}
  if(response[0]!=m31UnitId){error="holding unit mismatch";return false;}
  if(response[1]&0x80){error=String("modbus exception ")+String(response[2]);return false;}
  if(response[1]!=0x03){error="holding function mismatch";return false;}
  if(response[2]!=(uint8_t)(qty*2U)){error="holding byte count mismatch";return false;}
  if(count < (size_t)response[2]+5U){error="holding payload short";return false;}
  for(uint16_t i=0;i<qty;i++)out[i]=((uint16_t)response[3+i*2]<<8)|response[4+i*2];
  error="";return true;
}

static bool ep32M31WriteSingle(uint16_t address,uint16_t value,String& error){
  uint8_t req[8]={m31UnitId,0x06,(uint8_t)(address>>8),(uint8_t)address,(uint8_t)(value>>8),(uint8_t)value,0,0};
  uint16_t crc=modbusCRC(req,6); req[6]=(uint8_t)(crc&0xFF); req[7]=(uint8_t)(crc>>8);
  uint8_t response[64]; size_t count=0;
  if(!transact(req,sizeof(req),response,count,8,500)){error="write transaction failed";return false;}
  if(count<5){error="write response short";return false;}
  if(response[0]!=m31UnitId){error="write unit mismatch";return false;}
  if(response[1]&0x80){error=String("modbus exception ")+String(response[2]);return false;}
  if(response[1]!=0x06){error="write function mismatch";return false;}
  const uint16_t rxAddress=((uint16_t)response[2]<<8)|response[3];
  const uint16_t rxValue=((uint16_t)response[4]<<8)|response[5];
  if(rxAddress!=address || rxValue!=value){error="write echo mismatch";return false;}
  error="";return true;
}

static bool ep32M31ReadRackStatus(Ep32M31RackStatus& s){
  s=Ep32M31RackStatus{};
  uint16_t r[7]={0}; String error;
  if(!ep32M31ReadHolding(0x7584,7,r,error)){s.error=error;return false;}
  s.protect=r[0]; s.command=r[1]; s.complete=r[2]; s.deviceError=r[3];
  s.exceptionHi=r[4]; s.exceptionLo=r[5]; s.pending=r[6];
  s.valid=true; s.error=""; return true;
}

static String ep32M31RackStatusJson(const Ep32M31RackStatus& s,bool includeOk=false,bool ok=false,const String& actionError=String(),bool transitionObserved=false,uint8_t stableReads=0){
  String e=actionError.length()?actionError:s.error; e.replace("\\","\\\\"); e.replace("\"","\\\"");
  const uint32_t bitmap=((uint32_t)s.exceptionHi<<16)|(uint32_t)s.exceptionLo;
  String j; j.reserve(900); j="{";
  if(includeOk)j+="\"ok\":"+String(ok?"true":"false")+",";
  j+="\"transport\":\""+String(m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU")+"\"";
  j+=",\"target\":\""+String(m31TransportMode==M31_TRANSPORT_TCP?m31TcpHost:"RS485")+"\"";
  if(m31TransportMode==M31_TRANSPORT_TCP)j+=",\"port\":"+String(m31TcpPort);
  j+=",\"unit\":"+String(m31UnitId);
  j+=",\"valid\":"+String(s.valid?"true":"false");
  j+=",\"writeProtection\":"+String(s.protect);
  j+=",\"negotiationCommand\":"+String(s.command);
  j+=",\"negotiationComplete\":"+String(s.complete);
  j+=",\"deviceError\":"+String(s.deviceError);
  j+=",\"exceptionWord0\":"+String(s.exceptionHi);
  j+=",\"exceptionWord1\":"+String(s.exceptionLo);
  j+=",\"exceptionBitmap\":"+String((unsigned long)bitmap);
  j+=",\"negotiationRequired\":"+String(s.pending?"true":"false");
  j+=",\"healthy\":"+String((s.valid&&s.complete==1&&s.deviceError==0&&s.pending==0)?"true":"false");
  if(includeOk){
    j+=",\"transitionObserved\":"+String(transitionObserved?"true":"false");
    j+=",\"stableHealthyReads\":"+String(stableReads);
  }
  j+=",\"error\":\""+e+"\"}";
  return j;
}

static bool ep32M31NegotiateRack(Ep32M31RackStatus& before,Ep32M31RackStatus& after,bool& transitionObserved,uint8_t& stableReads,String& error){
  transitionObserved=false; stableReads=0;
  if(!ep32M31ReadRackStatus(before)){error=before.error;after=before;return false;}
  if(before.complete!=1 || before.deviceError!=0 || before.pending!=0){
    error="commission baseline not healthy/settled"; after=before; return false;
  }

  // Proven P4/M31-U sequence: arm write protection, then start negotiation.
  if(!ep32M31WriteSingle(0x7584,1,error)){after=before;return false;}
  if(!ep32M31WriteSingle(0x7585,1,error)){after=before;return false;}

  const uint32_t started=millis();
  Ep32M31RackStatus current;
  while(millis()-started<12000UL){
    if(!ep32M31ReadRackStatus(current)){error=current.error;after=current;return false;}
    if(current.protect!=before.protect || current.command!=before.command || current.complete!=before.complete ||
       current.deviceError!=before.deviceError || current.exceptionHi!=before.exceptionHi ||
       current.exceptionLo!=before.exceptionLo || current.pending!=before.pending)transitionObserved=true;
    after=current;

    const uint32_t elapsed=millis()-started;
    const bool healthyNow=(current.complete==1 && current.deviceError==0 && current.pending==0);
    if(elapsed>=3000UL && healthyNow){
      if(stableReads<255)stableReads++;
      if(stableReads>=3){error="";return true;}
    }else stableReads=0;
    delay(500);
  }

  if(after.deviceError!=0)error=String("commission device error ")+String(after.deviceError);
  else if(after.pending!=0)error="commission negotiation still pending";
  else error="commission negotiation did not settle";
  return false;
}

void serviceM31(){
  if(!modbusCommsEnabled){
    modbusOnline=false;
    consecutiveFails=0;
    return;
  }

  uint32_t now=millis();
  if(now-lastPoll<220)return;
  lastPoll=now;

  // Commissioning AO force has priority when a refresh is due, while
  // preserving the one-RTU-transaction-per-tick scheduler rule.
  if(ep32M31ExpansionPresent() && now-aoForceLastRefreshMs>=1000){
    for(uint8_t n=0;n<4;n++){
      uint8_t ch=aoForceNext;
      aoForceNext=(aoForceNext+1)%4;
      if(!aoForceActive[ch])continue;
      bool ok=writeM31AORaw(ch,aoForcePercent[ch]);
      aoForceLastRefreshMs=now;
      notePoll(ok);
      if(ok)displayDirty=true;
      return;
    }
  }

  // Normal control writes are lower priority than commissioning FORCE.
  static uint32_t lastControlWrite=0;
  static uint8_t nextControlRun=0;
  if(now-lastControlWrite>=1000){
    for(uint8_t n=0;n<FLOW_RUN_COUNT;n++){
      uint8_t run=nextControlRun;
      nextControlRun=(nextControlRun+1)%FLOW_RUN_COUNT;
      FlowPIDConfig &c=flowPIDCfg[run];
      FlowPIDRuntime &rt=flowPIDRt[run];
      if(!flowRunEnabled[run] || c.mode==FLOW_CTRL_DISABLED)continue;
      if(!ep32M31ExpansionPresent())continue;
      if(aoForceActive[c.aoChannel])continue;
      if(c.mode==FLOW_CTRL_AUTO && !rt.inputGood)continue;
      bool ok=writeM31AO(c.aoChannel,rt.output);
      lastControlWrite=now;
      notePoll(ok);
      if(ok)displayDirty=true;
      return;
    }
  }

  bool ok=false;
  const bool expansion=ep32M31ExpansionPresent();
  const bool slot2AI8=ep32M31Slot2Ai8Present();

  // Gate A2a:
  // 0 DI
  // 1 DO
  // 2 AI1-AI4
  // 3 AI5-AI8
  // 4 AI9-AI12
  // 5 AI13-AI16
  // 6 AO1-AO4
  switch(pollPhase){
    case 0:
      ok=readBits(0x02,diRawState);
      if(ok)updateEffectiveDI();
      break;

    case 1:
      ok=readBits(0x01,doState);
      break;

    case 2:
      ok=readAIBlock(0x0000,0);
      break;

    case 3:
      ok=expansion?readAIBlock(0x0004,4):true;
      break;

    case 4:
      ok=slot2AI8?readAIBlock(0x0008,8):true;
      break;

    case 5:
      ok=slot2AI8?readAIBlock(0x000C,12):true;
      break;

    default:
      ok=expansion?readAO():true;
      break;
  }

  notePoll(ok);

  if(slot2AI8){
    pollPhase=(pollPhase+1)%7;
  }else if(expansion){
    if(pollPhase==3)pollPhase=6;
    else if(pollPhase==6)pollPhase=0;
    else pollPhase++;
  }else{
    pollPhase=(pollPhase+1)%3;
  }

  if(ok)displayDirty=true;
}


void loadAnalogConfig(){
  Serial.println("OPCFG: legacy analog NVS loader retired; flash config is authoritative");
}

void saveAnalogChannel(uint8_t i){
  if(i>=8)return;
  (void)saveOperatorFlashConfig();
}

void loadAGAConfig(){
  Serial.println("AGACFG: legacy AGA NVS loader retired");
}

void saveAGAConfig(){
  (void)saveAGAFlashConfig();
}

void saveAGARunGas(uint8_t run){
  if(run>=FLOW_RUN_COUNT)return;
  (void)saveAGAFlashConfig();
}


void saveAGARunConfig(uint8_t run){
  if(run>=FLOW_RUN_COUNT)return;
  (void)saveAGAFlashConfig();
}

void agaTask(void*){
  agaEngineBegin();
  agaCoreReady=true;

  for(;;){
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);

    float dp,sp,tp;
    uint8_t run;
    portENTER_CRITICAL(&agaMux);
    dp=agaReqDp;
    sp=agaReqStatic;
    tp=agaReqTemp;
    run=agaReqRun;
    agaRequestPending=false;
    agaTaskBusy=true;
    portEXIT_CRITICAL(&agaMux);

    AGASetupConfig meterCfg=agaRunCfg[(run<FLOW_RUN_COUNT)?run:0];
    AGAEngineConfig cfg={
      meterCfg.pipeID_mm,
      meterCfg.orifice_mm,
      meterCfg.basePressure_kPaAbs,
      meterCfg.baseTemperature_C,
      meterCfg.atmosphericPressure_kPa,
      meterCfg.staticPressureIsAbsolute,
      meterCfg.tapType
    };

    uint32_t started=millis();
    AGAEngineResult r=agaEngineCalculate(cfg,agaRunMolPct[(run<FLOW_RUN_COUNT)?run:0],dp,sp,tp);
    uint32_t elapsed=millis()-started;

    portENTER_CRITICAL(&agaMux);
    if(run<FLOW_RUN_COUNT){
      flowRunResult[run]=r;
      flowRunValid[run]=r.valid;
      flowRunFlow[run]=r.valid?r.flow_E3m3_day:0.0f;
      flowRunCalcMs[run]=elapsed;
      flowRunCalcCount[run]++;
    }
    if(run==0){
      agaResult=r;
      agaTaskLastMs=elapsed;
      flowPreviewValid=r.valid;
      totalFlowPreview=r.valid?r.flow_E3m3_day:0.0f;
    }
    agaTaskBusy=false;
    agaCompleteCount++;
    portEXIT_CRITICAL(&agaMux);

    displayDirty=true;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void startAGATask(){
  if(agaTaskHandle)return;
  BaseType_t ok=xTaskCreatePinnedToCore(
    agaTask,
    "aga",
    32768,
    nullptr,
    1,
    &agaTaskHandle,
    1
  );
  if(ok!=pdPASS)agaTaskHandle=nullptr;
}

void serviceAGA(){
  static uint32_t lastRequest=0;
  static uint8_t nextRun=0;
  uint32_t now=millis();

  // Spread the DETAIL calculations across time. With all 6 runs enabled,
  // each run updates about every 1.5 seconds while the core loop stays free.
  if(now-lastRequest<250)return;

  bool canQueue=false;
  portENTER_CRITICAL(&agaMux);
  canQueue=!agaTaskBusy && !agaRequestPending;
  portEXIT_CRITICAL(&agaMux);
  if(!canQueue || !agaTaskHandle)return;

  for(uint8_t tries=0;tries<FLOW_RUN_COUNT;tries++){
    uint8_t run=nextRun;
    nextRun=(nextRun+1)%FLOW_RUN_COUNT;

    if(!flowRunEnabled[run]){
      flowRunValid[run]=false;
      flowRunFlow[run]=0.0f;
      continue;
    }

    bool ready=true;
    FlowResolvedPoint p[3];
    for(uint8_t role=0;role<3;role++){
      p[role]=resolveFlowRunInput(run,role);
      if(!isfinite(p[role].value)||p[role].quality==FLOW_QUALITY_BAD)ready=false;
    }

    if(!ready){
      flowRunValid[run]=false;
      flowRunFlow[run]=0.0f;
      if(run==0){
        flowPreviewValid=false;
        totalFlowPreview=0.0f;
      }
      continue;
    }

    portENTER_CRITICAL(&agaMux);
    if(!agaTaskBusy && !agaRequestPending){
      agaReqRun=run;
      agaReqDp=p[FLOW_ROLE_DP].value;
      agaReqStatic=p[FLOW_ROLE_STATIC].value;
      agaReqTemp=p[FLOW_ROLE_TEMP].value;
      agaRequestPending=true;
      agaRequestCount++;
      canQueue=true;
    }else{
      canQueue=false;
    }
    portEXIT_CRITICAL(&agaMux);

    if(canQueue){
      lastRequest=now;
      xTaskNotifyGive(agaTaskHandle);
    }
    return;
  }

  lastRequest=now;
}

void loadGoldenReference(){
  // Golden reference captured from the proven MultiFlow controller on 2026-08-28.
  // Run 1 resolved static source is 2000 kPa(g); AI2 diagnostics may show a different manual channel value.
  agaCfg.pipeID_mm=78.130f;
  agaCfg.orifice_mm=41.275f;
  agaCfg.basePressure_kPaAbs=101.325f;
  agaCfg.baseTemperature_C=15.000f;
  agaCfg.atmosphericPressure_kPa=88.653f;
  agaCfg.staticPressureIsAbsolute=false;
  agaCfg.tapType=0;

  const float ref[AGA_COMPONENTS]={
    89.2500f,0.4100f,1.3400f,6.7100f,1.5300f,
    0.2100f,0.2400f,0.0700f,0.0600f,0.0500f,
    0.1200f,0.0000f,0.0000f,0.0000f,0.0000f,
    0.0000f,0.0000f,0.0000f,0.0000f,0.0100f,0.0000f
  };
  for(int i=0;i<AGA_COMPONENTS;i++)agaMolPct[i]=ref[i];

  run1Bind[FLOW_ROLE_DP].manual=true;
  run1Bind[FLOW_ROLE_DP].manualValue=20.000f;
  run1Bind[FLOW_ROLE_STATIC].manual=true;
  run1Bind[FLOW_ROLE_STATIC].manualValue=2000.000f;
  run1Bind[FLOW_ROLE_TEMP].manual=true;
  run1Bind[FLOW_ROLE_TEMP].manualValue=21.544f;
  saveRun1Bindings();

  aiCfg[0].euLow=0.0f;
  aiCfg[0].euHigh=248.8429f;
  strncpy(aiCfg[0].units,"kPa",sizeof(aiCfg[0].units)-1);
  aiCfg[1].euLow=0.0f;
  aiCfg[1].euHigh=25000.3867f;
  strncpy(aiCfg[1].units,"kPa",sizeof(aiCfg[1].units)-1);
  aiCfg[2].euLow=-200.0f;
  aiCfg[2].euHigh=850.0f;
  strncpy(aiCfg[2].units,"C",sizeof(aiCfg[2].units)-1);

  for(uint8_t i=0;i<3;i++)saveAnalogChannel(i);
  saveAGAConfig();
  updateEngineeringUnits();

  portENTER_CRITICAL(&agaMux);
  flowPreviewValid=false;
  totalFlowPreview=0.0f;
  portEXIT_CRITICAL(&agaMux);

  displayDirty=true;
}

void loadNetworkConfig(){
  prefs.begin("riot",true);
  wifiStaSsid=prefs.getString("ssid","");
  wifiStaPass=prefs.getString("pass","");
  String legacySiteName=prefs.getString("siteName","");
  String legacySiteLSD=prefs.getString("siteLSD","");
  if(!legacySiteLSD.length())legacySiteLSD=prefs.getString("siteLoc","");
  prefs.end();

  bool fileLoaded=false;
  File sf=SPIFFS.open(SITE_IDENTITY_PATH,FILE_READ);
  if(sf){
    String n=sf.readStringUntil('\n'); n.trim();
    String l=sf.readStringUntil('\n'); l.trim();
    sf.close();
    if(n.length()){
      siteName=n;
      siteLSD=l;
      fileLoaded=true;
    }
  }

  if(!fileLoaded){
    siteName=legacySiteName.length()?legacySiteName:String("MultiFlow Site");
    siteLSD=legacySiteLSD;
    if(legacySiteName.length() || legacySiteLSD.length()){
      File out=SPIFFS.open(SITE_IDENTITY_PATH,FILE_WRITE);
      if(out){
        out.println(siteName);
        out.println(siteLSD);
        out.flush();
        out.close();
      }
    }
  }
}

void saveNetworkConfig(const String& ssid,const String& pass){
  prefs.begin("riot",false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
  prefs.end();
  wifiStaSsid=ssid;
  wifiStaPass=pass;
}

void saveSiteIdentity(const String& name,const String& lsd){
  siteName=name;
  siteName.trim();
  siteLSD=lsd;
  siteLSD.trim();
  if(siteName.length()>40)siteName=siteName.substring(0,40);
  if(siteLSD.length()>48)siteLSD=siteLSD.substring(0,48);
  if(!siteName.length())siteName="MultiFlow Site";

  File out=SPIFFS.open(SITE_IDENTITY_PATH,FILE_WRITE);
  if(out){
    out.println(siteName);
    out.println(siteLSD);
    out.flush();
    out.close();
  }

  // Gate5B: site identity remains SPIFFS-authoritative; no duplicate NVS write.

  requestFullEPaperRefresh();
}


static constexpr uint16_t COMMON_BACKUP_VERSION=2;
static constexpr size_t COMMON_BACKUP_MAX_BYTES=262144;

static const char* COMMON_BACKUP_NVS_NAMESPACES[]={
  "aga","agagas","agaruns","analog","display","flowmeta","flowruns","gaslib","riot"
};

struct CommonBackupFileSpec {
  const char* path;
  char scope; // C=common, H=hardware-specific
};

// RIOT BACKUP FORMAT COMPLETION GATE A5B V1
static const CommonBackupFileSpec COMMON_BACKUP_FILES[]={
  {"/site_identity.cfg",'C'},
  {"/control.cfg",'C'},
  {"/mqtt.cfg",'C'},
  {"/display_logo.bin",'H'},
  {"/operator_config_v2.bin",'H'},
  {"/rules_config_v1.bin",'C'}
};

bool commonBackupNamespaceAllowed(const char* ns){
  if(!ns||!*ns)return false;
  for(size_t i=0;i<sizeof(COMMON_BACKUP_NVS_NAMESPACES)/sizeof(COMMON_BACKUP_NVS_NAMESPACES[0]);i++){
    if(strcmp(ns,COMMON_BACKUP_NVS_NAMESPACES[i])==0)return true;
  }
  return false;
}

uint32_t commonBackupChecksum(const uint8_t* data,size_t len){
  uint32_t h=2166136261UL;
  for(size_t i=0;i<len;i++){h^=data[i];h*=16777619UL;}
  return h;
}

String commonBackupHex(const uint8_t* data,size_t len){
  static const char* HEX_DIGITS="0123456789ABCDEF";
  String out;
  out.reserve(len*2);
  for(size_t i=0;i<len;i++){
    uint8_t v=data[i];
    out+=HEX_DIGITS[v>>4];
    out+=HEX_DIGITS[v&0x0F];
  }
  return out;
}

String commonBackupHexText(const char* text){
  if(!text)return "";
  return commonBackupHex((const uint8_t*)text,strlen(text));
}

bool commonBackupReadNVSValue(nvs_handle_t h,const nvs_entry_info_t& info,std::vector<uint8_t>& bytes){
  bytes.clear();
  switch(info.type){
    case NVS_TYPE_U8:{uint8_t v=0;if(nvs_get_u8(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+1);return true;}
    case NVS_TYPE_I8:{int8_t v=0;if(nvs_get_i8(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+1);return true;}
    case NVS_TYPE_U16:{uint16_t v=0;if(nvs_get_u16(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+2);return true;}
    case NVS_TYPE_I16:{int16_t v=0;if(nvs_get_i16(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+2);return true;}
    case NVS_TYPE_U32:{uint32_t v=0;if(nvs_get_u32(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+4);return true;}
    case NVS_TYPE_I32:{int32_t v=0;if(nvs_get_i32(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+4);return true;}
    case NVS_TYPE_U64:{uint64_t v=0;if(nvs_get_u64(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+8);return true;}
    case NVS_TYPE_I64:{int64_t v=0;if(nvs_get_i64(h,info.key,&v)!=ESP_OK)return false;bytes.assign((uint8_t*)&v,(uint8_t*)&v+8);return true;}
    case NVS_TYPE_STR:{
      size_t n=0;
      if(nvs_get_str(h,info.key,nullptr,&n)!=ESP_OK||n==0)return false;
      bytes.resize(n);
      return nvs_get_str(h,info.key,(char*)bytes.data(),&n)==ESP_OK;
    }
    case NVS_TYPE_BLOB:{
      size_t n=0;
      if(nvs_get_blob(h,info.key,nullptr,&n)!=ESP_OK)return false;
      bytes.resize(n);
      if(!n)return true;
      return nvs_get_blob(h,info.key,bytes.data(),&n)==ESP_OK;
    }
    default:return false;
  }
}

String commonBackupTimestamp(){
  time_t t=time(nullptr);
  if(t>1700000000){
    struct tm tmv;
    localtime_r(&t,&tmv);
    char b[32];
    strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%S%z",&tmv);
    return String(b);
  }
  return String("uptime_ms:")+String(millis());
}

void cleanupLegacyHistoryNVS();

String commonBackupSourceId(){
  uint64_t chip=ESP.getEfuseMac();
  char b[20];
  snprintf(b,sizeof(b),"%04X%08X",(uint16_t)(chip>>32),(uint32_t)chip);
  return String(b);
}

String commonBackupSiteStem(){
  String s=siteName;
  s.trim();
  if(!s.length())s="MultiFlow";
  for(size_t i=0;i<s.length();i++){
    char ch=s[i];
    if(!isalnum((unsigned char)ch) && ch!='-' && ch!='_')s.setCharAt(i,'_');
  }
  while(s.indexOf("__")>=0)s.replace("__","_");
  if(s.length()>32)s=s.substring(0,32);
  return s;
}

struct CommonBackupBuildResult {
  String data;
  uint32_t nvsRecords=0;
  uint32_t fileRecords=0;
  uint32_t skippedFiles=0;
  bool ok=false;
  String error;
};

CommonBackupBuildResult buildCommonBackupV2(){
  CommonBackupBuildResult result;
  String out;
  out.reserve(32768);
  out+="MULTIFLOW_BACKUP|2|TRMNL_XIAO_EPAPER|";
  out+=commonBackupSourceId();
  out+="|"+commonBackupTimestamp()+"\n";
  out+="M|maxFlowRuns|36\n"; // ASCII "6"
  out+="M|firmware|"+commonBackupHexText(FW)+"\n";

#if ESP_IDF_VERSION_MAJOR >= 5
  nvs_iterator_t it=nullptr;
  esp_err_t iterResult=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY,&it);
  while(iterResult==ESP_OK && it!=nullptr){
    nvs_entry_info_t info={};
    if(nvs_entry_info(it,&info)==ESP_OK && commonBackupNamespaceAllowed(info.namespace_name)){
      nvs_handle_t h=0;
      if(nvs_open(info.namespace_name,NVS_READONLY,&h)==ESP_OK){
        std::vector<uint8_t> value;
        if(commonBackupReadNVSValue(h,info,value)){
          out+="R|"+String((uint8_t)info.type)+"|";
          out+=commonBackupHexText(info.namespace_name);
          out+="|"+commonBackupHexText(info.key)+"|";
          out+=commonBackupHex(value.data(),value.size());
          out+="\n";
          result.nvsRecords++;
        }
        nvs_close(h);
      }
    }
    iterResult=nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
#else
  nvs_iterator_t it=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY);
  while(it!=nullptr){
    nvs_entry_info_t info={};
    nvs_entry_info(it,&info);
    if(commonBackupNamespaceAllowed(info.namespace_name)){
      nvs_handle_t h=0;
      if(nvs_open(info.namespace_name,NVS_READONLY,&h)==ESP_OK){
        std::vector<uint8_t> value;
        if(commonBackupReadNVSValue(h,info,value)){
          out+="R|"+String((uint8_t)info.type)+"|";
          out+=commonBackupHexText(info.namespace_name);
          out+="|"+commonBackupHexText(info.key)+"|";
          out+=commonBackupHex(value.data(),value.size());
          out+="\n";
          result.nvsRecords++;
        }
        nvs_close(h);
      }
    }
    it=nvs_entry_next(it);
  }
#endif

  for(size_t i=0;i<sizeof(COMMON_BACKUP_FILES)/sizeof(COMMON_BACKUP_FILES[0]);i++){
    const CommonBackupFileSpec& spec=COMMON_BACKUP_FILES[i];
    if(!SPIFFS.exists(spec.path)){result.skippedFiles++;continue;}
    File file=SPIFFS.open(spec.path,FILE_READ);
    if(!file){result.skippedFiles++;continue;}
    size_t n=file.size();
    if(n>65536){
      file.close();
      result.error=String("Config file too large: ")+spec.path;
      return result;
    }
    std::vector<uint8_t> bytes(n);
    size_t got=n?file.read(bytes.data(),n):0;
    file.close();
    if(got!=n){
      result.error=String("Could not read config file: ")+spec.path;
      return result;
    }
    out+="F|";
    out+=spec.scope;
    out+="|"+commonBackupHexText(spec.path)+"|";
    out+=commonBackupHex(bytes.data(),bytes.size());
    out+="\n";
    result.fileRecords++;
  }

  if(result.nvsRecords==0 && result.fileRecords==0){
    result.error="No configuration records found";
    return result;
  }
  if(out.length()>COMMON_BACKUP_MAX_BYTES){
    result.error="Backup exceeds maximum size";
    return result;
  }

  uint32_t sum=commonBackupChecksum((const uint8_t*)out.c_str(),out.length());
  char checksumLine[24];
  snprintf(checksumLine,sizeof(checksumLine),"CHECKSUM|%08lX\n",(unsigned long)sum);
  out+=checksumLine;

  result.data=out;
  result.ok=true;
  return result;
}


struct CommonBackupValidateResult {
  bool ok=false;
  uint16_t version=0;
  String sourceModel="";
  uint32_t nvsRecords=0;
  uint32_t fileRecords=0;
  uint32_t metadataRecords=0;
  uint32_t unsupportedRecords=0;
  uint32_t skippedFiles=0;
  String supportedNamespaces="";
  String unsupportedNamespaces="";
  String supportedFiles="";
  String unsupportedFiles="";
  String error="";
};

void commonBackupAppendUniqueCSV(String& csv,const String& value){
  String token=","+csv+",";
  String needle=","+value+",";
  if(token.indexOf(needle)>=0)return;
  if(csv.length())csv+=",";
  csv+=value;
}

int commonBackupHexNibble(char ch){
  if(ch>='0'&&ch<='9')return ch-'0';
  if(ch>='A'&&ch<='F')return ch-'A'+10;
  if(ch>='a'&&ch<='f')return ch-'a'+10;
  return -1;
}

bool commonBackupUnhex(const String& s,std::vector<uint8_t>& out){
  if((s.length()&1)!=0)return false;
  out.clear();
  out.reserve(s.length()/2);
  for(size_t i=0;i<s.length();i+=2){
    int hi=commonBackupHexNibble(s[i]);
    int lo=commonBackupHexNibble(s[i+1]);
    if(hi<0||lo<0)return false;
    out.push_back((uint8_t)((hi<<4)|lo));
  }
  return true;
}

bool commonBackupDecodeText(const String& hex,String& out){
  std::vector<uint8_t> bytes;
  if(!commonBackupUnhex(hex,bytes))return false;
  out="";
  out.reserve(bytes.size());
  for(uint8_t v:bytes)out+=(char)v;
  return true;
}

bool commonBackupFileAllowed(const String& path,char scope){
  for(size_t i=0;i<sizeof(COMMON_BACKUP_FILES)/sizeof(COMMON_BACKUP_FILES[0]);i++){
    if(path==COMMON_BACKUP_FILES[i].path && scope==COMMON_BACKUP_FILES[i].scope)return true;
  }
  return false;
}

// RIOT CONTROLLED RESTORE POLICY GATE A5C V1
static bool commonBackupReviewOnlyFile(const String& path){
  return path=="/operator_config_v2.bin" ||
         path=="/rules_config_v1.bin";
}

static bool commonBackupRestoreFileAllowed(const String& path,char scope){
  // Review-only files remain blocked from the generic restore path.
  // A5c imports them only through explicit, separately validated handlers.
  return commonBackupFileAllowed(path,scope) && !commonBackupReviewOnlyFile(path);
}

static bool commonBackupExtractFile(
  const String& backup,
  const char* wantedPath,
  char wantedScope,
  std::vector<uint8_t>& value
){
  value.clear();
  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  int pos=backup.indexOf('\n')+1;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();
    if(line.startsWith("F|") && line.length()>=5){
      const char scope=line[2];
      int p1=line.indexOf('|',4);
      if(p1>=0){
        String pathHex=line.substring(4,p1);
        String dataHex=line.substring(p1+1);
        String path;
        std::vector<uint8_t> decoded;
        if(commonBackupDecodeText(pathHex,path) &&
           path==wantedPath &&
           scope==wantedScope &&
           commonBackupUnhex(dataHex,decoded)){
          value.swap(decoded);
          return true;
        }
      }
    }
    pos=eol+1;
  }
  return false;
}

static bool commonBackupCommitRawFile(
  const char* path,
  const char* tmpPath,
  const char* bakPath,
  const uint8_t* data,
  size_t len,
  String& error
){
  if(!path||!tmpPath||!bakPath||(!data&&len)){
    error="Invalid file commit arguments";
    return false;
  }
  SPIFFS.remove(tmpPath);
  File out=SPIFFS.open(tmpPath,FILE_WRITE);
  if(!out){error=String("Cannot stage ")+path;return false;}
  size_t n=len?out.write(data,len):0;
  out.flush();out.close();
  if(n!=len){SPIFFS.remove(tmpPath);error=String("Short write staging ")+path;return false;}

  SPIFFS.remove(bakPath);
  if(SPIFFS.exists(path) && !SPIFFS.rename(path,bakPath)){
    SPIFFS.remove(tmpPath);
    error=String("Cannot preserve current ")+path;
    return false;
  }
  if(!SPIFFS.rename(tmpPath,path)){
    if(SPIFFS.exists(bakPath))SPIFFS.rename(bakPath,path);
    error=String("Cannot commit ")+path;
    return false;
  }
  return true;
}

// RIOT CONTROLLED RESTORE PREFLIGHT GATE A5C1 V1
static bool commonBackupValidateOperatorImport(
  const String& backup,
  String& error
){
  std::vector<uint8_t> bytes;
  if(!commonBackupExtractFile(backup,OPERATOR_CFG_PATH,'H',bytes)){
    error="Backup does not contain operator_config_v2.bin";
    return false;
  }
  if(bytes.size()!=sizeof(OperatorFlashConfigV2)){
    error="Operator config size mismatch";
    return false;
  }
  OperatorFlashConfigV2 candidate;
  memcpy(&candidate,bytes.data(),sizeof(candidate));
  if(!operatorFlashConfigValid(candidate)){
    error="Operator config validation failed";
    return false;
  }
  return true;
}

static bool commonBackupValidateRulesImport(
  const String& backup,
  String& error
){
  std::vector<uint8_t> bytes;
  if(!commonBackupExtractFile(backup,RIOT_RULE_FILE_PATH,'C',bytes)){
    error="Backup does not contain rules_config_v1.bin";
    return false;
  }

  if(bytes.size()==sizeof(RiotRuleFileV2)){
    RiotRuleFileV2 incoming;
    memcpy(&incoming,bytes.data(),sizeof(incoming));
    if(incoming.magic!=RIOT_RULE_FILE_MAGIC ||
       incoming.version!=2 ||
       incoming.size!=sizeof(incoming) ||
       incoming.checksum!=riotRuleFileChecksumT(incoming)){
      error="Rules V2 validation failed";
      return false;
    }
    return true;
  }

  if(bytes.size()==sizeof(RiotRuleFileV1Legacy)){
    RiotRuleFileV1Legacy incoming;
    memcpy(&incoming,bytes.data(),sizeof(incoming));
    if(incoming.magic!=RIOT_RULE_FILE_MAGIC ||
       incoming.version!=1 ||
       incoming.size!=sizeof(incoming) ||
       incoming.checksum!=riotRuleFileChecksumT(incoming)){
      error="Rules V1 validation failed";
      return false;
    }
    return true;
  }

  error="Rules config size mismatch";
  return false;
}

static bool commonBackupImportOperatorConfig(
  const String& backup,
  String& error
){
  std::vector<uint8_t> bytes;
  if(!commonBackupExtractFile(backup,OPERATOR_CFG_PATH,'H',bytes)){
    error="Backup does not contain operator_config_v2.bin";
    return false;
  }
  if(bytes.size()!=sizeof(OperatorFlashConfigV2)){
    error="Operator config size mismatch";
    return false;
  }

  OperatorFlashConfigV2 candidate;
  memcpy(&candidate,bytes.data(),sizeof(candidate));
  if(!operatorFlashConfigValid(candidate)){
    error="Operator config validation failed";
    return false;
  }

  if(!commonBackupCommitRawFile(
       OPERATOR_CFG_PATH,OPERATOR_CFG_TMP,OPERATOR_CFG_BAK,
       bytes.data(),bytes.size(),error))return false;

  OperatorFlashConfigV2 verify;
  if(!operatorReadFile(OPERATOR_CFG_PATH,verify)){
    SPIFFS.remove(OPERATOR_CFG_PATH);
    if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.rename(OPERATOR_CFG_BAK,OPERATOR_CFG_PATH);
    error="Operator config final verify failed";
    return false;
  }
  if(SPIFFS.exists(OPERATOR_CFG_BAK))SPIFFS.remove(OPERATOR_CFG_BAK);
  return true;
}

static bool commonBackupImportRulesDisabled(
  const String& backup,
  String& error
){
  std::vector<uint8_t> bytes;
  if(!commonBackupExtractFile(backup,RIOT_RULE_FILE_PATH,'C',bytes)){
    error="Backup does not contain rules_config_v1.bin";
    return false;
  }

  RiotRuleFileV2 safe;
  memset(&safe,0,sizeof(safe));

  if(bytes.size()==sizeof(RiotRuleFileV2)){
    RiotRuleFileV2 incoming;
    memcpy(&incoming,bytes.data(),sizeof(incoming));
    if(incoming.magic!=RIOT_RULE_FILE_MAGIC ||
       incoming.version!=2 ||
       incoming.size!=sizeof(incoming) ||
       incoming.checksum!=riotRuleFileChecksumT(incoming)){
      error="Rules V2 validation failed";
      return false;
    }
    safe=incoming;
  }else if(bytes.size()==sizeof(RiotRuleFileV1Legacy)){
    RiotRuleFileV1Legacy incoming;
    memcpy(&incoming,bytes.data(),sizeof(incoming));
    if(incoming.magic!=RIOT_RULE_FILE_MAGIC ||
       incoming.version!=1 ||
       incoming.size!=sizeof(incoming) ||
       incoming.checksum!=riotRuleFileChecksumT(incoming)){
      error="Rules V1 validation failed";
      return false;
    }
    safe.magic=RIOT_RULE_FILE_MAGIC;
    safe.version=2;
    safe.size=sizeof(safe);
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      if(i<RIOT_RULE_LEGACY_COUNT)copyLegacyRuleV1(incoming.rules[i],safe.rules[i]);
      else{
        safe.rules[i]=RiotRuleConfig();
        snprintf(safe.rules[i].name,sizeof(safe.rules[i].name),"Rule %u",(unsigned)(i+1));
      }
    }
  }else{
    error="Rules config size mismatch";
    return false;
  }

  safe.masterEnabled=0;
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++)safe.rules[i].enabled=false;
  safe.checksum=riotRuleFileChecksumT(safe);

  if(!commonBackupCommitRawFile(
       RIOT_RULE_FILE_PATH,RIOT_RULE_FILE_TMP,RIOT_RULE_FILE_BAK,
       (const uint8_t*)&safe,sizeof(safe),error))return false;

  RiotRuleFileV2 verify;
  memset(&verify,0,sizeof(verify));
  if(!readRiotRuleFlashFileV2(RIOT_RULE_FILE_PATH,verify) || verify.masterEnabled!=0){
    SPIFFS.remove(RIOT_RULE_FILE_PATH);
    if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.rename(RIOT_RULE_FILE_BAK,RIOT_RULE_FILE_PATH);
    error="Rules final verify failed";
    return false;
  }
  for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
    if(verify.rules[i].enabled){
      SPIFFS.remove(RIOT_RULE_FILE_PATH);
      if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.rename(RIOT_RULE_FILE_BAK,RIOT_RULE_FILE_PATH);
      error="Rules safety verify found enabled rule";
      return false;
    }
  }
  if(SPIFFS.exists(RIOT_RULE_FILE_BAK))SPIFFS.remove(RIOT_RULE_FILE_BAK);
  return true;
}

CommonBackupValidateResult validateCommonBackupV2(const String& backup){
  CommonBackupValidateResult r;
  if(backup.length()<32 || backup.length()>COMMON_BACKUP_MAX_BYTES){
    r.error="Backup size invalid";
    return r;
  }

  int firstEol=backup.indexOf('\n');
  if(firstEol<0){r.error="Header missing";return r;}
  String header=backup.substring(0,firstEol);
  if(!header.startsWith("MULTIFLOW_BACKUP|")){
    r.error="Backup signature invalid";
    return r;
  }

  int h1=header.indexOf('|');
  int h2=header.indexOf('|',h1+1);
  int h3=header.indexOf('|',h2+1);
  int h4=header.indexOf('|',h3+1);
  if(h1<0||h2<0||h3<0||h4<0){r.error="Header malformed";return r;}

  r.version=(uint16_t)header.substring(h1+1,h2).toInt();
  r.sourceModel=header.substring(h2+1,h3);
  if(r.version!=2){
    r.error="Unsupported backup version";
    return r;
  }

  int checksumStart=backup.lastIndexOf("CHECKSUM|");
  if(checksumStart<0){r.error="Checksum missing";return r;}
  int checksumEnd=backup.indexOf('\n',checksumStart);
  if(checksumEnd<0)checksumEnd=backup.length();
  String expected=backup.substring(checksumStart+9,checksumEnd);
  expected.trim();

  String payload=backup.substring(0,checksumStart);
  uint32_t calc=commonBackupChecksum((const uint8_t*)payload.c_str(),payload.length());
  char actual[9];
  snprintf(actual,sizeof(actual),"%08lX",(unsigned long)calc);
  if(!expected.equalsIgnoreCase(actual)){
    r.error="Checksum mismatch";
    return r;
  }

  int pos=firstEol+1;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();
    if(line.length()){
      if(line.startsWith("R|")){
        int p1=line.indexOf('|',2);
        int p2=p1<0?-1:line.indexOf('|',p1+1);
        int p3=p2<0?-1:line.indexOf('|',p2+1);
        if(p1<0||p2<0||p3<0){r.error="Malformed NVS record";return r;}
        int type=line.substring(2,p1).toInt();
        String nsHex=line.substring(p1+1,p2);
        String keyHex=line.substring(p2+1,p3);
        String valHex=line.substring(p3+1);
        String ns,key;
        std::vector<uint8_t> value;
        if(type<NVS_TYPE_U8 || type>NVS_TYPE_BLOB ||
           !commonBackupDecodeText(nsHex,ns) ||
           !commonBackupDecodeText(keyHex,key) ||
           !commonBackupUnhex(valHex,value) ||
           ns.length()==0 ||
           key.length()==0 || key.length()>15){
          r.error="Invalid NVS record";
          return r;
        }
        r.nvsRecords++;
        if(commonBackupNamespaceAllowed(ns.c_str())){
          commonBackupAppendUniqueCSV(r.supportedNamespaces,ns);
        }else{
          r.unsupportedRecords++;
          commonBackupAppendUniqueCSV(r.unsupportedNamespaces,ns);
        }
      }else if(line.startsWith("F|")){
        if(line.length()<5){r.error="Malformed file record";return r;}
        char scope=line[2];
        int p1=line.indexOf('|',4);
        if(p1<0){r.error="Malformed file record";return r;}
        String pathHex=line.substring(4,p1);
        String dataHex=line.substring(p1+1);
        String path;
        std::vector<uint8_t> value;
        if((scope!='C'&&scope!='H') ||
           !commonBackupDecodeText(pathHex,path) ||
           !commonBackupUnhex(dataHex,value)){
          r.error="Invalid file record";
          return r;
        }
        if(!commonBackupFileAllowed(path,scope)){
          r.unsupportedRecords++;
          r.skippedFiles++;
          commonBackupAppendUniqueCSV(r.unsupportedFiles,path);
        }else{
          r.fileRecords++;
          commonBackupAppendUniqueCSV(r.supportedFiles,path);
        }
      }else if(line.startsWith("M|")){
        int p1=line.indexOf('|',2);
        if(p1<0){r.error="Malformed metadata record";return r;}
        String key=line.substring(2,p1);
        String valHex=line.substring(p1+1);
        std::vector<uint8_t> value;
        if(!key.length() || !commonBackupUnhex(valHex,value)){
          r.error="Invalid metadata record";
          return r;
        }
        r.metadataRecords++;
      }else{
        r.error="Unknown record type";
        return r;
      }
    }
    pos=eol+1;
  }

  if(r.nvsRecords==0 && r.fileRecords==0){
    r.error="Backup contains no restorable configuration";
    return r;
  }

  r.ok=true;
  return r;
}



esp_err_t commonBackupWriteNVSValue(nvs_handle_t h,const char* key,nvs_type_t type,const std::vector<uint8_t>& b){
  if(!key||!*key)return ESP_ERR_INVALID_ARG;
  switch(type){
    case NVS_TYPE_U8:{if(b.size()!=1)return ESP_ERR_INVALID_SIZE;uint8_t v;memcpy(&v,b.data(),1);return nvs_set_u8(h,key,v);}
    case NVS_TYPE_I8:{if(b.size()!=1)return ESP_ERR_INVALID_SIZE;int8_t v;memcpy(&v,b.data(),1);return nvs_set_i8(h,key,v);}
    case NVS_TYPE_U16:{if(b.size()!=2)return ESP_ERR_INVALID_SIZE;uint16_t v;memcpy(&v,b.data(),2);return nvs_set_u16(h,key,v);}
    case NVS_TYPE_I16:{if(b.size()!=2)return ESP_ERR_INVALID_SIZE;int16_t v;memcpy(&v,b.data(),2);return nvs_set_i16(h,key,v);}
    case NVS_TYPE_U32:{if(b.size()!=4)return ESP_ERR_INVALID_SIZE;uint32_t v;memcpy(&v,b.data(),4);return nvs_set_u32(h,key,v);}
    case NVS_TYPE_I32:{if(b.size()!=4)return ESP_ERR_INVALID_SIZE;int32_t v;memcpy(&v,b.data(),4);return nvs_set_i32(h,key,v);}
    case NVS_TYPE_U64:{if(b.size()!=8)return ESP_ERR_INVALID_SIZE;uint64_t v;memcpy(&v,b.data(),8);return nvs_set_u64(h,key,v);}
    case NVS_TYPE_I64:{if(b.size()!=8)return ESP_ERR_INVALID_SIZE;int64_t v;memcpy(&v,b.data(),8);return nvs_set_i64(h,key,v);}
    case NVS_TYPE_STR:{if(b.empty()||b.back()!=0)return ESP_ERR_INVALID_SIZE;return nvs_set_str(h,key,(const char*)b.data());}
    case NVS_TYPE_BLOB:return nvs_set_blob(h,key,b.empty()?nullptr:b.data(),b.size());
    default:return ESP_ERR_INVALID_ARG;
  }
}

struct CommonBackupRestoreResult {
  bool ok=false;
  uint32_t restoredNVS=0;
  uint32_t restoredFiles=0;
  uint32_t skippedNetwork=0;
  uint32_t skippedUnsupported=0;
  uint32_t translatedFlowRuns=0;
  bool hardwareImportRequested=false;
  bool rulesImportRequested=false;
  bool hardwareImported=false;
  bool rulesImported=false;
  bool rulesForcedDisabled=false;
  String error="";
};

struct CommonBackupDISFlowInputBinding {
  uint8_t sourceType;
  uint8_t sourceIndex;
  bool manual;
  float manualValue;
};

struct CommonBackupDISFlowRunBinding {
  CommonBackupDISFlowInputBinding input[3];
};

struct CommonBackupDISFlowStorageV4Prefix {
  uint32_t magic;
  uint16_t version;
  uint16_t runCount;
  uint16_t enableMask;
  uint16_t reserved;
  CommonBackupDISFlowRunBinding runs[FLOW_RUN_COUNT];
};

static constexpr uint32_t COMMON_BACKUP_DIS_FLOW_V4_MAGIC=0x46525334UL;

bool commonBackupExtractDISFlowV4(const String& backup,CommonBackupDISFlowStorageV4Prefix& out){
  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  int pos=backup.indexOf('\n')+1;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();
    if(line.startsWith("R|")){
      int p1=line.indexOf('|',2);
      int p2=p1<0?-1:line.indexOf('|',p1+1);
      int p3=p2<0?-1:line.indexOf('|',p2+1);
      if(p1>=0&&p2>=0&&p3>=0){
        nvs_type_t type=(nvs_type_t)line.substring(2,p1).toInt();
        String nsHex=line.substring(p1+1,p2);
        String keyHex=line.substring(p2+1,p3);
        String valHex=line.substring(p3+1);
        String ns,key;
        std::vector<uint8_t> value;
        if(type==NVS_TYPE_BLOB &&
           commonBackupDecodeText(nsHex,ns) &&
           commonBackupDecodeText(keyHex,key) &&
           ns=="flowruns" && key=="cfgblob" &&
           commonBackupUnhex(valHex,value) &&
           value.size()>=sizeof(out)){
          memcpy(&out,value.data(),sizeof(out));
          return out.magic==COMMON_BACKUP_DIS_FLOW_V4_MAGIC &&
                 out.version==4 &&
                 out.runCount==FLOW_RUN_COUNT;
        }
      }
    }
    pos=eol+1;
  }
  return false;
}

bool commonBackupTranslateDISFlowRuns(const String& backup,CommonBackupRestoreResult& rr){
  CommonBackupDISFlowStorageV4Prefix src={};
  if(!commonBackupExtractDISFlowV4(backup,src))return true;

  uint16_t mask=1U; // Run 1 is always available on TRMNL.
  Preferences p;
  if(!p.begin("flowruns",false)){
    rr.error="Cannot open flowruns for DIS translation";
    return false;
  }

  for(uint8_t run=1;run<FLOW_RUN_COUNT;run++){
    bool compatible=true;
    for(uint8_t role=0;role<3;role++){
      if(src.runs[run].input[role].sourceType!=FLOW_SRC_M31){
        compatible=false;
        break;
      }
    }

    const bool sourceEnabled=(src.enableMask&(1U<<run))!=0;
    if(sourceEnabled && compatible)mask|=(1U<<run);
    if(sourceEnabled && !compatible)rr.skippedUnsupported++;

    if(compatible){
      for(uint8_t role=0;role<3;role++){
        const CommonBackupDISFlowInputBinding& b=src.runs[run].input[role];
        String k="v4r"+String(run)+String(role);
        uint8_t ai=b.sourceIndex<8?b.sourceIndex:role;
        p.putUChar((k+"i").c_str(),ai);
        p.putBool((k+"m").c_str(),b.manual);
        p.putFloat((k+"v").c_str(),b.manualValue);
      }
      rr.translatedFlowRuns++;
    }
  }

  p.putUShort("v4mask",mask);
  p.end();
  return true;
}

bool commonBackupExtractStringRecord(const String& backup,const char* wantedNs,const char* wantedKey,String& out){
  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  int pos=backup.indexOf('\n')+1;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();
    if(line.startsWith("R|")){
      int p1=line.indexOf('|',2);
      int p2=p1<0?-1:line.indexOf('|',p1+1);
      int p3=p2<0?-1:line.indexOf('|',p2+1);
      if(p1>=0&&p2>=0&&p3>=0){
        nvs_type_t type=(nvs_type_t)line.substring(2,p1).toInt();
        String nsHex=line.substring(p1+1,p2);
        String keyHex=line.substring(p2+1,p3);
        String valHex=line.substring(p3+1);
        String ns,key;
        std::vector<uint8_t> value;
        if(type==NVS_TYPE_STR &&
           commonBackupDecodeText(nsHex,ns) &&
           commonBackupDecodeText(keyHex,key) &&
           ns==wantedNs && key==wantedKey &&
           commonBackupUnhex(valHex,value) &&
           !value.empty()){
          size_t n=value.size();
          if(value[n-1]==0)n--;
          out="";
          out.reserve(n);
          for(size_t i=0;i<n;i++)out+=(char)value[i];
          return true;
        }
      }
    }
    pos=eol+1;
  }
  return false;
}

bool commonBackupTranslateDISSiteIdentity(const String& backup){
  String station,lsd;
  const bool haveStation=commonBackupExtractStringRecord(backup,"site","station",station);
  const bool haveLSD=commonBackupExtractStringRecord(backup,"site","lsd",lsd);
  if(!haveStation && !haveLSD)return true;

  if(!haveStation)station="MultiFlow Site";
  if(!haveLSD)lsd="";
  saveSiteIdentity(station,lsd);
  return true;
}

bool commonBackupIsProtectedNetworkNamespace(const String& ns){
  return ns=="riot" || ns=="mqtt";
}

bool commonBackupIsProtectedNetworkFile(const String& path){
  return path=="/mqtt.cfg" || path=="/mqtt.bak";
}



struct CommonBackupPreservedNVSRecord {
  String ns;
  String key;
  nvs_type_t type=NVS_TYPE_ANY;
  std::vector<uint8_t> value;
};

bool commonBackupPreserveNamespace(const char* ns,std::vector<CommonBackupPreservedNVSRecord>& out,String& error){
  if(!ns||!*ns)return true;
#if ESP_IDF_VERSION_MAJOR >= 5
  nvs_iterator_t it=nullptr;
  esp_err_t iterResult=nvs_entry_find("nvs",ns,NVS_TYPE_ANY,&it);
  while(iterResult==ESP_OK && it!=nullptr){
    nvs_entry_info_t info={};
    if(nvs_entry_info(it,&info)==ESP_OK){
      nvs_handle_t h=0;
      if(nvs_open(ns,NVS_READONLY,&h)==ESP_OK){
        std::vector<uint8_t> value;
        if(commonBackupReadNVSValue(h,info,value)){
          CommonBackupPreservedNVSRecord r;
          r.ns=ns;r.key=info.key;r.type=info.type;r.value=value;
          out.push_back(r);
        }
        nvs_close(h);
      }
    }
    iterResult=nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
#else
  nvs_iterator_t it=nvs_entry_find("nvs",ns,NVS_TYPE_ANY);
  while(it!=nullptr){
    nvs_entry_info_t info={};
    nvs_entry_info(it,&info);
    nvs_handle_t h=0;
    if(nvs_open(ns,NVS_READONLY,&h)==ESP_OK){
      std::vector<uint8_t> value;
      if(commonBackupReadNVSValue(h,info,value)){
        CommonBackupPreservedNVSRecord r;
        r.ns=ns;r.key=info.key;r.type=info.type;r.value=value;
        out.push_back(r);
      }
      nvs_close(h);
    }
    it=nvs_entry_next(it);
  }
#endif
  return true;
}

bool commonBackupRestorePreservedNVS(const std::vector<CommonBackupPreservedNVSRecord>& records,String& error){
  String activeNs="";
  nvs_handle_t h=0;
  for(size_t i=0;i<records.size();i++){
    const CommonBackupPreservedNVSRecord& r=records[i];
    if(r.ns!=activeNs){
      if(h){
        esp_err_t ce=nvs_commit(h);
        nvs_close(h);h=0;
        if(ce!=ESP_OK){error="Preserved NVS commit failed: "+activeNs+" err="+String((int)ce);return false;}
      }
      esp_err_t oe=nvs_open(r.ns.c_str(),NVS_READWRITE,&h);
      if(oe!=ESP_OK){error="Preserved NVS open failed: "+r.ns+" err="+String((int)oe);return false;}
      activeNs=r.ns;
    }
    esp_err_t we=commonBackupWriteNVSValue(h,r.key.c_str(),r.type,r.value);
    if(we!=ESP_OK){
      nvs_close(h);
      error="Preserved NVS write failed: "+r.ns+"/"+r.key+" err="+String((int)we);
      return false;
    }
  }
  if(h){
    esp_err_t ce=nvs_commit(h);
    nvs_close(h);
    if(ce!=ESP_OK){error="Preserved NVS commit failed: "+activeNs+" err="+String((int)ce);return false;}
  }
  return true;
}

struct NVSCompactResult {
  bool ok=false;
  size_t preservedRecords=0;
  NVSStatsSnapshot before;
  NVSStatsSnapshot after;
  String error="";
};

bool preserveAllNVSRecords(std::vector<CommonBackupPreservedNVSRecord>& out,String& error){
  out.clear();
#if ESP_IDF_VERSION_MAJOR >= 5
  nvs_iterator_t it=nullptr;
  esp_err_t iterResult=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY,&it);
  while(iterResult==ESP_OK && it!=nullptr){
    nvs_entry_info_t info={};
    if(nvs_entry_info(it,&info)==ESP_OK){
      nvs_handle_t h=0;
      esp_err_t oe=nvs_open(info.namespace_name,NVS_READONLY,&h);
      if(oe!=ESP_OK){nvs_release_iterator(it);error="NVS preserve open failed "+String(info.namespace_name)+" err="+String((int)oe);return false;}
      std::vector<uint8_t> value;
      bool readOK=commonBackupReadNVSValue(h,info,value);
      nvs_close(h);
      if(!readOK){nvs_release_iterator(it);error="NVS preserve read failed "+String(info.namespace_name)+"/"+String(info.key);return false;}
      CommonBackupPreservedNVSRecord r;
      r.ns=info.namespace_name;r.key=info.key;r.type=info.type;r.value=value;
      out.push_back(r);
    }
    iterResult=nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
#else
  nvs_iterator_t it=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY);
  while(it!=nullptr){
    nvs_entry_info_t info={};nvs_entry_info(it,&info);
    nvs_handle_t h=0;
    esp_err_t oe=nvs_open(info.namespace_name,NVS_READONLY,&h);
    if(oe!=ESP_OK){error="NVS preserve open failed "+String(info.namespace_name)+" err="+String((int)oe);return false;}
    std::vector<uint8_t> value;
    bool readOK=commonBackupReadNVSValue(h,info,value);
    nvs_close(h);
    if(!readOK){error="NVS preserve read failed "+String(info.namespace_name)+"/"+String(info.key);return false;}
    CommonBackupPreservedNVSRecord r;
    r.ns=info.namespace_name;r.key=info.key;r.type=info.type;r.value=value;
    out.push_back(r);
    it=nvs_entry_next(it);
  }
#endif
  if(out.empty()){error="No NVS records found to preserve";return false;}
  return true;
}

bool verifyPreservedNVSRecords(const std::vector<CommonBackupPreservedNVSRecord>& records,String& error){
  for(const auto& r:records){
    nvs_handle_t h=0;
    esp_err_t oe=nvs_open(r.ns.c_str(),NVS_READONLY,&h);
    if(oe!=ESP_OK){error="NVS verify open failed "+r.ns+" err="+String((int)oe);return false;}
    nvs_entry_info_t info={};
    snprintf(info.namespace_name,sizeof(info.namespace_name),"%s",r.ns.c_str());
    snprintf(info.key,sizeof(info.key),"%s",r.key.c_str());
    info.type=r.type;
    std::vector<uint8_t> current;
    bool ok=commonBackupReadNVSValue(h,info,current);
    nvs_close(h);
    if(!ok || current!=r.value){error="NVS verify mismatch "+r.ns+"/"+r.key;return false;}
  }
  return true;
}

NVSCompactResult compactNVSNow(){
  NVSCompactResult result;
  result.before=getNVSStatsSnapshot();
  Serial.println("NVS COMPACT BEGIN");
  printNVSStats("before compact");

  CommonBackupBuildResult recovery=buildCommonBackupV2();
  if(!recovery.ok){result.error="Recovery backup build failed: "+recovery.error;return result;}
  CommonBackupValidateResult recoveryCheck=validateCommonBackupV2(recovery.data);
  if(!recoveryCheck.ok){result.error="Recovery backup validate failed: "+recoveryCheck.error;return result;}

  static constexpr const char* RECOVERY_PATH="/nvs_compact_recovery.bak";
  SPIFFS.remove(RECOVERY_PATH);
  File recoveryFile=SPIFFS.open(RECOVERY_PATH,FILE_WRITE);
  if(!recoveryFile){result.error="Cannot create NVS recovery backup";return result;}
  const size_t recoveryBytes=recoveryFile.write((const uint8_t*)recovery.data.c_str(),recovery.data.length());
  recoveryFile.flush();recoveryFile.close();
  if(recoveryBytes!=recovery.data.length()){result.error="Short write creating NVS recovery backup";return result;}
  File recoveryVerify=SPIFFS.open(RECOVERY_PATH,FILE_READ);
  if(!recoveryVerify || recoveryVerify.size()!=recovery.data.length()){
    if(recoveryVerify)recoveryVerify.close();
    result.error="NVS recovery backup verify failed";return result;
  }
  recoveryVerify.close();
  Serial.printf("NVS COMPACT recovery backup OK bytes=%u path=%s\n",(unsigned)recoveryBytes,RECOVERY_PATH);

  std::vector<CommonBackupPreservedNVSRecord> preserved;
  String preserveError;
  if(!preserveAllNVSRecords(preserved,preserveError)){result.error=preserveError;return result;}
  result.preservedRecords=preserved.size();
  Serial.printf("NVS COMPACT preserved records=%u\n",(unsigned)result.preservedRecords);

  esp_err_t deinitErr=nvs_flash_deinit();
  if(deinitErr!=ESP_OK && deinitErr!=ESP_ERR_NVS_NOT_INITIALIZED){result.error="NVS deinit failed err="+String((int)deinitErr);return result;}
  esp_err_t eraseErr=nvs_flash_erase();
  if(eraseErr!=ESP_OK){nvs_flash_init();result.error="NVS erase failed err="+String((int)eraseErr);return result;}
  esp_err_t initErr=nvs_flash_init();
  if(initErr!=ESP_OK){result.error="NVS reinit failed err="+String((int)initErr);return result;}

  if(!commonBackupRestorePreservedNVS(preserved,preserveError)){result.error=preserveError;return result;}
  if(!verifyPreservedNVSRecords(preserved,preserveError)){result.error=preserveError;return result;}

  result.after=getNVSStatsSnapshot();
  printNVSStats("after compact");
  result.ok=true;
  Serial.printf("NVS COMPACT PASS records=%u free=%u->%u\n",
    (unsigned)result.preservedRecords,
    (unsigned)(result.before.valid?result.before.free:0),
    (unsigned)(result.after.valid?result.after.free:0));
  return result;
}

CommonBackupRestoreResult restoreCommonBackupKeepCurrent(const String& backup,bool importHardware=false,bool importRules=false){
  CommonBackupRestoreResult rr;
  rr.hardwareImportRequested=importHardware;
  rr.rulesImportRequested=importRules;
  CommonBackupValidateResult v=validateCommonBackupV2(backup);
  if(!v.ok){rr.error=v.error;return rr;}
  if(importHardware && !commonBackupValidateOperatorImport(backup,rr.error))return rr;
  if(importRules && !commonBackupValidateRulesImport(backup,rr.error))return rr;

  std::vector<CommonBackupPreservedNVSRecord> preserved;
  String preserveError;
  const char* preserveNamespaces[]={"riot","flowtot","mqttday"};
  for(size_t i=0;i<sizeof(preserveNamespaces)/sizeof(preserveNamespaces[0]);i++){
    if(!commonBackupPreserveNamespace(preserveNamespaces[i],preserved,preserveError)){
      rr.error=preserveError;
      return rr;
    }
  }

  esp_err_t deinitErr=nvs_flash_deinit();
  if(deinitErr!=ESP_OK && deinitErr!=ESP_ERR_NVS_NOT_INITIALIZED){
    rr.error="NVS deinit failed err="+String((int)deinitErr);
    return rr;
  }
  esp_err_t erasePartitionErr=nvs_flash_erase();
  if(erasePartitionErr!=ESP_OK){
    rr.error="NVS partition erase failed err="+String((int)erasePartitionErr);
    nvs_flash_init();
    return rr;
  }
  esp_err_t initErr=nvs_flash_init();
  if(initErr!=ESP_OK){
    rr.error="NVS reinit failed err="+String((int)initErr);
    return rr;
  }

  if(!commonBackupRestorePreservedNVS(preserved,preserveError)){
    rr.error=preserveError;
    return rr;
  }

  printNVSStats("after compact + preserved state");

  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  const int firstRecord=backup.indexOf('\n')+1;

  // Restore one namespace at a time and commit once per namespace.
  // This avoids excessive NVS churn/fragmentation from committing every key.
  for(size_t i=0;i<sizeof(COMMON_BACKUP_NVS_NAMESPACES)/sizeof(COMMON_BACKUP_NVS_NAMESPACES[0]);i++){
    String targetNs=COMMON_BACKUP_NVS_NAMESPACES[i];
    if(commonBackupIsProtectedNetworkNamespace(targetNs))continue;

    nvs_handle_t h=0;
    esp_err_t openErr=nvs_open(targetNs.c_str(),NVS_READWRITE,&h);
    if(openErr!=ESP_OK){
      rr.error="Cannot open namespace "+targetNs+" err="+String((int)openErr);
      return rr;
    }

    uint32_t namespaceWrites=0;
    int pos=firstRecord;
    while(pos>0 && pos<checksumStart){
      int eol=backup.indexOf('\n',pos);
      if(eol<0 || eol>checksumStart)eol=checksumStart;
      String line=backup.substring(pos,eol);
      line.trim();

      if(line.startsWith("R|")){
        int p1=line.indexOf('|',2);
        int p2=line.indexOf('|',p1+1);
        int p3=line.indexOf('|',p2+1);
        nvs_type_t type=(nvs_type_t)line.substring(2,p1).toInt();
        String nsHex=line.substring(p1+1,p2);
        String keyHex=line.substring(p2+1,p3);
        String valHex=line.substring(p3+1);
        String ns,key;
        std::vector<uint8_t> value;

        if(!commonBackupDecodeText(nsHex,ns) ||
           !commonBackupDecodeText(keyHex,key) ||
           !commonBackupUnhex(valHex,value)){
          nvs_close(h);
          rr.error="Decode failed during NVS restore";
          return rr;
        }

        if(ns==targetNs){
          esp_err_t writeErr=commonBackupWriteNVSValue(h,key.c_str(),type,value);
          if(writeErr!=ESP_OK){
            nvs_close(h);
            rr.error="NVS write failed: "+ns+"/"+key+
                     " type="+String((int)type)+
                     " bytes="+String(value.size())+
                     " err="+String((int)writeErr);
            return rr;
          }
          namespaceWrites++;
        }
      }

      pos=eol+1;
    }

    esp_err_t commitErr=nvs_commit(h);
    nvs_close(h);
    if(commitErr!=ESP_OK){
      rr.error="NVS commit failed: "+targetNs+" err="+String((int)commitErr);
      return rr;
    }
    rr.restoredNVS+=namespaceWrites;
  }

  // Count protected network NVS records as intentionally skipped.
  int pos=firstRecord;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();

    if(line.startsWith("R|")){
      int p1=line.indexOf('|',2);
      int p2=line.indexOf('|',p1+1);
      int p3=line.indexOf('|',p2+1);
      String nsHex=line.substring(p1+1,p2);
      String ns;
      if(commonBackupDecodeText(nsHex,ns) && commonBackupIsProtectedNetworkNamespace(ns)){
        rr.skippedNetwork++;
      }
    }else if(line.startsWith("F|")){
      char scope=line[2];
      int p1=line.indexOf('|',4);
      String pathHex=line.substring(4,p1);
      String dataHex=line.substring(p1+1);
      String path;
      std::vector<uint8_t> value;
      if(!commonBackupDecodeText(pathHex,path) || !commonBackupUnhex(dataHex,value)){
        rr.error="Decode failed during file restore";
        return rr;
      }

      if(commonBackupIsProtectedNetworkFile(path)){
        rr.skippedNetwork++;
      }else if(commonBackupReviewOnlyFile(path)){
        rr.skippedUnsupported++;
      }else if(!commonBackupRestoreFileAllowed(path,scope)){
        rr.skippedUnsupported++;
      }else{
        String tmp=path+".restoretmp";
        SPIFFS.remove(tmp);
        File out=SPIFFS.open(tmp,FILE_WRITE);
        if(!out){rr.error="Cannot stage file "+path;return rr;}
        size_t n=value.empty()?0:out.write(value.data(),value.size());
        out.flush();
        out.close();
        if(n!=value.size()){
          SPIFFS.remove(tmp);
          rr.error="Short write staging "+path;
          return rr;
        }

        SPIFFS.remove(path);
        if(!SPIFFS.rename(tmp,path)){
          SPIFFS.remove(tmp);
          rr.error="Cannot commit file "+path;
          return rr;
        }

        File verify=SPIFFS.open(path,FILE_READ);
        if(!verify || verify.size()!=value.size()){
          if(verify)verify.close();
          rr.error="File verify failed "+path;
          return rr;
        }
        verify.close();
        rr.restoredFiles++;
      }
    }

    pos=eol+1;
  }

  if(v.sourceModel=="DIS0705H"){
    if(!commonBackupTranslateDISFlowRuns(backup,rr))return rr;
    if(!commonBackupTranslateDISSiteIdentity(backup)){rr.error="DIS site identity translation failed";return rr;}
  }

  if(importHardware){
    if(!commonBackupImportOperatorConfig(backup,rr.error))return rr;
    rr.hardwareImported=true;
  }
  if(importRules){
    if(!commonBackupImportRulesDisabled(backup,rr.error))return rr;
    rr.rulesImported=true;
    rr.rulesForcedDisabled=true;
  }

  printNVSStats("after restore writes");
  rr.ok=true;
  return rr;
}


CommonBackupRestoreResult restoreCommonBackupUseBackup(const String& backup,bool importHardware=false,bool importRules=false){
  CommonBackupRestoreResult rr;
  rr.hardwareImportRequested=importHardware;
  rr.rulesImportRequested=importRules;
  CommonBackupValidateResult v=validateCommonBackupV2(backup);
  if(!v.ok){rr.error=v.error;return rr;}
  if(importHardware && !commonBackupValidateOperatorImport(backup,rr.error))return rr;
  if(importRules && !commonBackupValidateRulesImport(backup,rr.error))return rr;

  std::vector<CommonBackupPreservedNVSRecord> preserved;
  String preserveError;
  const char* preserveNamespaces[]={"flowtot","mqttday"};
  for(size_t i=0;i<sizeof(preserveNamespaces)/sizeof(preserveNamespaces[0]);i++){
    if(!commonBackupPreserveNamespace(preserveNamespaces[i],preserved,preserveError)){
      rr.error=preserveError;
      return rr;
    }
  }

  esp_err_t deinitErr=nvs_flash_deinit();
  if(deinitErr!=ESP_OK && deinitErr!=ESP_ERR_NVS_NOT_INITIALIZED){
    rr.error="NVS deinit failed err="+String((int)deinitErr);
    return rr;
  }
  esp_err_t erasePartitionErr=nvs_flash_erase();
  if(erasePartitionErr!=ESP_OK){
    rr.error="NVS partition erase failed err="+String((int)erasePartitionErr);
    nvs_flash_init();
    return rr;
  }
  esp_err_t initErr=nvs_flash_init();
  if(initErr!=ESP_OK){
    rr.error="NVS reinit failed err="+String((int)initErr);
    return rr;
  }

  if(!commonBackupRestorePreservedNVS(preserved,preserveError)){
    rr.error=preserveError;
    return rr;
  }

  printNVSStats("after compact + preserved runtime state");

  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  const int firstRecord=backup.indexOf('\n')+1;

  // Restore one namespace at a time and commit once per namespace.
  // This avoids excessive NVS churn/fragmentation from committing every key.
  for(size_t i=0;i<sizeof(COMMON_BACKUP_NVS_NAMESPACES)/sizeof(COMMON_BACKUP_NVS_NAMESPACES[0]);i++){
    String targetNs=COMMON_BACKUP_NVS_NAMESPACES[i];

    nvs_handle_t h=0;
    esp_err_t openErr=nvs_open(targetNs.c_str(),NVS_READWRITE,&h);
    if(openErr!=ESP_OK){
      rr.error="Cannot open namespace "+targetNs+" err="+String((int)openErr);
      return rr;
    }

    uint32_t namespaceWrites=0;
    int pos=firstRecord;
    while(pos>0 && pos<checksumStart){
      int eol=backup.indexOf('\n',pos);
      if(eol<0 || eol>checksumStart)eol=checksumStart;
      String line=backup.substring(pos,eol);
      line.trim();

      if(line.startsWith("R|")){
        int p1=line.indexOf('|',2);
        int p2=line.indexOf('|',p1+1);
        int p3=line.indexOf('|',p2+1);
        nvs_type_t type=(nvs_type_t)line.substring(2,p1).toInt();
        String nsHex=line.substring(p1+1,p2);
        String keyHex=line.substring(p2+1,p3);
        String valHex=line.substring(p3+1);
        String ns,key;
        std::vector<uint8_t> value;

        if(!commonBackupDecodeText(nsHex,ns) ||
           !commonBackupDecodeText(keyHex,key) ||
           !commonBackupUnhex(valHex,value)){
          nvs_close(h);
          rr.error="Decode failed during NVS restore";
          return rr;
        }

        if(ns==targetNs){
          esp_err_t writeErr=commonBackupWriteNVSValue(h,key.c_str(),type,value);
          if(writeErr!=ESP_OK){
            nvs_close(h);
            rr.error="NVS write failed: "+ns+"/"+key+
                     " type="+String((int)type)+
                     " bytes="+String(value.size())+
                     " err="+String((int)writeErr);
            return rr;
          }
          namespaceWrites++;
        }
      }

      pos=eol+1;
    }

    esp_err_t commitErr=nvs_commit(h);
    nvs_close(h);
    if(commitErr!=ESP_OK){
      rr.error="NVS commit failed: "+targetNs+" err="+String((int)commitErr);
      return rr;
    }
    rr.restoredNVS+=namespaceWrites;
  }

  // Restore selected SPIFFS files, including the backup MQTT configuration.
  int pos=firstRecord;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();

    if(line.startsWith("R|")){
      int p1=line.indexOf('|',2);
      int p2=line.indexOf('|',p1+1);
      int p3=line.indexOf('|',p2+1);
      String nsHex=line.substring(p1+1,p2);
      String ns;
      if(commonBackupDecodeText(nsHex,ns) && !commonBackupNamespaceAllowed(ns.c_str())){
        rr.skippedUnsupported++;
      }
    }else if(line.startsWith("F|")){
      char scope=line[2];
      int p1=line.indexOf('|',4);
      String pathHex=line.substring(4,p1);
      String dataHex=line.substring(p1+1);
      String path;
      std::vector<uint8_t> value;
      if(!commonBackupDecodeText(pathHex,path) || !commonBackupUnhex(dataHex,value)){
        rr.error="Decode failed during file restore";
        return rr;
      }

      if(commonBackupReviewOnlyFile(path)){
        rr.skippedUnsupported++;
      }else if(!commonBackupRestoreFileAllowed(path,scope)){
        rr.skippedUnsupported++;
      }else{
        String tmp=path+".restoretmp";
        SPIFFS.remove(tmp);
        File out=SPIFFS.open(tmp,FILE_WRITE);
        if(!out){rr.error="Cannot stage file "+path;return rr;}
        size_t n=value.empty()?0:out.write(value.data(),value.size());
        out.flush();
        out.close();
        if(n!=value.size()){
          SPIFFS.remove(tmp);
          rr.error="Short write staging "+path;
          return rr;
        }

        SPIFFS.remove(path);
        if(!SPIFFS.rename(tmp,path)){
          SPIFFS.remove(tmp);
          rr.error="Cannot commit file "+path;
          return rr;
        }

        File verify=SPIFFS.open(path,FILE_READ);
        if(!verify || verify.size()!=value.size()){
          if(verify)verify.close();
          rr.error="File verify failed "+path;
          return rr;
        }
        verify.close();
        rr.restoredFiles++;
      }
    }

    pos=eol+1;
  }

  if(v.sourceModel=="DIS0705H"){
    if(!commonBackupTranslateDISFlowRuns(backup,rr))return rr;
    if(!commonBackupTranslateDISSiteIdentity(backup)){rr.error="DIS site identity translation failed";return rr;}
  }

  if(importHardware){
    if(!commonBackupImportOperatorConfig(backup,rr.error))return rr;
    rr.hardwareImported=true;
  }
  if(importRules){
    if(!commonBackupImportRulesDisabled(backup,rr.error))return rr;
    rr.rulesImported=true;
    rr.rulesForcedDisabled=true;
  }

  printNVSStats("after restore writes");
  rr.ok=true;
  return rr;
}

CommonBackupRestoreResult restoreCommonBackupUseNew(
  const String& backup,
  const String& newWifiSsid,
  const String& newWifiPass,
  const MQTTConfig& newMqttCfg,
  bool importHardware=false,
  bool importRules=false
){
  CommonBackupRestoreResult rr;
  String ssid=newWifiSsid;
  ssid.trim();
  String mqttHost=newMqttCfg.host;
  mqttHost.trim();

  // Validate all operator-supplied network data before the restore engine
  // performs any erase/write work.
  if(!ssid.length()){
    rr.error="New Wi-Fi SSID is required";
    return rr;
  }
  if(!mqttHost.length()){
    rr.error="New MQTT broker host is required";
    return rr;
  }
  if(newMqttCfg.port<1){
    rr.error="New MQTT broker port is invalid";
    return rr;
  }

  // Reuse the already bench-validated KEEP CURRENT restore transaction:
  // backup network records are skipped while runtime-only state is preserved.
  // Only after that succeeds do we replace the target network settings.
  rr=restoreCommonBackupKeepCurrent(backup,importHardware,importRules);
  if(!rr.ok)return rr;

  saveNetworkConfig(ssid,newWifiPass);

  mqttCfg=newMqttCfg;
  mqttCfg.host=mqttHost;
  if(!mqttCfg.baseTopic.length())mqttCfg.baseTopic="multiflow/controller";
  if(!saveMQTTConfig()){
    rr.ok=false;
    rr.error="New MQTT settings could not be persisted";
    return rr;
  }

  rr.ok=true;
  return rr;
}


// RIOT RESTORE CLASSIFICATION DRY-RUN GATE A5A V1
// RIOT RESTORE CLASSIFICATION RUNTIME POLISH A5A1 V1
// Read-only commissioning classification. This intentionally does not alter
// any restore path. Its purpose is to tell the operator what a selected V2
// backup actually contains and how later commissioning gates should treat it.

String commonBackupValidateJson(const CommonBackupValidateResult& r);

struct CommonBackupClassificationResult {
  uint32_t portableRecords=0;
  uint32_t optionalNetworkRecords=0;
  uint32_t hardwareReviewRecords=0;
  uint32_t reviewRequiredRecords=0;
  uint32_t unsupportedRecords=0;
  bool operatorConfigPresent=false;
  bool rulesPresent=false;
  bool siteIdentityPresent=false;
  bool controlConfigPresent=false;
  bool mqttPresent=false;
  bool wifiPresent=false;
  String sourceId="";
};

static String commonBackupJsonEscape(String s){
  s.replace("\\","\\\\");
  s.replace("\"","\\\"");
  s.replace("\r","\\r");
  s.replace("\n","\\n");
  return s;
}

static bool commonBackupHeaderSourceId(const String& backup,String& sourceId){
  int eol=backup.indexOf('\n');
  if(eol<0)return false;
  String h=backup.substring(0,eol);
  int p1=h.indexOf('|');
  int p2=p1<0?-1:h.indexOf('|',p1+1);
  int p3=p2<0?-1:h.indexOf('|',p2+1);
  int p4=p3<0?-1:h.indexOf('|',p3+1);
  if(p1<0||p2<0||p3<0||p4<0)return false;
  sourceId=h.substring(p3+1,p4);
  return sourceId.length()>0;
}

static CommonBackupClassificationResult commonBackupClassify(const String& backup){
  CommonBackupClassificationResult c;
  commonBackupHeaderSourceId(backup,c.sourceId);

  const int checksumStart=backup.lastIndexOf("CHECKSUM|");
  int pos=backup.indexOf('\n')+1;
  while(pos>0 && pos<checksumStart){
    int eol=backup.indexOf('\n',pos);
    if(eol<0 || eol>checksumStart)eol=checksumStart;
    String line=backup.substring(pos,eol);
    line.trim();

    if(line.startsWith("R|")){
      int p1=line.indexOf('|',2);
      int p2=p1<0?-1:line.indexOf('|',p1+1);
      int p3=p2<0?-1:line.indexOf('|',p2+1);
      String nsHex=p2>=0?line.substring(p1+1,p2):String();
      String keyHex=p3>=0?line.substring(p2+1,p3):String();
      String ns,key;
      if(p1<0||p2<0||p3<0 ||
         !commonBackupDecodeText(nsHex,ns) ||
         !commonBackupDecodeText(keyHex,key)){
        c.unsupportedRecords++;
      }else if(ns=="riot"){
        c.optionalNetworkRecords++;
        c.wifiPresent=true;
      }else if(ns=="display"){
        c.hardwareReviewRecords++;
      }else if(ns=="rules" || ns=="rulecfg"){
        c.reviewRequiredRecords++;
        c.rulesPresent=true;
      }else if(ns=="operator" || ns=="modbus"){
        c.hardwareReviewRecords++;
        c.operatorConfigPresent=true;
      }else if(ns=="aga" || ns=="agagas" || ns=="agaruns" ||
               ns=="analog" || ns=="flowmeta" || ns=="flowruns" ||
               ns=="gaslib"){
        c.portableRecords++;
      }else{
        c.unsupportedRecords++;
      }
    }else if(line.startsWith("F|")){
      int p1=line.indexOf('|',4);
      String pathHex=p1>=0?line.substring(4,p1):String();
      String path;
      if(p1<0 || !commonBackupDecodeText(pathHex,path)){
        c.unsupportedRecords++;
      }else if(path=="/mqtt.cfg" || path=="/mqtt.bak"){
        c.optionalNetworkRecords++;
        c.mqttPresent=true;
      }else if(path=="/operator_config_v2.bin" || path=="/operator_config_v1.bin"){
        c.hardwareReviewRecords++;
        c.operatorConfigPresent=true;
      }else if(path=="/rules_config_v1.bin" || path=="/rules_config_v2.bin"){
        c.reviewRequiredRecords++;
        c.rulesPresent=true;
      }else if(path=="/display_logo.bin"){
        c.hardwareReviewRecords++;
      }else if(path=="/site_identity.cfg"){
        c.portableRecords++;
        c.siteIdentityPresent=true;
      }else if(path=="/control.cfg"){
        c.portableRecords++;
        c.controlConfigPresent=true;
      }else{
        c.unsupportedRecords++;
      }
    }

    pos=eol+1;
  }
  return c;
}

static String commonBackupClassificationJson(
  const String& backup,
  const CommonBackupValidateResult& v
){
  String j=commonBackupValidateJson(v);
  if(!j.endsWith("}"))return j;
  j.remove(j.length()-1);

  CommonBackupClassificationResult c=commonBackupClassify(backup);
  const String currentId=riotDeviceId();
  const String currentSourceId=commonBackupSourceId();
  const bool sameDevice=c.sourceId.length() && c.sourceId==currentSourceId;

  j+=",\"gate\":\"A5a\"";
  j+=",\"classificationOnly\":true";
  j+=",\"sourceId\":\""+commonBackupJsonEscape(c.sourceId)+"\"";
  j+=",\"currentSourceId\":\""+commonBackupJsonEscape(currentSourceId)+"\"";
  j+=",\"currentDeviceId\":\""+commonBackupJsonEscape(currentId)+"\"";
  j+=",\"sameDeviceHint\":"+String(sameDevice?"true":"false");

  j+=",\"recordClasses\":{";
  j+="\"portable\":"+String(c.portableRecords);
  j+=",\"optionalNetwork\":"+String(c.optionalNetworkRecords);
  j+=",\"hardwareReview\":"+String(c.hardwareReviewRecords);
  j+=",\"reviewRequired\":"+String(c.reviewRequiredRecords);
  j+=",\"unsupported\":"+String(c.unsupportedRecords);
  j+="}";

  j+=",\"sections\":{";
  j+="\"deviceIdentity\":{";
  j+="\"policy\":\"LOCAL_ONLY\"";
  j+=",\"presentInBackup\":false";
  j+=",\"action\":\"KEEP_CURRENT_DEVICE_ID_MAC_AND_FACTORY_AP\"";
  j+="}";

  j+=",\"hardwareProfile\":{";
  j+="\"policy\":\"PRESERVE_BY_DEFAULT\"";
  j+=",\"presentInBackup\":"+String(c.operatorConfigPresent?"true":"false");
  j+=",\"action\":\"REQUIRE_EXPLICIT_REPLACE_IN_FUTURE_GATE\"";
  j+="}";

  j+=",\"processApplication\":{";
  j+="\"policy\":\"PORTABLE\"";
  j+=",\"records\":"+String(c.portableRecords);
  j+=",\"siteIdentityPresent\":"+String(c.siteIdentityPresent?"true":"false");
  j+=",\"controlConfigPresent\":"+String(c.controlConfigPresent?"true":"false");
  j+=",\"action\":\"ELIGIBLE_FOR_IMPORT\"";
  j+="}";

  j+=",\"rulesAlarms\":{";
  j+="\"policy\":\"REVIEW_REQUIRED\"";
  j+=",\"presentInBackup\":"+String(c.rulesPresent?"true":"false");
  j+=",\"action\":\"IMPORT_DISABLED_UNTIL_REVIEWED\"";
  j+="}";

  j+=",\"network\":{";
  j+="\"policy\":\"OPTIONAL_EXPLICIT\"";
  j+=",\"wifiPresent\":"+String(c.wifiPresent?"true":"false");
  j+=",\"mqttPresent\":"+String(c.mqttPresent?"true":"false");
  j+=",\"action\":\"KEEP_CURRENT_UNLESS_OPERATOR_SELECTS_BACKUP_OR_NEW\"";
  j+="}";

  j+=",\"displayHardware\":{";
  j+="\"policy\":\"REVIEW_ON_TARGET_HARDWARE\"";
  j+=",\"records\":"+String(c.hardwareReviewRecords);
  j+=",\"action\":\"DO_NOT_ASSUME_PORTABLE_ACROSS_DISPLAY_TYPES\"";
  j+="}";
  j+="}";

  j+=",\"formatGaps\":{";
  j+="\"operatorConfigV2Absent\":"+String(c.operatorConfigPresent?"false":"true");
  j+=",\"rulesConfigAbsent\":"+String(c.rulesPresent?"false":"true");
  j+=",\"note\":\"Current V2 backup only classifies records actually present; A5a performs no restore changes.\"";
  j+="}";

  j+=",\"commissioningRequired\":";
  j+=(c.rulesPresent || c.operatorConfigPresent || !sameDevice)?"true":"false";
  j+="}";
  return j;
}

String commonBackupRestoreJson(const CommonBackupRestoreResult& r,const char* mode){
  String j="{\"ok\":"+String(r.ok?"true":"false");
  j+=",\"mode\":\""+String(mode?mode:"UNKNOWN")+"\"";
  j+=",\"restoredNVS\":"+String(r.restoredNVS);
  j+=",\"restoredFiles\":"+String(r.restoredFiles);
  j+=",\"skippedNetwork\":"+String(r.skippedNetwork);
  j+=",\"skippedUnsupported\":"+String(r.skippedUnsupported);
  j+=",\"translatedFlowRuns\":"+String(r.translatedFlowRuns);
  j+=",\"hardwareImportRequested\":"+String(r.hardwareImportRequested?"true":"false");
  j+=",\"rulesImportRequested\":"+String(r.rulesImportRequested?"true":"false");
  j+=",\"hardwareImported\":"+String(r.hardwareImported?"true":"false");
  j+=",\"rulesImported\":"+String(r.rulesImported?"true":"false");
  j+=",\"rulesForcedDisabled\":"+String(r.rulesForcedDisabled?"true":"false");
  if(!r.ok)j+=",\"error\":\""+r.error+"\"";
  j+="}";
  return j;
}

String commonBackupRestoreUploadBuffer;
bool commonBackupRestoreTooLarge=false;
size_t commonBackupRestoreReceived=0;
uint32_t commonBackupRebootAtMs=0;

String commonBackupValidateJson(const CommonBackupValidateResult& r){
  String j="{\"ok\":"+String(r.ok?"true":"false");
  j+=",\"version\":"+String(r.version);
  j+=",\"sourceModel\":\""+r.sourceModel+"\"";
  j+=",\"nvsRecords\":"+String(r.nvsRecords);
  j+=",\"fileRecords\":"+String(r.fileRecords);
  j+=",\"metadataRecords\":"+String(r.metadataRecords);
  j+=",\"unsupportedRecords\":"+String(r.unsupportedRecords);
  j+=",\"skippedFiles\":"+String(r.skippedFiles);
  j+=",\"supportedNamespaces\":\""+r.supportedNamespaces+"\"";
  j+=",\"unsupportedNamespaces\":\""+r.unsupportedNamespaces+"\"";
  j+=",\"supportedFiles\":\""+r.supportedFiles+"\"";
  j+=",\"unsupportedFiles\":\""+r.unsupportedFiles+"\"";
  j+=",\"writeActions\":0";
  j+=",\"eraseActions\":0";
  if(!r.ok)j+=",\"error\":\""+r.error+"\"";
  j+="}";
  return j;
}

String commonBackupDryRunUploadBuffer;
bool commonBackupDryRunTooLarge=false;
size_t commonBackupDryRunReceived=0;

String activeIpString(){
  if(WiFi.status()==WL_CONNECTED)return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();
}

String powerOverrideName(){
  if(powerGoodOverride==POWER_OVERRIDE_FORCE_ON)return "FORCE_ON";
  if(powerGoodOverride==POWER_OVERRIDE_FORCE_OFF)return "FORCE_OFF";
  return "AUTO";
}

String powerEventTimestamp(){
  time_t t=time(nullptr);
  if(t>1700000000){
    struct tm tmv;
    localtime_r(&t,&tmv);
    char b[32];
    strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%S%z",&tmv);
    return String(b);
  }
  return String("uptime_ms:")+String(millis());
}

void appendPowerEventLog(const String& type,uint32_t durationSec){
  const char* path="/power_events.log";
  File existing=SPIFFS.open(path,FILE_READ);
  size_t sz=existing?existing.size():0;
  if(existing)existing.close();
  if(sz>16384){
    SPIFFS.remove("/power_events.old");
    SPIFFS.rename(path,"/power_events.old");
  }

  File file=SPIFFS.open(path,FILE_APPEND);
  if(!file)return;
  String line=powerEventTimestamp()+","+type;
  line+=",batteryV="+String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,3);
  line+=",durationSec="+String(durationSec);
  line+=",m31="+String(modbusOnline?"ONLINE":"OFFLINE");
  line+=",wifi="+String(WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE");
  line+=",mqtt="+String(mqttClient.connected()?"ONLINE":"OFFLINE");
  line+="\n";
  file.print(line);
  file.flush();
  file.close();
}

bool publishPowerEventMQTT(const String& type,uint32_t durationSec);

void queueOrPublishPowerEvent(const String& type,uint32_t durationSec){
  if(publishPowerEventMQTT(type,durationSec)){
    powerEventPending=false;
    powerPendingEvent="";
    powerPendingDurationSec=0;
  }else{
    powerEventPending=true;
    powerPendingEvent=type;
    powerPendingDurationSec=durationSec;
  }
}

void applyPowerGoodState(bool newGood){
  if(newGood==powerGood)return;
  powerGood=newGood;

  if(!powerGood){
    powerOutageActive=true;
    powerOutageStartMs=millis();
    powerOutageCount++;
    appendPowerEventLog("MAIN_POWER_FAIL",0);
    queueOrPublishPowerEvent("MAIN_POWER_FAIL",0);
    Serial.printf("POWER EVENT: MAIN_POWER_FAIL battery=%.3fV count=%lu\n",
      isfinite(batteryVoltageV)?batteryVoltageV:0.0f,
      (unsigned long)powerOutageCount);
  }else{
    uint32_t durationSec=powerOutageActive
      ? (millis()-powerOutageStartMs)/1000UL
      : 0;
    powerLastOutageDurationSec=durationSec;
    powerOutageActive=false;
    appendPowerEventLog("MAIN_POWER_RESTORED",durationSec);
    queueOrPublishPowerEvent("MAIN_POWER_RESTORED",durationSec);
    Serial.printf("POWER EVENT: MAIN_POWER_RESTORED duration=%lus battery=%.3fV\n",
      (unsigned long)durationSec,
      isfinite(batteryVoltageV)?batteryVoltageV:0.0f);
  }

  if(!newGood && riotDemoQrActive){
    riotDemoQrActive=false;
    Serial.println("KEY1 DEMO QR OFF: power outage screen priority");
  }
  displayDirty=true;
  epaperForceFullRefresh=true;
}

bool requestedPowerGoodState(){
  if(powerGoodOverride==POWER_OVERRIDE_FORCE_ON)return true;
  if(powerGoodOverride==POWER_OVERRIDE_FORCE_OFF)return false;
  if(!POWER_GOOD_SENSE_ENABLED)return true;
  return digitalRead(POWER_GOOD_PIN)==HIGH;
}

void servicePowerGood(){
  bool requested=requestedPowerGoodState();
  uint32_t now=millis();

  if(requested!=powerInputCandidate){
    powerInputCandidate=requested;
    powerInputCandidateSinceMs=now;
    return;
  }

  if(requested!=powerGood &&
     now-powerInputCandidateSinceMs>=POWER_GOOD_DEBOUNCE_MS){
    applyPowerGoodState(requested);
  }
}

void startFallbackAP(){
  WiFi.mode(WIFI_AP_STA);
  riotFallbackApSsid=riotDefaultApSsid();
  if(!WiFi.softAP(riotFallbackApSsid.c_str())) return;
  captivePortalActive=true;
  dnsServer.start(53,"*",WiFi.softAPIP());
  Serial.printf("Device ID: %s | MAC: %s | Factory AP: %s\n",
    riotDeviceId().c_str(),riotDeviceMac().c_str(),riotFallbackApSsid.c_str());
  Serial.print("Fallback AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void beginStationConnect(){
  if(!wifiStaSsid.length())return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiStaSsid.c_str(),wifiStaPass.c_str());
  wifiConnectStart=millis();
  Serial.print("Connecting WiFi: ");
  Serial.println(wifiStaSsid);
}

void serviceWiFi(){
  uint32_t now=millis();

  if(WiFi.status()==WL_CONNECTED){
    startTimeService();
    return;
  }

  if(wifiStaSsid.length() && now-lastReconnectAttempt>10000){
    lastReconnectAttempt=now;
    WiFi.disconnect(false,false);
    WiFi.begin(wifiStaSsid.c_str(),wifiStaPass.c_str());
  }
}

String statusJson(){
  String j="{\"firmware\":\""+String(FW)+"\"";
  j+=",\"modbus\":\""+String(modbusOnline?"ONLINE":"OFFLINE")+"\"";
  j+=",\"good\":"+String(modbusGood);
  j+=",\"bad\":"+String(modbusBad);
  j+=",\"ip\":\""+activeIpString()+"\"";
  j+=",\"deviceId\":\""+riotDeviceId()+"\"";
  j+=",\"mac\":\""+riotDeviceMac()+"\"";
  j+=",\"apSsid\":\""+riotFallbackApSsid+"\"";
  j+=",\"displayMode\":\""+String(riotDisplayModeName(displayCfg.displayMode))+"\"";
  j+=",\"epaperRuntimeEnabled\":"+String(epaperRuntimeEnabled?"true":"false");
  j+=",\"epaperPresenceDetected\":"+String(epaperAutoPresenceDetected?"true":"false");
  j+=",\"epaperProbeSignature\":\""+epaperAutoProbeSignature+"\"";
  j+=",\"wifiConnected\":"+String(WiFi.status()==WL_CONNECTED?"true":"false");
  j+=",\"siteName\":\""+siteName+"\"";
  j+=",\"siteLSD\":\""+siteLSD+"\"";
  j+=",\"di\":[";
  for(int i=0;i<4;i++){if(i)j+=",";j+=diState[i]?"true":"false";}
  j+="],\"do\":[";
  for(int i=0;i<4;i++){if(i)j+=",";j+=doState[i]?"true":"false";}
  j+="],\"ai_mA\":[";
  for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+=String(ai_mA[i],3);}
  j+="],\"aiEU\":[";
  for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+=String(aiEU[i],3);}
  j+="],\"flowValid\":"+String(flowPreviewValid?"true":"false");
  j+=",\"totalFlow\":"+String(totalFlowPreview,3);
  j+=",\"batteryAvailable\":"+String(BATTERY_TELEMETRY_AVAILABLE?"true":"false");
  j+=",\"batteryValid\":"+String(batteryTelemetryValid()?"true":"false");
  j+=",\"batteryV\":"+String(batteryTelemetryValid()?batteryVoltageV:0.0f,3);
  j+=",\"batteryAdcRaw\":"+String(batteryAdcRaw);
  j+=",\"batterySamples\":"+String(batterySampleCounter);
  j+=",\"batteryCalibration\":"+String(BATTERY_ADC_CALIBRATION,4);
  j+=",\"powerSenseEnabled\":"+String(POWER_GOOD_SENSE_ENABLED?"true":"false");
  j+=",\"powerGoodPin\":"+String(POWER_GOOD_PIN);
  j+=",\"powerOverride\":\""+powerOverrideName()+"\"";
  j+=",\"powerGood\":"+String(powerGood?"true":"false");
  j+=",\"onBattery\":"+String(powerOutageActive?"true":"false");
  j+=",\"powerOutageCount\":"+String(powerOutageCount);
  j+=",\"lastOutageDurationSec\":"+String(powerLastOutageDurationSec);
  j+=",\"dateTime\":\""+displayDateTime()+"\"";
  j+="}";
  return j;
}

void saveMQTTDailyUsage(){
  // Persist only today's compact counters. The offline usage queue remains RAM-only.
  // If NVS cannot accept a write, back off instead of hammering a fragmented/full page.
  const uint32_t now=millis();
  if(mqttDailyNvsRetryAfterMs && (int32_t)(now-mqttDailyNvsRetryAfterMs)<0)return;

  prefs.begin("mqttday",false);
  const size_t wDate=prefs.putUInt("date",mqttDailyUsage.date);
  const size_t wPub=prefs.putUInt("pub",mqttDailyUsage.publishes);
  const size_t wPay=prefs.putULong64("pay",mqttDailyUsage.payloadBytes);
  const size_t wTop=prefs.putULong64("top",mqttDailyUsage.topicBytes);
  prefs.end();

  if(wDate!=sizeof(uint32_t) || wPub!=sizeof(uint32_t) ||
     wPay!=sizeof(uint64_t) || wTop!=sizeof(uint64_t)){
    mqttDailyNvsWriteFailures++;
    mqttDailyNvsRetryAfterMs=now+MQTT_DAILY_NVS_RETRY_BACKOFF_MS;
    Serial.printf("MQTT DAILY NVS SAVE FAIL count=%lu; retry in %lus\n",
      (unsigned long)mqttDailyNvsWriteFailures,
      (unsigned long)(MQTT_DAILY_NVS_RETRY_BACKOFF_MS/1000UL));
  }else{
    mqttDailyNvsRetryAfterMs=0;
  }
}

void loadMQTTDailyUsage(){
  memset(mqttDailyUsageQueue,0,sizeof(mqttDailyUsageQueue));
  mqttDailyUsageCount=0;
  prefs.begin("mqttday",true);
  mqttDailyUsage.date=prefs.getUInt("date",0);
  mqttDailyUsage.publishes=prefs.getUInt("pub",0);
  mqttDailyUsage.payloadBytes=prefs.getULong64("pay",0);
  mqttDailyUsage.topicBytes=prefs.getULong64("top",0);
  prefs.end();
}

void cleanupLegacyHistoryNVS(){
  // Remove only historical queue blobs created by recent development builds.
  // These namespaces contain no operator configuration.
  prefs.begin("hourq",false);
  prefs.clear();
  prefs.end();

  prefs.begin("rollq",false);
  prefs.clear();
  prefs.end();

  prefs.begin("mqttday",false);
  prefs.remove("qcount");
  prefs.remove("queue");
  prefs.end();

  Serial.println("Legacy NVS history blobs cleared; config namespaces preserved.");
  printNVSStats("after history cleanup");
}

void queueMQTTDailyUsage(const MQTTDailyUsage& u){
  if(!u.date)return;
  if(mqttDailyUsageCount>=MQTT_DAILY_USAGE_QUEUE_SIZE){
    memmove(
      &mqttDailyUsageQueue[0],
      &mqttDailyUsageQueue[1],
      sizeof(MQTTDailyUsageRecord)*(MQTT_DAILY_USAGE_QUEUE_SIZE-1)
    );
    mqttDailyUsageCount=MQTT_DAILY_USAGE_QUEUE_SIZE-1;
  }
  MQTTDailyUsageRecord &r=mqttDailyUsageQueue[mqttDailyUsageCount++];
  r.date=u.date;
  r.publishes=u.publishes;
  r.payloadBytes=u.payloadBytes;
  r.topicBytes=u.topicBytes;
}

void removeMQTTDailyUsageFront(){
  if(!mqttDailyUsageCount)return;
  if(mqttDailyUsageCount>1){
    memmove(
      &mqttDailyUsageQueue[0],
      &mqttDailyUsageQueue[1],
      sizeof(MQTTDailyUsageRecord)*(mqttDailyUsageCount-1)
    );
  }
  mqttDailyUsageCount--;
  memset(&mqttDailyUsageQueue[mqttDailyUsageCount],0,sizeof(MQTTDailyUsageRecord));
  saveMQTTDailyUsage();
}

void rolloverMQTTDailyUsage(uint32_t newDate){
  if(!newDate)return;
  if(mqttDailyUsage.date && mqttDailyUsage.date!=newDate){
    queueMQTTDailyUsage(mqttDailyUsage);
  }
  if(mqttDailyUsage.date!=newDate){
    mqttDailyUsage.date=newDate;
    mqttDailyUsage.publishes=0;
    mqttDailyUsage.payloadBytes=0;
    mqttDailyUsage.topicBytes=0;
    saveMQTTDailyUsage();
  }
}

String mqttBaseTopicClean(){
  String b=mqttCfg.baseTopic;
  b.trim();
  b.replace(" ","_");
  while(b.endsWith("/"))b.remove(b.length()-1);
  if(!b.length())b="multiflow/controller";
  return b;
}


struct MQTTFileConfigV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t enabled;
  uint8_t retain;
  uint16_t port;
  uint32_t liveIntervalSec;
  uint32_t totalizerIntervalSec;
  uint32_t statusIntervalSec;
  char host[128];
  char user[64];
  char pass[96];
  char baseTopic[128];
  uint32_t checksum;
};

static constexpr uint32_t MQTT_FILE_MAGIC=0x4D515431UL; // "MQT1"
static constexpr uint16_t MQTT_FILE_VERSION=1;

uint32_t mqttFileChecksum(const MQTTFileConfigV1& in){
  const uint8_t* p=(const uint8_t*)&in;
  size_t n=offsetof(MQTTFileConfigV1,checksum);
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){
    h^=p[i];
    h*=16777619UL;
  }
  return h;
}

void copyStringToFixed(char* dst,size_t n,const String& src){
  if(!dst || !n)return;
  memset(dst,0,n);
  src.substring(0,n-1).toCharArray(dst,n);
}

bool mqttConfigToFileStruct(const MQTTConfig& cfg,MQTTFileConfigV1& out){
  if(!cfg.host.length())return false;
  memset(&out,0,sizeof(out));
  out.magic=MQTT_FILE_MAGIC;
  out.version=MQTT_FILE_VERSION;
  out.size=sizeof(out);
  out.enabled=cfg.enabled?1:0;
  out.retain=cfg.retain?1:0;
  out.port=cfg.port;
  out.liveIntervalSec=cfg.liveIntervalSec;
  out.totalizerIntervalSec=cfg.totalizerIntervalSec;
  out.statusIntervalSec=cfg.statusIntervalSec;
  copyStringToFixed(out.host,sizeof(out.host),cfg.host);
  copyStringToFixed(out.user,sizeof(out.user),cfg.user);
  copyStringToFixed(out.pass,sizeof(out.pass),cfg.pass);
  copyStringToFixed(out.baseTopic,sizeof(out.baseTopic),cfg.baseTopic);
  out.checksum=mqttFileChecksum(out);
  return true;
}

bool mqttFileStructToConfig(const MQTTFileConfigV1& in,MQTTConfig& out){
  if(in.magic!=MQTT_FILE_MAGIC ||
     in.version!=MQTT_FILE_VERSION ||
     in.size!=sizeof(in) ||
     in.checksum!=mqttFileChecksum(in) ||
     !in.host[0])return false;

  out.enabled=in.enabled!=0;
  out.retain=in.retain!=0;
  out.port=in.port?in.port:1883;
  out.liveIntervalSec=(uint32_t)constrain((int)in.liveIntervalSec,1,3600);
  out.totalizerIntervalSec=(uint32_t)constrain((int)in.totalizerIntervalSec,10,86400);
  out.statusIntervalSec=(uint32_t)constrain((int)in.statusIntervalSec,10,3600);
  out.host=String(in.host);
  out.user=String(in.user);
  out.pass=String(in.pass);
  out.baseTopic=String(in.baseTopic);
  if(!out.baseTopic.length())out.baseTopic="multiflow/controller";
  return true;
}

bool readMQTTConfigFile(const char* path,MQTTConfig& out){
  File file=SPIFFS.open(path,FILE_READ);
  if(!file)return false;
  if(file.size()!=sizeof(MQTTFileConfigV1)){
    file.close();
    return false;
  }
  MQTTFileConfigV1 raw;
  size_t n=file.read((uint8_t*)&raw,sizeof(raw));
  file.close();
  return n==sizeof(raw) && mqttFileStructToConfig(raw,out);
}

bool writeMQTTConfigFile(const char* path,const MQTTConfig& cfg){
  MQTTFileConfigV1 raw;
  if(!mqttConfigToFileStruct(cfg,raw))return false;

  File file=SPIFFS.open(path,FILE_WRITE);
  if(!file)return false;
  size_t n=file.write((const uint8_t*)&raw,sizeof(raw));
  file.flush();
  file.close();

  MQTTConfig verify;
  return n==sizeof(raw) && readMQTTConfigFile(path,verify) && verify.host==cfg.host;
}

void loadMQTTConfig(){
  MQTTConfig primaryCfg;
  MQTTConfig backupCfg;

  bool primaryOK=readMQTTConfigFile("/mqtt.cfg",primaryCfg);
  bool backupOK=readMQTTConfigFile("/mqtt.bak",backupCfg);

  mqttFlashBootPrimaryOK=primaryOK;
  mqttFlashBootBackupOK=backupOK;
  mqttFlashBootLoadOK=primaryOK||backupOK;

  if(primaryOK){
    mqttCfg=primaryCfg;
    if(!backupOK)writeMQTTConfigFile("/mqtt.bak",mqttCfg);
  }else if(backupOK){
    mqttCfg=backupCfg;
    writeMQTTConfigFile("/mqtt.cfg",mqttCfg);
  }

  Serial.printf(
    "MQTT file config load: primary=%s backup=%s host=%s port=%u enabled=%s\n",
    primaryOK?"OK":"MISSING",
    backupOK?"OK":"MISSING",
    mqttCfg.host.c_str(),
    mqttCfg.port,
    mqttCfg.enabled?"YES":"NO"
  );

  mqttClient.setServer(mqttCfg.host.c_str(),mqttCfg.port);
}

bool saveMQTTConfig(){
  if(!mqttCfg.host.length()){
    Serial.println("MQTT config save refused: broker host is blank");
    return false;
  }

  bool primaryOK=writeMQTTConfigFile("/mqtt.cfg",mqttCfg);
  bool backupOK=writeMQTTConfigFile("/mqtt.bak",mqttCfg);

  MQTTConfig verifyPrimary;
  MQTTConfig verifyBackup;
  bool primaryReadback=readMQTTConfigFile("/mqtt.cfg",verifyPrimary);
  bool backupReadback=readMQTTConfigFile("/mqtt.bak",verifyBackup);

  Serial.printf(
    "MQTT file save: write primary=%s backup=%s | readback primary=%s backup=%s\n",
    primaryOK?"OK":"FAIL",
    backupOK?"OK":"FAIL",
    primaryReadback?"OK":"FAIL",
    backupReadback?"OK":"FAIL"
  );

  mqttClient.disconnect();
  mqttClient.setServer(mqttCfg.host.c_str(),mqttCfg.port);
  mqttLastConnectAttempt=0;
  mqttLastLivePublish=0;
  mqttLastTotalizerPublish=0;
  mqttLastStatusPublish=0;

  mqttFlashLastSaveOK=primaryOK && backupOK && primaryReadback && backupReadback;
  if(mqttFlashLastSaveOK)mqttFlashSaveCount++;
  return mqttFlashLastSaveOK;
}


String mqttClientId(){
  uint64_t chip=ESP.getEfuseMac();
  char b[32];
  snprintf(b,sizeof(b),"MultiFlow-%06lX",(unsigned long)(chip&0xFFFFFFUL));
  return String(b);
}

bool mqttPublish(const String& topic,const String& payload,bool retain){
  if(!mqttClient.connected())return false;
  bool ok=mqttClient.publish(topic.c_str(),payload.c_str(),retain);
  if(ok){
    mqttPublishCount++;
    mqttPayloadBytes+=(uint64_t)payload.length();
    mqttTopicBytes+=(uint64_t)topic.length();
    mqttDailyUsage.publishes++;
    mqttDailyUsage.payloadBytes+=(uint64_t)payload.length();
    mqttDailyUsage.topicBytes+=(uint64_t)topic.length();
    if(!mqttTrafficStartMs)mqttTrafficStartMs=millis();
  }else{
    mqttPublishFailCount++;
  }
  return ok;
}

// EP32 IO MQTT GATE6B2 ACTUAL PUBLISH TAG TRUTH V1
static const char* ioMqttQualityAI(uint8_t i){
  if(aiManual[i])return "MANUAL";
  if(!modbusCommsEnabled)return "DISABLED";
  return modbusOnline?"GOOD":"BAD";
}
static const char* ioMqttQualityDI(uint8_t i){
  if(diForceActive[i])return "FORCED";
  if(!modbusCommsEnabled)return "DISABLED";
  return modbusOnline?"GOOD":"BAD";
}
static const char* ioMqttQualityDO(uint8_t i){
  if(doForceActive[i])return "FORCED";
  if(!modbusCommsEnabled)return "DISABLED";
  return modbusOnline?"GOOD":"BAD";
}
static const char* ioMqttQualityAO(uint8_t i){
  if(aoForceActive[i])return "FORCED";
  if(!modbusCommsEnabled)return "DISABLED";
  return modbusOnline?"GOOD":"BAD";
}

bool publishMQTTIO(bool changeTriggered=false){
  if(!mqttClient.connected())return false;
  const uint8_t activeAI=ep32M31ActiveAiCount();
  const uint8_t activeAO=ep32M31ActiveAoCount();
  bool any=false;
  for(uint8_t i=0;i<activeAI;i++)if(ioMqttAI[i])any=true;
  for(uint8_t i=0;i<4;i++)if(ioMqttDI[i]||ioMqttDO[i])any=true;
  for(uint8_t i=0;i<activeAO;i++)if(ioMqttAO[i])any=true;
  if(!any)return true;

  String j;
  j.reserve(1800);
  j="{\"ts\":"+String((unsigned long)time(nullptr));
  j+=",\"modbus\":\""+String(!modbusCommsEnabled?"DISABLED":(modbusOnline?"ONLINE":"OFFLINE"))+"\"";
  j+=",\"trigger\":\""+String(changeTriggered?"change":"heartbeat")+"\"";

  j+=",\"ai\":{";
  bool first=true;
  for(uint8_t i=0;i<activeAI;i++)if(ioMqttAI[i]){
    if(!first)j+=",";
    first=false;
    j+="\""+String(i+1)+"\":{\"value\":"+String(aiEU[i],6)+",\"mA\":"+String(ai_mA[i],3)+",\"quality\":\""+String(ioMqttQualityAI(i))+"\"}";
  }
  j+="}";

  j+=",\"di\":{";
  first=true;
  for(uint8_t i=0;i<4;i++)if(ioMqttDI[i]){
    if(!first)j+=",";
    first=false;
    j+="\""+String(i+1)+"\":{\"value\":"+String(diState[i]?"true":"false")+",\"raw\":"+String(diRawState[i]?"true":"false")+",\"quality\":\""+String(ioMqttQualityDI(i))+"\"}";
  }
  j+="}";

  j+=",\"do\":{";
  first=true;
  for(uint8_t i=0;i<4;i++)if(ioMqttDO[i]){
    if(!first)j+=",";
    first=false;
    j+="\""+String(i+1)+"\":{\"value\":"+String(doState[i]?"true":"false")+",\"quality\":\""+String(ioMqttQualityDO(i))+"\"}";
  }
  j+="}";

  j+=",\"ao\":{";
  first=true;
  for(uint8_t i=0;i<activeAO;i++)if(ioMqttAO[i]){
    if(!first)j+=",";
    first=false;
    j+="\""+String(i+1)+"\":{\"mA\":"+String(ao_mA[i],3)+",\"quality\":\""+String(ioMqttQualityAO(i))+"\"}";
  }
  j+="}}";

  bool ok=mqttPublish(mqttBaseTopicClean()+"/io/live",j,mqttCfg.retain);
  if(ok){
    ioMqttPublishCount++;
    if(changeTriggered)ioMqttChangePublishCount++;
    ioMqttLastPublishMs=millis();
    for(uint8_t i=0;i<4;i++){
      if(ioMqttDI[i]){ioMqttDigitalSeen[i]=true;ioMqttDigitalLast[i]=diState[i];}
      if(ioMqttDO[i]){ioMqttDigitalSeen[4+i]=true;ioMqttDigitalLast[4+i]=doState[i];}
    }
  }
  return ok;
}

void serviceMQTTIOChange(){
  if(!mqttClient.connected())return;
  bool changed=false;
  for(uint8_t i=0;i<4;i++){
    if(ioMqttDI[i]){
      if(!ioMqttDigitalSeen[i]){ioMqttDigitalSeen[i]=true;ioMqttDigitalLast[i]=diState[i];}
      else if(ioMqttDigitalLast[i]!=diState[i])changed=true;
    }else ioMqttDigitalSeen[i]=false;
    if(ioMqttDO[i]){
      if(!ioMqttDigitalSeen[4+i]){ioMqttDigitalSeen[4+i]=true;ioMqttDigitalLast[4+i]=doState[i];}
      else if(ioMqttDigitalLast[4+i]!=doState[i])changed=true;
    }else ioMqttDigitalSeen[4+i]=false;
  }
  if(changed)publishMQTTIO(true);
}

void publishMQTTLive(){
  String base=mqttBaseTopicClean();
  time_t nowTime=time(nullptr);

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    if(!flowRunEnabled[run])continue;

    FlowResolvedPoint dp=resolveFlowRunInput(run,FLOW_ROLE_DP);
    FlowResolvedPoint sp=resolveFlowRunInput(run,FLOW_ROLE_STATIC);
    FlowResolvedPoint tp=resolveFlowRunInput(run,FLOW_ROLE_TEMP);

    String j="{\"run\":"+String(run+1);
    j+=",\"ts\":"+String((unsigned long)nowTime);
    j+=",\"valid\":"+String(flowRunValid[run]?"true":"false");
    j+=",\"flow_E3m3_day\":"+String(flowRunValid[run]?flowRunFlow[run]:0.0f,6);
    j+=",\"dp_kPa\":"+String(isfinite(dp.value)?dp.value:0.0f,3);
    j+=",\"static_kPa\":"+String(isfinite(sp.value)?sp.value:0.0f,3);
    j+=",\"temp_C\":"+String(isfinite(tp.value)?tp.value:0.0f,3);
    j+="}";
    mqttPublish(base+"/flow/"+String(run+1)+"/live",j,mqttCfg.retain);
  }
}

void publishMQTTTotalizers(){
  String base=mqttBaseTopicClean();
  time_t nowTime=time(nullptr);

  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    if(!flowRunEnabled[run])continue;

    String j="{\"run\":"+String(run+1);
    j+=",\"ts\":"+String((unsigned long)nowTime);
    j+=",\"date\":"+String(flowTotalizerDate);
    j+=",\"flowToday_E3m3\":"+String(flowDay[run].volumeE3m3,6);
    j+=",\"flowHours\":"+String(flowDay[run].flowHours,6);
    j+="}";
    mqttPublish(base+"/flow/"+String(run+1)+"/totalizer",j,mqttCfg.retain);
  }
}

bool publishMQTTHourlyHistory(const HourlyHistoryRecord& r){
  if(!mqttClient.connected())return false;

  String base=mqttBaseTopicClean();
  time_t nowTime=time(nullptr);
  String j="{\"run\":"+String(r.run+1);
  j+=",\"ts\":"+String((unsigned long)nowTime);
  j+=",\"hour\":"+String(r.hourKey);
  j+=",\"flow_E3m3\":"+String(r.volumeE3m3,6);
  j+=",\"flowHours\":"+String(r.flowHours,6);
  j+=",\"avgDP_kPa\":"+String(r.avgDP,3);
  j+=",\"avgStatic_kPa\":"+String(r.avgStatic,3);
  j+=",\"avgTemp_C\":"+String(r.avgTemp,3);
  j+=",\"recordType\":\"hourly_history\"";
  j+="}";

  return mqttPublish(
    base+"/flow/"+String(r.run+1)+"/hist/hour",
    j,
    true
  );
}

void serviceMQTTHourlyHistoryQueue(){
  if(!mqttClient.connected() || !hourlyHistoryCount)return;
  HourlyHistoryRecord r=hourlyHistoryQueue[0];
  if(publishMQTTHourlyHistory(r))removeHourlyHistoryFront();
}

bool publishMQTTDailyRollover(const DailyRolloverRecord& r){
  if(!mqttClient.connected())return false;

  String base=mqttBaseTopicClean();
  time_t nowTime=time(nullptr);
  String j="{\"run\":"+String(r.run+1);
  j+=",\"ts\":"+String((unsigned long)nowTime);
  j+=",\"date\":"+String(r.date);
  j+=",\"flow_E3m3\":"+String(r.volumeE3m3,6);
  j+=",\"flowHours\":"+String(r.flowHours,6);
  j+=",\"recordType\":\"daily_rollover\"";
  j+="}";

  // Finalized rollover records are retained so a late subscriber can still
  // see the last completed contract day for each run.
  return mqttPublish(
    base+"/flow/"+String(r.run+1)+"/hist/day",
    j,
    true
  );
}

void serviceMQTTRolloverQueue(){
  if(!mqttClient.connected() || !dailyRolloverCount)return;

  // Publish at most one queued record per loop iteration to keep the controller
  // responsive and avoid a burst after a long communications outage.
  DailyRolloverRecord r=dailyRolloverQueue[0];
  if(publishMQTTDailyRollover(r))removeDailyRolloverFront();
}

bool publishMQTTDailyUsage(const MQTTDailyUsageRecord& r){
  if(!mqttClient.connected())return false;

  uint64_t appBytes=r.payloadBytes+r.topicBytes;
  char payBuf[32],topBuf[32],appBuf[32];
  snprintf(payBuf,sizeof(payBuf),"%llu",(unsigned long long)r.payloadBytes);
  snprintf(topBuf,sizeof(topBuf),"%llu",(unsigned long long)r.topicBytes);
  snprintf(appBuf,sizeof(appBuf),"%llu",(unsigned long long)appBytes);

  String base=mqttBaseTopicClean();
  time_t nowTime=time(nullptr);
  String j="{\"ts\":"+String((unsigned long)nowTime);
  j+=",\"date\":"+String(r.date);
  j+=",\"publishes\":"+String(r.publishes);
  j+=",\"payloadBytes\":";
  j+=payBuf;
  j+=",\"topicBytes\":";
  j+=topBuf;
  j+=",\"appBytes\":";
  j+=appBuf;
  j+=",\"appMB\":"+String((double)appBytes/1000000.0,6);
  j+=",\"recordType\":\"mqtt_daily_usage\"";
  j+="}";

  return mqttPublish(base+"/hist/day/mqtt",j,true);
}

void serviceMQTTDailyUsageQueue(){
  if(!mqttClient.connected() || !mqttDailyUsageCount)return;
  MQTTDailyUsageRecord r=mqttDailyUsageQueue[0];
  if(publishMQTTDailyUsage(r))removeMQTTDailyUsageFront();
}

bool publishPowerEventMQTT(const String& type,uint32_t durationSec){
  if(!mqttClient.connected())return false;
  String base=mqttBaseTopicClean();
  String j="{\"event\":\""+type+"\"";
  j+=",\"ts\":"+String((unsigned long)time(nullptr));
  j+=",\"batteryV\":"+String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,3);
  j+=",\"durationSec\":"+String(durationSec);
  j+=",\"modbus\":\""+String(modbusOnline?"ONLINE":"OFFLINE")+"\"";
  j+=",\"wifi\":\""+String(WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE")+"\"";
  j+=",\"outageCount\":"+String(powerOutageCount);
  j+="}";
  return mqttPublish(base+"/event/power",j,true);
}

void servicePendingPowerEvent(){
  if(!powerEventPending || !mqttClient.connected())return;
  if(publishPowerEventMQTT(powerPendingEvent,powerPendingDurationSec)){
    powerEventPending=false;
    powerPendingEvent="";
    powerPendingDurationSec=0;
  }
}

void publishMQTTStatus(){
  String base=mqttBaseTopicClean();
  String j="{\"firmware\":\""+String(FW)+"\"";
  j+=",\"ip\":\""+activeIpString()+"\"";
  j+=",\"modbus\":\""+String(modbusOnline?"ONLINE":"OFFLINE")+"\"";
  j+=",\"modbusGood\":"+String(modbusGood);
  j+=",\"modbusBad\":"+String(modbusBad);
  j+=",\"batteryAvailable\":"+String(BATTERY_TELEMETRY_AVAILABLE?"true":"false");
  j+=",\"batteryV\":"+String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,3);
  j+=",\"batteryAdcRaw\":"+String(batteryAdcRaw);
  j+=",\"uptimeSec\":"+String(millis()/1000UL);
  j+="}";
  mqttPublish(base+"/status",j,mqttCfg.retain);
}

void serviceMQTT(){
  if(!mqttCfg.enabled || !mqttCfg.host.length()){
    if(mqttClient.connected())mqttClient.disconnect();
    return;
  }
  if(WiFi.status()!=WL_CONNECTED){
    if(mqttClient.connected())mqttClient.disconnect();
    return;
  }

  uint32_t now=millis();
  if(!mqttClient.connected()){
    if(now-mqttLastConnectAttempt<10000UL)return;
    mqttLastConnectAttempt=now;

    String cid=mqttClientId();
    bool ok=false;
    if(mqttCfg.user.length()){
      ok=mqttClient.connect(cid.c_str(),mqttCfg.user.c_str(),mqttCfg.pass.c_str());
    }else{
      ok=mqttClient.connect(cid.c_str());
    }
    if(!ok)return;

    mqttConnectCount++;
    mqttLastLivePublish=0;
    mqttLastTotalizerPublish=0;
    mqttLastStatusPublish=0;
  }

  mqttClient.loop();
  serviceMQTTIOChange();
  servicePendingPowerEvent();
  serviceMQTTHourlyHistoryQueue();
  serviceMQTTRolloverQueue();
  serviceMQTTDailyUsageQueue();

  if(mqttLastLivePublish==0 || now-mqttLastLivePublish>=mqttCfg.liveIntervalSec*1000UL){
    mqttLastLivePublish=now;
    publishMQTTLive();
    publishMQTTIO(false);
  }
  if(mqttLastTotalizerPublish==0 || now-mqttLastTotalizerPublish>=mqttCfg.totalizerIntervalSec*1000UL){
    mqttLastTotalizerPublish=now;
    publishMQTTTotalizers();
  }
  if(mqttLastStatusPublish==0 || now-mqttLastStatusPublish>=mqttCfg.statusIntervalSec*1000UL){
    mqttLastStatusPublish=now;
    publishMQTTStatus();
  }
}

String mqttConfigJson(){
  MQTTConfig checkPrimary;
  MQTTConfig checkBackup;
  bool primaryStored=readMQTTConfigFile("/mqtt.cfg",checkPrimary);
  bool backupStored=readMQTTConfigFile("/mqtt.bak",checkBackup);
  String j="{\"configLoaded\":true";
  j+=",\"primaryStored\":"+String(primaryStored?"true":"false");
  j+=",\"backupStored\":"+String(backupStored?"true":"false");
  j+=",\"enabled\":"+String(mqttCfg.enabled?"true":"false");
  j+=",\"host\":\""+mqttCfg.host+"\"";
  j+=",\"port\":"+String(mqttCfg.port);
  j+=",\"user\":\""+mqttCfg.user+"\"";
  j+=",\"hasPassword\":"+String(mqttCfg.pass.length()?"true":"false");
  j+=",\"baseTopic\":\""+mqttCfg.baseTopic+"\"";
  j+=",\"liveInterval\":"+String(mqttCfg.liveIntervalSec);
  j+=",\"totalizerInterval\":"+String(mqttCfg.totalizerIntervalSec);
  j+=",\"statusInterval\":"+String(mqttCfg.statusIntervalSec);
  j+=",\"retain\":"+String(mqttCfg.retain?"true":"false");
  j+=",\"connected\":"+String(mqttClient.connected()?"true":"false");
  j+=",\"state\":"+String(mqttClient.state());
  j+=",\"connectCount\":"+String(mqttConnectCount);
  j+=",\"publishCount\":"+String(mqttPublishCount);
  j+=",\"publishFails\":"+String(mqttPublishFailCount);
  j+=",\"hourlyHistoryQueued\":"+String(hourlyHistoryCount);
  j+=",\"dailyRolloverQueued\":"+String(dailyRolloverCount);
  NVSStatsSnapshot nvs=getNVSStatsSnapshot();
  j+=",\"nvsValid\":"+String(nvs.valid?"true":"false");
  j+=",\"nvsUsed\":"+String((unsigned)nvs.used);
  j+=",\"nvsFree\":"+String((unsigned)nvs.free);
  j+=",\"nvsTotal\":"+String((unsigned)nvs.total);
  j+="}";
  return j;
}


String mqttTrafficJson(){
  uint64_t appBytes=mqttPayloadBytes+mqttTopicBytes;
  uint32_t elapsedSec=mqttTrafficStartMs?((millis()-mqttTrafficStartMs)/1000UL):0;

  char payloadBuf[32],topicBuf[32],appBuf[32];
  snprintf(payloadBuf,sizeof(payloadBuf),"%llu",(unsigned long long)mqttPayloadBytes);
  snprintf(topicBuf,sizeof(topicBuf),"%llu",(unsigned long long)mqttTopicBytes);
  snprintf(appBuf,sizeof(appBuf),"%llu",(unsigned long long)appBytes);

  String j="{\"payloadBytes\":";
  j+=payloadBuf;
  j+=",\"topicBytes\":";
  j+=topicBuf;
  j+=",\"appBytes\":";
  j+=appBuf;
  j+=",\"trafficElapsedSec\":"+String(elapsedSec);
  j+=",\"dailyDate\":"+String(mqttDailyUsage.date);
  j+=",\"dailyPublishes\":"+String(mqttDailyUsage.publishes);
  char dailyBuf[32];
  snprintf(dailyBuf,sizeof(dailyBuf),"%llu",(unsigned long long)(mqttDailyUsage.payloadBytes+mqttDailyUsage.topicBytes));
  j+=",\"dailyAppBytes\":";
  j+=dailyBuf;
  j+=",\"dailyUsageQueued\":"+String(mqttDailyUsageCount);
  j+="}";
  return j;
}


bool epaperThemeIsDark(){
  if(displayCfg.themeMode==EPAPER_THEME_DARK)return true;
  if(displayCfg.themeMode==EPAPER_THEME_LIGHT)return false;
  struct tm t;
  if(!getLocalTime(&t,5))return false;
  uint8_t h=(uint8_t)t.tm_hour;
  if(displayCfg.lightHour==displayCfg.darkHour)return false;
  if(displayCfg.lightHour<displayCfg.darkHour)
    return h>=displayCfg.darkHour || h<displayCfg.lightHour;
  return h>=displayCfg.darkHour && h<displayCfg.lightHour;
}

bool drawStoredLogo(int x,int y,uint16_t ink=TFT_BLACK){
#ifdef EPAPER_ENABLE
  File f=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_READ);
  if(!f)return false;
  uint8_t hdr[4]={0};
  if(f.read(hdr,4)!=4){f.close();return false;}
  uint16_t w=(uint16_t)hdr[0]|((uint16_t)hdr[1]<<8);
  uint16_t h=(uint16_t)hdr[2]|((uint16_t)hdr[3]<<8);
  if(!w || !h || w>DISPLAY_LOGO_MAX_W || h>DISPLAY_LOGO_MAX_H){
    f.close();
    return false;
  }
  size_t bytes=((size_t)w*h+7)/8;
  uint8_t* bits=(uint8_t*)malloc(bytes);
  if(!bits){f.close();return false;}
  size_t got=f.read(bits,bytes);
  f.close();
  if(got!=bytes){free(bits);return false;}
  for(uint16_t py=0;py<h;py++){
    for(uint16_t px=0;px<w;px++){
      size_t bit=(size_t)py*w+px;
      bool set=(bits[bit>>3]&(0x80>>(bit&7)))!=0;
      if(epaperLogoInvert)set=!set;
      if(set)epaper.drawPixel(x+px,y+py,ink);
    }
  }
  free(bits);
  return true;
#else
  return false;
#endif
}

void drawPowerOutageScreen(bool fullRefresh=true){
#ifdef EPAPER_ENABLE
  if(!epaperRuntimeEnabled)return;
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK,TFT_WHITE);
  epaper.setTextDatum(TL_DATUM);
  epaper.drawRect(6,6,epaper.width()-12,epaper.height()-12,TFT_BLACK);

  // Site/company logo sits on the same top identity line.
  epaper.drawRect(558,20,194,74,TFT_BLACK);
  if(!drawStoredLogo(565,28)){
    epaper.setTextSize(1);
    epaper.setTextDatum(TC_DATUM);
    epaper.drawString("LOGO",655,48);
    epaper.setTextDatum(TL_DATUM);
  }

  epaper.setTextSize(2);
  String outageSite=siteName;
  if(outageSite.length()>24)outageSite=outageSite.substring(0,24);
  epaper.drawString(outageSite,52,22);
  epaper.setTextSize(1);
  String outageLocation=siteLSD;
  if(outageLocation.length()>36)outageLocation=outageLocation.substring(0,36);
  if(outageLocation.length())epaper.drawString(outageLocation,52,46);

  epaper.setTextSize(4);
  epaper.drawString("MAIN POWER LOST",52,68);
  epaper.drawFastHLine(24,116,752,TFT_BLACK);

  epaper.setTextSize(3);
  epaper.drawString("RUNNING ON BATTERY",52,138);

  epaper.setTextSize(2);
  epaper.drawString("Battery",52,206);
  epaper.setTextSize(4);
  epaper.drawString(String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,3)+" V",220,194);

  uint32_t elapsed=(millis()-powerOutageStartMs)/1000UL;
  epaper.setTextSize(2);
  epaper.drawString("Outage",52,270);
  epaper.drawString(String(elapsed)+" sec",220,270);

  epaper.drawString(String("M31  ")+(!modbusCommsEnabled?"DISABLED":(modbusOnline?"ONLINE":"OFFLINE")),52,322);
  epaper.drawString(String("WiFi ")+(WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE"),300,322);
  epaper.drawString(String("MQTT ")+(mqttClient.connected()?"ONLINE":"OFFLINE"),548,322);

  epaper.drawFastHLine(24,382,752,TFT_BLACK);
  epaper.setTextSize(1);
  epaper.drawString("MultiFlow outage record active",52,402);
  epaper.drawString(String("IP ")+activeIpString(),52,424);
  epaper.setTextDatum(TR_DATUM);
  epaper.drawString(String(FW),752,424);
  epaper.setTextDatum(TL_DATUM);

  if(fullRefresh){
    epaper.update();
    epaperPartialRefreshes=0;
    epaperFlowPartialRefreshes=0;
  }else{
#ifdef USE_PARTIAL_EPAPER
    epaper.updataPartial(40,184,720,180);
    epaperPartialRefreshes++;
#else
    epaper.update();
    epaperPartialRefreshes=0;
#endif
  }
#endif
}


void scrubEpaperFlowValueBands(){
#ifdef EPAPER_ENABLE
#ifdef USE_PARTIAL_EPAPER
  uint8_t enabledCount=0;
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++)if(flowRunEnabled[run])enabledCount++;
  if(!enabledCount)return;

  uint8_t visibleCount=min((uint8_t)6,enabledCount);
  int flowTop=displayCfg.showTotalFlow?136:76;
  int cols=(visibleCount<=2)?1:((visibleCount<=4)?2:3);
  int rows=(visibleCount+cols-1)/cols;
  int gap=8;
  int left=18;
  int right=782;
  int usableW=right-left;
  int cardW=(usableW-gap*(cols-1))/cols;
  int bottomLimit=382;
  int usableH=bottomLimit-flowTop;
  int cardH=(usableH-gap*(rows-1))/rows;

  // Clear only the fast-changing text bands to the current theme background,
  // push that local clear, then the normal redraw restores crisp text. This
  // avoids the highly visible full-panel cleanup waveform.
  const uint16_t bg=epaperThemeIsDark()?TFT_BLACK:TFT_WHITE;
  for(uint8_t slot=0;slot<visibleCount;slot++){
    int col=slot%cols;
    int row=slot/cols;
    int x=left+col*(cardW+gap);
    int y=flowTop+row*(cardH+gap);
    epaper.fillRect(x+5,y+49,cardW-10,29,bg);
  }
  // Push cleared flow-value bands only.
  epaper.updataPartial(16,64,768,320);
#endif
#endif
}


// EP32 EPAPER I/O SCRUB GATE4B V1
// EP32 EPAPER I/O STRONG SCRUB GATE4B2 V1
// Strong local cleanup for repeated DI/DO commissioning changes:
// 1) invert only the Station I/O band and push it,
// 2) restore the normal background and push it,
// 3) the normal dashboard redraw then paints the fresh DI/DO text.
// This avoids the full-panel black/white cleanup waveform while giving the
// local ePaper cells a stronger reset than a single background scrub.
void scrubEpaperIOBand(){
#ifdef EPAPER_ENABLE
#ifdef USE_PARTIAL_EPAPER
  const uint16_t bg=epaperThemeIsDark()?TFT_BLACK:TFT_WHITE;
  const uint16_t inv=epaperThemeIsDark()?TFT_WHITE:TFT_BLACK;
  epaper.fillRect(16,384,768,56,inv);
  epaper.updataPartial(16,384,768,56);
  delay(35);
  epaper.fillRect(16,384,768,56,bg);
  epaper.updataPartial(16,384,768,56);
#endif
#endif
}

void scrubEpaperAlarmBand(){
#ifdef EPAPER_ENABLE
#ifdef USE_PARTIAL_EPAPER
  const uint16_t bg=epaperThemeIsDark()?TFT_BLACK:TFT_WHITE;
  epaper.fillRect(16,352,768,36,bg);
  epaper.updataPartial(16,352,768,36);
#endif
#endif
}

// RIOT A9B COMPILE REPAIR V1
void drawDashboard(bool fullRefresh);

void drawTechnicianAccessQrScreen(bool fullRefresh=true){
#ifdef EPAPER_ENABLE
  if(!epaperRuntimeEnabled)return;

  // Keep the demo/access screen black-on-white for maximum phone QR contrast.
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK,TFT_WHITE);
  epaper.setTextDatum(TL_DATUM);
  epaper.drawRect(6,6,epaper.width()-12,epaper.height()-12,TFT_BLACK);

  // Left: large locally generated Wi-Fi QR.
  static constexpr uint8_t QR_VERSION=3;
  static uint8_t qrData[256];
  const size_t required=qrcode_getBufferSize(QR_VERSION);
  if(required>sizeof(qrData)){
    epaper.setTextSize(2);
    epaper.drawString("QR BUFFER ERROR",30,30);
  }else{
    String payload="WIFI:T:nopass;S:"+riotFallbackApSsid+";;";
    QRCode qr={};
    qrcode_initText(&qr,qrData,QR_VERSION,ECC_LOW,payload.c_str());

    const int scale=10;
    const int quiet=4;
    const int qrSide=((int)qr.size+quiet*2)*scale;
    const int qrX=30;
    const int qrY=(epaper.height()-qrSide)/2;

    epaper.fillRect(qrX,qrY,qrSide,qrSide,TFT_WHITE);
    for(uint8_t y=0;y<qr.size;y++){
      for(uint8_t x=0;x<qr.size;x++){
        if(!qrcode_getModule(&qr,x,y))continue;
        epaper.fillRect(
          qrX+(x+quiet)*scale,
          qrY+(y+quiet)*scale,
          scale,scale,TFT_BLACK
        );
      }
    }
  }

  // Right: owner identity and simple walk-up instructions.
  const int rightX=430;
  if(displayLogoExists()){
    drawStoredLogo(rightX,24,TFT_BLACK);
  }else{
    epaper.setTextSize(3);
    epaper.drawString("RIOT-FLOW",rightX,28);
  }

  epaper.setTextSize(2);
  epaper.drawString("TECHNICIAN ACCESS",rightX,104);
  epaper.drawFastHLine(rightX,136,330,TFT_BLACK);

  epaper.setTextSize(1);
  epaper.drawString("Scan QR to join this controller",rightX,154);
  epaper.drawString("No site LAN or Internet required",rightX,174);

  epaper.setTextSize(2);
  epaper.drawString("AP",rightX,214);
  epaper.setTextSize(2);
  epaper.drawString(riotFallbackApSsid,rightX+58,214);

  epaper.setTextSize(1);
  String demoSite=siteName;
  if(demoSite.length()>32)demoSite=demoSite.substring(0,32);
  epaper.drawString(String("Site: ")+demoSite,rightX,262);

  String demoLsd=siteLSD;
  if(demoLsd.length()>34)demoLsd=demoLsd.substring(0,34);
  epaper.drawString(String("LSD:  ")+(demoLsd.length()?demoLsd:String("--")),rightX,284);
  epaper.drawString(String("Device: ")+riotDeviceId(),rightX,306);
  epaper.drawString("Local browser: 192.168.4.1",rightX,328);

  epaper.drawFastHLine(rightX,366,330,TFT_BLACK);
  epaper.setTextSize(2);
  epaper.drawString("Press KEY1 to return",rightX,388);

  epaper.setTextSize(1);
  epaper.drawString("Riot-Flow walk-up demo / technician access",rightX,430);

  if(fullRefresh){
    epaper.update();
    epaperPartialRefreshes=0;
    epaperFlowPartialRefreshes=0;
  }else{
    epaper.update();
    epaperPartialRefreshes=0;
    epaperFlowPartialRefreshes=0;
  }
#endif
}

void serviceRiotDemoKey(){
  const uint32_t now=millis();
  const bool raw=digitalRead(RIOT_KEY1_PIN);

  if(raw!=riotKey1LastRaw){
    riotKey1LastRaw=raw;
    riotKey1ChangedMs=now;
  }

  if(raw!=riotKey1Stable && now-riotKey1ChangedMs>=RIOT_KEY_DEBOUNCE_MS){
    riotKey1Stable=raw;
    if(riotKey1Stable==LOW){
      if(powerOutageActive){
        Serial.println("KEY1 ignored: power outage screen has priority");
        return;
      }

      riotDemoQrActive=!riotDemoQrActive;
      Serial.printf("KEY1 DEMO QR %s\n",riotDemoQrActive?"ON":"OFF");

      // A mode switch should always be a clean full ePaper refresh.
      epaperPendingRegionMask=EPAPER_REGION_NONE;
      epaperPartialRegionMask=EPAPER_REGION_NONE;
      epaperForceFullRefresh=false;
      displayDirty=false;

      if(riotDemoQrActive)drawTechnicianAccessQrScreen(true);
      else drawDashboard(true);
    }
  }
}

void drawDashboard(bool fullRefresh=true){
#ifdef EPAPER_ENABLE
  if(!epaperRuntimeEnabled)return;
  if(powerOutageActive){
    drawPowerOutageScreen(fullRefresh);
    return;
  }

  const bool darkTheme=epaperThemeIsDark();
  const uint16_t bg=darkTheme?TFT_BLACK:TFT_WHITE;
  const uint16_t fg=darkTheme?TFT_WHITE:TFT_BLACK;
  epaper.fillScreen(bg);
  epaper.setTextColor(fg,bg);
  epaper.setTextDatum(TL_DATUM);

  float enabledTotal=0.0f;
  uint8_t enabledCount=0;
  uint8_t validCount=0;
  uint8_t enabledRuns[FLOW_RUN_COUNT]={0};
  for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
    if(!flowRunEnabled[run])continue;
    enabledRuns[enabledCount++]=run;
    if(flowRunValid[run]){
      enabledTotal+=flowRunFlow[run];
      validCount++;
    }
  }

  epaper.drawRect(6,6,epaper.width()-12,epaper.height()-12,fg);

  // Header: strong station identity + useful clock.
  int headerTextX=22;
  if(displayLogoExists()){
    drawStoredLogo(22,8,fg);
    headerTextX=218;
  }

  String hdrLocation=siteLSD;
  if(hdrLocation.length()>22)hdrLocation=hdrLocation.substring(0,22);
  epaper.setTextSize(3);
  epaper.drawString(hdrLocation.length()?String("LSD ")+hdrLocation:String("LSD --"),headerTextX,12);

  String hdrSite=siteName;
  if(hdrSite.length()>34)hdrSite=hdrSite.substring(0,34);
  epaper.setTextSize(1);
  if(hdrSite.length())epaper.drawString(hdrSite,headerTextX+2,46);

  epaper.setTextSize(2);
  epaper.setTextDatum(TR_DATUM);
  epaper.drawString(displayDateTime(),776,24);
  epaper.setTextDatum(TL_DATUM);
  epaper.drawFastHLine(18,66,764,fg);

  // Combined total remains a summary, not another well.
  int flowTop=76;
  if(displayCfg.showTotalFlow){
    epaper.setTextSize(1);
    epaper.drawString("TOTAL FLOW",26,78);
    epaper.setTextSize(4);
    epaper.drawString(
      validCount ? String(enabledTotal,(unsigned int)displayCfg.flowDecimals) : String("---"),
      180,72
    );
    epaper.setTextSize(2);
    epaper.drawString("E3 m3/day",565,88);
    epaper.setTextSize(1);
    epaper.drawString(String(validCount)+"/"+String(enabledCount)+" RUNS VALID",26,112);
    epaper.drawFastHLine(18,126,764,fg);
    flowTop=136;
  }

  // ePaper operator display is intentionally optimized for a maximum of six
  // visible flow runs. The calculation engine remains capable of ten runs.
  // Each card reserves vertical room for future PID SP/OUT and one spare line.
  if(enabledCount>0){
    uint8_t visibleCount=min((uint8_t)6,enabledCount);

    int cols=(visibleCount<=2)?1:((visibleCount<=4)?2:3);
    int rows=(visibleCount+cols-1)/cols;
    int gap=8;
    int left=18;
    int right=782;
    int usableW=right-left;
    int cardW=(usableW-gap*(cols-1))/cols;

    int bottomLimit=382;
    int usableH=bottomLimit-flowTop;
    int cardH=(usableH-gap*(rows-1))/rows;

    for(uint8_t slot=0;slot<visibleCount;slot++){
      uint8_t run=enabledRuns[slot];
      int col=slot%cols;
      int row=slot/cols;
      int x=left+col*(cardW+gap);
      int y=flowTop+row*(cardH+gap);

      epaper.drawRect(x,y,cardW,cardH,fg);

      String nm=flowRunName[run][0]?String(flowRunName[run]):("FLOW RUN "+String(run+1));
      String lsd=String(flowRunLSD[run]);
      if(nm.length()>18)nm=nm.substring(0,18);
      if(lsd.length()>18)lsd=lsd.substring(0,18);

      // Larger identity line for operators.
      epaper.setTextSize(2);
      epaper.drawString(nm,x+8,y+6);

      if(lsd.length()){
        epaper.setTextSize(2);
        epaper.drawString(lsd,x+8,y+28);
      }

      if(displayCfg.showRunStatus){
        epaper.setTextDatum(TR_DATUM);
        epaper.setTextSize(1);
        epaper.drawString(flowRunValid[run]?"OK":"BAD",x+cardW-8,y+8);
        epaper.setTextDatum(TL_DATUM);
      }

      // One clean flow line: value + engineering unit together.
      epaper.setTextSize(2);
      String flowLine=flowRunValid[run]
        ? String(flowRunFlow[run],(unsigned int)displayCfg.flowDecimals)+" E3 m3/day"
        : String("--- E3 m3/day");
      epaper.drawString(flowLine,x+8,y+54);

      // Process values remain visible but compact.
      if(displayCfg.showRunInputs){
        FlowResolvedPoint dp=resolveFlowRunInput(run,FLOW_ROLE_DP);
        FlowResolvedPoint sp=resolveFlowRunInput(run,FLOW_ROLE_STATIC);
        FlowResolvedPoint tp=resolveFlowRunInput(run,FLOW_ROLE_TEMP);

        epaper.setTextSize(1);
        epaper.drawString(
          "DP "+String(isfinite(dp.value)?dp.value:0.0f,1)+(dp.quality==FLOW_QUALITY_MANUAL?"*":"")+
          "  P "+String(isfinite(sp.value)?sp.value:0.0f,1)+(sp.quality==FLOW_QUALITY_MANUAL?"*":"")+
          "  T "+String(isfinite(tp.value)?tp.value:0.0f,1)+(tp.quality==FLOW_QUALITY_MANUAL?"*":""),
          x+8,y+78
        );
      }

      // Keep totals compact; control mode gets the larger operator line.
      epaper.setTextSize(1);
      epaper.drawString(
        "TODAY "+String(flowDay[run].volumeE3m3,2)+"  HRS "+String(flowDay[run].flowHours,1),
        x+8,y+92
      );

      epaper.setTextSize(2);
      String modeLine=flowPIDRt[run].pressureOverrideActive
        ? String("MODE P-OVERRIDE")
        : String("MODE ")+flowControlModeName(flowPIDCfg[run].mode);
      if(flowPIDCfg[run].mode!=FLOW_CTRL_DISABLED){
        modeLine+=" "+String(flowPIDRt[run].output,0)+"%";
      }
      epaper.drawString(modeLine,x+8,y+102);
    }

  }else{
    epaper.setTextSize(2);
    epaper.drawString("NO ENABLED FLOW RUNS",250,flowTop+70);
  }

  bool anyManualDisplayed=false;
  for(uint8_t slot=0;slot<min((uint8_t)6,enabledCount);slot++){
    uint8_t run=enabledRuns[slot];
    for(uint8_t role=0;role<3;role++){
      if(resolveFlowRunInput(run,role).quality==FLOW_QUALITY_MANUAL){
        anyManualDisplayed=true;
        break;
      }
    }
    if(anyManualDisplayed)break;
  }

  if(anyManualDisplayed){
    epaper.setTextDatum(TR_DATUM);
    epaper.setTextSize(1);
    epaper.drawString("* MANUAL",776,368);
    epaper.setTextDatum(TL_DATUM);
  }

  // Optional compact active alarm banner. ePaper shows the first active/latched
  // rule; the web dashboard continues to show all active alarms.
  if(displayCfg.showActiveAlarms){
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      RiotRuleRuntime &rt=riotRuleRt[i];
      if(!(rt.alarm||rt.latched))continue;
      float liveValue=0.0f;String liveQuality;
      riotRuleTagValue(riotRules[i].tagIndex,liveValue,liveQuality);
      String units=riotRuleTagUnits(riotRules[i].tagIndex);
      String label=strlen(riotRules[i].message)?String(riotRules[i].message):String(riotRules[i].name);
      String alarmLine="ALARM "+label+" "+String(isfinite(liveValue)?liveValue:0.0f,3);
      if(units.length())alarmLine+=" "+units;
      if(strlen(rt.firedAt)){
        String ts=rt.firedAt;
        int sp=ts.lastIndexOf(' ');
        alarmLine+=" @ ";
        alarmLine+=(sp>=0)?ts.substring(sp+1):ts;
      }
      if(alarmLine.length()>58)alarmLine=alarmLine.substring(0,58);
      epaper.setTextSize(2);
      epaper.setTextDatum(TL_DATUM);
      epaper.drawString(alarmLine,24,366);
      break;
    }
  }

  // Station discretes are always visible for operations.
  epaper.drawFastHLine(18,386,764,fg);
  epaper.setTextSize(1);
  epaper.drawString("STATION I/O",24,392);

  epaper.setTextSize(2);
  String di="DI";
  String dout="DO";
  for(uint8_t i=0;i<4;i++){
    di+="  "+String(i+1)+":"+(diState[i]?"ON":"OFF");
    dout+="  "+String(i+1)+":"+(doState[i]?"ON":"OFF");
  }
  epaper.drawString(di,24,408);
  epaper.drawString(dout,410,408);

  // Footer: communications + version, all useful field information.
  epaper.drawFastHLine(18,438,764,fg);
  epaper.setTextSize(1);
  if(displayCfg.showWiFi){
    epaper.drawString(
      String(WiFi.status()==WL_CONNECTED?"LAN ":"AP ")+activeIpString(),
      24,447
    );
  }

  if(displayCfg.showM31){
    // RIOT EPAPER M31 SAME ROW FOOTER V1
    // Keep communications information together on one footer row.
    epaper.setTextDatum(TL_DATUM);
    String m31Footer;
    if(m31TransportMode==M31_TRANSPORT_TCP){
      m31Footer="M31 TCP "+m31TcpHost;
    }else{
      m31Footer="M31 RTU ADD "+String(m31UnitId);
    }
    m31Footer+=" ";
    m31Footer+=!modbusCommsEnabled?"DISABLED":(modbusOnline?"ONLINE":"OFFLINE");
    epaper.drawString(m31Footer,190,447);
  }

  // Compact live power condition beside the firmware version.
  // Until the physical POWER_GOOD divider is enabled, report SUPPLY using the
  // software state and always include the measured battery voltage.
  epaper.setTextDatum(TR_DATUM);
  String powerFooter=powerOutageActive ? "BAT " : "SUPPLY ";
  powerFooter+=String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,2)+"V";
  // RIOT EPAPER SUPPLY LEFT SHIFT V1
  epaper.drawString(powerFooter,635,447);
  epaper.drawString(String(FW),776,447);
  epaper.setTextDatum(TL_DATUM);

  if(fullRefresh){
    epaper.update();
    epaperPartialRefreshes=0;
    epaperFlowPartialRefreshes=0;
  }else{
#ifdef USE_PARTIAL_EPAPER
    if(epaperPartialRegionMask&EPAPER_REGION_HEADER)
      epaper.updataPartial(16,8,768,64);
    if(epaperPartialRegionMask&EPAPER_REGION_FLOW){
      epaper.updataPartial(16,64,768,320);
      epaperFlowPartialRefreshes++;
    }
    if(epaperPartialRegionMask&EPAPER_REGION_ALARM){
      scrubEpaperAlarmBand();
      epaper.updataPartial(16,352,768,36);
    }
    if(epaperPartialRegionMask&EPAPER_REGION_IO)
      epaper.updataPartial(16,384,768,56);
    if(epaperPartialRegionMask&EPAPER_REGION_FOOTER)
      epaper.updataPartial(16,432,768,40);
    epaperPartialRefreshes++;
#else
    epaper.update();
    epaperPartialRefreshes=0;
#endif
  }
#endif
}

void setupWeb(){
  server.on("/ping",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"text/plain","OK\n");
  });

  server.on("/",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow Operator Overview</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#090d0f;color:#eee;margin:0}
.wrap{max-width:1180px;margin:auto;padding:12px}
.top{display:grid;grid-template-columns:1fr 220px 1fr;align-items:center;gap:14px;border-bottom:1px solid #344;padding:6px 2px 11px;min-height:76px}
.identity{min-width:0}.logoWrap{height:58px;display:flex;align-items:center;justify-content:center}.logoCanvas{display:none;background:transparent;max-width:180px;max-height:56px}.title{font-size:25px;font-weight:700;color:#63e6be}.sub{font-size:17px;color:#ddd;text-align:left;line-height:1.35}.dateTime{font-size:18px;color:#ddd;text-align:right;line-height:1.35}
.footer{display:grid!important;grid-template-columns:auto 1fr auto auto;align-items:center;gap:16px}.footerHealth{display:flex;justify-content:center;gap:12px;flex-wrap:wrap}.footerSupply{white-space:nowrap}.footerVersion{text-align:right;white-space:nowrap}
.nav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.nav a,.drop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:17px;cursor:pointer}
.drop{position:relative}.menu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:200px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.drop:hover .menu,.drop:focus-within .menu{display:block}.menu a{display:block;margin:2px 0;white-space:nowrap}
.health{display:flex;gap:12px;flex-wrap:wrap;padding:7px 2px}
.h{font-weight:700;font-size:17px}.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;background:#ff6b6b}.ok .dot{background:#63e6be}.ok{color:#dffff6}.bad{color:#ff8b8b}
.alarmStrip{display:none;border:1px solid #8b3a3a;background:#241313;border-radius:8px;padding:9px 11px;margin:0 0 10px}
.alarmTitle{font-weight:700;color:#ff8b8b;margin-right:10px}.alarmLine{margin:3px 0;color:#ffd0d0}.alarmMeta{color:#aaa;font-size:13px}
.summary{display:grid;grid-template-columns:2fr 1fr;gap:10px;margin-bottom:10px}
.card{border:1px solid #455;border-radius:9px;background:#101516;padding:12px}
.cardTitle{font-size:17px;font-weight:700;color:#75d7ff;margin-bottom:9px}
.total{font-size:56px;font-weight:700;color:#63e6be;line-height:1;margin:18px 0 4px}
.unit{font-size:16px;color:#ccc}
.runGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:10px}
.run{border:1px solid #455;border-radius:9px;background:#101516;padding:12px}
.run.badRun{border-color:#734}.runTitle{display:flex;justify-content:space-between;gap:8px;align-items:center}
.runName{font-size:18px;font-weight:700;color:#75d7ff}.state{font-size:16px;font-weight:700}
.flow{font-size:34px;font-weight:700;color:#63e6be;margin:10px 0 2px}.badRun .flow{color:#ff8b8b}
.inputs{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:10px}
.totals{display:grid;grid-template-columns:repeat(2,1fr);gap:6px;margin-top:8px}
.input{background:#0c1112;border:1px solid #344;border-radius:6px;padding:8px}.lbl{font-size:16px;color:#d0d5d5;line-height:1.35}.val{font-size:19px;font-weight:700}
.ioGrid{display:grid;grid-template-columns:repeat(4,1fr);gap:7px}.io{background:#0c1112;border:1px solid #344;border-radius:7px;padding:9px;text-align:center}.io b{display:block;color:#75d7ff;margin-bottom:3px}
.footer{border-top:1px solid #344;margin-top:10px;padding:10px 2px;color:#ddd;font-size:16px;display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap}
/* RIOT A9A MOBILE DASHBOARD POLISH V1 */
@media(max-width:760px){
 .wrap{padding:8px}
 .top{grid-template-columns:1fr;gap:6px;min-height:0;padding:4px 2px 9px;text-align:center}
 .identity{order:2;text-align:center}
 .logoWrap{order:1;height:auto;min-height:42px;margin:0 auto}
 .logoCanvas{max-width:150px;max-height:48px}
 .title{font-size:21px;line-height:1.2;text-align:center;overflow-wrap:anywhere}
 .sub{font-size:16px;line-height:1.25;text-align:center;overflow-wrap:anywhere}
 .dateTime{order:3;font-size:14px;line-height:1.2;text-align:center;color:#bbb}
 .nav{margin:8px 0 10px}
 .summary{grid-template-columns:1fr}
 .inputs{grid-template-columns:1fr}
 .total{font-size:44px}
 .ioGrid{grid-template-columns:repeat(2,1fr)}
 .footer{grid-template-columns:1fr 1fr!important;gap:7px 10px;padding:9px 2px;font-size:13px}
 .footerHealth{grid-column:1 / -1;justify-content:flex-start;gap:10px;order:3}
 .footerSupply{text-align:right;white-space:normal}
 .footerVersion{text-align:right;white-space:normal;overflow-wrap:anywhere}
}
@media(max-width:420px){
 .title{font-size:19px}
 .sub{font-size:15px}
 .footer{grid-template-columns:1fr!important}
 .footerHealth{grid-column:1;order:2}
 .footerSupply,.footerVersion{text-align:left}
 .footerSupply{order:3}.footerVersion{order:4}
}
</style></head><body><div class=wrap>
<div class=top>
 <div class=identity><div><div id=siteTitle class=title>LSD --</div><div id=siteLoc class=sub>MultiFlow Site</div></div></div>
 <div class=logoWrap><canvas id=siteLogo class=logoCanvas></canvas></div>
 <div id=dateTime class=dateTime>--</div>
</div>
<!-- EP32 MENU CLEANUP GATE1 SIX GROUP NAV V1 -->
<div class=nav>
<a href="/">Dashboard</a>
<div class=drop tabindex=0><button type=button>Flow &#9662;</button><div class=menu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=drop tabindex=0><button type=button>I/O &#9662;</button><div class=menu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=drop tabindex=0><button type=button>Setup &#9662;</button><div class=menu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=drop tabindex=0><button type=button>Maintenance &#9662;</button><div class=menu>
<a href="/tags">Tags</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/system-status">System Status</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>

<div id=alarmStrip class=alarmStrip></div>
<div class=summary>
 <div class=card>
  <div class=cardTitle>ENABLED RUN TOTAL</div>
  <div id=total class=total>---</div><div class=unit>E3 m3/day</div>
  <div id=runSummary class=lbl>--</div>
 </div>
</div>

<div id=runs class=runGrid></div>
<div class=card style="margin-top:10px">
 <div class=cardTitle>STATION DIGITAL I/O</div>
 <div id=io class=ioGrid></div>
</div>
<div class=footer>
 <span id=footerIp>IP --</span>
 <span id=health class=footerHealth>Loading controller status...</span>
 <span id=footerSupply class=footerSupply>SUPPLY --</span>
 <span id=footerVersion class=footerVersion>--</span>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
const fmt=(v,d=2)=>Number.isFinite(Number(v))?Number(v).toFixed(d):'---';
function healthItem(label,ok){return '<span class="h '+(ok?'ok':'bad')+'"><span class=dot></span>'+label+'</span>'}
function runCard(r){
 const q=r.inputs||[];
 const good=r.valid;
 return '<div class="run '+(good?'':'badRun')+'">'+
  '<div class=runTitle><span class=runName>'+(r.displayName||('FLOW RUN '+r.run))+'</span><span class="state '+(good?'ok':'bad')+'">'+(good?'VALID':'INVALID')+'</span></div>'+
  '<div class=flow>'+(good?fmt(r.flow,3):'---')+'</div><div class=unit>E3 m3/day</div>'+
  '<div class=inputs>'+
   '<div class=input><div class=lbl>DP</div><div class=val>'+fmt(q[0]?.value,2)+(q[0]?.quality==='MANUAL'?'*':'')+'</div><div class=lbl>'+String(q[0]?.quality||'')+'</div></div>'+
   '<div class=input><div class=lbl>STATIC</div><div class=val>'+fmt(q[1]?.value,2)+(q[1]?.quality==='MANUAL'?'*':'')+'</div><div class=lbl>'+String(q[1]?.quality||'')+'</div></div>'+
   '<div class=input><div class=lbl>TEMP</div><div class=val>'+fmt(q[2]?.value,2)+(q[2]?.quality==='MANUAL'?'*':'')+'</div><div class=lbl>'+String(q[2]?.quality||'')+'</div></div>'+
  '</div></div>';
}
async function loadHeaderLogo(){
 try{
  const r=await fetch('/api/display-logo-bin?x='+Date.now(),{cache:'no-store'});
  const cv=$('siteLogo');
  if(!r.ok){cv.style.display='none';return;}
  const b=new Uint8Array(await r.arrayBuffer());
  if(b.length<5){cv.style.display='none';return;}
  const w=b[0]|(b[1]<<8),h=b[2]|(b[3]<<8);
  if(!w||!h||w>180||h>56){cv.style.display='none';return;}

  // Web headers are dark, so identify the logo artwork independently of
  // ePaper polarity. The dominant bitmap state is treated as background and
  // left transparent; only the minority state is drawn as white artwork.
  let setCount=0;
  const pixelCount=w*h;
  for(let bit=0;bit<pixelCount;bit++){
    if(b[4+(bit>>3)]&(0x80>>(bit&7)))setCount++;
  }
  const drawSet=setCount<=pixelCount/2;

  cv.width=w;cv.height=h;cv.style.display='block';
  const ctx=cv.getContext('2d');
  ctx.clearRect(0,0,w,h);
  ctx.fillStyle='#fff';

  for(let y=0;y<h;y++)for(let x=0;x<w;x++){
    const bit=y*w+x;
    const set=(b[4+(bit>>3)]&(0x80>>(bit&7)))!==0;
    if(set===drawSet)ctx.fillRect(x,y,1,1);
  }
 }catch(_){$('siteLogo').style.display='none'}
}
function dashboardAlarmValue(r){
 const u=r.units?(' '+r.units):'';
 return fmt(r.value,3)+u;
}
function renderDashboardAlarms(rr){
 const box=$('alarmStrip');
 const a=(rr||[]).filter(r=>r.alarm||r.latched);
 if(!a.length){box.style.display='none';box.innerHTML='';return;}
 box.style.display='block';
 const unack=a.filter(r=>!r.acknowledged);
 box.innerHTML='<span class=alarmTitle>ACTIVE ALARMS</span>'+
   (unack.length?'<button onclick="ackAllAlarms()" style="margin-left:8px">ACK ALL ('+unack.length+')</button>':' <span class=alarmMeta>ALL ACKNOWLEDGED</span>')+
   a.map(r=>'<div class=alarmLine><b>'+(r.message||r.name||('Rule '+(r.index+1)))+'</b> - '+dashboardAlarmValue(r)+
   ' <span class=alarmMeta>'+(r.acknowledged?'ACKNOWLEDGED - ':'UNACKNOWLEDGED - ')+'Triggered '+(r.firedAt||'time unavailable')+' at '+fmt(r.fireValue,3)+(r.units?(' '+r.units):'')+' (Rule '+(r.index+1)+')</span>'+
   (!r.acknowledged?' <button onclick="ackAlarm('+r.index+')">ACK</button>':'')+
   ' <button onclick="clearAlarm('+r.index+')">CLEAR</button></div>').join('');
}
async function ackAlarm(index){
 try{
  await fetch('/api/rule-ack',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+encodeURIComponent(index)});
  refresh();
 }catch(_){}
}
async function ackAllAlarms(){
 try{
  await fetch('/api/rules-ack-all',{method:'POST'});
  refresh();
 }catch(_){}
}
async function clearAlarm(index){
 try{
  await fetch('/api/rule-reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+encodeURIComponent(index)});
  refresh();
 }catch(_){}
}
async function refresh(){
 try{
  const now=Date.now();
  const [s,x,c,ar]=await Promise.all([
    fetch('/status?x='+now,{cache:'no-store'}).then(r=>r.json()),
    fetch('/api/flow-runs?x='+now,{cache:'no-store'}).then(r=>r.json()),
    fetch('/api/display-config?x='+now,{cache:'no-store'}).then(r=>r.json()),
    fetch('/api/rules?x='+now,{cache:'no-store'}).then(r=>r.json())
  ]);
  renderDashboardAlarms(c.showActiveAlarms?(ar.rules||[]):[]);
  $('siteTitle').textContent=(s.siteLSD&&s.siteLSD.length)?('LSD '+s.siteLSD):'LSD --';
  $('siteLoc').textContent=s.siteName||'MultiFlow Site';
  $('dateTime').textContent=s.dateTime||'--';
  $('footerIp').textContent='IP '+s.ip;
  $('footerVersion').textContent=s.firmware||'Riot-Flow-v1';
  $('footerSupply').textContent='SUPPLY --';
  const enabled=x.runs.filter(r=>r.enabled);
  const valid=enabled.filter(r=>r.valid);
  const total=valid.reduce((a,r)=>a+Number(r.flow||0),0);
  let hh='';
  if(c.showWiFi)hh+=healthItem('WiFi',true);
  if(c.showM31)hh+=healthItem('M31',s.modbus==='ONLINE');
  hh+=healthItem('FLOW',enabled.length>0&&valid.length===enabled.length);
  $('health').innerHTML=hh;
  const totalCard=$('total').closest('.card');
  totalCard.style.display=c.showTotalFlow?'block':'none';
  $('total').textContent=valid.length?fmt(total,c.flowDecimals):'---';
  $('runSummary').textContent=valid.length+' of '+enabled.length+' enabled runs valid';
  $('runs').innerHTML=enabled.map(r=>{
    const q=r.inputs||[];
    const good=r.valid;
    return '<div class="run '+(good?'':'badRun')+'">'+
      '<div class=runTitle><span class=runName>'+(r.displayName||('FLOW RUN '+r.run))+'</span>'+
      (c.showRunStatus?'<span class="state '+(good?'ok':'bad')+'">'+(good?'VALID':'INVALID')+'</span>':'')+'</div>'+
      '<div class=flow>'+(good?fmt(r.flow,c.flowDecimals):'---')+'</div><div class=unit>E3 m3/day</div>'+
      (c.showRunInputs?'<div class=inputs>'+
       '<div class=input><div class=lbl>DP</div><div class=val>'+fmt(q[0]?.value,2)+'</div><div class=lbl>'+String(q[0]?.quality||'')+'</div></div>'+
       '<div class=input><div class=lbl>STATIC</div><div class=val>'+fmt(q[1]?.value,2)+'</div><div class=lbl>'+String(q[1]?.quality||'')+'</div></div>'+
       '<div class=input><div class=lbl>TEMP</div><div class=val>'+fmt(q[2]?.value,2)+'</div><div class=lbl>'+String(q[2]?.quality||'')+'</div></div></div>':'')+
      '<div class=totals>'+
       '<div class=input><div class=lbl>FLOW TODAY</div><div class=val>'+fmt(r.flowToday,3)+'</div><div class=lbl>E3 m3</div></div>'+
       '<div class=input><div class=lbl>FLOW HRS</div><div class=val>'+fmt(r.flowHours,2)+'</div><div class=lbl>Hours</div></div>'+
      '</div>'+
      '<div class=input style="margin-top:8px"><div class=lbl>CONTROL MODE</div><div class=val>'+
       (r.pressureOverrideActive?'PRESSURE OVERRIDE':r.controlMode)+
       (r.controlMode!=='DISABLED'?' | OUT '+fmt(r.controlOut,1)+'%':'')+
      '</div></div>'+
      '</div>';
  }).join('')||'<div class=card>No enabled flow runs</div>';
  let h='';
  for(let i=0;i<4;i++)h+='<div class=io><b>DI'+(i+1)+'</b>'+(s.di[i]?'ON':'OFF')+'</div>';
  for(let i=0;i<4;i++)h+='<div class=io><b>DO'+(i+1)+'</b>'+(s.do[i]?'ON':'OFF')+'</div>';
  $('io').innerHTML=h;
 }catch(e){
  $('health').innerHTML='<span class="h bad"><span class=dot></span>Dashboard data error: '+e.message+'</span>';
 }
}
loadHeaderLogo();refresh();setInterval(refresh,1500);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/epaper-refresh",HTTP_POST,[](){
    if(!epaperRuntimeEnabled){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"headless display mode active\"}");
      return;
    }
    requestFullEPaperRefresh();
    server.send(200,"application/json","{\"ok\":true,\"refresh\":\"full\"}");
  });

  server.on("/api/site-identity",HTTP_GET,[](){
    String j="{\"siteName\":\""+siteName+"\"";
    j+=",\"siteLSD\":\""+siteLSD+"\"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/site-identity",HTTP_POST,[](){
    String name=server.hasArg("siteName")?server.arg("siteName"):siteName;
    String lsd=server.hasArg("siteLSD")?server.arg("siteLSD"):siteLSD;
    saveSiteIdentity(name,lsd);

    // Return the controller's accepted values immediately. The NVS write is
    // synchronous; avoid turning a successful operator update into a false
    // browser error because of an over-strict readback comparison.
    String j="{\"ok\":true";
    j+=",\"siteName\":\""+siteName+"\"";
    j+=",\"siteLSD\":\""+siteLSD+"\"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/display-config-storage",HTTP_GET,[](){
    String j="{\"loaded\":"+String(displayFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(displayFlashLastSaveOK?"true":"false");
    j+=",\"status\":\""+displayFlashStatus+"\"";
    j+=",\"path\":\""+String(DISPLAY_CFG_PATH)+"\"";
    j+=",\"bytes\":"+String((unsigned)sizeof(DisplayFlashConfigV1));
    j+=",\"displayMode\":"+String(displayCfg.displayMode);
    j+=",\"displayModeName\":\""+String(riotDisplayModeName(displayCfg.displayMode))+"\"";
    j+=",\"epaperRuntimeEnabled\":"+String(epaperRuntimeEnabled?"true":"false");
    j+=",\"themeMode\":"+String(displayCfg.themeMode);
    j+=",\"lightHour\":"+String(displayCfg.lightHour);
    j+=",\"darkHour\":"+String(displayCfg.darkHour);
    j+=",\"logoInvert\":"+String(epaperLogoInvert?"true":"false")+"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/display-config-retire-legacy-nvs",HTTP_POST,[](){
    if(!displayFlashLoaded){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"valid flash display config must be loaded first\"}");
      return;
    }
    bool ok=false;
    if(prefs.begin("display",false)){ok=prefs.clear();prefs.end();}
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:500,"application/json",ok?
      "{\"ok\":true,\"cleared\":[\"display\"],\"flashAuthoritative\":true}":
      "{\"ok\":false,\"error\":\"display NVS clear failed\"}");
  });

  server.on("/api/display-config",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",displayConfigJson());
  });

  server.on("/api/display-config",HTTP_POST,[](){
    if(server.hasArg("displayMode"))displayCfg.displayMode=(uint8_t)constrain(server.arg("displayMode").toInt(),0,2);
    if(server.hasArg("flowDecimals"))displayCfg.flowDecimals=(uint8_t)constrain(server.arg("flowDecimals").toInt(),0,5);
    if(server.hasArg("showRunStatus"))displayCfg.showRunStatus=server.arg("showRunStatus")=="1";
    if(server.hasArg("showRunInputs"))displayCfg.showRunInputs=server.arg("showRunInputs")=="1";
    if(server.hasArg("showTotalFlow"))displayCfg.showTotalFlow=server.arg("showTotalFlow")=="1";
    if(server.hasArg("showWiFi"))displayCfg.showWiFi=server.arg("showWiFi")=="1";
    if(server.hasArg("showM31"))displayCfg.showM31=server.arg("showM31")=="1";
    if(server.hasArg("showActiveAlarms"))displayCfg.showActiveAlarms=server.arg("showActiveAlarms")=="1";
    if(server.hasArg("themeMode"))displayCfg.themeMode=(uint8_t)constrain(server.arg("themeMode").toInt(),0,2);
    if(server.hasArg("lightHour"))displayCfg.lightHour=(uint8_t)constrain(server.arg("lightHour").toInt(),0,23);
    if(server.hasArg("darkHour"))displayCfg.darkHour=(uint8_t)constrain(server.arg("darkHour").toInt(),0,23);
    if(server.hasArg("epaperLogoInvert"))epaperLogoInvert=server.arg("epaperLogoInvert")=="1";
    if(server.hasArg("siteName") || server.hasArg("siteLSD")){
      saveSiteIdentity(
        server.hasArg("siteName")?server.arg("siteName"):siteName,
        server.hasArg("siteLSD")?server.arg("siteLSD"):siteLSD
      );
    }
    saveDisplayConfig();
    requestFullEPaperRefresh();
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",displayConfigJson());
  });

  server.on("/api/display-presence-probe",HTTP_GET,[](){
    RiotEpaperProbeResult r=riotProbeEpaperBusyPassive();
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",riotEpaperProbeJson(r));
  });

  server.on("/api/display-debug",HTTP_GET,[](){
    String j="{\"siteName\":\""+siteName+"\"";
    j+=",\"siteLSD\":\""+siteLSD+"\"";
    j+=",\"logoStored\":"+String(displayLogoExists()?"true":"false");
    if(displayLogoExists()){
      File f=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_READ);
      uint8_t hdr[4]={0};
      if(f && f.read(hdr,4)==4){
        uint16_t w=(uint16_t)hdr[0]|((uint16_t)hdr[1]<<8);
        uint16_t h=(uint16_t)hdr[2]|((uint16_t)hdr[3]<<8);
        j+=",\"logoW\":"+String(w);
        j+=",\"logoH\":"+String(h);
        size_t bytes=((size_t)w*h+7)/8;
        uint32_t setBits=0;
        for(size_t n=0;n<bytes;n++){
          int v=f.read();
          if(v<0)break;
          uint8_t b=(uint8_t)v;
          for(uint8_t k=0;k<8;k++)if(b&(1U<<k))setBits++;
        }
        j+=",\"logoSetBits\":"+String(setBits);
      }
      if(f)f.close();
    }
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/display-logo-bin",HTTP_GET,[](){
    if(!displayLogoExists()){
      server.send(404,"text/plain","No logo");
      return;
    }
    File f=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_READ);
    if(!f){
      server.send(500,"text/plain","Logo open failed");
      return;
    }
    server.sendHeader("Cache-Control","no-store");
    server.streamFile(f,"application/octet-stream");
    f.close();
  });

  server.on("/api/display-logo",HTTP_GET,[](){
    String j="{\"stored\":"+String(displayLogoExists()?"true":"false");
    if(displayLogoExists()){
      File f=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_READ);
      uint8_t hdr[4]={0};
      if(f && f.read(hdr,4)==4){
        uint16_t w=(uint16_t)hdr[0]|((uint16_t)hdr[1]<<8);
        uint16_t h=(uint16_t)hdr[2]|((uint16_t)hdr[3]<<8);
        j+=",\"width\":"+String(w);
        j+=",\"height\":"+String(h);
      }
      if(f)f.close();
    }
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/display-logo",HTTP_DELETE,[](){
    bool existed=SPIFFS.exists(DISPLAY_LOGO_PATH);
    bool ok=!existed || SPIFFS.remove(DISPLAY_LOGO_PATH);
    if(ok)requestFullEPaperRefresh();
    server.send(ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/api/display-logo-data",HTTP_POST,[](){
    if(!server.hasArg("hex")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"hex required\"}");
      return;
    }
    String hex=server.arg("hex");
    hex.trim();
    if(hex.length()<10 || (hex.length()&1) || hex.length()>2600){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid hex length\"}");
      return;
    }

    size_t n=hex.length()/2;
    uint8_t* data=(uint8_t*)malloc(n);
    if(!data){
      server.send(500,"application/json","{\"ok\":false,\"error\":\"memory\"}");
      return;
    }
    auto hv=[](char ch)->int{
      if(ch>='0'&&ch<='9')return ch-'0';
      if(ch>='a'&&ch<='f')return ch-'a'+10;
      if(ch>='A'&&ch<='F')return ch-'A'+10;
      return -1;
    };
    bool valid=true;
    for(size_t i=0;i<n;i++){
      int a=hv(hex[i*2]),b=hv(hex[i*2+1]);
      if(a<0||b<0){valid=false;break;}
      data[i]=(uint8_t)((a<<4)|b);
    }
    if(!valid || n<5){
      free(data);
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid hex\"}");
      return;
    }

    uint16_t w=(uint16_t)data[0]|((uint16_t)data[1]<<8);
    uint16_t h=(uint16_t)data[2]|((uint16_t)data[3]<<8);
    size_t expected=4+(((size_t)w*h+7)/8);
    if(!w||!h||w>DISPLAY_LOGO_MAX_W||h>DISPLAY_LOGO_MAX_H||n!=expected){
      free(data);
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid dimensions\"}");
      return;
    }

    uint32_t setBits=0;
    for(size_t i=4;i<n;i++){
      uint8_t b=data[i];
      for(uint8_t k=0;k<8;k++)if(b&(1U<<k))setBits++;
    }
    if(!setBits){
      free(data);
      server.send(400,"application/json","{\"ok\":false,\"error\":\"empty bitmap\"}");
      return;
    }

    File out=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_WRITE);
    bool ok=false;
    if(out){
      size_t written=out.write(data,n);
      out.flush();
      out.close();
      ok=written==n && displayLogoExists();
    }
    free(data);

    if(ok)requestFullEPaperRefresh();
    String j="{\"ok\":"+String(ok?"true":"false");
    j+=",\"width\":"+String(w);
    j+=",\"height\":"+String(h);
    j+=",\"setBits\":"+String(setBits)+"}";
    server.send(ok?200:500,"application/json",j);
  });

  server.on("/api/display-logo",HTTP_POST,
    [](){
      bool ok=logoUploadOK && displayLogoExists();
      if(ok)requestFullEPaperRefresh();
      server.send(ok?200:400,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"invalid logo bitmap\"}");
      logoUploadOK=false;
    },
    [](){
      HTTPUpload& upload=server.upload();
      if(upload.status==UPLOAD_FILE_START){
        logoUploadOK=false;
        if(logoUploadFile)logoUploadFile.close();
        SPIFFS.remove(DISPLAY_LOGO_PATH);
        logoUploadFile=SPIFFS.open(DISPLAY_LOGO_PATH,FILE_WRITE);
      }else if(upload.status==UPLOAD_FILE_WRITE){
        if(logoUploadFile && upload.currentSize){
          logoUploadFile.write(upload.buf,upload.currentSize);
        }
      }else if(upload.status==UPLOAD_FILE_END){
        if(logoUploadFile){
          logoUploadFile.flush();
          logoUploadFile.close();
        }
        logoUploadOK=displayLogoExists();
        if(!logoUploadOK)SPIFFS.remove(DISPLAY_LOGO_PATH);
      }else if(upload.status==UPLOAD_FILE_ABORTED){
        if(logoUploadFile)logoUploadFile.close();
        SPIFFS.remove(DISPLAY_LOGO_PATH);
        logoUploadOK=false;
      }
    }
  );

  server.on("/display-setup",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow Display Setup</title>
<style>
body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:900px}a{color:#63e6be}
.card{border:1px solid #444;border-radius:9px;padding:14px;margin:10px 0;background:#171717}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;border-bottom:1px solid #303030;padding:9px 0}
.row:last-child{border-bottom:0}select,button,input{background:#222;color:#fff;border:1px solid #666;padding:8px;border-radius:5px}
input[type=text]{width:60%}.ok{color:#7f7}.muted{color:#aaa}.preview{background:#fff;border:1px solid #666;padding:6px;max-width:360px}.preview canvas{max-width:100%;height:auto}
</style></head><body>
<h2>Operator Display Setup</h2>
// EP32 MENU CLEANUP GATE2 SECONDARY NAV V3
// EP32 MENU CLEANUP GATE3 FIVE GROUP NAV V1
// EP32 MENU CLEANUP GATE4 UNIFIED DROPDOWN NAV V1
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div class=card>
<h3>Site Identity</h3>
<div class=row><span>Site Name</span><input id=siteName type=text maxlength=40></div>
<div class=row><span>Site LSD</span><input id=siteLSD type=text maxlength=48 placeholder="Example: 04-12-045-06W5"></div>
<div style="margin-top:10px"><button onclick=saveIdentity()>Save Site Identity</button> <span id=identityMsg class=ok></span></div>
</div>
<div class=card>
<h3>Site Logo</h3>
<p class=muted>Select a PNG or JPG. The browser scales and converts it to a 1-bit ePaper bitmap before upload. Maximum stored size is 180 x 56 pixels.</p>
<input id=logoFile type=file accept="image/png,image/jpeg,image/webp">
<div class=row><span>Logo conversion</span><select id=logoInvert onchange="if(e('logoSource').width>1)logoBitmapBytes()"><option value=0>Auto background</option><option value=1>Reverse selection</option></select></div>
<div class=row><span>Logo render polarity</span><select id=epaperLogoInvert><option value=0>Normal</option><option value=1>Reverse</option></select></div>
<div style="margin-top:10px"><button onclick=prepareLogo()>Preview Logo</button> <button onclick=uploadLogo()>Upload Logo</button> <button onclick=deleteLogo()>Delete Logo</button></div>
<canvas id=logoSource width=1 height=1 style="display:none"></canvas>
<div class=preview style="margin-top:10px"><canvas id=logoCanvas width=180 height=56></canvas></div>
<div id=logoState class=muted style="margin-top:7px"></div>
</div>
<div class=card>
<h3>Display Hardware</h3>
<div class=row><span>Display mode</span><select id=displayMode><option value=0>AUTO</option><option value=1>EPAPER</option><option value=2>HEADLESS</option></select></div>
<p class=muted>AUTO passively probes the ePaper BUSY line at boot: a driven signal enables ePaper, while floating/ambiguous input safely falls back to HEADLESS. EPAPER forces the display; HEADLESS never initializes it.</p>
</div>
<div class=card>
<h3>ePaper Theme</h3>
<div class=row><span>Theme</span><select id=theme><option value=0>Light</option><option value=1>Dark</option><option value=2>Auto</option></select></div>
<div class=row><span>Light starts</span><select id=lightHr><option value=0>00:00</option><option value=1>01:00</option><option value=2>02:00</option><option value=3>03:00</option><option value=4>04:00</option><option value=5>05:00</option><option value=6>06:00</option><option value=7>07:00</option><option value=8>08:00</option><option value=9>09:00</option><option value=10>10:00</option><option value=11>11:00</option><option value=12>12:00</option><option value=13>13:00</option><option value=14>14:00</option><option value=15>15:00</option><option value=16>16:00</option><option value=17>17:00</option><option value=18>18:00</option><option value=19>19:00</option><option value=20>20:00</option><option value=21>21:00</option><option value=22>22:00</option><option value=23>23:00</option></select></div>
<div class=row><span>Dark starts</span><select id=darkHr><option value=0>00:00</option><option value=1>01:00</option><option value=2>02:00</option><option value=3>03:00</option><option value=4>04:00</option><option value=5>05:00</option><option value=6>06:00</option><option value=7>07:00</option><option value=8>08:00</option><option value=9>09:00</option><option value=10>10:00</option><option value=11>11:00</option><option value=12>12:00</option><option value=13>13:00</option><option value=14>14:00</option><option value=15>15:00</option><option value=16>16:00</option><option value=17>17:00</option><option value=18>18:00</option><option value=19>19:00</option><option value=20>20:00</option><option value=21>21:00</option><option value=22>22:00</option><option value=23>23:00</option></select></div>
<p class=muted>Auto switches by the controller's local clock and uses a full refresh when the theme changes.</p>
</div>
<div class=card>
<h3>Flow Display</h3>
<div class=row><span>Flow decimals</span><select id=dec><option>0</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select></div>
<div class=row><span>Show run status</span><select id=runstat><option value=1>YES</option><option value=0>NO</option></select></div>
<div class=row><span>Show DP / Static / Temperature</span><select id=runin><option value=1>YES</option><option value=0>NO</option></select></div>
<div class=row><span>Show combined enabled-run total</span><select id=total><option value=1>YES</option><option value=0>NO</option></select></div>
</div>
<div class=card>
<h3>System Status</h3>
<div class=row><span>WiFi indicator</span><select id=wifi><option value=1>YES</option><option value=0>NO</option></select></div>
<div class=row><span>M31 indicator</span><select id=m31><option value=1>YES</option><option value=0>NO</option></select></div>
<div class=row><span>Active alarm banner</span><select id=alarms><option value=1>YES</option><option value=0>NO</option></select></div>
</div>
<div class=card><button id=saveBtn onclick=save() disabled>Save Display Setup</button> <button onclick=refreshPaper()>Force Full ePaper Refresh</button> <span id=msg class=ok>Loading current settings...</span>
</div>
<script>
const e=id=>document.getElementById(id);
let displaySettingsLoaded=false;
async function load(){
 displaySettingsLoaded=false;
 e('saveBtn').disabled=true;
 e('msg').textContent='Loading current settings...';
 try{
   const r=await fetch('/api/display-config?x='+Date.now(),{cache:'no-store'});
   if(!r.ok)throw new Error('display config HTTP '+r.status);
   const x=await r.json();
   e('siteName').value=x.siteName||'';
   e('siteLSD').value=x.siteLSD||'';
   e('epaperLogoInvert').value=x.epaperLogoInvert?'1':'0';
   e('displayMode').value=String(typeof x.displayMode==='number'?x.displayMode:0);
   e('theme').value=String(typeof x.themeMode==='number'?x.themeMode:0);
   e('lightHr').value=String(typeof x.lightHour==='number'?x.lightHour:7);
   e('darkHr').value=String(typeof x.darkHour==='number'?x.darkHour:19);
   e('dec').value=String(x.flowDecimals);
   e('runstat').value=x.showRunStatus?'1':'0';
   e('runin').value=x.showRunInputs?'1':'0';
   e('total').value=x.showTotalFlow?'1':'0';
   e('wifi').value=x.showWiFi?'1':'0';
   e('m31').value=x.showM31?'1':'0';
   e('alarms').value=x.showActiveAlarms?'1':'0';
   displaySettingsLoaded=true;
   e('saveBtn').disabled=false;
   e('msg').textContent='Current settings loaded';
   try{
     const lr=await fetch('/api/display-logo?x='+Date.now(),{cache:'no-store'});
     const l=await lr.json();
     e('logoState').textContent=l.stored?('Logo stored: '+l.width+' x '+l.height):'No logo stored';
   }catch(_){
     e('logoState').textContent='Logo status unavailable';
   }
 }catch(err){
   e('msg').textContent='LOAD FAILED - settings not changed';
   e('logoState').textContent=String(err&&err.message?err.message:err);
 }
}
async function saveIdentity(){
 let p=new URLSearchParams({siteName:e('siteName').value,siteLSD:e('siteLSD').value});
 let r=await fetch('/api/site-identity',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};
 try{x=await r.json()}catch(_){}
 if(r.ok&&x.ok){
   e('siteName').value=x.siteName||'';
   e('siteLSD').value=x.siteLSD||'';
   e('identityMsg').textContent='Saved: '+(x.siteName||'')+' | LSD '+(x.siteLSD||'--');
   return true;
 }
 e('identityMsg').textContent='SAVE FAILED';
 return false;
}
async function save(){
 if(!displaySettingsLoaded){e('msg').textContent='Cannot save until current settings load';return;}
 let p=new URLSearchParams({siteName:e('siteName').value,siteLSD:e('siteLSD').value,displayMode:e('displayMode').value,epaperLogoInvert:e('epaperLogoInvert').value,themeMode:e('theme').value,lightHour:e('lightHr').value,darkHour:e('darkHr').value,flowDecimals:e('dec').value,showRunStatus:e('runstat').value,showRunInputs:e('runin').value,showTotalFlow:e('total').value,showWiFi:e('wifi').value,showM31:e('m31').value,showActiveAlarms:e('alarms').value});
 let r=await fetch('/api/display-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 e('msg').textContent=r.ok?'Saved - full refresh requested':'SAVE FAILED';if(r.ok)load();
}
async function refreshPaper(){
 let r=await fetch('/api/epaper-refresh',{method:'POST'});
 e('msg').textContent=r.ok?'Full ePaper refresh requested':'REFRESH FAILED';
}
function logoBitmapBytes(){
 const src=e('logoSource'),sctx=src.getContext('2d'),im=sctx.getImageData(0,0,src.width,src.height);
 const bytes=new Uint8Array(4+Math.ceil(src.width*src.height/8));
 bytes[0]=src.width&255;bytes[1]=(src.width>>8)&255;bytes[2]=src.height&255;bytes[3]=(src.height>>8)&255;

 let transparent=0;
 for(let i=3;i<im.data.length;i+=4)if(im.data[i]<48)transparent++;
 const hasTransparency=transparent>0;

 function lumAt(x,y){
   const i=(y*src.width+x)*4;
   return 0.299*im.data[i]+0.587*im.data[i+1]+0.114*im.data[i+2];
 }
 // For opaque images, infer the background from the four corners. This handles
 // both dark logos on white backgrounds and white logos on dark backgrounds.
 let bgLum=255;
 if(!hasTransparency){
   bgLum=(lumAt(0,0)+lumAt(src.width-1,0)+lumAt(0,src.height-1)+lumAt(src.width-1,src.height-1))/4;
 }

 let setBits=0;
 const cv=e('logoCanvas');cv.width=src.width;cv.height=src.height;
 const ctx=cv.getContext('2d');ctx.fillStyle='#fff';ctx.fillRect(0,0,cv.width,cv.height);ctx.fillStyle='#000';

 for(let y=0;y<src.height;y++)for(let x=0;x<src.width;x++){
   const i=(y*src.width+x)*4;
   const a=im.data[i+3];
   const lum=0.299*im.data[i]+0.587*im.data[i+1]+0.114*im.data[i+2];

   // Transparent PNG: alpha defines the artwork.
   // Opaque image: artwork is whatever differs substantially from corner background.
   let ink=hasTransparency ? a>48 : Math.abs(lum-bgLum)>55;

   // Optional manual reverse remains available for unusual source graphics.
   if(e('logoInvert').value==='1')ink=!ink;

   if(ink){
     const bit=y*src.width+x;
     bytes[4+(bit>>3)]|=(0x80>>(bit&7));
     ctx.fillRect(x,y,1,1);
     setBits++;
   }
 }
 e('logoState').textContent='Converted preview: '+src.width+' x '+src.height+
   ' | '+setBits+' logo pixels | background '+Math.round(bgLum);
 return {bytes,setBits,transparent,bgLum};
}
function prepareLogo(){
 const file=e('logoFile').files[0];
 if(!file){e('logoState').textContent='Choose an image first';return;}
 const img=new Image();
 img.onload=()=>{
   const maxW=180,maxH=56,scale=Math.min(maxW/img.width,maxH/img.height,1);
   const w=Math.max(1,Math.round(img.width*scale)),h=Math.max(1,Math.round(img.height*scale));
   const src=e('logoSource');src.width=w;src.height=h;
   const ctx=src.getContext('2d');ctx.clearRect(0,0,w,h);ctx.drawImage(img,0,0,w,h);
   logoBitmapBytes();
   URL.revokeObjectURL(img.src);
 };
 img.src=URL.createObjectURL(file);
}
async function uploadLogo(){
 const src=e('logoSource');
 if(src.width<=1||src.height<=1){e('logoState').textContent='Preview the logo first';return;}
 if(!(await saveIdentity())){e('logoState').textContent='Site identity save failed';return;}
 const pack=logoBitmapBytes();
 if(pack.setBits===0){
   e('logoState').textContent='Logo conversion produced 0 pixels. Try Invert.';
   return;
 }
 let hex='';
 for(const b of pack.bytes)hex+=b.toString(16).padStart(2,'0');
 const p=new URLSearchParams({hex});
 const r=await fetch('/api/display-logo-data',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json()}catch(_){}
 e('logoState').textContent=r.ok?('Logo stored: '+x.width+' x '+x.height+' | '+x.setBits+' pixels'):'LOGO UPLOAD FAILED '+(x.error||'');
 if(r.ok){loadHeaderLogo?.();load();}
}
async function deleteLogo(){
 let r=await fetch('/api/display-logo',{method:'DELETE'});
 e('logoState').textContent=r.ok?'Logo deleted - full refresh requested':'DELETE FAILED';
 if(r.ok)load();
}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });




  server.on(
    "/api/device-restore-keep-current",
    HTTP_POST,
    [](){
      if(commonBackupRestoreTooLarge){
        server.sendHeader("Cache-Control","no-store");
        server.send(413,"application/json","{\"ok\":false,\"error\":\"backup too large\"}");
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        return;
      }

      const bool importHardware=server.arg("importHardware")=="1";
      const bool importRules=server.arg("importRules")=="1";
      CommonBackupRestoreResult rr=restoreCommonBackupKeepCurrent(commonBackupRestoreUploadBuffer,importHardware,importRules);
      String j=commonBackupRestoreJson(rr,"KEEP_CURRENT");
      commonBackupRestoreUploadBuffer="";
      commonBackupRestoreReceived=0;
      server.sendHeader("Cache-Control","no-store");
      server.send(rr.ok?200:400,"application/json",j);
      if(rr.ok)commonBackupRebootAtMs=millis()+1800UL;
    },
    [](){
      HTTPUpload& upload=server.upload();
      if(upload.status==UPLOAD_FILE_START){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        commonBackupRestoreUploadBuffer.reserve(32768);
      }else if(upload.status==UPLOAD_FILE_WRITE){
        commonBackupRestoreReceived+=upload.currentSize;
        if(!commonBackupRestoreTooLarge){
          if(commonBackupRestoreUploadBuffer.length()+upload.currentSize>COMMON_BACKUP_MAX_BYTES){
            commonBackupRestoreTooLarge=true;
            commonBackupRestoreUploadBuffer="";
          }else{
            commonBackupRestoreUploadBuffer.concat((const char*)upload.buf,upload.currentSize);
          }
        }
      }else if(upload.status==UPLOAD_FILE_ABORTED){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
      }
    }
  );

  server.on(
    "/api/device-restore-use-backup",
    HTTP_POST,
    [](){
      if(commonBackupRestoreTooLarge){
        server.sendHeader("Cache-Control","no-store");
        server.send(413,"application/json","{\"ok\":false,\"error\":\"backup too large\"}");
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        return;
      }

      const bool importHardware=server.arg("importHardware")=="1";
      const bool importRules=server.arg("importRules")=="1";
      CommonBackupRestoreResult rr=restoreCommonBackupUseBackup(commonBackupRestoreUploadBuffer,importHardware,importRules);
      String j=commonBackupRestoreJson(rr,"USE_BACKUP");
      commonBackupRestoreUploadBuffer="";
      commonBackupRestoreReceived=0;
      server.sendHeader("Cache-Control","no-store");
      server.send(rr.ok?200:400,"application/json",j);
      if(rr.ok)commonBackupRebootAtMs=millis()+1800UL;
    },
    [](){
      HTTPUpload& upload=server.upload();
      if(upload.status==UPLOAD_FILE_START){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        commonBackupRestoreUploadBuffer.reserve(32768);
      }else if(upload.status==UPLOAD_FILE_WRITE){
        commonBackupRestoreReceived+=upload.currentSize;
        if(!commonBackupRestoreTooLarge){
          if(commonBackupRestoreUploadBuffer.length()+upload.currentSize>COMMON_BACKUP_MAX_BYTES){
            commonBackupRestoreTooLarge=true;
            commonBackupRestoreUploadBuffer="";
          }else{
            commonBackupRestoreUploadBuffer.concat((const char*)upload.buf,upload.currentSize);
          }
        }
      }else if(upload.status==UPLOAD_FILE_ABORTED){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
      }
    }
  );

  server.on(
    "/api/device-restore-use-new",
    HTTP_POST,
    [](){
      if(commonBackupRestoreTooLarge){
        server.sendHeader("Cache-Control","no-store");
        server.send(413,"application/json","{\"ok\":false,\"error\":\"backup too large\"}");
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        return;
      }

      String newSsid=server.arg("wifiSsid");
      newSsid.trim();
      String newWifiPass=server.arg("wifiPass");

      MQTTConfig newMqtt;
      newMqtt.enabled=server.arg("mqttEnabled")=="1";
      newMqtt.host=server.arg("mqttHost");
      newMqtt.host.trim();

      int port=server.arg("mqttPort").toInt();
      if(port<1 || port>65535)port=1883;
      newMqtt.port=(uint16_t)port;

      newMqtt.user=server.arg("mqttUser");
      newMqtt.pass=server.arg("mqttPass");
      newMqtt.baseTopic=server.arg("mqttBase");
      newMqtt.baseTopic.trim();
      if(!newMqtt.baseTopic.length())newMqtt.baseTopic="multiflow/controller";

      int live=server.arg("mqttLive").toInt();
      int totalizer=server.arg("mqttTotalizer").toInt();
      int status=server.arg("mqttStatus").toInt();
      newMqtt.liveIntervalSec=(uint32_t)constrain(live?live:5,1,3600);
      newMqtt.totalizerIntervalSec=(uint32_t)constrain(totalizer?totalizer:60,10,86400);
      newMqtt.statusIntervalSec=(uint32_t)constrain(status?status:60,10,3600);
      newMqtt.retain=server.arg("mqttRetain")=="1";

      const bool importHardware=server.arg("importHardware")=="1";
      const bool importRules=server.arg("importRules")=="1";
      CommonBackupRestoreResult rr=restoreCommonBackupUseNew(
        commonBackupRestoreUploadBuffer,
        newSsid,
        newWifiPass,
        newMqtt,
        importHardware,
        importRules
      );
      String j=commonBackupRestoreJson(rr,"USE_NEW");
      commonBackupRestoreUploadBuffer="";
      commonBackupRestoreReceived=0;
      server.sendHeader("Cache-Control","no-store");
      server.send(rr.ok?200:400,"application/json",j);
      if(rr.ok)commonBackupRebootAtMs=millis()+1800UL;
    },
    [](){
      HTTPUpload& upload=server.upload();
      if(upload.status==UPLOAD_FILE_START){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
        commonBackupRestoreUploadBuffer.reserve(32768);
      }else if(upload.status==UPLOAD_FILE_WRITE){
        commonBackupRestoreReceived+=upload.currentSize;
        if(!commonBackupRestoreTooLarge){
          if(commonBackupRestoreUploadBuffer.length()+upload.currentSize>COMMON_BACKUP_MAX_BYTES){
            commonBackupRestoreTooLarge=true;
            commonBackupRestoreUploadBuffer="";
          }else{
            commonBackupRestoreUploadBuffer.concat((const char*)upload.buf,upload.currentSize);
          }
        }
      }else if(upload.status==UPLOAD_FILE_ABORTED){
        commonBackupRestoreUploadBuffer="";
        commonBackupRestoreTooLarge=false;
        commonBackupRestoreReceived=0;
      }
    }
  );

  server.on(
    "/api/device-restore-dryrun",
    HTTP_POST,
    [](){
      if(commonBackupDryRunTooLarge){
        String j="{\"ok\":false,\"error\":\"backup too large\",\"writeActions\":0,\"eraseActions\":0}";
        server.sendHeader("Cache-Control","no-store");
        server.send(413,"application/json",j);
        commonBackupDryRunUploadBuffer="";
        commonBackupDryRunTooLarge=false;
        commonBackupDryRunReceived=0;
        return;
      }

      CommonBackupValidateResult v=validateCommonBackupV2(commonBackupDryRunUploadBuffer);
      String j=commonBackupClassificationJson(commonBackupDryRunUploadBuffer,v);
      commonBackupDryRunUploadBuffer="";
      commonBackupDryRunReceived=0;
      server.sendHeader("Cache-Control","no-store");
      server.send(v.ok?200:400,"application/json",j);
    },
    [](){
      HTTPUpload& upload=server.upload();
      if(upload.status==UPLOAD_FILE_START){
        commonBackupDryRunUploadBuffer="";
        commonBackupDryRunTooLarge=false;
        commonBackupDryRunReceived=0;
        commonBackupDryRunUploadBuffer.reserve(32768);
      }else if(upload.status==UPLOAD_FILE_WRITE){
        commonBackupDryRunReceived+=upload.currentSize;
        if(!commonBackupDryRunTooLarge){
          if(commonBackupDryRunUploadBuffer.length()+upload.currentSize>COMMON_BACKUP_MAX_BYTES){
            commonBackupDryRunTooLarge=true;
            commonBackupDryRunUploadBuffer="";
          }else{
            commonBackupDryRunUploadBuffer.concat((const char*)upload.buf,upload.currentSize);
          }
        }
      }else if(upload.status==UPLOAD_FILE_ABORTED){
        commonBackupDryRunUploadBuffer="";
        commonBackupDryRunTooLarge=false;
        commonBackupDryRunReceived=0;
      }
    }
  );


  server.on("/backup-restore",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MultiFlow Backup / Restore</title>
<style>
body{font-family:Arial;background:#111;color:#eee;margin:18px;max-width:900px}
a{color:#63e6be}.card{border:1px solid #444;border-radius:10px;padding:16px;margin:12px 0;background:#171717}
button{padding:10px 14px;margin:6px 6px 6px 0;border:0;border-radius:7px;font-weight:700}
.primary{background:#63e6be;color:#102018}.secondary{background:#333;color:#eee;border:1px solid #666}
.ok{color:#7CFC98}.bad{color:#ff8080}.warn{color:#ffd166}.small{font-size:13px;color:#aaa}
pre{white-space:pre-wrap;background:#0b0b0b;border:1px solid #333;border-radius:7px;padding:12px;min-height:90px}
input[type=file]{display:block;margin:12px 0;padding:8px;background:#222;color:#eee;border:1px solid #555;border-radius:6px;width:calc(100% - 18px)}
.netgrid{display:grid;grid-template-columns:repeat(3,minmax(170px,1fr));gap:8px;margin:10px 0}
.netfield{background:#101415;border:1px solid #344;border-radius:7px;padding:8px}
.netfield label{display:block;color:#aaa;font-size:12px;margin-bottom:4px}
.netfield input,.netfield select{width:100%;box-sizing:border-box;padding:8px;background:#222;color:#eee;border:1px solid #666;border-radius:5px}
@media(max-width:720px){.netgrid{grid-template-columns:1fr}}
</style></head><body>
<h2>MultiFlow Backup / Restore</h2>
<p><a href="/">Dashboard</a></p>

<div class="card">
<h3>Configuration Backup</h3>
<p>Download a versioned MultiFlow configuration backup from this controller.</p>
<button class="secondary" onclick="location.href='/api/device-backup'">Download .mfbak</button>
<button class="secondary" onclick="inspectBackupInfo()">Inspect Current Backup</button>
<pre id="backupInfo">Not checked yet.</pre>
</div>

<div class="card">
<h3>Commissioning Classification / Dry Run</h3>
<p><b>A5a is read-only. It does not restore, erase, reboot, or change controller configuration.</b></p>
<p class="small">The report classifies what is actually inside the selected backup as LOCAL ONLY, PRESERVE BY DEFAULT, PORTABLE, REVIEW REQUIRED, hardware/display review, or optional network.</p>
<input id="file" type="file" accept=".mfbak,application/octet-stream" onchange="validateBackup()">
<button type="button" class="primary" onclick="validateBackup();return false;">Classify + Validate Backup</button>
<pre id="classification">No commissioning classification yet.</pre>
<div class="card" style="background:#101415">
<h4>A5c Explicit Import Options</h4>
<label><input id="importHardware" type="checkbox"> Replace M31 hardware/profile from backup</label><br>
<label><input id="importRules" type="checkbox"> Import rules from backup <b>DISABLED</b> for review</label>
<p class="small">Both options default OFF. Rules are forced master-disabled and every imported rule is disabled before the file is committed.</p>
</div>
<button class="secondary" onclick="restoreKeepCurrent()">Restore — Keep Current Network</button>
<button class="secondary" onclick="restoreUseBackup()">Restore — Use Backup Network</button>
<hr style="border:0;border-top:1px solid #444;margin:16px 0">
<h4>Restore — Use New Network</h4>
<div class="small">Backup Wi-Fi/MQTT settings are ignored. Enter the replacement controller network settings below.</div>
<div class="netgrid">
 <div class="netfield"><label>New Wi-Fi SSID</label><input id="newWifiSsid" autocomplete="off"></div>
 <div class="netfield"><label>New Wi-Fi Password</label><input id="newWifiPass" type="password" autocomplete="new-password"></div>
 <div class="netfield"><label>MQTT</label><select id="newMqttEnabled"><option value="1">Enabled</option><option value="0">Disabled</option></select></div>
 <div class="netfield"><label>MQTT Broker Host / IP</label><input id="newMqttHost" placeholder="192.168.1.10"></div>
 <div class="netfield"><label>MQTT Port</label><input id="newMqttPort" type="number" min="1" max="65535" value="1883"></div>
 <div class="netfield"><label>MQTT Username</label><input id="newMqttUser" autocomplete="off"></div>
 <div class="netfield"><label>MQTT Password</label><input id="newMqttPass" type="password" autocomplete="new-password"></div>
 <div class="netfield"><label>MQTT Base Topic</label><input id="newMqttBase" value="multiflow/controller"></div>
 <div class="netfield"><label>Retain Routine Publishes</label><select id="newMqttRetain"><option value="0">No</option><option value="1">Yes</option></select></div>
 <div class="netfield"><label>Live Data (sec)</label><input id="newMqttLive" type="number" min="1" max="3600" value="5"></div>
 <div class="netfield"><label>Totalizers (sec)</label><input id="newMqttTotalizer" type="number" min="10" max="86400" value="60"></div>
 <div class="netfield"><label>Status (sec)</label><input id="newMqttStatus" type="number" min="10" max="3600" value="60"></div>
</div>
<button class="secondary" onclick="restoreUseNew()">Restore — Use New Network</button>
<div id="state" class="small">Choose a .mfbak file to inspect.</div>
<pre id="result">No backup validated yet.</pre>
</div>

<script>
const e=id=>document.getElementById(id);
function addA5cPolicy(data){
  data.append('importHardware',e('importHardware').checked?'1':'0');
  data.append('importRules',e('importRules').checked?'1':'0');
}
function a5cPolicyText(){
  const p=[];
  if(e('importHardware').checked)p.push('REPLACE HARDWARE PROFILE');
  if(e('importRules').checked)p.push('IMPORT RULES DISABLED');
  return p.length?p.join(' + '):'PRESERVE HARDWARE + SKIP RULES';
}
async function inspectBackupInfo(){
  e('backupInfo').textContent='Checking...';
  try{
    const r=await fetch('/api/device-backup-info',{cache:'no-store'});
    const j=await r.json();
    e('backupInfo').textContent=JSON.stringify(j,null,2);
  }catch(err){e('backupInfo').textContent='ERROR: '+err;}
}

async function restoreKeepCurrent(){
  const f=e('file').files[0];
  if(!f){e('state').className='bad';e('state').textContent='Choose a .mfbak file first.';return;}
  const typed=prompt('This will restore controller configuration and reboot. Wi-Fi and MQTT network settings will be kept. A5c policy: '+a5cPolicyText()+'. Type RESTORE to continue:');
  if(typed!=='RESTORE'){e('state').className='warn';e('state').textContent='Restore cancelled.';return;}
  e('state').className='warn';
  e('state').textContent='Restoring configuration — keep current network mode...';
  e('result').textContent='Uploading '+f.name+' for restore...';
  const data=new FormData();
  addA5cPolicy(data);
  data.append('file',f,f.name);
  try{
    const r=await fetch('/api/device-restore-keep-current',{method:'POST',body:data});
    const text=await r.text();
    let j;
    try{j=JSON.parse(text);}catch(_){throw new Error(text||('HTTP '+r.status));}
    e('result').textContent=JSON.stringify(j,null,2);
    if(j.ok){
      e('state').className='ok';
      e('state').textContent='Restore accepted. Controller will reboot; current network settings were preserved.';
    }else{
      e('state').className='bad';
      e('state').textContent='Restore failed. Review result before retrying.';
    }
  }catch(err){
    e('state').className='bad';
    e('state').textContent='Restore request error.';
    e('result').textContent='ERROR: '+err;
  }
}

async function restoreUseBackup(){
  const f=e('file').files[0];
  if(!f){e('state').className='bad';e('state').textContent='Choose a .mfbak file first.';return;}
  const typed=prompt("This will restore controller configuration and reboot. Wi-Fi and MQTT settings from the backup will replace this controller's current network settings. A5c policy: "+a5cPolicyText()+". Type RESTORE to continue:");
  if(typed!=='RESTORE'){e('state').className='warn';e('state').textContent='Restore cancelled.';return;}
  e('state').className='warn';
  e('state').textContent='Restoring configuration — use backup network mode...';
  e('result').textContent='Uploading '+f.name+' for restore...';
  const data=new FormData();
  addA5cPolicy(data);
  data.append('file',f,f.name);
  try{
    const r=await fetch('/api/device-restore-use-backup',{method:'POST',body:data});
    const text=await r.text();
    let j;
    try{j=JSON.parse(text);}catch(_){throw new Error(text||('HTTP '+r.status));}
    e('result').textContent=JSON.stringify(j,null,2);
    if(j.ok){
      e('state').className='ok';
      e('state').textContent='Restore accepted. Controller will reboot; backup Wi-Fi/MQTT settings were restored. Reconnect using the backup network configuration.';
    }else{
      e('state').className='bad';
      e('state').textContent='Restore failed. Review result before retrying.';
    }
  }catch(err){
    e('state').className='bad';
    e('state').textContent='Restore request error.';
    e('result').textContent='ERROR: '+err;
  }
}

async function restoreUseNew(){
  const f=e('file').files[0];
  if(!f){e('state').className='bad';e('state').textContent='Choose a .mfbak file first.';return;}

  const ssid=e('newWifiSsid').value.trim();
  const host=e('newMqttHost').value.trim();
  if(!ssid){e('state').className='bad';e('state').textContent='New Wi-Fi SSID is required.';return;}
  if(!host){e('state').className='bad';e('state').textContent='New MQTT broker host is required.';return;}

  const typed=prompt("This will restore controller configuration, ignore network settings in the backup, save the NEW Wi-Fi/MQTT settings shown below, and reboot. A5c policy: "+a5cPolicyText()+". Type RESTORE to continue:");
  if(typed!=='RESTORE'){e('state').className='warn';e('state').textContent='Restore cancelled.';return;}

  e('state').className='warn';
  e('state').textContent='Restoring configuration — use new network mode...';
  e('result').textContent='Uploading '+f.name+' and applying new network settings...';

  const data=new FormData();
  data.append('wifiSsid',ssid);
  data.append('wifiPass',e('newWifiPass').value);
  data.append('mqttEnabled',e('newMqttEnabled').value);
  data.append('mqttHost',host);
  data.append('mqttPort',e('newMqttPort').value||'1883');
  data.append('mqttUser',e('newMqttUser').value);
  data.append('mqttPass',e('newMqttPass').value);
  data.append('mqttBase',e('newMqttBase').value.trim()||'multiflow/controller');
  data.append('mqttRetain',e('newMqttRetain').value);
  data.append('mqttLive',e('newMqttLive').value||'5');
  data.append('mqttTotalizer',e('newMqttTotalizer').value||'60');
  data.append('mqttStatus',e('newMqttStatus').value||'60');
  addA5cPolicy(data);
  data.append('file',f,f.name);

  try{
    const r=await fetch('/api/device-restore-use-new',{method:'POST',body:data});
    const text=await r.text();
    let j;
    try{j=JSON.parse(text);}catch(_){throw new Error(text||('HTTP '+r.status));}
    e('result').textContent=JSON.stringify(j,null,2);
    if(j.ok){
      e('state').className='ok';
      e('state').textContent='Restore accepted. Controller will reboot using the NEW Wi-Fi/MQTT settings. Reconnect on the new network.';
    }else{
      e('state').className='bad';
      e('state').textContent='Restore failed. Review result before retrying.';
    }
  }catch(err){
    e('state').className='bad';
    e('state').textContent='Restore request error.';
    e('result').textContent='ERROR: '+err;
  }
}

function renderClassification(j){
  if(!j || !j.sections){
    e('classification').textContent='Classification unavailable.';
    return;
  }
  const s=j.sections;
  const lines=[
    'A5a COMMISSIONING CLASSIFICATION',
    'Source: '+(j.sourceModel||'?')+' | Source ID: '+(j.sourceId||'?')+
      ' | Current Device: '+(j.currentDeviceId||'?')+
      ' | Same-device hint: '+(j.sameDeviceHint?'YES':'NO'),
    '',
    'LOCAL ONLY',
    '  Device identity: '+s.deviceIdentity.action,
    '',
    'PRESERVE BY DEFAULT',
    '  M31 hardware/profile: '+s.hardwareProfile.action+
      ' | present in this backup: '+(s.hardwareProfile.presentInBackup?'YES':'NO'),
    '',
    'PORTABLE',
    '  Site/process/flow/AGA/PID/I/O configuration records: '+(s.processApplication.records||0),
    '',
    'REVIEW REQUIRED',
    '  Rules/alarms: '+s.rulesAlarms.action+
      ' | present in this backup: '+(s.rulesAlarms.presentInBackup?'YES':'NO'),
    '  Display/hardware-dependent records: '+(s.displayHardware.records||0),
    '',
    'OPTIONAL / EXPLICIT',
    '  Network: '+s.network.action+
      ' | Wi-Fi: '+(s.network.wifiPresent?'YES':'NO')+
      ' | MQTT: '+(s.network.mqttPresent?'YES':'NO'),
    '',
    'CURRENT V2 FORMAT GAPS',
    '  operator_config_v2 absent: '+((j.formatGaps&&j.formatGaps.operatorConfigV2Absent)?'YES':'NO'),
    '  rules config absent: '+((j.formatGaps&&j.formatGaps.rulesConfigAbsent)?'YES':'NO'),
    '',
    'DRY RUN: writeActions='+j.writeActions+' eraseActions='+j.eraseActions
  ];
  e('classification').textContent=lines.join('\n');
}

async function validateBackup(){
  const f=e('file').files[0];
  if(!f){e('state').className='bad';e('state').textContent='Choose a .mfbak file first.';return;}
  e('state').className='warn';
  e('state').textContent='Validating only — no writes or erases...';
  e('result').textContent='Uploading '+f.name+' ('+f.size+' bytes)...';
  const data=new FormData();
  data.append('file',f,f.name);
  try{
    const r=await fetch('/api/device-restore-dryrun',{method:'POST',body:data});
    const text=await r.text();
    let j;
    try{j=JSON.parse(text);}catch(_){throw new Error(text||('HTTP '+r.status));}
    e('result').textContent=JSON.stringify(j,null,2);
    renderClassification(j);
    if(j.ok && j.writeActions===0 && j.eraseActions===0 && j.classificationOnly===true){
      e('state').className='ok';
      e('state').textContent='VALID BACKUP — A5a classification complete; dry run only, controller unchanged.';
    }else{
      e('state').className='bad';
      e('state').textContent='Backup validation failed or returned an unexpected result.';
    }
  }catch(err){
    e('state').className='bad';
    e('state').textContent='Validation error.';
    e('result').textContent='ERROR: '+err;
  }
}
inspectBackupInfo();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/ota-partitions",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",riotOtaPartitionsJson());
  });

  // RIOT BROWSER MEDIATED OTA GATE A7B1 V1
  server.on("/api/ota-browser-config",HTTP_GET,[](){
    String j="{\"ok\":true";
    j+=",\"gate\":\"A7b1\"";
    j+=",\"product\":\""+String(RIOT_OTA_PRODUCT)+"\"";
    j+=",\"platform\":\""+String(RIOT_OTA_PLATFORM)+"\"";
    j+=",\"channel\":\""+String(RIOT_OTA_CHANNEL)+"\"";
    j+=",\"currentVersion\":\""+String(RIOT_OTA_VERSION)+"\"";
    j+=",\"build\":\""+String(RIOT_OTA_BUILD)+"\"";
    j+=",\"manifestUrl\":\""+String(RIOT_OTA_MANIFEST_URL)+"\"";
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/ota-remote-check",HTTP_GET,[](){
    String j=riotRemoteOtaCheckJson();
    bool ok=j.indexOf("\"ok\":true")>=0;
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:502,"application/json",j);
  });

  server.on("/api/ota-remote-install",HTTP_POST,[](){
    if(riotRemoteOtaActive || riotOtaActive){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"OTA is already active\"}");
      return;
    }

    RiotRemoteOtaManifest m;
    if(!riotFetchRemoteOtaManifest(m)){
      String err=m.error; err.replace("\\","\\\\"); err.replace("\"","\\\"");
      server.send(502,"application/json","{\"ok\":false,\"error\":\""+err+"\"}");
      return;
    }

    if(!server.hasArg("confirmVersion") || server.arg("confirmVersion")!=m.version){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"confirmVersion must match current manifest release\"}");
      return;
    }

    bool reinstall=server.hasArg("reinstall") &&
      (server.arg("reinstall")=="1" || server.arg("reinstall")=="true");
    if(m.version==RIOT_OTA_VERSION && !reinstall){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"Controller already has this release; set reinstall=1 to verify/reinstall it\"}");
      return;
    }

    String message;
    bool ok=riotInstallRemoteOta(m,message);
    String safe=message; safe.replace("\\","\\\\"); safe.replace("\"","\\\"");
    String j="{\"ok\":"+String(ok?"true":"false");
    j+=",\"gate\":\"A6c2\"";
    j+=",\"version\":\""+m.version+"\"";
    j+=",\"bytes\":"+String((unsigned long)riotRemoteOtaBytes);
    j+=",\"message\":\""+safe+"\"";
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:500,"application/json",j);
    if(ok)riotOtaRebootAtMs=millis()+2500UL;
  });

  server.on("/api/ota-upload-status",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",riotOtaUploadStatusJson());
  });

  server.on(
    "/api/ota-browser-upload",
    HTTP_POST,
    [](){
      server.sendHeader("Cache-Control","no-store");
      String j=riotBrowserOtaStatusJson();
      if(riotOtaComplete && !riotOtaFailed){
        server.send(200,"application/json",j);
        riotOtaRebootAtMs=millis()+2500UL;
      }else{
        server.send(400,"application/json",j);
      }
    },
    [](){
      HTTPUpload& upload=server.upload();

      if(upload.status==UPLOAD_FILE_START){
        if(riotOtaActive)riotOtaAbortUpload(ESP_ERR_INVALID_STATE);
        riotOtaResetUploadState();
        riotBrowserOtaResetVerify();

        riotOtaFilename=upload.filename;
        String lower=riotOtaFilename;
        lower.toLowerCase();
        if(!lower.endsWith(".bin")){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_INVALID_ARG;
          return;
        }

        if(!server.hasArg("sha256") || !server.hasArg("size") || !server.hasArg("version")){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_INVALID_ARG;
          return;
        }

        riotBrowserOtaExpectedSha=server.arg("sha256");
        riotBrowserOtaExpectedSha.toLowerCase();
        riotBrowserOtaExpectedSize=(size_t)server.arg("size").toInt();
        riotBrowserOtaVersion=server.arg("version");

        if(!riotOtaSha256LooksValid(riotBrowserOtaExpectedSha) ||
           riotBrowserOtaExpectedSize<1024 ||
           !riotBrowserOtaVersion.length()){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_INVALID_ARG;
          return;
        }

        const esp_partition_t* running=esp_ota_get_running_partition();
        riotOtaTarget=esp_ota_get_next_update_partition(nullptr);
        if(!riotOtaTarget || !running || riotOtaTarget->address==running->address){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_NOT_FOUND;
          return;
        }

        if(riotBrowserOtaExpectedSize>riotOtaTarget->size){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_INVALID_SIZE;
          return;
        }

        riotOtaLastError=esp_ota_begin(riotOtaTarget,riotBrowserOtaExpectedSize,&riotOtaHandle);
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          riotOtaActive=false;
          return;
        }

        mbedtls_sha256_init(&riotBrowserOtaSha);
        if(mbedtls_sha256_starts_ret(&riotBrowserOtaSha,0)!=0){
          esp_ota_abort(riotOtaHandle);
          riotOtaHandle=0;
          riotOtaFailed=true;
          riotOtaLastError=ESP_FAIL;
          return;
        }
        riotBrowserOtaShaActive=true;
        riotOtaActive=true;

        Serial.printf("OTA A7B2 START version=%s file=%s expected=%lu target=%s\n",
          riotBrowserOtaVersion.c_str(),
          riotOtaFilename.c_str(),
          (unsigned long)riotBrowserOtaExpectedSize,
          riotOtaTarget->label);
      }
      else if(upload.status==UPLOAD_FILE_WRITE){
        if(!riotOtaActive || riotOtaFailed || !upload.currentSize)return;

        if(riotOtaBytes==0 && upload.buf[0]!=0xE9){
          Serial.printf("OTA A7B2 REJECT bad image magic=0x%02X\n",(unsigned)upload.buf[0]);
          riotOtaAbortUpload(ESP_ERR_INVALID_ARG);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        if(riotOtaBytes+upload.currentSize>riotBrowserOtaExpectedSize ||
           riotOtaBytes+upload.currentSize>riotOtaTarget->size){
          Serial.println("OTA A7B2 REJECT image exceeds expected size/partition");
          riotOtaAbortUpload(ESP_ERR_INVALID_SIZE);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        if(!riotBrowserOtaShaActive ||
           mbedtls_sha256_update_ret(&riotBrowserOtaSha,upload.buf,upload.currentSize)!=0){
          Serial.println("OTA A7B2 SHA256 UPDATE FAIL");
          riotOtaAbortUpload(ESP_FAIL);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        riotOtaLastError=esp_ota_write(riotOtaHandle,upload.buf,upload.currentSize);
        if(riotOtaLastError!=ESP_OK){
          Serial.printf("OTA A7B2 WRITE FAIL err=%d bytes=%lu\n",
            (int)riotOtaLastError,(unsigned long)riotOtaBytes);
          riotOtaAbortUpload(riotOtaLastError);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        riotOtaBytes+=upload.currentSize;
      }
      else if(upload.status==UPLOAD_FILE_END){
        if(!riotOtaActive || riotOtaFailed)return;

        if(riotOtaBytes!=riotBrowserOtaExpectedSize){
          Serial.printf("OTA A7B2 SIZE FAIL actual=%lu expected=%lu\n",
            (unsigned long)riotOtaBytes,
            (unsigned long)riotBrowserOtaExpectedSize);
          riotOtaAbortUpload(ESP_ERR_INVALID_SIZE);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        uint8_t digest[32];
        if(!riotBrowserOtaShaActive ||
           mbedtls_sha256_finish_ret(&riotBrowserOtaSha,digest)!=0){
          Serial.println("OTA A7B2 SHA256 FINISH FAIL");
          riotOtaAbortUpload(ESP_FAIL);
          if(riotBrowserOtaShaActive){
            mbedtls_sha256_free(&riotBrowserOtaSha);
            riotBrowserOtaShaActive=false;
          }
          return;
        }

        mbedtls_sha256_free(&riotBrowserOtaSha);
        riotBrowserOtaShaActive=false;
        riotBrowserOtaActualSha=riotOtaHexLower(digest,sizeof(digest));

        if(!riotBrowserOtaActualSha.equalsIgnoreCase(riotBrowserOtaExpectedSha)){
          Serial.printf("OTA A7B2 SHA256 FAIL actual=%s expected=%s\n",
            riotBrowserOtaActualSha.c_str(),
            riotBrowserOtaExpectedSha.c_str());
          riotOtaAbortUpload(ESP_ERR_INVALID_CRC);
          return;
        }

        Serial.println("OTA A7B2 SHA256 VERIFIED; finalizing image");
        riotOtaLastError=esp_ota_end(riotOtaHandle);
        riotOtaActive=false;
        riotOtaHandle=0;
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          Serial.printf("OTA A7B2 END/VALIDATE FAIL err=%d\n",(int)riotOtaLastError);
          return;
        }

        riotOtaLastError=esp_ota_set_boot_partition(riotOtaTarget);
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          Serial.printf("OTA A7B2 SET BOOT FAIL err=%d\n",(int)riotOtaLastError);
          return;
        }

        riotOtaComplete=true;
        riotOtaFailed=false;
        Serial.printf("OTA A7B2 COMPLETE version=%s bytes=%lu sha=%s nextBoot=%s\n",
          riotBrowserOtaVersion.c_str(),
          (unsigned long)riotOtaBytes,
          riotBrowserOtaActualSha.c_str(),
          riotOtaTarget->label);
      }
      else if(upload.status==UPLOAD_FILE_ABORTED){
        Serial.println("OTA A7B2 HTTP UPLOAD ABORTED");
        riotOtaAbortUpload(ESP_ERR_INVALID_STATE);
        if(riotBrowserOtaShaActive){
          mbedtls_sha256_free(&riotBrowserOtaSha);
          riotBrowserOtaShaActive=false;
        }
      }
    }
  );

  server.on(
    "/api/ota-upload",
    HTTP_POST,
    [](){
      server.sendHeader("Cache-Control","no-store");
      if(riotOtaComplete && !riotOtaFailed){
        String j=riotOtaUploadStatusJson();
        server.send(200,"application/json",j);
        riotOtaRebootAtMs=millis()+2500UL;
      }else{
        server.send(400,"application/json",riotOtaUploadStatusJson());
      }
    },
    [](){
      HTTPUpload& upload=server.upload();

      if(upload.status==UPLOAD_FILE_START){
        if(riotOtaActive)riotOtaAbortUpload(ESP_ERR_INVALID_STATE);
        riotOtaResetUploadState();
        riotOtaFilename=upload.filename;
        String lower=riotOtaFilename;
        lower.toLowerCase();
        if(!lower.endsWith(".bin")){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_INVALID_ARG;
          return;
        }

        const esp_partition_t* running=esp_ota_get_running_partition();
        riotOtaTarget=esp_ota_get_next_update_partition(nullptr);
        if(!riotOtaTarget || !running || riotOtaTarget->address==running->address){
          riotOtaFailed=true;
          riotOtaLastError=ESP_ERR_NOT_FOUND;
          return;
        }

        riotOtaLastError=esp_ota_begin(riotOtaTarget,OTA_SIZE_UNKNOWN,&riotOtaHandle);
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          riotOtaActive=false;
          return;
        }
        riotOtaActive=true;
        Serial.printf("OTA A6B START file=%s target=%s size=%lu\n",
          riotOtaFilename.c_str(),riotOtaTarget->label,(unsigned long)riotOtaTarget->size);
      }
      else if(upload.status==UPLOAD_FILE_WRITE){
        if(!riotOtaActive || riotOtaFailed || !upload.currentSize)return;

        if(riotOtaBytes==0 && upload.buf[0]!=0xE9){
          Serial.printf("OTA A6B REJECT bad image magic=0x%02X\n",(unsigned)upload.buf[0]);
          riotOtaAbortUpload(ESP_ERR_INVALID_ARG);
          return;
        }

        if(riotOtaBytes+upload.currentSize>riotOtaTarget->size){
          Serial.println("OTA A6B REJECT image exceeds target partition");
          riotOtaAbortUpload(ESP_ERR_INVALID_SIZE);
          return;
        }

        riotOtaLastError=esp_ota_write(riotOtaHandle,upload.buf,upload.currentSize);
        if(riotOtaLastError!=ESP_OK){
          Serial.printf("OTA A6B WRITE FAIL err=%d bytes=%lu\n",
            (int)riotOtaLastError,(unsigned long)riotOtaBytes);
          riotOtaAbortUpload(riotOtaLastError);
          return;
        }
        riotOtaBytes+=upload.currentSize;
      }
      else if(upload.status==UPLOAD_FILE_END){
        if(!riotOtaActive || riotOtaFailed)return;
        if(!riotOtaBytes){
          riotOtaAbortUpload(ESP_ERR_INVALID_SIZE);
          return;
        }

        riotOtaLastError=esp_ota_end(riotOtaHandle);
        riotOtaActive=false;
        riotOtaHandle=0;
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          Serial.printf("OTA A6B END/VALIDATE FAIL err=%d\n",(int)riotOtaLastError);
          return;
        }

        riotOtaLastError=esp_ota_set_boot_partition(riotOtaTarget);
        if(riotOtaLastError!=ESP_OK){
          riotOtaFailed=true;
          Serial.printf("OTA A6B SET BOOT FAIL err=%d\n",(int)riotOtaLastError);
          return;
        }

        riotOtaComplete=true;
        riotOtaFailed=false;
        Serial.printf("OTA A6B COMPLETE bytes=%lu nextBoot=%s\n",
          (unsigned long)riotOtaBytes,riotOtaTarget->label);
      }
      else if(upload.status==UPLOAD_FILE_ABORTED){
        Serial.println("OTA A6B HTTP UPLOAD ABORTED");
        riotOtaAbortUpload(ESP_ERR_INVALID_STATE);
      }
    }
  );

  server.on("/firmware-update",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Firmware Update</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:900px}
a{color:#63e6be}.card{background:#171717;border:1px solid #444;border-radius:9px;padding:14px;margin:10px 0}
button,input{padding:9px;background:#222;color:#fff;border:1px solid #666;border-radius:6px}
button{cursor:pointer;font-weight:700}.ok{color:#63e6be}.bad{color:#ff8b8b}.warn{color:#ffd166}.muted{color:#aaa}
pre{white-space:pre-wrap;background:#0b0b0b;border:1px solid #333;border-radius:7px;padding:12px}
</style></head><body>
<h2>Riot-Flow Firmware Update</h2>
<p><a href="/">Dashboard</a> | <a href="/system-status">System Status</a> | <a href="/backup-restore">Backup / Restore</a></p>

<div class=card>
<h3>OTA Slots</h3>
<pre id=slots>Loading...</pre>
</div>

<div class=card id=public-release>
<h3>Public Release - Browser Mediated</h3>
<p class=muted>The technician browser checks the public MultiFlowOTA release. The controller does not contact GitHub for this check.</p>
<!-- RIOT A7B2 FIELD PROGRESS UI V1 -->
<button onclick=checkBrowserRelease()>Check Public Release</button>
<button id=browserInstall disabled onclick=installBrowserRelease()>Download &amp; Install Public Release</button>
<div id=browserReady class=muted style="margin-top:10px">Install button: DISABLED until a valid public manifest check passes.</div>
<div id=browserState class=muted style="margin-top:6px">Stage: IDLE</div>
<pre id=browserResult>Public manifest not checked.</pre>
</div>

<!-- RIOT A7B2 UI DISAMBIGUATION V1 -->
<div class=card>
<h3>Engineering Fallback - Controller Direct OTA</h3>
<p class=muted>This is the legacy A6c2 engineering path. The controller itself contacts the public repository. Field technicians should normally use the Browser Mediated section above.</p>
<p class=muted>Product: <b>Riot-Flow</b> &nbsp; Platform: <b>TRMNL_XIAO_EPAPER</b> &nbsp; Channel: <b>stable</b></p>
<button onclick=checkRemote()>Direct Check Public Repo</button>
<button id=remoteInstall disabled style="display:none" onclick=installRemote()>Direct Download &amp; Install</button>
<div id=remoteState class=muted style="margin-top:10px">No remote check performed yet.</div>
<pre id=remoteResult>Manifest: riot-flow/manifest.json</pre>
</div>

<div class=card>
<h3>Local Firmware Upload</h3>
<p class=warn>This writes only to the inactive OTA slot. The boot slot changes only after the uploaded ESP32 image validates successfully.</p>
<p class=muted>A6b accepts a local <b>.bin</b> file only. Product/platform manifest validation and remote GitHub releases are added in later OTA gates.</p>
<p><a href="/api/device-backup">Download configuration backup before update</a></p>
<input id=file type=file accept=".bin,application/octet-stream">
<button id=go onclick=uploadFirmware()>Upload &amp; Install</button>
<div id=state class=muted style="margin-top:10px">Choose an approved Riot-Flow firmware .bin file.</div>
<pre id=result>No update attempted.</pre>
</div>

<script>
const e=id=>document.getElementById(id);
async function loadSlots(){
 try{
  const x=await fetch('/api/ota-partitions?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  e('slots').textContent=JSON.stringify(x,null,2);
 }catch(err){e('slots').textContent='ERROR: '+err;}
}
// RIOT BROWSER OTA A7B1 JS REPAIR V1
let browserManifest=null;
async function checkBrowserRelease(){
 browserManifest=null;
 const state=e('browserState');
 const result=e('browserResult');
 state.className='warn';
 state.textContent='Browser is checking the public MultiFlowOTA manifest...';
 result.textContent='Checking...';
 try{
  const cr=await fetch('/api/ota-browser-config?x='+Date.now(),{cache:'no-store'});
  const cfg=await cr.json();
  if(!cr.ok||!cfg.ok)throw new Error(cfg.error||('Controller config HTTP '+cr.status));

  const mr=await fetch(cfg.manifestUrl+'?x='+Date.now(),{cache:'no-store'});
  if(!mr.ok)throw new Error('Public manifest HTTP '+mr.status);
  const m=await mr.json();

  if(Number(m.schema)!==1)throw new Error('Unsupported manifest schema');
  if(m.product!==cfg.product)throw new Error('Manifest product mismatch');
  if(m.platform!==cfg.platform)throw new Error('Manifest platform mismatch');
  if(m.channel!==cfg.channel)throw new Error('Manifest channel mismatch');
  if(!m.version)throw new Error('Manifest version missing');
  if(typeof m.firmware_url!=='string'||!m.firmware_url.startsWith('https://'))throw new Error('Firmware URL must use HTTPS');
  if(!/^[0-9a-fA-F]{64}$/.test(String(m.sha256||'')))throw new Error('Manifest SHA-256 missing or invalid');

  const size=Number(m.size);
  if(!Number.isFinite(size)||size<1024)throw new Error('Manifest firmware size invalid');

  const sr=await fetch('/api/ota-partitions?x='+Date.now(),{cache:'no-store'});
  const slots=await sr.json();
  if(!sr.ok)throw new Error('OTA partition query HTTP '+sr.status);
  if(!slots.nextUpdateUsable||!slots.nextUpdate)throw new Error('No usable inactive OTA partition');
  if(size>Number(slots.nextUpdate.sizeBytes))throw new Error('Release is larger than inactive OTA slot');

  browserManifest=m;
  browserManifest.currentVersion=cfg.currentVersion;
  const available=m.version!==cfg.currentVersion;
  e('browserInstall').disabled=false;
  e('browserInstall').textContent=available
   ?'Download & Install Public Release'
   :'Reinstall Current Public Release';
  e('browserReady').className='ok';
  e('browserReady').textContent='Install button: ENABLED - verified manifest for '+m.version;
  state.className=available?'ok':'muted';
  state.textContent=available
   ?('Public release available: '+m.version+' ('+size+' bytes)')
   :('Controller is already current at '+cfg.currentVersion+'.');

  const nl=String.fromCharCode(10);
  result.textContent=
   'Current: '+cfg.currentVersion+nl+
   'Public:  '+m.version+nl+
   'Size:    '+size+' bytes'+nl+
   'SHA-256: '+m.sha256+nl+
   'Notes:   '+(m.notes||'(none)')+nl+nl+
   'A7b1 check passed in the technician browser. No firmware has been written.';
 }catch(err){
  state.className='bad';
  state.textContent='Browser-mediated release check failed.';
  result.textContent='ERROR: '+String(err);
 }
}

async function installBrowserRelease(){
 const state=e('browserState');
 const result=e('browserResult');
 const ready=e('browserReady');

 state.className='warn';
 state.textContent='Stage: BUTTON CLICK RECEIVED';
 result.textContent='A7b2 install handler started.';

 if(!browserManifest){
  ready.className='bad';
  ready.textContent='Install button: NOT READY';
  state.className='bad';
  state.textContent='Stage: BLOCKED - run Check Public Release first.';
  result.textContent='No firmware was downloaded or written.';
  return;
 }

 state.textContent='Stage: WAITING FOR CONFIRMATION';
 const typed=prompt(
  'Browser-mediated install for '+browserManifest.version+
  '. Type INSTALL to continue:'
 );

 if(typed!=='INSTALL'){
  state.className='warn';
  state.textContent='Stage: CANCELLED - confirmation did not equal INSTALL';
  result.textContent='No firmware was downloaded or written.';
  return;
 }

 state.textContent='Stage: CONFIRMED - downloading public firmware';
 result.textContent='Downloading '+browserManifest.firmware_url;

 try{
  const fr=await fetch(browserManifest.firmware_url,{cache:'no-store'});
  if(!fr.ok)throw new Error('Firmware download HTTP '+fr.status);

  state.textContent='Stage: DOWNLOADING';
  const blob=await fr.blob();

  state.textContent='Stage: DOWNLOADED - '+blob.size+' bytes';
  if(blob.size!==Number(browserManifest.size)){
    throw new Error('Downloaded firmware size mismatch: '+blob.size+' vs '+browserManifest.size);
  }

  const first=new Uint8Array(await blob.slice(0,1).arrayBuffer());
  if(first.length!==1||first[0]!==0xE9){
    throw new Error('Downloaded file is not an ESP32 application image');
  }

  state.textContent='Stage: UPLOADING TO CONTROLLER FOR SHA-256 VERIFY';

  const fd=new FormData();
  fd.append('file',blob,'Riot-Flow-'+browserManifest.version+'.bin');

  const q=new URLSearchParams({
    sha256:String(browserManifest.sha256),
    size:String(browserManifest.size),
    version:String(browserManifest.version)
  });

  const ur=await fetch('/api/ota-browser-upload?'+q.toString(),{
    method:'POST',
    body:fd
  });

  state.textContent='Stage: CONTROLLER RESPONSE RECEIVED';

  const txt=await ur.text();
  let x={};
  try{x=JSON.parse(txt)}catch(_){throw new Error(txt||('Upload HTTP '+ur.status))}
  result.textContent=JSON.stringify(x,null,2);

  if(ur.ok&&x.complete&&!x.failed&&
     String(x.actualSha256||'').toLowerCase()===String(browserManifest.sha256).toLowerCase()){
    state.className='ok';
    state.textContent='Stage: VERIFIED - reboot pending';
    ready.className='ok';
    ready.textContent='Install result: SHA-256 VERIFIED';
  }else{
    throw new Error('Controller rejected browser-mediated firmware upload');
  }
 }catch(err){
  state.className='bad';
  state.textContent='Stage: FAILED';
  result.textContent='ERROR: '+String(err);
  ready.className='bad';
  ready.textContent='Install result: FAILED - current running firmware remains selected';
  e('browserInstall').disabled=false;
 }
}

let remoteManifest=null;
async function checkRemote(){
 e('remoteInstall').disabled=true;
 remoteManifest=null;
 e('remoteState').className='warn';
 e('remoteState').textContent='Checking public Riot-Flow OTA manifest...';
 try{
  const r=await fetch('/api/ota-remote-check?x='+Date.now(),{cache:'no-store'});
  const txt=await r.text();
  let x={};try{x=JSON.parse(txt)}catch(_){throw new Error(txt||('HTTP '+r.status))}
  e('remoteResult').textContent=JSON.stringify(x,null,2);
  if(r.ok&&x.ok){
   remoteManifest=x;
   e('remoteInstall').disabled=false;
   e('remoteInstall').textContent=x.updateAvailable?'Direct Download & Install':'Direct Reinstall Current Release';
   e('remoteState').className=x.updateAvailable?'ok':'muted';
   e('remoteState').textContent=x.updateAvailable
    ?('Remote release available: '+x.remoteVersion+' ('+x.size+' bytes)')
    :('Already current: '+x.currentVersion+'; reinstall remains available for recovery/validation.');
  }else{
   e('remoteState').className='bad';
   e('remoteState').textContent=x.error||'Remote manifest check failed.';
  }
 }catch(err){
  e('remoteState').className='bad';
  e('remoteState').textContent='Remote manifest check error: '+err;
 }
}

async function installRemote(){
 if(!remoteManifest){e('remoteState').className='bad';e('remoteState').textContent='Check the repository first.';return;}
 const same=!remoteManifest.updateAvailable;
 const action=same?'REINSTALL':'UPDATE';
 const typed=prompt(
  'The controller will download '+remoteManifest.remoteVersion+
  ', verify SHA-256, write the inactive OTA slot, and reboot only after validation. Type '+action+' to continue:'
 );
 if(typed!==action){e('remoteState').className='warn';e('remoteState').textContent='Remote install cancelled.';return;}

 e('remoteInstall').disabled=true;
 e('remoteState').className='warn';
 e('remoteState').textContent='Downloading and SHA-256 verifying '+remoteManifest.remoteVersion+'... Do not remove power.';

 const body=new URLSearchParams({
  confirmVersion:remoteManifest.remoteVersion,
  reinstall:same?'1':'0'
 });
 try{
  const r=await fetch('/api/ota-remote-install',{
   method:'POST',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body
  });
  const txt=await r.text();
  let x={};try{x=JSON.parse(txt)}catch(_){throw new Error(txt||('HTTP '+r.status))}
  e('remoteResult').textContent=JSON.stringify(x,null,2);
  if(r.ok&&x.ok){
   e('remoteState').className='ok';
   e('remoteState').textContent=x.message||'Verified remote firmware installed. Reboot pending.';
  }else{
   e('remoteState').className='bad';
   e('remoteState').textContent=x.error||x.message||'Remote install failed; current boot partition remains selected.';
   e('remoteInstall').disabled=false;
  }
 }catch(err){
  e('remoteState').className='bad';
  e('remoteState').textContent='Remote install request interrupted: '+err;
 }
}

async function uploadFirmware(){
 const f=e('file').files[0];
 if(!f){e('state').className='bad';e('state').textContent='Choose a .bin file first.';return;}
 if(!f.name.toLowerCase().endsWith('.bin')){e('state').className='bad';e('state').textContent='Firmware filename must end in .bin';return;}
 const typed=prompt('Firmware will be written to the inactive OTA slot and the controller will reboot only after validation. Type UPDATE to continue:');
 if(typed!=='UPDATE'){e('state').className='warn';e('state').textContent='Update cancelled.';return;}

 e('go').disabled=true;
 e('state').className='warn';
 e('state').textContent='Uploading '+f.name+' ('+f.size+' bytes)...';
 e('result').textContent='Do not remove power during firmware upload.';
 const data=new FormData();
 data.append('file',f,f.name);
 try{
  const r=await fetch('/api/ota-upload',{method:'POST',body:data});
  const txt=await r.text();
  let x={};try{x=JSON.parse(txt)}catch(_){throw new Error(txt||('HTTP '+r.status))}
  e('result').textContent=JSON.stringify(x,null,2);
  if(r.ok&&x.complete&&!x.failed){
    e('state').className='ok';
    e('state').textContent='Firmware validated. Controller will reboot into '+((x.target&&x.target.label)||'new OTA slot')+'.';
  }else{
    e('state').className='bad';
    e('state').textContent='UPDATE FAILED — current running firmware remains selected.';
    e('go').disabled=false;
  }
 }catch(err){
  e('state').className='bad';
  e('state').textContent='Upload request failed. Current running firmware should remain selected.';
  e('result').textContent='ERROR: '+err;
  e('go').disabled=false;
 }
}
loadSlots();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/device-backup-info",HTTP_GET,[](){
    CommonBackupBuildResult b=buildCommonBackupV2();
    String j="{\"ok\":"+String(b.ok?"true":"false");
    j+=",\"version\":"+String(COMMON_BACKUP_VERSION);
    j+=",\"model\":\"TRMNL_XIAO_EPAPER\"";
    j+=",\"nvsRecords\":"+String(b.nvsRecords);
    j+=",\"fileRecords\":"+String(b.fileRecords);
    j+=",\"skippedFiles\":"+String(b.skippedFiles);
    j+=",\"bytes\":"+String(b.data.length());
    if(!b.ok)j+=",\"error\":\""+b.error+"\"";
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(b.ok?200:500,"application/json",j);
  });

  server.on("/api/device-backup",HTTP_GET,[](){
    CommonBackupBuildResult b=buildCommonBackupV2();
    if(!b.ok){
      server.sendHeader("Cache-Control","no-store");
      server.send(500,"application/json","{\"ok\":false,\"error\":\""+b.error+"\"}");
      return;
    }
    String filename=commonBackupSiteStem()+"_TRMNL_config_v2.mfbak";
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Disposition","attachment; filename=\""+filename+"\"");
    server.send(200,"application/octet-stream",b.data);
  });

// EP32 UI CLEANUP GATE5 OPERATOR STATUS NAV V4
  server.on("/system-status",HTTP_GET,[](){
    String h;
    h.reserve(9000);
    h+="<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Riot-Flow System Status</title>";
    h+="<style>*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:980px}a{color:#63e6be}.statusGrid{display:grid;grid-template-columns:repeat(2,minmax(220px,1fr));gap:10px}.card{background:#171717;border:1px solid #444;border-radius:9px;padding:14px}.k{color:#aaa;font-size:12px}.v{font-size:22px;font-weight:700;margin-top:3px}.ok{color:#63e6be}.warn{color:#ffd166}.bad{color:#ff8b8b}.muted{color:#aaa}.detail{display:grid;grid-template-columns:150px 1fr;gap:7px 12px;margin-top:10px}.btn{display:inline-block;padding:9px 12px;border:1px solid #666;border-radius:6px;background:#222;text-decoration:none}@media(max-width:600px){.statusGrid{grid-template-columns:1fr}.detail{grid-template-columns:1fr}.v{font-size:20px}}</style></head><body>";
    h+="<h2>System Status</h2>";
    h+=R"NAV(NAV_PLACEHOLDER)NAV";
    h+=R"HTML(
<div class=statusGrid>
<div class=card><div class=k>Controller</div><div id=controller class="v warn">Loading...</div></div>
<div class=card><div class=k>M31 I/O</div><div id=m31 class="v warn">Loading...</div></div>
<div class=card><div class=k>WiFi / Network</div><div id=wifi class="v warn">Loading...</div></div>
<div class=card><div class=k>MQTT</div><div id=mqtt class="v warn">Loading...</div></div>
<div class=card><div class=k>Power</div><div id=power class="v warn">Loading...</div></div>
<div class=card><div class=k>Storage</div><div id=storage class="v warn">Loading...</div></div>
<div class=card><div class=k>Alarm / Event Audit Log</div><div id=audit class="v warn">Loading...</div></div>
</div>
<div class=card style="margin-top:10px"><div class=k>Controller Information</div><div class=detail>
<div>IP Address</div><div id=ip>--</div>
<div>Firmware</div><div id=fw>--</div>
<div>Site</div><div id=site>--</div>
<div>Controller Time</div><div id=dt>--</div>
<div>Battery</div><div id=batt>--</div>
</div></div>
<div class=card style="margin-top:10px"><b>Service / Engineering</b><p class=muted>Detailed raw diagnostics and firmware maintenance are available for field troubleshooting.</p><a class=btn href="/status">Advanced Diagnostics (raw)</a> <a class=btn href="/firmware-update">Firmware Update</a></div>
<script>
const e=id=>document.getElementById(id);
function setv(id,t,c){const x=e(id);x.textContent=t;x.className='v '+c}
async function load(){
 try{
  const [s,m,a]=await Promise.all([
   fetch('/status?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/api/mqtt-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/api/history-v2?limit=1&offset=0&x='+Date.now(),{cache:'no-store'}).then(r=>r.json())
  ]);
  setv('controller','RUNNING','ok');
  setv('m31',s.modbus==='ONLINE'?'ONLINE':'OFFLINE',s.modbus==='ONLINE'?'ok':'bad');
  setv('wifi',s.wifiConnected?'CONNECTED':'LOCAL / DISCONNECTED',s.wifiConnected?'ok':'warn');
  const mt=!m.enabled?'DISABLED':(m.connected?'CONNECTED':'OFFLINE');
  setv('mqtt',mt,!m.enabled?'muted':(m.connected?'ok':'warn'));
  const pt=s.onBattery?'ON BATTERY':(s.powerSenseEnabled?(s.powerGood?'NORMAL':'POWER LOST'):'SENSE DISABLED');
  setv('power',pt,s.onBattery?'warn':((!s.powerSenseEnabled||s.powerGood)?'ok':'bad'));
  let st='UNKNOWN',sc='warn';
  if(m.nvsValid&&m.nvsTotal>0){const p=100*m.nvsFree/m.nvsTotal;st=p<8?'CRITICAL':(p<15?'LOW':'OK');sc=p<8?'bad':(p<15?'warn':'ok')}
  setv('storage',st,sc);
  setv('audit',a.storageOK?((a.count||0)+' / '+(a.capacity||0)+' RECORDS'):'STORAGE FAULT',a.storageOK?'ok':'bad');
  e('ip').textContent=s.ip||'--';e('fw').textContent=s.firmware||'--';
  e('site').textContent=((s.siteName||'')+(s.siteLSD?(' / '+s.siteLSD):''))||'--';
  e('dt').textContent=s.dateTime||'--';
  e('batt').textContent=s.batteryValid?(Number(s.batteryV).toFixed(3)+' V'):'Unavailable';
 }catch(_){setv('controller','STATUS READ FAILED','bad')}
}
load();setInterval(load,3000);
</script></body></html>)HTML";
    h.replace("NAV_PLACEHOLDER",R"NAV(<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
@media(max-width:600px){.rfNav{display:grid;grid-template-columns:1fr}.rfNav>a,.rfDrop,.rfDrop>button{width:100%}.rfDrop>button{text-align:left}.rfMenu{position:static;min-width:0;width:100%;margin-top:4px}}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a><a href="/flow-setup">Flow Setup</a><a href="/aga-setup">AGA / Gas</a><a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a><a href="/pid-control">PID / Control</a><a href="/control-output">Manual Outputs</a><a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a><a href="/display-setup">Display</a><a href="/mqtt-setup">MQTT</a><a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a><a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a><a href="/backup-restore">Backup / Restore</a><a href="/nvs-maintenance">NVS Storage</a><a href="/modbus-debug">Modbus Debug</a><a href="/tags">Tags</a><a href="/api-reference">API Reference</a>
</div></div>
</div>)NAV");
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"text/html",h);
  });

  server.on("/status",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",statusJson());
  });

  server.on("/api/state",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",statusJson());
  });

  server.on("/api/test",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json","{\"ok\":true,\"firmware\":\"TRMNL-M31-v5\"}");
  });

  server.on("/api/power",HTTP_GET,[](){
    String j="{\"senseEnabled\":"+String(POWER_GOOD_SENSE_ENABLED?"true":"false");
    j+=",\"pin\":"+String(POWER_GOOD_PIN);
    j+=",\"override\":\""+powerOverrideName()+"\"";
    j+=",\"powerGood\":"+String(powerGood?"true":"false");
    j+=",\"onBattery\":"+String(powerOutageActive?"true":"false");
    j+=",\"batteryV\":"+String(isfinite(batteryVoltageV)?batteryVoltageV:0.0f,3);
    j+=",\"outageCount\":"+String(powerOutageCount);
    j+=",\"lastOutageDurationSec\":"+String(powerLastOutageDurationSec);
    j+=",\"eventPending\":"+String(powerEventPending?"true":"false");
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/power-test",HTTP_POST,[](){
    if(!server.hasArg("state")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"state required: fail, restore, auto\"}");
      return;
    }
    String s=server.arg("state");
    s.toLowerCase();
    if(s=="fail")powerGoodOverride=POWER_OVERRIDE_FORCE_OFF;
    else if(s=="restore")powerGoodOverride=POWER_OVERRIDE_FORCE_ON;
    else if(s=="auto")powerGoodOverride=POWER_OVERRIDE_AUTO;
    else{
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid state\"}");
      return;
    }
    powerInputCandidate=requestedPowerGoodState();
    powerInputCandidateSinceMs=millis()-POWER_GOOD_DEBOUNCE_MS;
    servicePowerGood();
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/power-test",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow Power Test</title>
<style>body{font-family:Arial;background:#111;color:#eee;max-width:620px;margin:24px auto;padding:12px}.card{border:1px solid #555;border-radius:9px;padding:16px}button{padding:12px;margin:5px;font-size:16px}pre{background:#181818;padding:12px;white-space:pre-wrap}</style>
</head><body><div class=card><h2>Power-Fail Commissioning</h2>
<p>Physical POWER_GOOD sensing is disabled until the VBUS divider is wired.</p>
<button onclick="setp('fail')">Simulate Power Fail</button>
<button onclick="setp('restore')">Simulate Restore</button>
<button onclick="setp('auto')">Return to Auto</button>
<pre id=o>Loading...</pre></div>
<script>
async function stat(){o.textContent=JSON.stringify(await fetch('/api/power?x='+Date.now()).then(r=>r.json()),null,2)}
async function setp(s){await fetch('/api/power-test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'state='+encodeURIComponent(s)});setTimeout(stat,1200)}
stat();setInterval(stat,2000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/wifi",HTTP_GET,[](){
    String h=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow WiFi</title>
<style>body{font-family:Arial;background:#111;color:#eee;margin:18px}.card{max-width:620px;margin:auto;background:#1b2229;padding:18px;border-radius:10px}input,button{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;font-size:16px}label{display:block;color:#bbb;margin-top:8px}</style>
</head><body><div class=card><h2>MultiFlow WiFi Setup</h2>
<form method=post action=/wifi>
<label>WiFi SSID</label><input name=ssid value=")HTML"+wifiStaSsid+R"HTML(" required>
<label>Password</label><input name=pass type=password placeholder="Leave blank to keep current password">
<button type=submit>Save WiFi</button></form>
<p>Current IP: )HTML"+activeIpString()+R"HTML(</p>
</div></body></html>)HTML";
    server.send(200,"text/html",h);
  });

  server.on("/wifi",HTTP_POST,[](){
    if(!server.hasArg("ssid") || !server.arg("ssid").length()){
      server.send(400,"text/plain","SSID required");
      return;
    }
    String newPass=server.arg("pass");
    if(!newPass.length())newPass=wifiStaPass;
    saveNetworkConfig(server.arg("ssid"),newPass);
    server.send(200,"text/html","<html><body><h3>WiFi saved.</h3><p>Reconnect to the controller after a few seconds.</p></body></html>");
    WiFi.disconnect(false,false);
    delay(100);
    beginStationConnect();
    displayDirty=true;
  });

  server.on("/api/mqtt-config-storage",HTTP_GET,[](){
    MQTTConfig p,b;
    bool primaryNow=readMQTTConfigFile("/mqtt.cfg",p);
    bool backupNow=readMQTTConfigFile("/mqtt.bak",b);
    String j="{\"bootLoadOK\":"+String(mqttFlashBootLoadOK?"true":"false");
    j+=",\"bootPrimaryOK\":"+String(mqttFlashBootPrimaryOK?"true":"false");
    j+=",\"bootBackupOK\":"+String(mqttFlashBootBackupOK?"true":"false");
    j+=",\"primaryNowOK\":"+String(primaryNow?"true":"false");
    j+=",\"backupNowOK\":"+String(backupNow?"true":"false");
    j+=",\"lastSaveOK\":"+String(mqttFlashLastSaveOK?"true":"false");
    j+=",\"saveCount\":"+String(mqttFlashSaveCount);
    j+=",\"path\":\"/mqtt.cfg\"";
    j+=",\"backupPath\":\"/mqtt.bak\"";
    j+=",\"enabled\":"+String(mqttCfg.enabled?"true":"false");
    j+=",\"host\":\""+mqttCfg.host+"\"";
    j+=",\"port\":"+String(mqttCfg.port);
    j+=",\"liveSec\":"+String(mqttCfg.liveIntervalSec);
    j+=",\"totalizerSec\":"+String(mqttCfg.totalizerIntervalSec);
    j+=",\"statusSec\":"+String(mqttCfg.statusIntervalSec)+"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/mqtt-config",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",mqttConfigJson());
  });

  server.on("/api/mqtt-traffic",HTTP_GET,[](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",mqttTrafficJson());
  });

  server.on("/api/mqtt-config",HTTP_POST,[](){
    if(server.hasArg("enabled"))mqttCfg.enabled=server.arg("enabled")=="1";
    if(server.hasArg("host")){
      String h=server.arg("host");
      h.trim();
      if(h.length())mqttCfg.host=h;
    }
    if(server.hasArg("port")){
      int p=server.arg("port").toInt();
      if(p>=1 && p<=65535)mqttCfg.port=(uint16_t)p;
    }
    if(server.hasArg("user"))mqttCfg.user=server.arg("user");
    if(server.hasArg("pass") && server.arg("pass").length())mqttCfg.pass=server.arg("pass");
    if(server.hasArg("clearPass") && server.arg("clearPass")=="1")mqttCfg.pass="";
    if(server.hasArg("base"))mqttCfg.baseTopic=server.arg("base");
    if(server.hasArg("live")){
      int v=server.arg("live").toInt();
      mqttCfg.liveIntervalSec=(uint32_t)constrain(v,1,3600);
    }
    if(server.hasArg("totalizer")){
      int v=server.arg("totalizer").toInt();
      mqttCfg.totalizerIntervalSec=(uint32_t)constrain(v,10,86400);
    }
    if(server.hasArg("status")){
      int v=server.arg("status").toInt();
      mqttCfg.statusIntervalSec=(uint32_t)constrain(v,10,3600);
    }
    if(server.hasArg("retain"))mqttCfg.retain=server.arg("retain")=="1";
    if(!mqttCfg.host.length()){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"broker host required\"}");
      return;
    }
    bool persisted=saveMQTTConfig();
    if(!persisted){
      server.send(507,"application/json","{\"ok\":false,\"error\":\"MQTT settings could not be persisted to flash\"}");
      return;
    }
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/mqtt-setup",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow MQTT Setup</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:980px}
a{color:#63e6be}.card{border:1px solid #444;border-radius:9px;background:#171717;padding:14px;margin:10px 0}
.grid{display:grid;grid-template-columns:repeat(3,minmax(180px,1fr));gap:9px}
.field{background:#101415;border:1px solid #344;border-radius:7px;padding:8px}
label{display:block;color:#aaa;font-size:12px;margin-bottom:4px}
input,select,button{width:100%;padding:9px;background:#222;color:#fff;border:1px solid #666}
button{cursor:pointer}.ok{color:#8f8}.bad{color:#ff8b8b}.muted{font-size:12px;color:#999}
@media(max-width:720px){.grid{grid-template-columns:1fr}}
</style></head><body>
<h2>MQTT Setup</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div class=card>
 <div class=grid>
  <div class=field><label>MQTT</label><select id=enabled><option value=0>Disabled</option><option value=1>Enabled</option></select></div>
  <div class=field><label>Broker Host / IP</label><input id=host placeholder="192.168.1.10"></div>
  <div class=field><label>Port</label><input id=port type=number min=1 max=65535></div>
  <div class=field><label>Username</label><input id=user></div>
  <div class=field><label>Password</label><input id=pass type=password placeholder="Leave blank to keep saved password"></div>
  <div class=field><label>Clear Saved Password</label><select id=clearPass><option value=0>No</option><option value=1>Yes</option></select></div>
  <div class=field><label>Base Topic</label><input id=base placeholder="multiflow/site"></div>
  <div class=field><label>Retain Routine Publishes</label><select id=retain><option value=0>No</option><option value=1>Yes</option></select></div>
 </div>
</div>
<div class=card>
 <h3>Publish Frequency</h3>
 <div class=muted>Routine traffic is rate-limited independently. Future alarm and rollover events will publish immediately.</div>
 <div class=grid style="margin-top:10px">
  <div class=field><label>Live Process Data (seconds)</label><input id=live type=number min=1 max=3600></div>
  <div class=field><label>Totalizer Snapshot (seconds)</label><input id=totalizer type=number min=10 max=86400></div>
  <div class=field><label>Status / Health (seconds)</label><input id=status type=number min=10 max=3600></div>
 </div>
</div>
<div class=card>
 <div id=conn>Loading...</div>
 <div id=cfgState class=muted></div>
 <div id=stats class=muted></div>
 <div class=muted style="margin-top:6px">Traffic count is MQTT application data (topic + payload). TCP/IP and MQTT packet overhead are not included.</div>
</div>
<p><button onclick=save()>Save MQTT Settings</button> <span id=msg class=ok></span></p>
<script>
const e=id=>document.getElementById(id);
async function fetchCfg(){
 return fetch('/api/mqtt-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
}
async function fetchTraffic(){
 return fetch('/api/mqtt-traffic?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
}
function updateStatus(x,t){
 e('conn').className=x.connected?'ok':'bad';
 e('conn').textContent=x.connected?'MQTT CONNECTED':'MQTT DISCONNECTED (state '+x.state+')';
 e('cfgState').textContent='Saved config: '+(x.host?x.host+':'+x.port:'no broker stored')+
   ' | Flash Primary '+(x.primaryStored?'OK':'MISSING')+
   ' | Flash Backup '+(x.backupStored?'OK':'MISSING');
 const mins=Math.max((t.trafficElapsedSec||0)/60,0);
 const bytes=Number(t.appBytes||0);
 const bpm=mins>0?bytes/mins:0;
 const mbDay=bpm*1440/1000000;
 e('stats').innerHTML=
   'Connections '+x.connectCount+' | Publishes '+x.publishCount+' | Publish failures '+x.publishFails+
   '<br>MQTT app data: '+bytes.toLocaleString()+' bytes ('+(bytes/1000000).toFixed(3)+' MB)'+
   ' | Avg '+bpm.toFixed(1)+' bytes/min | '+mbDay.toFixed(3)+' MB/day equivalent'+
   '<br>Payload '+Number(t.payloadBytes||0).toLocaleString()+' B | Topics '+Number(t.topicBytes||0).toLocaleString()+' B'+
   '<br>Today '+Number(t.dailyAppBytes||0).toLocaleString()+' B | '+Number(t.dailyPublishes||0).toLocaleString()+' publishes | Daily usage queued '+Number(t.dailyUsageQueued||0)+
   ' | Hourly history queued '+x.hourlyHistoryQueued+' | Daily history queued '+x.dailyRolloverQueued+
   '<br>NVS entries: '+(x.nvsValid?(x.nvsUsed+' used / '+x.nvsFree+' free / '+x.nvsTotal+' total'):'unavailable')+
   (x.hasPassword?' | Password saved':'');
}
async function loadForm(){
 try{
  const x=await fetchCfg();

  // Configuration is authoritative and must never depend on the optional
  // traffic-statistics endpoint.
  e('enabled').value=x.enabled?'1':'0';
  e('host').value=x.host||'';
  e('port').value=x.port;
  e('user').value=x.user||'';
  e('base').value=x.baseTopic||'';
  e('retain').value=x.retain?'1':'0';
  e('live').value=x.liveInterval;
  e('totalizer').value=x.totalizerInterval;
  e('status').value=x.statusInterval;
  let t={payloadBytes:0,topicBytes:0,appBytes:0,trafficElapsedSec:0};
  try{t=await fetchTraffic();}catch(_){}
  updateStatus(x,t);
 }catch(err){
  e('conn').className='bad';
  e('conn').textContent='MQTT CONFIG LOAD FAILED';
  e('stats').textContent=String(err);
 }
}
async function refreshStatus(){
 try{
  const x=await fetchCfg();
  let t={payloadBytes:0,topicBytes:0,appBytes:0,trafficElapsedSec:0};
  try{t=await fetchTraffic();}catch(_){}
  updateStatus(x,t);
 }catch(_){}
}
async function save(){
 let p=new URLSearchParams({
  enabled:e('enabled').value,host:e('host').value,port:e('port').value,user:e('user').value,
  pass:e('pass').value,clearPass:e('clearPass').value,base:e('base').value,retain:e('retain').value,
  live:e('live').value,totalizer:e('totalizer').value,status:e('status').value
 });
 let r=await fetch('/api/mqtt-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 e('msg').textContent=r.ok?'Saved - MQTT connection will update automatically':'SAVE FAILED';
 if(r.ok){
  e('pass').value='';
  e('clearPass').value='0';
 }
 setTimeout(refreshStatus,500);
}
loadForm();
setInterval(refreshStatus,3000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/run1-bindings",HTTP_GET,[](){
    String j="{\"inputs\":[";
    for(uint8_t role=0;role<3;role++){
      if(role)j+=",";
      FlowResolvedPoint p=resolveRun1Input(role);
      j+="{\"role\":"+String(role)+",\"ai\":"+String(run1Bind[role].sourceIndex+1);
      j+=",\"manual\":"+String(run1Bind[role].manual?"true":"false");
      j+=",\"manualValue\":"+String(run1Bind[role].manualValue,3);
      j+=",\"value\":"+String(isfinite(p.value)?p.value:0.0f,3);
      j+=",\"quality\":\""+String(flowQualityName(p.quality))+"\"}";
    }
    j+="]}";
    server.send(200,"application/json",j);
  });

  server.on("/api/run1-bindings",HTTP_POST,[](){
    for(uint8_t role=0;role<3;role++){
      String rp=String(role);
      String ai="ai"+rp, man="manual"+rp, val="value"+rp;
      if(server.hasArg(ai)){
        int n=server.arg(ai).toInt();
        if(n>=1&&n<=8)run1Bind[role].sourceIndex=(uint8_t)(n-1);
      }
      if(server.hasArg(man))run1Bind[role].manual=server.arg(man)=="1"||server.arg(man)=="true"||server.arg(man)=="on";
      if(server.hasArg(val))run1Bind[role].manualValue=server.arg(val).toFloat();
    }
    saveRun1Bindings();
    displayDirty=true;
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/flow-run-storage",HTTP_GET,[](){
    String j="{\"loaded\":"+String(flowRunFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(flowRunFlashLastSaveOK?"true":"false");
    j+=",\"status\":\""+flowRunFlashStatus+"\"";
    j+=",\"path\":\""+String(FLOWRUN_CFG_PATH)+"\"";
    j+=",\"bytes\":"+String((unsigned)sizeof(FlowRunsFlashConfigV1));
    j+=",\"enabled\":[";
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){if(run)j+=",";j+=String(flowRunEnabled[run]?"true":"false");}
    j+="],\"names\":[";
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){if(run)j+=",";j+="\""+String(flowRunName[run])+"\"";}
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/flow-run-retire-legacy-nvs",HTTP_POST,[](){
    if(!flowRunFlashLoaded){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"valid flash flow-run config must be loaded first\"}");
      return;
    }
    bool ok=true;
    if(!prefs.begin("flowruns",false))ok=false;
    else{ok=prefs.clear()&&ok;prefs.end();}
    if(!prefs.begin("flowmeta",false))ok=false;
    else{ok=prefs.clear()&&ok;prefs.end();}
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:500,"application/json",ok?
      "{\"ok\":true,\"cleared\":[\"flowruns\",\"flowmeta\"],\"flashAuthoritative\":true}":
      "{\"ok\":false,\"error\":\"legacy flow-run NVS clear failed\"}");
  });

  // EP32 FLOW READY REPORTING GATE A2D V1
  server.on("/api/flow-runs",HTTP_GET,[](){
    String j="{\"runs\":[";
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      if(run)j+=",";
      j+="{\"run\":"+String(run+1);
      j+=",\"enabled\":"+String(flowRunEnabled[run]?"true":"false");
      j+=",\"name\":\""+String(flowRunName[run])+"\"";
      j+=",\"lsd\":\""+String(flowRunLSD[run])+"\"";
      bool ready=true;
      for(uint8_t role=0;role<3;role++){
        FlowResolvedPoint rp=resolveFlowRunInput(run,role);
        if(!isfinite(rp.value) || rp.quality==FLOW_QUALITY_BAD)ready=false;
      }
      bool calcActive=flowRunEnabled[run]&&ready;
      bool currentValid=ready&&flowRunValid[run];
      j+=",\"displayName\":\""+flowRunDisplayName(run)+"\"";
      j+=",\"ready\":"+String(ready?"true":"false");
      j+=",\"calcActive\":"+String(calcActive?"true":"false");
      j+=",\"valid\":"+String(currentValid?"true":"false");
      j+=",\"flow\":"+String(currentValid?flowRunFlow[run]:0.0f,6);
      j+=",\"flowToday\":"+String(flowDay[run].volumeE3m3,6);
      j+=",\"flowHours\":"+String(flowDay[run].flowHours,6);
      j+=",\"calcCount\":"+String(flowRunCalcCount[run]);
      j+=",\"calcMs\":"+String(flowRunCalcMs[run]);
      j+=",\"controlMode\":\""+String(flowControlModeName(flowPIDCfg[run].mode))+"\"";
      j+=",\"controlOut\":"+String(flowPIDRt[run].output,1);
      j+=",\"pressureOverrideActive\":"+String(flowPIDRt[run].pressureOverrideActive?"true":"false");
      j+=",\"inputs\":[";
      for(uint8_t role=0;role<3;role++){
        if(role)j+=",";
        FlowInputBinding &b=(run==0)?run1Bind[role]:flowRunBind[run][role];
        FlowResolvedPoint p=resolveFlowRunInput(run,role);
        j+="{\"role\":"+String(role)+",\"ai\":"+String(b.sourceIndex+1);
        j+=",\"manual\":"+String(b.manual?"true":"false");
        j+=",\"manualValue\":"+String(b.manualValue,6);
        j+=",\"value\":"+String(isfinite(p.value)?p.value:0.0f,6);
        j+=",\"quality\":\""+String(flowQualityName(p.quality))+"\"}";
      }
      j+="]}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/flow-run-save",HTTP_POST,[](){
    if(!server.hasArg("run")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"missing run\"}");
      return;
    }
    int n=server.arg("run").toInt();
    if(n<1||n>FLOW_RUN_COUNT){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    uint8_t run=(uint8_t)(n-1);

    if(server.hasArg("enabled"))flowRunEnabled[run]=(run==0)||server.arg("enabled")=="1";
    if(server.hasArg("name")){
      String v=server.arg("name");v.toCharArray(flowRunName[run],sizeof(flowRunName[run]));
    }
    if(server.hasArg("lsd")){
      String v=server.arg("lsd");v.toCharArray(flowRunLSD[run],sizeof(flowRunLSD[run]));
    }
    for(uint8_t role=0;role<3;role++){
      String rs=String(role);
      String ai="ai"+rs,man="manual"+rs,val="value"+rs;
      FlowInputBinding &b=(run==0)?run1Bind[role]:flowRunBind[run][role];
      if(server.hasArg(ai)){
        int ch=server.arg(ai).toInt();
        if(ch>=1&&ch<=8)b.sourceIndex=(uint8_t)(ch-1);
      }
      if(server.hasArg(man))b.manual=server.arg(man)=="1";
      if(server.hasArg(val))b.manualValue=server.arg(val).toFloat();
    }
    if(run==0)saveRun1Bindings();
    for(uint8_t role=0;role<3;role++)flowRunBind[0][role]=run1Bind[role];
    saveFlowRunFoundation(run);
    requestFullEPaperRefresh();
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/flow-setup",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Flow Setup</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#0d1113;color:#eee;margin:0}
.wrap{max-width:1180px;margin:auto;padding:14px}a{color:#63e6be}
.flow{border:1px solid #46545a;border-radius:10px;background:#171d20;margin:10px 0;overflow:hidden}
.flowHead{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;padding:11px 13px;background:#11181b;border-bottom:1px solid #354247}
.flowTitle{font-size:19px;font-weight:700;color:#63e6be}.status{font-weight:700}.good{color:#8f8}.warn{color:#ffd166}.bad{color:#ff8b8b}.muted{font-size:12px;color:#999}
.line{display:flex;gap:8px;align-items:end;flex-wrap:wrap;padding:10px 12px;border-bottom:1px solid #2d363a}
.line:last-child{border-bottom:0}.field{min-width:125px;flex:1}.wide{flex:1.4}.smallField{flex:.65}
label{display:block;color:#aaa;font-size:11px;margin-bottom:4px}
input,select,button{width:100%;padding:8px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}
button{cursor:pointer;width:auto}.save{background:#203a35;border-color:#4d8177;font-weight:700}
.live{min-width:150px;font-size:17px;font-weight:700;padding-bottom:7px}
.rowTag{min-width:82px;color:#75d7ff;font-weight:700;padding-bottom:8px}
details.gas{width:100%;background:#101619;border:1px solid #344;border-radius:7px;padding:8px}
details.gas summary{cursor:pointer;color:#63e6be;font-weight:700}
.gasGrid{display:grid;grid-template-columns:repeat(4,minmax(120px,1fr));gap:7px;margin-top:8px}
.gasTotal{font-weight:700;margin-top:8px}.gasGood{color:#8f8}.gasBad{color:#ff8b8b}
.msg{margin:10px 0;min-height:20px}
@media(max-width:760px){.line{align-items:stretch}.field,.wide,.smallField{min-width:46%;flex:1}.rowTag{width:100%;padding-bottom:0}.gasGrid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:480px){.field,.wide,.smallField{min-width:100%}.gasGrid{grid-template-columns:1fr}}
</style></head><body><div class=wrap>
<h2 style="margin-bottom:4px">Riot-Flow Flow Setup</h2>
<div class=muted>Six-run commissioning: identity, AGA meter information, and process inputs on one page.</div>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div id=flows>Loading...</div><div id=msg class=msg></div>
</div>
<script>
const roles=['DP','Static','Temperature'];
const gasNames=['Methane','Nitrogen','Carbon Dioxide','Ethane','Propane','Isobutane','n-Butane','Isopentane','n-Pentane','n-Hexane','n-Heptane','n-Octane','n-Nonane','n-Decane','Hydrogen','Oxygen','Carbon Monoxide','Water','Hydrogen Sulfide','Helium','Argon'];
let gasCache={};

function esc(v){return String(v??'').replaceAll('&','&amp;').replaceAll('"','&quot;').replaceAll('<','&lt;')}
function aiOpts(sel){let h='';for(let i=1;i<=8;i++)h+='<option value="'+i+'" '+(i==sel?'selected':'')+'>M31 AI'+i+'</option>';return h}
function fld(label,id,val,cls='field',type='number',step='.001'){return '<div class="'+cls+'"><label>'+label+'</label><input id="'+id+'" type="'+type+'" '+(type==='number'?'step="'+step+'" ':'')+'value="'+esc(val)+'"></div>'}
function sel(label,id,html,cls='field'){return '<div class="'+cls+'"><label>'+label+'</label><select id="'+id+'">'+html+'</select></div>'}

function flowBlock(r,a){
 const enabled=r.enabled, stat=enabled?(r.valid?'VALID':'WAITING'):'DISABLED';
 const statClass=enabled?(r.valid?'good':'warn'):'muted';
 let inputHtml='';
 (r.inputs||[]).forEach((p,i)=>{
   inputHtml += '<div class="field"><label>'+roles[i]+' Source</label><select id="ai'+r.run+'_'+i+'">'+aiOpts(p.ai)+'</select></div>'+
    '<div class="smallField"><label>'+roles[i]+' Mode</label><select id="man'+r.run+'_'+i+'"><option value=0 '+(!p.manual?'selected':'')+'>LIVE</option><option value=1 '+(p.manual?'selected':'')+'>MANUAL</option></select></div>'+
    '<div class="field"><label>'+roles[i]+' Manual Value</label><input id="val'+r.run+'_'+i+'" type=number step=.001 value="'+p.manualValue+'"><div id="q'+r.run+'_'+i+'" class=muted>'+Number(p.value).toFixed(3)+' · '+p.quality+'</div></div>';
 });
 return '<div class=flow id=flow'+r.run+'>'+
  '<div class=flowHead><div><span class=flowTitle>Flow '+r.run+'</span> <span id=st'+r.run+' class="status '+statClass+'">'+stat+'</span></div>'+
  '<button class=save onclick="saveFlow('+r.run+')">Save Flow '+r.run+'</button></div>'+
  '<div class=line><div class=rowTag>AGA Name</div>'+
    '<div class=smallField><label>Enabled</label><input id=en'+r.run+' type=checkbox '+(enabled?'checked':'')+' '+(r.run===1?'disabled':'')+'></div>'+
    fld('Name','name'+r.run,r.name||'','wide','text')+
    fld('LSD','lsd'+r.run,r.lsd||'','wide','text')+
    '<div class=live id=live'+r.run+'>'+(r.valid?Number(r.flow).toFixed(3):'---')+' E3 m3/day</div></div>'+
  '<div class=line><div class=rowTag>AGA Info</div>'+
    fld('Pipe ID mm','pipe'+r.run,a.pipe)+fld('Orifice mm','orifice'+r.run,a.orifice)+
    fld('Base P kPa(a)','baseP'+r.run,a.baseP)+fld('Base Temp C','baseT'+r.run,a.baseT)+
    fld('Atmospheric kPa','atm'+r.run,a.atm)+
    sel('Static Pressure','abs'+r.run,'<option value=0 '+(!a.absolute?'selected':'')+'>Gauge</option><option value=1 '+(a.absolute?'selected':'')+'>Absolute</option>')+
    sel('Tap Type','tap'+r.run,'<option value=0 '+(a.tap===0?'selected':'')+'>Flange</option><option value=1 '+(a.tap===1?'selected':'')+'>Corner</option><option value=2 '+(a.tap===2?'selected':'')+'>D-D/2</option>')+
    '<details class=gas><summary>Gas Analysis</summary><div id=gas'+r.run+' class=muted style="margin-top:8px">Open to load analysis...</div></details></div>'+
  '<div class=line><div class=rowTag>AGA Inputs</div>'+inputHtml+'</div></div>';
}

async function loadGas(run){
 if(gasCache[run])return;
 const box=document.getElementById('gas'+run); if(!box)return;
 try{
  const x=await fetch('/api/aga-gas?run='+run+'&x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  gasCache[run]=x.gas.slice();
  box.className='';
  box.innerHTML='<div class=gasGrid>'+gasNames.map((n,i)=>'<div><label>'+n+'</label><input id="g'+run+'_'+i+'" type=number step=.0001 value="'+x.gas[i]+'" oninput="gasTotal('+run+')"></div>').join('')+
   '</div><div id=gt'+run+' class="gasTotal"></div><p><button onclick="saveGas('+run+')">Save Gas Analysis</button></p>';
  gasTotal(run);
 }catch(e){box.textContent='Gas analysis load failed';}
}
function gasTotal(run){
 let t=0;for(let i=0;i<21;i++){const e=document.getElementById('g'+run+'_'+i);if(e)t+=Number(e.value)||0}
 const o=document.getElementById('gt'+run);if(o){o.textContent='Mole % Total: '+t.toFixed(4)+'%';o.className='gasTotal '+(Math.abs(t-100)<=.01?'gasGood':'gasBad')}
}
async function saveGas(run){
 let p=new URLSearchParams({run:String(run)});for(let i=0;i<21;i++)p.set('c'+i,document.getElementById('g'+run+'_'+i).value);
 const r=await fetch('/api/aga-gas-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json()}catch(e){}
 msg.textContent=r.ok?'Flow '+run+' gas saved':(x.error||'Gas save failed');msg.className='msg '+(r.ok?'good':'bad');
}
async function load(){
 const [fr,ag]=await Promise.all([
  fetch('/api/flow-runs?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
  fetch('/api/aga-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json())
 ]);
 flows.innerHTML=fr.runs.map((r,i)=>flowBlock(r,ag.runs[i])).join('');
 document.querySelectorAll('details.gas').forEach((d,i)=>d.addEventListener('toggle',()=>{if(d.open)loadGas(i+1)}));
}
async function saveFlow(run){
 msg.textContent='Saving Flow '+run+'...';msg.className='msg muted';
 const p=new URLSearchParams({run:String(run),enabled:document.getElementById('en'+run).checked?'1':'0',
  name:document.getElementById('name'+run).value,lsd:document.getElementById('lsd'+run).value});
 for(let i=0;i<3;i++){p.set('ai'+i,document.getElementById('ai'+run+'_'+i).value);p.set('manual'+i,document.getElementById('man'+run+'_'+i).value);p.set('value'+i,document.getElementById('val'+run+'_'+i).value)}
 const a=new URLSearchParams({run:String(run),pipe:document.getElementById('pipe'+run).value,orifice:document.getElementById('orifice'+run).value,
  baseP:document.getElementById('baseP'+run).value,baseT:document.getElementById('baseT'+run).value,atm:document.getElementById('atm'+run).value,
  absolute:document.getElementById('abs'+run).value,tap:document.getElementById('tap'+run).value});
 const r1=await fetch('/api/flow-run-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 const r2=await fetch('/api/aga-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:a});
 msg.textContent=(r1.ok&&r2.ok)?'Flow '+run+' saved':'SAVE FAILED';msg.className='msg '+((r1.ok&&r2.ok)?'good':'bad');
 if(r1.ok&&r2.ok)setTimeout(load,250);
}
async function refreshLive(){
 try{
  const x=await fetch('/api/flow-runs?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  for(const r of x.runs){
   const st=document.getElementById('st'+r.run), lv=document.getElementById('live'+r.run);
   if(st){st.textContent=r.enabled?(r.valid?'VALID':'WAITING'):'DISABLED';st.className='status '+(r.enabled?(r.valid?'good':'warn'):'muted')}
   if(lv)lv.textContent=(r.valid?Number(r.flow).toFixed(3):'---')+' E3 m3/day';
   (r.inputs||[]).forEach((p,i)=>{const q=document.getElementById('q'+r.run+'_'+i);if(q)q.textContent=Number(p.value).toFixed(3)+' · '+p.quality});
  }
 }catch(e){}
}
load();setInterval(refreshLive,1500);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/flow-runs",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow Flow Runs</title>
<style>
body{font-family:Arial;background:#111;color:#eee;margin:16px}a{color:#63e6be}
.run{border:1px solid #445;border-radius:9px;background:#171717;margin:9px 0;padding:10px}
.head{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}
.name{font-size:18px;font-weight:700;color:#63e6be}.muted{color:#999;font-size:12px}.active{color:#8f8}.standby{color:#ffd166}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;margin-top:8px}
.box{background:#0d1213;border:1px solid #344;border-radius:7px;padding:8px}
input,select,button{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:4px}
input.txt{width:180px}input.num{width:110px}.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
@media(max-width:760px){.grid{grid-template-columns:1fr}}
</style></head><body>
<h2>Flow Runs</h2><style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div id=runs>Loading...</div><p id=msg class=active></p>
<script>
const roleNames=['DP','Static','Temperature'];
function opts(sel){let o='';for(let n=1;n<=8;n++)o+='<option value="'+n+'" '+(n===sel?'selected':'')+'>M31 AI'+n+'</option>';return o}
function render(x){
 runs.innerHTML=x.runs.map(r=>{
  let ins=r.inputs.map((p,i)=>'<div class=box><b>'+roleNames[i]+'</b><div class=row><select id=a'+r.run+'_'+i+'>'+opts(p.ai)+'</select>'+
   '<select id=m'+r.run+'_'+i+'><option value=0 '+(!p.manual?'selected':'')+'>LIVE</option><option value=1 '+(p.manual?'selected':'')+'>MANUAL</option></select>'+
   '<input class=num id=v'+r.run+'_'+i+' type=number step=.001 value="'+p.manualValue+'"></div>'+
   '<div class=muted id=q'+r.run+'_'+i+'>Resolved '+Number(p.value).toFixed(3)+' | '+p.quality+'</div></div>').join('');
  return '<div class=run><div class=head><div><span class=name>Run '+r.run+'</span> <span id=s'+r.run+' class="'+(r.calcActive?'active':'standby')+'">'+(r.calcActive?(r.valid?'CALCULATING':'WAITING INPUT'):'DISABLED')+'</span></div>'+
   '<label><input type=checkbox id=e'+r.run+' '+(r.enabled?'checked':'')+' '+(r.run===1?'disabled':'')+'> Enabled</label></div>'+
   '<div class=row style="margin-top:8px"><input class=txt id=n'+r.run+' placeholder="Run name" value="'+r.name+'"><input class=txt id=l'+r.run+' placeholder="LSD" value="'+r.lsd+'">'+
   (r.calcActive?'<b id=f'+r.run+'>'+(r.valid?Number(r.flow).toFixed(3):'---')+' E3 m3/day</b>':'<b id=f'+r.run+'></b>')+'</div><div class=grid>'+ins+'</div>'+
   '<p><button onclick="save('+r.run+')">Save Run '+r.run+'</button></p></div>'
 }).join('');
}
async function load(){let x=await fetch('/api/flow-runs?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());render(x)}
async function refreshLive(){
 try{
  let x=await fetch('/api/flow-runs?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  for(const r of x.runs){
   const st=document.getElementById('s'+r.run);
   if(st){
    st.textContent=r.calcActive?(r.valid?'CALCULATING':'WAITING INPUT'):'DISABLED';
    st.className=r.calcActive?'active':'standby';
   }
   const fl=document.getElementById('f'+r.run);
   if(fl)fl.textContent=r.calcActive?((r.valid?Number(r.flow).toFixed(3):'---')+' E3 m3/day'):'';
   (r.inputs||[]).forEach((p,i)=>{
    const q=document.getElementById('q'+r.run+'_'+i);
    if(q)q.textContent='Resolved '+Number(p.value).toFixed(3)+' | '+p.quality;
   });
  }
 }catch(e){}
}
async function save(run){
 let p=new URLSearchParams({run:run,enabled:document.getElementById('e'+run).checked?'1':'0',name:document.getElementById('n'+run).value,lsd:document.getElementById('l'+run).value});
 for(let i=0;i<3;i++){p.set('ai'+i,document.getElementById('a'+run+'_'+i).value);p.set('manual'+i,document.getElementById('m'+run+'_'+i).value);p.set('value'+i,document.getElementById('v'+run+'_'+i).value)}
 let r=await fetch('/api/flow-run-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Run '+run+' saved':'SAVE FAILED';if(r.ok)setTimeout(load,250)
}
load();
setInterval(refreshLive,1000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/golden-reference",HTTP_POST,[](){
    loadGoldenReference();
    requestFullEPaperRefresh();
    server.send(200,"application/json","{\"ok\":true,\"referenceFlow\":75.787048}");
  });

  server.on("/api/process-config",HTTP_GET,[](){
    String j="{\"ai\":[";
    for(int i=0;i<8;i++){
      if(i)j+=",";
      j+="{\"name\":\""+String(aiCfg[i].name)+"\",\"units\":\""+String(aiCfg[i].units)+"\",\"lo\":"+String(aiCfg[i].euLow,4)+",\"hi\":"+String(aiCfg[i].euHigh,4)+",\"manual\":"+String(aiManual[i]?"true":"false")+",\"manualValue\":"+String(aiManualEU[i],4)+"}";
    }
    j+="],\"meter\":{\"pipe\":"+String(agaCfg.pipeID_mm,4)+",\"orifice\":"+String(agaCfg.orifice_mm,4)+",\"baseP\":"+String(agaCfg.basePressure_kPaAbs,4)+",\"baseT\":"+String(agaCfg.baseTemperature_C,4)+",\"atm\":"+String(agaCfg.atmosphericPressure_kPa,4)+",\"absolute\":"+String(agaCfg.staticPressureIsAbsolute?"true":"false")+",\"tap\":"+String(agaCfg.tapType)+"}}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/process-setup",HTTP_GET,[](){
    String h=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>MultiFlow Process Setup</title>
<style>body{font-family:Arial;background:#111;color:#eee;margin:18px}.card{max-width:760px;margin:auto;background:#1b2229;padding:18px;border-radius:10px}input,button{padding:9px;margin:4px}table{width:100%}td{padding:5px}</style></head>
<body><div class=card><h2>Process / AGA Setup</h2><p>This commissioning page uses the same NVS keys as the proven MultiFlow firmware.</p>
<p><button onclick="gold()">Load Golden Reference</button> <span id=g></span></p>
<div id=o>Loading...</div><style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div></div>
<script>
async function gold(){let r=await fetch('/api/golden-reference',{method:'POST'});g.textContent=r.ok?'Loaded reference target 75.787048 E3 m3/day':'FAILED';setTimeout(load,500)}
async function load(){let x=await fetch('/api/process-config').then(r=>r.json());let h='<h3>Analog Inputs</h3><table>';
x.ai.forEach((a,i)=>h+='<tr><td>AI'+(i+1)+'</td><td>'+a.name+'</td><td>'+a.lo+' to '+a.hi+' '+a.units+'</td><td>'+(a.manual?'MANUAL '+a.manualValue:'LIVE')+'</td></tr>');
h+='</table><h3>Run 1 Meter</h3><p>Pipe '+x.meter.pipe+' mm &nbsp; Orifice '+x.meter.orifice+' mm</p><p>Base '+x.meter.baseP+' kPa(a) @ '+x.meter.baseT+' C</p><p><b>AGA calculation core:</b> NIST DETAIL + AGA3 Run 1 active.</p>';o.innerHTML=h}load();
</script></body></html>)HTML";
    server.send(200,"text/html",h);
  });


  server.on("/api/operator-config-retire-legacy-nvs",HTTP_POST,[](){
    if(!operatorFlashLoaded){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"valid flash operator config must be loaded first\"}");
      return;
    }
    bool ok=true;
    if(!prefs.begin("analog",false))ok=false;
    else{ok=prefs.clear()&&ok;prefs.end();}
    if(!prefs.begin("modbus",false))ok=false;
    else{ok=prefs.clear()&&ok;prefs.end();}
    String j="{\"ok\":"+String(ok?"true":"false");
    j+=",\"cleared\":[\"analog\",\"modbus\"]";
    j+=",\"flashAuthoritative\":true}";
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:500,"application/json",j);
  });

  server.on("/api/operator-config-status",HTTP_GET,[](){
    String j="{\"loaded\":"+String(operatorFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(operatorFlashLastSaveOK?"true":"false");
    j+=",\"status\":\""+operatorFlashStatus+"\"";
    j+=",\"path\":\""+String(OPERATOR_CFG_PATH)+"\"";
    j+=",\"version\":"+String(OPERATOR_CFG_VERSION);
    j+=",\"bytes\":"+String((unsigned)sizeof(OperatorFlashConfigV2));
    j+=",\"rackHost\":\""+String(ep32M31HostModel())+"\"";
    j+=",\"slot1\":\""+String(ep32M31ModuleModel(ep32M31Slot1))+"\"";
    j+=",\"slot2\":\""+String(ep32M31ModuleModel(ep32M31Slot2))+"\"";
    j+=",\"transport\":\""+String(m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU")+"\"";
    j+=",\"enabled\":"+String(modbusCommsEnabled?"true":"false");
    j+=",\"host\":\""+m31TcpHost+"\",\"port\":"+String(m31TcpPort);
    j+=",\"unitId\":"+String(m31UnitId)+",\"rtuBaud\":"+String(m31RtuBaud);
    j+=",\"expansion\":\""+String(ep32M31ExpansionModel())+"\"";
    j+=",\"aiManual\":[";
    for(uint8_t i=0;i<16;i++){if(i)j+=",";j+=String(aiManual[i]?"true":"false");}
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  // EP32 IO MQTT GATE6B2 API QUOTE FIX V2
  server.on("/api/io-mqtt-config",HTTP_GET,[](){
    String j="{\"loaded\":"+String(ioMqttFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(ioMqttFlashLastSaveOK?"true":"false");
    j+=",\"status\":\""+ioMqttFlashStatus+"\"";
    j+=",\"ai\":[";
    for(uint8_t i=0;i<8;i++){if(i)j+=",";j+=String(ioMqttAI[i]?"true":"false");}
    j+="],\"di\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(ioMqttDI[i]?"true":"false");}
    j+="],\"do\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(ioMqttDO[i]?"true":"false");}
    j+="],\"ao\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(ioMqttAO[i]?"true":"false");}
    j+="],\"topic\":\""+mqttBaseTopicClean()+"/io/live\"";
    j+=",\"heartbeatSec\":"+String(mqttCfg.liveIntervalSec);
    j+=",\"publishCount\":"+String(ioMqttPublishCount);
    j+=",\"changePublishCount\":"+String(ioMqttChangePublishCount);
    j+=",\"lastPublishAgeMs\":"+String(ioMqttLastPublishMs?millis()-ioMqttLastPublishMs:0);
    j+="}";
    server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
  });

  server.on("/api/io-mqtt-config",HTTP_POST,[](){
    if(!server.hasArg("type") || !server.hasArg("ch") || !server.hasArg("enabled")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"type, ch and enabled required\"}");return;
    }
    String type=server.arg("type");type.toUpperCase();
    int ch=server.arg("ch").toInt();
    bool enabled=server.arg("enabled")=="1" || server.arg("enabled")=="true";
    if(type=="AI"){
      if(ch<1||ch>8){server.send(400,"application/json","{\"ok\":false,\"error\":\"AI channel must be 1-8\"}");return;}
      ioMqttAI[ch-1]=enabled;
    }else if(type=="DI"){
      if(ch<1||ch>4){server.send(400,"application/json","{\"ok\":false,\"error\":\"DI channel must be 1-4\"}");return;}
      ioMqttDI[ch-1]=enabled;
    }else if(type=="DO"){
      if(ch<1||ch>4){server.send(400,"application/json","{\"ok\":false,\"error\":\"DO channel must be 1-4\"}");return;}
      ioMqttDO[ch-1]=enabled;
    }else if(type=="AO"){
      if(ch<1||ch>4){server.send(400,"application/json","{\"ok\":false,\"error\":\"AO channel must be 1-4\"}");return;}
      ioMqttAO[ch-1]=enabled;
    }else{
      server.send(400,"application/json","{\"ok\":false,\"error\":\"type must be AI, DI, DO or AO\"}");return;
    }
    if(!saveIOMQTTConfig()){server.send(500,"application/json","{\"ok\":false,\"error\":\"I/O MQTT flash save failed\"}");return;}
    String j="{\"ok\":true,\"type\":\""+type+"\",\"ch\":"+String(ch)+",\"enabled\":"+String(enabled?"true":"false")+"}";
    server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
  });

  server.on("/api/io-config",HTTP_GET,[](){
    String j="{\"channels\":[";
    for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){
      if(i)j+=",";
      j+="{\"ch\":"+String(i+1);
      j+=",\"name\":\""+String(aiCfg[i].name)+"\"";
      j+=",\"units\":\""+String(aiCfg[i].units)+"\"";
      j+=",\"low\":"+String(aiCfg[i].euLow,6);
      j+=",\"high\":"+String(aiCfg[i].euHigh,6);
      j+=",\"manual\":"+String(aiManual[i]?"true":"false");
      j+=",\"manualEU\":"+String(aiManualEU[i],6);
      j+=",\"mA\":"+String(ai_mA[i],6);
      j+=",\"value\":"+String(aiEU[i],6)+"}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/io-save",HTTP_POST,[](){
    if(!server.hasArg("ch")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"missing channel\"}");
      return;
    }
    int ch=server.arg("ch").toInt()-1;
    if(ch<0||ch>=ep32M31ActiveAiCount()){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad channel\"}");
      return;
    }
    if(server.hasArg("name")){
      String v=server.arg("name"); v.toCharArray(aiCfg[ch].name,sizeof(aiCfg[ch].name));
    }
    if(server.hasArg("units")){
      String v=server.arg("units"); v.toCharArray(aiCfg[ch].units,sizeof(aiCfg[ch].units));
    }
    if(server.hasArg("low"))aiCfg[ch].euLow=server.arg("low").toFloat();
    if(server.hasArg("high"))aiCfg[ch].euHigh=server.arg("high").toFloat();
    if(server.hasArg("manual"))aiManual[ch]=server.arg("manual")=="1";
    if(server.hasArg("manualEU"))aiManualEU[ch]=server.arg("manualEU").toFloat();
    saveAnalogChannel((uint8_t)ch);
    updateEngineeringUnits();
    displayDirty=true;
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/io-di-force",HTTP_POST,[](){
    if(!server.hasArg("ch") || !server.hasArg("mode")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"channel and mode required\"}");return;
    }
    int ch=server.arg("ch").toInt();
    if(ch<1 || ch>4){server.send(400,"application/json","{\"ok\":false,\"error\":\"DI channel must be 1-4\"}");return;}
    uint8_t i=(uint8_t)(ch-1);
    String mode=server.arg("mode");mode.toUpperCase();
    if(mode=="NORMAL" || mode=="RELEASE"){
      diForceActive[i]=false;updateEffectiveDI();displayDirty=true;
      String j="{\"ok\":true,\"channel\":"+String(ch)+",\"mode\":\"NORMAL\",\"raw\":"+String(diRawState[i]?"true":"false")+",\"effective\":"+String(diState[i]?"true":"false")+"}";
      server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);return;
    }
    if(mode!="FORCE" || !server.hasArg("state")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"FORCE requires state=0 or 1\"}");return;
    }
    diForceValue[i]=server.arg("state")=="1";diForceActive[i]=true;updateEffectiveDI();displayDirty=true;
    String j="{\"ok\":true,\"channel\":"+String(ch)+",\"mode\":\"FORCE\",\"forced\":"+String(diForceValue[i]?"true":"false")+",\"raw\":"+String(diRawState[i]?"true":"false")+",\"effective\":"+String(diState[i]?"true":"false")+"}";
    server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
  });

  server.on("/api/io-do-force",HTTP_POST,[](){
    if(!server.hasArg("ch") || !server.hasArg("mode")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"channel and mode required\"}");return;
    }
    int ch=server.arg("ch").toInt();
    if(ch<1 || ch>4){server.send(400,"application/json","{\"ok\":false,\"error\":\"DO channel must be 1-4\"}");return;}
    uint8_t i=(uint8_t)(ch-1);
    String mode=server.arg("mode");mode.toUpperCase();
    if(mode=="NORMAL" || mode=="RELEASE"){
      doForceActive[i]=false;displayDirty=true;
      String j="{\"ok\":true,\"channel\":"+String(ch)+",\"mode\":\"NORMAL\",\"readback\":"+String(doState[i]?"true":"false")+"}";
      server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);return;
    }
    if(mode!="FORCE" || !server.hasArg("state")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"FORCE requires state=0 or 1\"}");return;
    }
    if(!modbusOnline){server.send(503,"application/json","{\"ok\":false,\"error\":\"M31 offline - DO not written\"}");return;}
    bool on=server.arg("state")=="1";
    if(!writeM31CoilRaw(i,on)){server.send(502,"application/json","{\"ok\":false,\"error\":\"Modbus DO force write failed\"}");return;}
    doForceValue[i]=on;doForceActive[i]=true;displayDirty=true;
    String j="{\"ok\":true,\"channel\":"+String(ch)+",\"mode\":\"FORCE\",\"forced\":"+String(on?"true":"false")+",\"readback\":"+String(doState[i]?"true":"false")+"}";
    server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
  });

  server.on("/api/io-release-forces",HTTP_POST,[](){
    for(uint8_t i=0;i<4;i++){diForceActive[i]=false;doForceActive[i]=false;aoForceActive[i]=false;}
    updateEffectiveDI();displayDirty=true;
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json","{\"ok\":true,\"released\":\"DI,DO,AO\"}");
  });

  server.on("/api/io-ao-force",HTTP_POST,[](){
    if(!ep32M31ExpansionPresent()){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"no AO module in active rack profile\"}");return;
    }
    if(!server.hasArg("ch") || !server.hasArg("mode")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"channel and mode required\"}");
      return;
    }

    int ch=server.arg("ch").toInt();
    if(ch<1 || ch>4){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"AO channel must be 1-4\"}");
      return;
    }
    uint8_t i=(uint8_t)(ch-1);
    String mode=server.arg("mode");
    mode.toUpperCase();

    if(mode=="NORMAL" || mode=="RELEASE"){
      aoForceActive[i]=false;
      String j="{\"ok\":true,\"channel\":"+String(ch)+",\"mode\":\"NORMAL\",\"released\":true}";
      server.sendHeader("Cache-Control","no-store");
      server.send(200,"application/json",j);
      return;
    }

    if(mode!="FORCE"){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"mode must be NORMAL or FORCE\"}");
      return;
    }
    if(!server.hasArg("percent")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"force percent required\"}");
      return;
    }
    if(!modbusOnline){
      server.send(503,"application/json","{\"ok\":false,\"error\":\"M31 offline - AO not written\"}");
      return;
    }

    float percent=constrain(server.arg("percent").toFloat(),0.0f,100.0f);
    bool ok=writeM31AORaw(i,percent);
    if(!ok){
      server.send(502,"application/json","{\"ok\":false,\"error\":\"Modbus AO write failed\"}");
      return;
    }

    aoForcePercent[i]=percent;
    aoForceActive[i]=true;
    aoForceLastRefreshMs=millis();

    float mA=4.0f+percent*0.16f;
    String j="{\"ok\":true,\"channel\":"+String(ch);
    j+=",\"mode\":\"FORCE\"";
    j+=",\"percent\":"+String(percent,1);
    j+=",\"mA\":"+String(mA,3)+"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/m31-active-profile",HTTP_GET,[](){
    String j="{\"host\":\""+String(ep32M31HostModel())+"\"";
    j+=",\"slot1\":\""+String(ep32M31ModuleModel(ep32M31Slot1))+"\"";
    j+=",\"slot2\":\""+String(ep32M31ModuleModel(ep32M31Slot2))+"\"";

    // Legacy compatibility field used by current polling/UI paths.
    j+=",\"expansion\":\""+String(ep32M31ExpansionModel())+"\"";

    j+=",\"configuredAiCount\":"+String(ep32M31ConfiguredAiCount());
    j+=",\"configuredAoCount\":"+String(ep32M31ConfiguredAoCount());

    j+=",\"aiCount\":"+String(ep32M31ActiveAiCount());
    j+=",\"aoCount\":"+String(ep32M31ActiveAoCount());

    j+=",\"diCount\":4";
    j+=",\"doCount\":4";

    j+=",\"slot1PollingEnabled\":";
    j+=(ep32M31Slot1==EP32_M31_MODULE_GXAXA0404)?"true":"false";

    j+=",\"slot2PollingEnabled\":"+String(ep32M31Slot2Ai8Present()?"true":"false");

    j+=",\"enabled\":"+String(modbusCommsEnabled?"true":"false");

    j+=",\"transport\":\"";
    j+=m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU";
    j+="\"";

    j+=",\"tcpHost\":\""+m31TcpHost+"\"";
    j+=",\"tcpPort\":"+String(m31TcpPort);
    j+=",\"unitId\":"+String(m31UnitId);
    j+=",\"rtuBaud\":"+String(m31RtuBaud);
    j+=",\"rtuTxPin\":"+String(MODBUS_TX_PIN);
    j+=",\"rtuRxPin\":"+String(MODBUS_RX_PIN);

    j+=",\"runtimeOnly\":false";
    j+=",\"profilePersistence\":\"OPERATOR_CONFIG_V2\"}";

    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/m31-active-profile",HTTP_POST,[](){

    Ep32M31HostModel newHost=ep32M31Host;
    Ep32M31ModuleType newSlot1=ep32M31Slot1;
    Ep32M31ModuleType newSlot2=ep32M31Slot2;

    if(server.hasArg("host")){
      String s=server.arg("host");
      s.toUpperCase();

      if(s=="AAAX4440G" || s=="M31-AAAX4440G"){
        newHost=EP32_M31_HOST_AAAX4440G;
      }
      else if(s=="AFAX4440G-U" || s=="M31-AFAX4440G-U"){
        newHost=EP32_M31_HOST_AFAX4440G_U;
      }
      else{
        server.send(
          400,
          "application/json",
          "{\"ok\":false,\"error\":\"unsupported host model\"}"
        );
        return;
      }
    }

    auto parseModule=[](
      String s,
      Ep32M31ModuleType& out
    )->bool{

      s.toUpperCase();

      if(s=="NONE"){
        out=EP32_M31_MODULE_NONE;
        return true;
      }

      if(s=="GXAXA0404" || s=="M31-GXAXA0404"){
        out=EP32_M31_MODULE_GXAXA0404;
        return true;
      }

      if(s=="GXAXX0800" || s=="M31-GXAXX0800"){
        out=EP32_M31_MODULE_GXAXX0800;
        return true;
      }

      return false;
    };

    if(server.hasArg("slot1")){
      if(!parseModule(server.arg("slot1"),newSlot1)){
        server.send(
          400,
          "application/json",
          "{\"ok\":false,\"error\":\"unsupported slot1 module\"}"
        );
        return;
      }
    }

    if(server.hasArg("slot2")){
      if(!parseModule(server.arg("slot2"),newSlot2)){
        server.send(
          400,
          "application/json",
          "{\"ok\":false,\"error\":\"unsupported slot2 module\"}"
        );
        return;
      }
    }

    // Backward compatibility with old expansion-only API.
    if(server.hasArg("expansion") && !server.hasArg("slot1")){
      if(!parseModule(server.arg("expansion"),newSlot1)){
        server.send(
          400,
          "application/json",
          "{\"ok\":false,\"error\":\"unsupported expansion profile\"}"
        );
        return;
      }
    }

    Ep32M31HostModel oldHost=ep32M31Host;
    Ep32M31ModuleType oldSlot1=ep32M31Slot1;
    Ep32M31ModuleType oldSlot2=ep32M31Slot2;
    Ep32M31ExpansionProfile oldExpansion=ep32M31ExpansionProfile;

    ep32M31Host=newHost;
    ep32M31Slot1=newSlot1;
    ep32M31Slot2=newSlot2;

    // Preserve proven poller behavior in Gate A1.
    ep32M31ExpansionProfile=
      (ep32M31Slot1==EP32_M31_MODULE_GXAXA0404)
      ? EP32_M31_EXP_GXAXA0404
      : EP32_M31_EXP_NONE;

    if(!ep32M31ExpansionPresent()){

      for(uint8_t i=4;i<8;i++){
        ai_mA[i]=0.0f;
        aiEU[i]=aiManual[i]?aiManualEU[i]:0.0f;
      }

      for(uint8_t i=0;i<4;i++){
        ao_mA[i]=0.0f;
        aoForceActive[i]=false;
      }
    }

    if(!saveOperatorFlashConfig()){
      ep32M31Host=oldHost;
      ep32M31Slot1=oldSlot1;
      ep32M31Slot2=oldSlot2;
      ep32M31ExpansionProfile=oldExpansion;
      server.send(500,"application/json","{\"ok\":false,\"error\":\"rack profile flash save failed\"}");
      return;
    }

    pollPhase=0;
    displayDirty=true;

    String j="{\"ok\":true";

    j+=",\"host\":\""+String(ep32M31HostModel())+"\"";
    j+=",\"slot1\":\""+String(ep32M31ModuleModel(ep32M31Slot1))+"\"";
    j+=",\"slot2\":\""+String(ep32M31ModuleModel(ep32M31Slot2))+"\"";

    j+=",\"configuredAiCount\":";
    j+=String(ep32M31ConfiguredAiCount());

    j+=",\"configuredAoCount\":";
    j+=String(ep32M31ConfiguredAoCount());

    j+=",\"runtimeAiCount\":";
    j+=String(ep32M31ActiveAiCount());

    j+=",\"runtimeAoCount\":";
    j+=String(ep32M31ActiveAoCount());

    j+=",\"slot2PollingEnabled\":"+String(ep32M31Slot2Ai8Present()?"true":"false");
    j+=",\"runtimeOnly\":false,\"persisted\":true}";

    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

// EP32 IO MQTT GATE6B1 JS QUOTE FIX V4
  // EP32 M31 AI16 IO SETUP GATE A2B V3
  server.on("/io-setup",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow I/O Setup</title>
<style>
body{font-family:Arial;background:#111;color:#eee;margin:16px}a{color:#63e6be}
.card{border:1px solid #444;border-radius:9px;padding:12px;margin:10px 0;overflow-x:auto;background:#151515}
table{width:100%;border-collapse:collapse;min-width:980px}
td,th{padding:7px;border-bottom:1px solid #333;text-align:left}
input,select{padding:6px;background:#222;color:#fff;border:1px solid #666}
.tag{width:150px}.num{width:80px}.unit{width:70px}.manual{width:90px}
button{padding:7px 12px;background:#333;color:#fff;border:1px solid #777}
.ok{color:#63e6be}.warn{color:#ffd166}.note{color:#aaa}.stateOn{color:#7f7;font-weight:bold}.stateOff{color:#aaa;font-weight:bold}
.moduleBlock{border:1px solid #444;border-radius:9px;margin:10px 0;background:#151515;overflow:hidden}
.moduleBlock>summary{cursor:pointer;list-style:none;background:#202a2a;border-bottom:1px solid #4d7772;padding:10px 12px}
.moduleBlock>summary::-webkit-details-marker{display:none}
.moduleBlock>summary:before{content:'';display:inline-block;width:0;height:0;border-top:5px solid transparent;border-bottom:5px solid transparent;border-left:7px solid #63e6be;margin-right:8px}
.moduleBlock[open]>summary:before{transform:rotate(90deg)}
.moduleTitle{font-size:16px;font-weight:bold;color:#63e6be}.moduleMeta{margin-left:10px;color:#bbb;font-size:13px}
.ioGroup{border-top:1px solid #333}.ioGroup>summary{cursor:pointer;list-style:none;background:#181d1d;color:#bbb;font-size:12px;font-weight:bold;letter-spacing:.5px;padding:7px 10px}
.ioGroup>summary::-webkit-details-marker{display:none}.ioGroup>summary:before{content:'';display:inline-block;width:0;height:0;border-top:4px solid transparent;border-bottom:4px solid transparent;border-left:6px solid #aaa;margin-right:7px}
.ioGroup[open]>summary:before{transform:rotate(90deg)}
.moduleTable{margin:0}.moduleChannel{color:#888;font-size:11px}
.nav{display:flex;gap:6px;flex-wrap:wrap;margin:8px 0 14px}.nav a{background:#202020;border:1px solid #444;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:bold}
.commGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;align-items:end}.commGrid label{display:block;color:#bbb;font-size:12px}.commGrid input,.commGrid select{width:100%;margin-top:4px}.commState{font-weight:bold}.commHelp{color:#999;font-size:12px;margin-top:8px}
</style></head><body>
<h2>I/O Engineering Setup</h2>
<p class=note>Channels are grouped by the physical M31 rack modules. Logical AI numbering remains continuous across the rack.</p>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<!-- EP32 IO SETUP M31 COMMS PANEL GATE4G V1 -->
<div class=card>
 <h3 style="margin-top:0">M31 Communications</h3>
 <div class=commGrid>
  <label>Communications
   <select id=mbEnabled><option value=1>ENABLED</option><option value=0>DISABLED</option></select>
  </label>
  <label>Transport
   <select id=mbTransport onchange="commModeChanged()"><option value=TCP>TCP/IP</option><option value=RTU>RS485 RTU</option></select>
  </label>
  <label id=tcpHostLabel>TCP/IP Address
   <input id=mbHost value="">
  </label>
  <label id=tcpPortLabel>TCP Port
   <input id=mbPort type=number min=1 max=65535 value=502>
  </label>
  <label>Modbus Unit / Address
   <input id=mbUnit type=number min=1 max=247>
  </label>
  <label id=rtuBaudLabel>RS485 Baud
   <select id=mbBaud><option>1200</option><option>2400</option><option>4800</option><option selected>9600</option><option>19200</option><option>38400</option><option>57600</option><option>115200</option></select>
  </label>
 </div>
 <div style="margin-top:10px"><button onclick="applyComms()">Apply Communications</button> <span id=commState class=commState></span></div>
 <div id=commDetail class=commHelp></div>
</div>
<div id=modules></div>
<p id=msg class=ok></p>
<script>
let cfg=[];
function aiRow(c,local,mq){
 return '<tr><td><b>AI'+local+'</b><br><span class=moduleChannel>Logical AI'+c.ch+'</span></td>'+
 '<td><input class=tag id=n'+c.ch+' value="'+c.name+'"></td>'+
 '<td><input class=num id=l'+c.ch+' type=number step=.001 value="'+c.low+'"></td>'+
 '<td><input class=num id=h'+c.ch+' type=number step=.001 value="'+c.high+'"></td>'+
 '<td><input class=unit id=u'+c.ch+' value="'+c.units+'"></td>'+
 '<td><select id=m'+c.ch+'><option value=0 '+(!c.manual?'selected':'')+'>LIVE</option><option value=1 '+(c.manual?'selected':'')+'>MANUAL</option></select></td>'+
 '<td><input class=manual id=v'+c.ch+' type=number step=.001 value="'+c.manualEU+'"></td>'+
 '<td id=live'+c.ch+'>'+Number(c.mA).toFixed(3)+' mA / <span class='+(c.manual?'warn':'ok')+'>'+Number(c.value).toFixed(3)+' '+c.units+'</span></td>'+
 '<td>'+(c.ch<=8
   ?('<input type=checkbox '+(mq?'checked':'')+' onchange="setIOMqtt(\'AI\','+c.ch+',this.checked)">')
   :'<input type=checkbox disabled title="AI9-AI16 MQTT persistence is not enabled yet">')+'</td>'+
 '<td><button onclick="save('+c.ch+')">Save</button></td></tr>';
}
// EP32 IO WEB LIVE DIGITAL REFRESH GATE4D V1
function digitalRows(prefix,count,states,mq){
 let h='';
 for(let i=0;i<count;i++){
  const ch=i+1;
  h+='<tr><td><b>'+prefix+ch+'</b></td><td>'+prefix+' channel '+ch+'</td><td colspan=5></td><td><span id='+prefix.toLowerCase()+'state'+ch+' class='+(states[i]?'stateOn':'stateOff')+'>'+(states[i]?'ON':'OFF')+'</span></td><td><input type=checkbox '+((mq||[])[i]?'checked':'')+' onchange="setIOMqtt(\''+prefix+'\','+ch+',this.checked)"></td><td></td></tr>';
 }
 return h;
}
function group(title,rows){
 return '<details class=ioGroup open><summary>'+title+'</summary><table class=moduleTable>'+
 '<tr><th>I/O</th><th>Process Tag / Type</th><th>4 mA</th><th>20 mA</th><th>Units</th><th>Mode</th><th>Manual EU</th><th>Live / Effective</th><th>MQTT</th><th></th></tr>'+rows+'</table></details>';
}
let aoSet=[50,50,50,50];
let aoMode=[0,0,0,0];

function rememberAO(){
 for(let i=1;i<=4;i++){
  const el=document.getElementById('aop'+i);
  const md=document.getElementById('aom'+i);
  if(el && Number.isFinite(Number(el.value)))aoSet[i-1]=Number(el.value);
  if(md)aoMode[i-1]=Number(md.value);
 }
}

function aoRows(readback,force,mq){
 let h='';
 for(let i=1;i<=4;i++){
  const rb=Number(readback[i-1]||0);
  const f=(force||[])[i-1]||{};
  const active=!!f.active;
  if(active){
   aoMode[i-1]=1;
   aoSet[i-1]=Number(f.percent||0);
  }
  const pct=aoSet[i-1];
  const mode=aoMode[i-1];
  h+='<tr>'+
    '<td><b>AO'+i+'</b></td>'+
    '<td>Analog Output</td>'+
    '<td>4.000 mA</td><td>20.000 mA</td><td>mA</td>'+
    '<td><select id=aom'+i+' onchange="aoModeChanged('+i+')">'+
      '<option value=0 '+(mode===0?'selected':'')+'>NORMAL</option>'+
      '<option value=1 '+(mode===1?'selected':'')+'>FORCE</option>'+
    '</select></td>'+
    '<td><input class=manual id=aop'+i+' type=number min=0 max=100 step=.1 value="'+pct.toFixed(1)+'" '+(mode===0?'disabled':'')+'> %</td>'+
    '<td><span class='+(active?'warn':'ok')+' id=aor'+i+'>'+rb.toFixed(3)+' mA readback | '+(active?'FORCED':'NORMAL')+'</span></td>'+
    '<td><input type=checkbox '+((mq||[])[i-1]?'checked':'')+' onchange="setIOMqtt(\'AO\','+i+',this.checked)"></td>'+
    '<td><button id=aob'+i+' onclick="applyAO('+i+')">Apply AO'+i+'</button></td>'+
    '</tr>';
 }
 return h;
}

function aoModeChanged(ch){
 const md=document.getElementById('aom'+ch);
 const val=document.getElementById('aop'+ch);
 aoMode[ch-1]=Number(md?.value||0);
 if(val)val.disabled=aoMode[ch-1]===0;
}

function commModeChanged(){
 const tcp=(document.getElementById('mbTransport')?.value||'TCP')==='TCP';
 document.getElementById('mbHost').disabled=!tcp;
 document.getElementById('mbPort').disabled=!tcp;
}
function renderComms(p){
 document.getElementById('mbEnabled').value=p.enabled?'1':'0';
 document.getElementById('mbTransport').value=p.transport||'RTU';
 document.getElementById('mbHost').value=p.tcpHost||'';
 document.getElementById('mbPort').value=p.tcpPort||502;
 document.getElementById('mbUnit').value=p.unitId??1;
 document.getElementById('mbBaud').value=p.rtuBaud??9600;
 const on=!!p.enabled;
 const tr=p.transport||'RTU';
 document.getElementById('commState').textContent=(on?'ENABLED':'DISABLED')+' | '+tr;
 document.getElementById('commState').className='commState '+(on?'ok':'warn');
 document.getElementById('commDetail').textContent=tr==='TCP'
   ?('TCP '+(p.tcpHost||'')+':'+(p.tcpPort||502)+' | Modbus Unit '+(p.unitId??1))
   :('RS485 '+(p.rtuBaud??9600)+' 8N1 | Unit '+(p.unitId??1)+' | TX GPIO'+(p.rtuTxPin??43)+' / RX GPIO'+(p.rtuRxPin??8));
 commModeChanged();
}
async function applyComms(){
 const enabled=document.getElementById('mbEnabled').value==='1';
 const tr=document.getElementById('mbTransport').value;
 const state=document.getElementById('commState');
 state.textContent='Applying...';state.className='commState warn';
 try{
  const p=new URLSearchParams({mode:tr.toLowerCase()});
  p.set('unit',document.getElementById('mbUnit').value);
  p.set('baud',document.getElementById('mbBaud').value);
  if(tr==='TCP'){
   p.set('host',document.getElementById('mbHost').value.trim());
   p.set('port',document.getElementById('mbPort').value);
  }
  const r1=await fetch('/api/m31-transport-runtime',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
  if(!r1.ok){let x={};try{x=await r1.json()}catch(_){ }throw new Error(x.error||'transport update failed');}
  const r2=await fetch('/api/modbus-comms',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({enabled:enabled?'1':'0'})});
  if(!r2.ok){let x={};try{x=await r2.json()}catch(_){ }throw new Error(x.error||'enable update failed');}
  const fresh=await fetch('/api/m31-active-profile?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  renderComms(fresh);
 }catch(err){
  state.textContent='FAILED: '+String(err.message||err);state.className='commState warn';
 }
}

async function load(){
 rememberAO();
 // EP32 IO WEB REMOVE DIAG POLL GATE4F V1
 const [x,s,p,mq]=await Promise.all([
   fetch('/api/io-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/status?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/api/m31-active-profile?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/api/io-mqtt-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json())
 ]);
 let d={ao_mA:s.ao_mA||[],aoForce:[]};
 cfg=x.channels||[];
 const host=cfg.filter(c=>c.ch>=1 && c.ch<=4);
 const slot1=cfg.filter(c=>c.ch>=5 && c.ch<=8);
 const slot2=cfg.filter(c=>c.ch>=9 && c.ch<=16);
 renderComms(p);
 const hasSlot1=(p.slot1&&p.slot1!=='NONE');
 const hasSlot2=(p.slot2&&p.slot2!=='NONE');
 let h='';
 h+='<details class=moduleBlock open><summary><span class=moduleTitle>'+p.host+'</span><span class=moduleMeta>Host | DI1-4 DO1-4 AI1-4 | ACTIVE</span></summary>';
 h+=group('ANALOG INPUTS',host.map((c,i)=>aiRow(c,i+1,(mq.ai||[])[c.ch-1])).join(''));
 h+=group('DIGITAL INPUTS',digitalRows('DI',4,s.di||[],mq.di||[]));
 h+=group('DIGITAL OUTPUTS',digitalRows('DO',4,s.do||[],mq.do||[]));
 h+='</details>';
 if(hasSlot1){
  h+='<details class=moduleBlock open><summary><span class=moduleTitle>'+p.slot1+'</span><span class=moduleMeta>Slot 1 | AI5-8 AO1-4 | ACTIVE</span></summary>';
  h+=group('ANALOG INPUTS',slot1.map((c,i)=>aiRow(c,i+1,(mq.ai||[])[c.ch-1])).join(''));
  h+=group('ANALOG OUTPUTS',aoRows(d.ao_mA||[],d.aoForce||[],mq.ao||[]));
  h+='</details>';
 }
 if(hasSlot2){
  h+='<details class=moduleBlock open><summary><span class=moduleTitle>'+p.slot2+'</span><span class=moduleMeta>Slot 2 | AI9-16 | ACTIVE</span></summary>';
  h+=group('ANALOG INPUTS',slot2.map((c,i)=>aiRow(c,i+1,false)).join(''));
  h+='<div class=note style="padding:8px 10px">AI9-AI16 configuration changes are runtime-only in Gate A2b; persistence and MQTT selection remain unchanged.</div>';
  h+='</details>';
 }
 if(!hasSlot1 && !hasSlot2){
  h+='<div class=card><span class=moduleTitle>NO EXPANSION MODULE ACTIVE</span><span class=moduleMeta>Host-only rack: AI1-4, DI1-4, DO1-4.</span></div>';
 }
 modules.innerHTML=h;
}
async function setIOMqtt(type,ch,enabled){
 try{
  const q=new URLSearchParams({type:type,ch:String(ch),enabled:enabled?'1':'0'});
  const r=await fetch('/api/io-mqtt-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q});
  const x=await r.json();
  if(!r.ok)throw new Error(x.error||'MQTT selection save failed');
  msg.textContent=type+ch+' MQTT '+(enabled?'ENABLED':'DISABLED')+' (selection saved)';
  msg.className='ok';
 }catch(err){
  msg.textContent=String(err.message||err);msg.className='warn';
  setTimeout(load,400);
 }
}

async function applyAO(ch){
 const el=document.getElementById('aop'+ch);
 const md=document.getElementById('aom'+ch);
 const btn=document.getElementById('aob'+ch);
 const mode=Number(md?.value||0);
 const percent=Math.max(0,Math.min(100,Number(el?.value||0)));
 aoMode[ch-1]=mode;
 aoSet[ch-1]=percent;

 if(btn){
  btn.disabled=true;
  btn.textContent=mode===1?'Forcing...':'Releasing...';
 }

 let p=new URLSearchParams({
  ch:String(ch),
  mode:mode===1?'FORCE':'NORMAL',
  percent:String(percent)
 });
 let r=await fetch('/api/io-ao-force',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json();}catch(e){}

 if(r.ok){
  if(mode===1){
   const ma=Number(x.mA||0);
   msg.textContent='AO'+ch+' FORCED: '+percent.toFixed(1)+'% = '+ma.toFixed(3)+' mA';
   if(btn)btn.textContent='✓ AO'+ch+' FORCED';
  }else{
   msg.textContent='AO'+ch+' RELEASED TO NORMAL';
   if(btn)btn.textContent='✓ AO'+ch+' NORMAL';
  }
  msg.className='ok';
  setTimeout(()=>{
   if(btn){btn.textContent='Apply AO'+ch;btn.disabled=false;}
   load();
  },900);
 }else{
  msg.textContent=x.error||('AO'+ch+' ACTION FAILED');
  msg.className='warn';
  if(btn){
   btn.textContent='✕ ACTION FAILED';
   setTimeout(()=>{btn.textContent='Apply AO'+ch;btn.disabled=false;},1800);
  }
 }
}

async function save(ch){
 let p=new URLSearchParams({ch:ch,name:document.getElementById('n'+ch).value,low:document.getElementById('l'+ch).value,high:document.getElementById('h'+ch).value,units:document.getElementById('u'+ch).value,manual:document.getElementById('m'+ch).value,manualEU:document.getElementById('v'+ch).value});
 let r=await fetch('/api/io-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'AI'+ch+' saved':'SAVE FAILED';if(r.ok)setTimeout(refreshLive,180);
}

// EP32 IO WEB FAST STATUS REFRESH GATE4E V1
async function refreshLive(){
 try{
  // Fast path: DI/DO/AI all come from /status. Do not hold them behind the
  // heavier Modbus diagnostics request.
  const st=await fetch('/status?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());

  for(let ch=1;ch<=cfg.length;ch++){
   const el=document.getElementById('live'+ch);
   if(!el)continue;
   const c=cfg[ch-1];
   const ma=Number((st.ai_mA||[])[ch-1]||0);
   const eu=Number((st.aiEU||[])[ch-1]||0);
   const manual=document.getElementById('m'+ch)?.value==='1';
   el.innerHTML=ma.toFixed(3)+' mA / <span class='+(manual?'warn':'ok')+'>'+eu.toFixed(3)+' '+(c?.units||'')+'</span>';
  }

  for(let ch=1;ch<=4;ch++){
   const diEl=document.getElementById('distate'+ch);
   if(diEl){
    const on=!!(st.di||[])[ch-1];
    diEl.textContent=on?'ON':'OFF';
    diEl.className=on?'stateOn':'stateOff';
   }
   const doEl=document.getElementById('dostate'+ch);
   if(doEl){
    const on=!!(st.do||[])[ch-1];
    doEl.textContent=on?'ON':'OFF';
    doEl.className=on?'stateOn':'stateOff';
   }
  }

  for(let ch=1;ch<=4;ch++){
   const el=document.getElementById('aor'+ch);
   if(el){
    const rb=Number((st.ao_mA||[])[ch-1]||0);
    el.textContent=rb.toFixed(3)+' mA readback';
   }
  }
 }catch(e){}
}

// Gate4F: no automatic /api/modbus-diag polling from /io-setup.
// Diagnostics remain available on demand from /modbus-monitor and the API.

load();
setInterval(refreshLive,1000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/aga-config-storage",HTTP_GET,[](){
    String j="{\"loaded\":"+String(agaFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(agaFlashLastSaveOK?"true":"false");
    j+=",\"status\":\""+agaFlashStatus+"\"";
    j+=",\"path\":\""+String(AGA_CFG_PATH)+"\"";
    j+=",\"bytes\":"+String((unsigned)sizeof(AGAFlashConfigV1));
    j+=",\"run1Pipe\":"+String(agaRunCfg[0].pipeID_mm,3);
    j+=",\"run1Orifice\":"+String(agaRunCfg[0].orifice_mm,3);
    j+=",\"run1GasTotal\":";
    float total=0.0f;for(uint8_t i=0;i<AGA_COMPONENTS;i++)total+=agaRunMolPct[0][i];
    j+=String(total,4);
    j+=",\"libraryStandard\":";
    j+=(gasLibraryStandard<GAS_LIBRARY_SLOTS)?String(gasLibraryStandard):String(-1);
    j+=",\"libraryUsed\":[";
    for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){if(slot)j+=",";j+=String(gasLibraryUsed[slot]?"true":"false");}
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/aga-config-retire-legacy-nvs",HTTP_POST,[](){
    if(!agaFlashLoaded){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"valid flash AGA config must be loaded first\"}");
      return;
    }
    bool ok=true;
    const char* ns[]={"aga","agaruns","agagas","gaslib"};
    for(uint8_t i=0;i<4;i++){
      if(!prefs.begin(ns[i],false))ok=false;
      else{ok=prefs.clear()&&ok;prefs.end();}
    }
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:500,"application/json",ok?
      "{\"ok\":true,\"cleared\":[\"aga\",\"agaruns\",\"agagas\",\"gaslib\"],\"flashAuthoritative\":true}":
      "{\"ok\":false,\"error\":\"legacy AGA NVS clear failed\"}");
  });

  server.on("/api/aga-config",HTTP_GET,[](){
    String j="{\"runs\":[";
    for(uint8_t run=0;run<6;run++){
      if(run)j+=",";
      AGASetupConfig &c=agaRunCfg[run];
      j+="{\"run\":"+String(run+1);
      j+=",\"name\":\""+String(flowRunName[run])+"\"";
      j+=",\"lsd\":\""+String(flowRunLSD[run])+"\"";
      j+=",\"enabled\":"+String(flowRunEnabled[run]?"true":"false");
      j+=",\"pipe\":"+String(c.pipeID_mm,6);
      j+=",\"orifice\":"+String(c.orifice_mm,6);
      j+=",\"baseP\":"+String(c.basePressure_kPaAbs,6);
      j+=",\"baseT\":"+String(c.baseTemperature_C,6);
      j+=",\"atm\":"+String(c.atmosphericPressure_kPa,6);
      j+=",\"absolute\":"+String(c.staticPressureIsAbsolute?"true":"false");
      j+=",\"tap\":"+String(c.tapType)+"}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/aga-save",HTTP_POST,[](){
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    if(n<1||n>6){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    uint8_t run=(uint8_t)(n-1);
    AGASetupConfig &c=agaRunCfg[run];
    if(server.hasArg("pipe"))c.pipeID_mm=server.arg("pipe").toFloat();
    if(server.hasArg("orifice"))c.orifice_mm=server.arg("orifice").toFloat();
    if(server.hasArg("baseP"))c.basePressure_kPaAbs=server.arg("baseP").toFloat();
    if(server.hasArg("baseT"))c.baseTemperature_C=server.arg("baseT").toFloat();
    if(server.hasArg("atm"))c.atmosphericPressure_kPa=server.arg("atm").toFloat();
    if(server.hasArg("absolute"))c.staticPressureIsAbsolute=server.arg("absolute")=="1";
    if(server.hasArg("tap"))c.tapType=(uint8_t)constrain(server.arg("tap").toInt(),0,2);
    saveAGARunConfig(run);
    displayDirty=true;
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/aga-gas",HTTP_GET,[](){
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    if(n<1||n>6){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    uint8_t run=(uint8_t)(n-1);
    String j="{\"run\":"+String(n)+",\"gas\":[";
    for(uint8_t i=0;i<AGA_COMPONENTS;i++){
      if(i)j+=",";
      j+=String(agaRunMolPct[run][i],6);
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/aga-gas-save",HTTP_POST,[](){
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    if(n<1||n>6){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    uint8_t run=(uint8_t)(n-1);
    float proposed[AGA_COMPONENTS];
    float total=0.0f;
    for(uint8_t i=0;i<AGA_COMPONENTS;i++){
      String k="c"+String(i);
      proposed[i]=server.hasArg(k)?server.arg(k).toFloat():agaRunMolPct[run][i];
      total+=proposed[i];
    }
    if(fabsf(total-100.0f)>0.01f){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"gas total must equal 100 percent\"}");
      return;
    }
    for(uint8_t i=0;i<AGA_COMPONENTS;i++)agaRunMolPct[run][i]=proposed[i];
    saveAGARunGas(run);
    displayDirty=true;
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/aga-gas-copy",HTTP_POST,[](){
    int from=server.hasArg("from")?server.arg("from").toInt():0;
    int to=server.hasArg("to")?server.arg("to").toInt():0;
    if(from<1||from>6||to<1||to>6||from==to){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run selection\"}");
      return;
    }
    bool ok=copyGasRun((uint8_t)(from-1),(uint8_t)(to-1));
    server.send(ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/api/gas-library",HTTP_GET,[](){
    String j="{\"standard\":";
    if(gasLibraryStandard<GAS_LIBRARY_SLOTS)j+=String(gasLibraryStandard);
    else j+="-1";
    j+=",\"slots\":[";
    for(uint8_t slot=0;slot<GAS_LIBRARY_SLOTS;slot++){
      if(slot)j+=",";
      j+="{\"slot\":"+String(slot)+",\"used\":"+String(gasLibraryUsed[slot]?"true":"false");
      j+=",\"name\":\""+String(gasLibraryName[slot])+"\"}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/gas-library-save",HTTP_POST,[](){
    String name=server.hasArg("name")?server.arg("name"):"";
    name.trim();
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    if(n<1||n>6){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    if(!name.length()){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"analysis name required\"}");
      return;
    }

    // Save As is intentionally non-destructive: always use a free record.
    int slot=-1;
    for(uint8_t i=0;i<GAS_LIBRARY_SLOTS;i++){
      if(!gasLibraryUsed[i]){slot=i;break;}
    }
    if(slot<0){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"named analysis library full\"}");
      return;
    }

    bool ok=saveGasLibrarySlot((uint8_t)slot,name,(uint8_t)(n-1));
    server.send(ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");
  });

  server.on("/api/gas-library-load",HTTP_POST,[](){
    int slot=server.hasArg("slot")?server.arg("slot").toInt():-1;
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    bool ok=slot>=0 && slot<GAS_LIBRARY_SLOTS && n>=1 && n<=6 &&
      loadGasLibrarySlot((uint8_t)slot,(uint8_t)(n-1));
    server.send(ok?200:400,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"empty or bad slot\"}");
  });

  server.on("/api/gas-library-standard",HTTP_POST,[](){
    int slot=server.hasArg("slot")?server.arg("slot").toInt():-1;
    if(slot<0 || slot>=GAS_LIBRARY_SLOTS || !gasLibraryUsed[slot]){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"empty or bad slot\"}");
      return;
    }
    setGasLibraryStandard((uint8_t)slot);
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/gas-standard-load",HTTP_POST,[](){
    int n=server.hasArg("run")?server.arg("run").toInt():1;
    bool ok=gasLibraryStandard<GAS_LIBRARY_SLOTS && n>=1 && n<=6 &&
      loadGasLibrarySlot(gasLibraryStandard,(uint8_t)(n-1));
    server.send(ok?200:400,"application/json",ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"no standard selected\"}");
  });

  server.on("/aga-setup",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow AGA Setup</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1180px}a{color:#63e6be}
.card{border:1px solid #444;border-radius:9px;padding:14px;margin:10px 0;background:#171717}
.runHead{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:10px}
.runName{font-size:18px;font-weight:700;color:#63e6be}.muted{font-size:12px;color:#999}
.grid{display:grid;grid-template-columns:1.05fr 1.05fr 1.15fr 1.15fr 1.15fr 1fr 1fr;gap:8px;align-items:end}
.field{background:#101415;border:1px solid #344;border-radius:7px;padding:7px}
input,select,button{padding:7px;background:#222;color:#fff;border:1px solid #666;width:100%}
button{width:auto;cursor:pointer}label{display:block;color:#aaa;font-size:12px;margin-bottom:4px}.ok{color:#8f8}
.gasGrid{display:grid;grid-template-columns:repeat(4,minmax(150px,1fr));gap:8px}
.gasSummary{display:flex;justify-content:flex-end;align-items:center;gap:10px;margin:10px 0 0}
.gasTotal{font-size:20px;font-weight:700;padding:6px 10px;border-radius:6px;border:1px solid #555}
.gasGood{color:#8f8}.gasBad{color:#ff8b8b;border-color:#833}
@media(max-width:1050px){.grid{grid-template-columns:repeat(4,minmax(150px,1fr))}}
@media(max-width:850px){.grid,.gasGrid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:520px){.grid,.gasGrid{grid-template-columns:1fr}}
</style></head><body>
<h2>AGA Setup</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div id=runs></div>
<div class=card>
 <div class=runHead>
  <div><div class=runName>Flow Run Gas Composition</div><div class=muted>Each flow run has its own active gas analysis</div></div>
  <button id=saveGasBtn onclick=saveGas()>Save Gas for Flow 1</button>
 </div>
 <div class=grid style="margin-bottom:10px">
  <div class=field><label>Flow Run</label><select id=gasRun onchange=loadGasRun()></select></div>
  <div class=field><label>Copy This Gas To</label><select id=gasCopyTo></select><button style="margin-top:5px" onclick=copyGas()>Copy Gas</button></div>
  <div class=field><label>Standard Analysis</label><button onclick=loadStd()>Load Standard into This Flow</button></div>
 </div>
 <div id=gas class=gasGrid></div>
 <div class=gasSummary><span class=muted>Mole % Total</span><span id=gasTotal class="gasTotal gasBad">0.0000%</span></div>
</div>

<details class=card>
 <summary class=runName style="cursor:pointer">Named Gas Analyses</summary>
 <div class=muted style="margin:8px 0 10px">Optional reusable lab analyses. Saving a new named analysis never overwrites an existing one.</div>
 <div class=grid>
  <div class=field><label>Saved Analysis</label><select id=gasSlot></select></div>
  <div class=field><label>New Analysis Name</label><input id=gasName maxlength=24 placeholder="e.g. Lab Aug 2026"></div>
  <div class=field><label>Save Current Flow Gas</label><button onclick=saveLib()>Save as New Named Analysis</button></div>
  <div class=field><label>Load Into Current Flow</label><button onclick=loadLib()>Load Selected Analysis</button></div>
  <div class=field><label>Station Standard</label><button onclick=setStd()>Set Selected as Standard</button></div>
  <div class=field><label>Status</label><div id=gasLibStatus class=muted>--</div></div>
 </div>
</details>
<p><button onclick=gold()>Load Golden Reference into Run 1</button> <span id=msg class=ok></span></p>
<script>
const names=['Methane','Nitrogen','Carbon Dioxide','Ethane','Propane','Isobutane','n-Butane','Isopentane','n-Pentane','n-Hexane','n-Heptane','n-Octane','n-Nonane','n-Decane','Hydrogen','Oxygen','Carbon Monoxide','Water','Hydrogen Sulfide','Helium','Argon'];
let cfg;
function gasSum(){
 let total=0;
 for(let i=0;i<21;i++){
  const el=document.getElementById('c'+i);
  if(el){
   const v=Number(el.value);
   if(Number.isFinite(v))total+=v;
  }
 }
 return total;
}
function updateGasTotal(){
 const total=gasSum();
 const el=document.getElementById('gasTotal');
 if(!el)return;
 el.textContent=total.toFixed(4)+'%';
 const good=Math.abs(total-100.0)<=0.01;
 el.className='gasTotal '+(good?'gasGood':'gasBad');
}
function gasValid(){
 return Math.abs(gasSum()-100.0)<=0.01;
}
function field(label,id,value,step='.001'){return '<div class=field><label>'+label+'</label><input id='+id+' type=number step="'+step+'" value="'+value+'"></div>'}
function runCard(r){
 const n=r.name||('Flow Run '+r.run), l=r.lsd?(' | '+r.lsd):'';
 return '<div class=card><div class=runHead><div><div class=runName>Run '+r.run+' - '+n+'</div><div class=muted>'+l.replace(' | ','')+(r.enabled?'':' | DISABLED')+'</div></div><button onclick=saveRun('+r.run+')>Save Run '+r.run+'</button></div>'+
 '<div class=grid>'+
 field('Pipe ID mm','pipe'+r.run,r.pipe)+
 field('Orifice mm','orifice'+r.run,r.orifice)+
 field('Base Pressure kPa(a)','baseP'+r.run,r.baseP)+
 field('Base Temperature C','baseT'+r.run,r.baseT)+
 field('Atmospheric Pressure kPa','atm'+r.run,r.atm)+
 '<div class=field><label>Static Pressure</label><select id=absolute'+r.run+'><option value=0 '+(!r.absolute?'selected':'')+'>Gauge</option><option value=1 '+(r.absolute?'selected':'')+'>Absolute</option></select></div>'+
 '<div class=field><label>Tap Type</label><select id=tap'+r.run+'><option value=0 '+(r.tap===0?'selected':'')+'>Flange</option><option value=1 '+(r.tap===1?'selected':'')+'>Corner</option><option value=2 '+(r.tap===2?'selected':'')+'>D-D/2</option></select></div>'+
 '</div></div>';
}
async function load(){
 cfg=await fetch('/api/aga-config?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 runs.innerHTML=cfg.runs.map(runCard).join('');
 gasRun.innerHTML=cfg.runs.map(r=>'<option value="'+r.run+'">Run '+r.run+' - '+(r.name||('Flow Run '+r.run))+'</option>').join('');
 gasCopyTo.innerHTML=cfg.runs.slice(1).map(r=>'<option value="'+r.run+'">Run '+r.run+' - '+(r.name||('Flow Run '+r.run))+'</option>').join('');
 await loadGasRun();
 await loadLibrary();
}
async function loadGasRun(){
 const run=Number(gasRun.value||1);
 saveGasBtn.textContent='Save Gas for Flow '+run;
 const x=await fetch('/api/aga-gas?run='+run+'&x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 gas.innerHTML=names.map((n,i)=>'<div class=field><label>'+n+'</label><input id=c'+i+' type=number step=.0001 value="'+x.gas[i]+'"></div>').join('');
 for(let i=0;i<21;i++)document.getElementById('c'+i).addEventListener('input',updateGasTotal);
 updateGasTotal();
 gasCopyTo.innerHTML=cfg.runs.filter(r=>r.run!==run).map(r=>'<option value="'+r.run+'">Run '+r.run+' - '+(r.name||('Flow Run '+r.run))+'</option>').join('');
}
async function loadLibrary(){
 let x=await fetch('/api/gas-library?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 const used=x.slots.filter(s=>s.used);
 gasSlot.innerHTML=used.length
  ? used.map(s=>'<option value="'+s.slot+'">'+s.name+(x.standard===s.slot?' [STANDARD]':'')+'</option>').join('')
  : '<option value="">No named analyses saved</option>';
 gasLibStatus.textContent=x.standard>=0?'Standard: '+x.slots[x.standard].name:'No standard selected';
}
async function saveRun(run){
 let p=new URLSearchParams({
  run:run,
  pipe:document.getElementById('pipe'+run).value,
  orifice:document.getElementById('orifice'+run).value,
  baseP:document.getElementById('baseP'+run).value,
  baseT:document.getElementById('baseT'+run).value,
  atm:document.getElementById('atm'+run).value,
  absolute:document.getElementById('absolute'+run).value,
  tap:document.getElementById('tap'+run).value
 });
 let r=await fetch('/api/aga-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Run '+run+' AGA saved':'SAVE FAILED';
}
async function saveGas(){
 if(!gasValid()){
  msg.textContent='Gas analysis must total 100.0000% before save';
  updateGasTotal();
  return false;
 }
 let p=new URLSearchParams({run:gasRun.value});
 for(let i=0;i<21;i++)p.set('c'+i,document.getElementById('c'+i).value);
 let r=await fetch('/api/aga-gas-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Run '+gasRun.value+' gas composition saved':'SAVE FAILED';
 return r.ok;
}
async function copyGas(){
 if(!(await saveGas()))return;
 let p=new URLSearchParams({from:gasRun.value,to:gasCopyTo.value});
 let r=await fetch('/api/aga-gas-copy',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Gas copied to Run '+gasCopyTo.value:'COPY FAILED';
}
async function saveLib(){
 if(!(await saveGas()))return;
 const name=gasName.value.trim();
 if(!name){msg.textContent='Enter a name for the saved analysis';return;}
 let p=new URLSearchParams({name:name,run:gasRun.value});
 let r=await fetch('/api/gas-library-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 if(r.ok){
  msg.textContent='Saved "'+name+'" as a new named analysis';
  gasName.value='';
  await loadLibrary();
 }else{
  let e={};try{e=await r.json()}catch(_){}
  msg.textContent=e.error||'SAVE NAMED ANALYSIS FAILED';
 }
}
async function loadLib(){
 if(gasSlot.value===''){msg.textContent='No named analysis selected';return;}
 let p=new URLSearchParams({slot:gasSlot.value,run:gasRun.value});
 let r=await fetch('/api/gas-library-load',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Stored analysis loaded into Run '+gasRun.value:'LOAD FAILED';
 if(r.ok)await loadGasRun();
}
async function setStd(){
 if(gasSlot.value===''){msg.textContent='No named analysis selected';return;}
 let p=new URLSearchParams({slot:gasSlot.value});
 let r=await fetch('/api/gas-library-standard',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Standard gas analysis selected':'STANDARD FAILED';
 if(r.ok)await loadLibrary();
}
async function loadStd(){
 let p=new URLSearchParams({run:gasRun.value});
 let r=await fetch('/api/gas-standard-load',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 msg.textContent=r.ok?'Standard loaded into Run '+gasRun.value:'NO STANDARD SAVED';
 if(r.ok)await loadGasRun();
}
async function gold(){
 let r=await fetch('/api/golden-reference',{method:'POST'});
 msg.textContent=r.ok?'Golden reference loaded into Run 1':'FAILED';
 if(r.ok){gasRun.value='1';setTimeout(load,250);}
}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/pid-config-storage",HTTP_GET,[](){
    String j="{\"loaded\":"+String(pidFlashLoaded?"true":"false");
    j+=",\"lastSaveOK\":"+String(pidFlashLastSaveOK?"true":"false");
    j+=",\"bootLoadOK\":"+String(pidFlashBootLoadOK?"true":"false");
    j+=",\"saveCount\":"+String(pidFlashSaveCount);
    j+=",\"status\":\""+pidFlashStatus+"\"";
    j+=",\"path\":\""+String(CONTROL_FILE_PATH)+"\"";
    j+=",\"version\":2";
    j+=",\"bytes\":"+String((unsigned)sizeof(ControlFileV2));
    j+=",\"modes\":[";
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){if(r)j+=",";j+=String(flowPIDCfg[r].mode);}
    j+="],\"ao\":[";
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){if(r)j+=",";j+=String(flowPIDCfg[r].aoChannel+1);}
    j+="],\"sp\":[";
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){if(r)j+=",";j+=String(flowPIDCfg[r].flowSP,3);}
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/pid-control",HTTP_GET,[](){
    String j="{\"runs\":[";
    for(uint8_t r=0;r<FLOW_RUN_COUNT;r++){
      if(r)j+=",";
      FlowPIDConfig &c=flowPIDCfg[r];
      FlowPIDRuntime &rt=flowPIDRt[r];
      j+="{\"run\":"+String(r+1);
      j+=",\"name\":\""+flowRunDisplayName(r)+"\"";
      j+=",\"enabled\":"+String(flowRunEnabled[r]?"true":"false");
      j+=",\"mode\":"+String(c.mode);
      j+=",\"modeName\":\""+String(flowControlModeName(c.mode))+"\"";
      j+=",\"ao\":"+String(c.aoChannel+1);
      j+=",\"sp\":"+String(c.flowSP,4);
      j+=",\"p\":"+String(c.p,6);
      j+=",\"i\":"+String(c.i,6);
      j+=",\"d\":"+String(c.d,6);
      j+=",\"manualOut\":"+String(c.manualOut,2);
      j+=",\"outMin\":"+String(c.outMin,2);
      j+=",\"outMax\":"+String(c.outMax,2);
      j+=",\"pv\":"+String(flowRunFlow[r],4);
      j+=",\"pvGood\":"+String(flowRunValid[r]?"true":"false");
      j+=",\"output\":"+String(rt.output,2);
      j+=",\"flowOutput\":"+String(rt.flowOutput,2);
      j+=",\"inputGood\":"+String(rt.inputGood?"true":"false");
      FlowResolvedPoint pp=resolveFlowRunInput(r,FLOW_ROLE_STATIC);
      j+=",\"pressureEnabled\":"+String(c.pressureOverrideEnabled?"true":"false");
      j+=",\"pressureSP\":"+String(c.pressureSP,3);
      j+=",\"pressureP\":"+String(c.pressureP,6);
      j+=",\"pressureI\":"+String(c.pressureI,6);
      j+=",\"pressureD\":"+String(c.pressureD,6);
      j+=",\"pressurePV\":"+String(isfinite(pp.value)?pp.value:0.0f,3);
      j+=",\"pressureGood\":"+String(rt.pressureGood?"true":"false");
      j+=",\"pressureOutput\":"+String(rt.pressureOutput,2);
      j+=",\"overrideActive\":"+String(rt.pressureOverrideActive?"true":"false");
      j+="}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/pid-control",HTTP_POST,[](){
    if(!server.hasArg("run")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"run required\"}");
      return;
    }
    int n=server.arg("run").toInt();
    if(n<1||n>FLOW_RUN_COUNT){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"bad run\"}");
      return;
    }
    uint8_t r=(uint8_t)(n-1);
    FlowPIDConfig proposed=flowPIDCfg[r];

    if(server.hasArg("mode"))proposed.mode=(uint8_t)constrain(server.arg("mode").toInt(),0,2);
    if(server.hasArg("ao"))proposed.aoChannel=(uint8_t)constrain(server.arg("ao").toInt()-1,0,3);
    if(server.hasArg("sp"))proposed.flowSP=server.arg("sp").toFloat();
    if(server.hasArg("p"))proposed.p=server.arg("p").toFloat();
    if(server.hasArg("i"))proposed.i=server.arg("i").toFloat();
    if(server.hasArg("d"))proposed.d=server.arg("d").toFloat();
    if(server.hasArg("manualOut"))proposed.manualOut=server.arg("manualOut").toFloat();
    if(server.hasArg("outMin"))proposed.outMin=constrain(server.arg("outMin").toFloat(),0.0f,100.0f);
    if(server.hasArg("outMax"))proposed.outMax=constrain(server.arg("outMax").toFloat(),0.0f,100.0f);
    if(server.hasArg("pressureEnabled"))proposed.pressureOverrideEnabled=server.arg("pressureEnabled")=="1";
    if(server.hasArg("pressureSP"))proposed.pressureSP=server.arg("pressureSP").toFloat();
    if(server.hasArg("pressureP"))proposed.pressureP=server.arg("pressureP").toFloat();
    if(server.hasArg("pressureI"))proposed.pressureI=server.arg("pressureI").toFloat();
    if(server.hasArg("pressureD"))proposed.pressureD=server.arg("pressureD").toFloat();

    if(proposed.outMax<proposed.outMin){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"output max must be >= min\"}");
      return;
    }
    proposed.manualOut=constrain(proposed.manualOut,proposed.outMin,proposed.outMax);

    if(proposed.mode!=FLOW_CTRL_DISABLED && flowPIDChannelConflict(r,proposed.aoChannel)){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"AO already assigned to another active PID run\"}");
      return;
    }

    bool modeChanged=proposed.mode!=flowPIDCfg[r].mode ||
      proposed.aoChannel!=flowPIDCfg[r].aoChannel ||
      proposed.pressureOverrideEnabled!=flowPIDCfg[r].pressureOverrideEnabled;
    flowPIDCfg[r]=proposed;
    if(modeChanged)flowPIDRt[r]=FlowPIDRuntime();

    if(!saveFlowPIDConfig()){
      server.send(500,"application/json","{\"ok\":false,\"error\":\"control config save failed\"}");
      return;
    }
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/pid-control",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow PID Setup</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#0d1113;color:#eee;margin:0}
.wrap{max-width:1220px;margin:auto;padding:14px}a{color:#63e6be}
.loop{border:1px solid #46545a;border-radius:10px;background:#171d20;margin:10px 0;overflow:hidden}
.head{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;padding:11px 13px;background:#11181b;border-bottom:1px solid #354247}
.name{font-size:19px;font-weight:700;color:#63e6be}.muted{font-size:12px;color:#999}.good{color:#8f8}.warn{color:#ffd166}.bad{color:#ff8b8b}
.line{display:flex;gap:8px;align-items:end;flex-wrap:wrap;padding:10px 12px;border-bottom:1px solid #2d363a}.line:last-child{border-bottom:0}
.tag{min-width:90px;color:#75d7ff;font-weight:700;padding-bottom:8px}.field{min-width:95px;flex:1}.wide{flex:1.25}.small{flex:.7}
label{display:block;color:#aaa;font-size:11px;margin-bottom:4px}
input,select,button{width:100%;padding:8px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}
button{cursor:pointer;width:auto}.save{background:#203a35;border-color:#4d8177;font-weight:700}
.live{min-width:125px;background:#101619;border:1px solid #344;border-radius:6px;padding:7px 9px}.live b{font-size:15px}
.overrideLine{background:#18150d}.msg{margin:10px 0;min-height:20px}
@media(max-width:760px){.line{align-items:stretch}.field,.wide,.small{min-width:46%}.tag{width:100%;padding-bottom:0}}
@media(max-width:480px){.field,.wide,.small{min-width:100%}}
</style></head><body><div class=wrap>
<h2 style="margin-bottom:4px">Riot-Flow PID Setup</h2>
<div class=muted>Six-loop commissioning. Flow PID is primary control; high static-pressure override shares the same AO using the existing low-selector logic.</div>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div id=loops>Loading...</div><div id=msg class=msg></div>
</div>
<script>
const e=id=>document.getElementById(id);
function aoOpts(sel){let h='';for(let i=1;i<=4;i++)h+='<option value="'+i+'" '+(i==sel?'selected':'')+'>AO'+i+'</option>';return h}
function fld(label,id,val,cls='field',step='.001'){return '<div class="'+cls+'"><label>'+label+'</label><input id="'+id+'" type=number step="'+step+'" value="'+val+'"></div>'}
function sel(label,id,html,cls='field'){return '<div class="'+cls+'"><label>'+label+'</label><select id="'+id+'">'+html+'</select></div>'}
function loop(r){
 const runState=r.enabled?'RUN ENABLED':'RUN DISABLED';
 return '<div class=loop>'+
  '<div class=head><div><span class=name>'+r.name+'</span> <span class="'+(r.enabled?'good':'bad')+'">'+runState+'</span></div>'+
  '<button class=save onclick="save('+r.run+')">Save PID '+r.run+'</button></div>'+
  '<div class=line><div class=tag>PID Name</div>'+
   '<div class=wide><label>Flow / PID</label><input value="'+r.name+'" disabled></div>'+
   sel('Mode','m'+r.run,'<option value=0 '+(r.mode==0?'selected':'')+'>DISABLED</option><option value=1 '+(r.mode==1?'selected':'')+'>MANUAL</option><option value=2 '+(r.mode==2?'selected':'')+'>AUTO</option>','small')+
   sel('Control Output','a'+r.run,aoOpts(r.ao),'small')+
   fld('Flow SP E3 m3/day','s'+r.run,r.sp,'field')+
   '<div class=live>PV<br><b id=pv'+r.run+'>'+(r.pvGood?Number(r.pv).toFixed(3):'---')+'</b></div>'+
   '<div class=live>Final OUT<br><b id=out'+r.run+'>'+Number(r.output).toFixed(1)+'%</b></div></div>'+
  '<div class=line><div class=tag>PID Params</div>'+
   fld('P','p'+r.run,r.p,'small')+fld('I','i'+r.run,r.i,'small')+fld('D','d'+r.run,r.d,'small')+
   fld('OUT Min %','n'+r.run,r.outMin,'small','.1')+fld('OUT Max %','x'+r.run,r.outMax,'small','.1')+
   fld('Manual OUT %','o'+r.run,r.manualOut,'small','.1')+
   '<div class=live>Flow OUT<br><b id=fout'+r.run+'>'+Number(r.flowOutput).toFixed(1)+'%</b></div>'+
   '<div class=live>State<br><b id=st'+r.run+'>'+(r.overrideActive?'PRESSURE OVERRIDE':r.modeName)+'</b></div></div>'+
  '<div class="line overrideLine"><div class=tag>PID Override</div>'+
   sel('Pressure Override','pe'+r.run,'<option value=0 '+(!r.pressureEnabled?'selected':'')+'>DISABLED</option><option value=1 '+(r.pressureEnabled?'selected':'')+'>ENABLED</option>','field')+
   fld('Pressure SP','ps'+r.run,r.pressureSP,'field','.1')+
   fld('P','pp'+r.run,r.pressureP,'small')+fld('I','pi'+r.run,r.pressureI,'small')+fld('D','pd'+r.run,r.pressureD,'small')+
   '<div class=live>Pressure PV<br><b id=prpv'+r.run+'>'+(r.pressureGood?Number(r.pressurePV).toFixed(1):'---')+'</b></div>'+
   '<div class=live>Override OUT<br><b id=pout'+r.run+'>'+Number(r.pressureOutput).toFixed(1)+'%</b></div>'+
   '<div><label>Output Test</label><button onclick="location.href=\'/control-output\'">Test Outputs</button></div></div>'+
  '</div>';
}
async function load(){
 const x=await fetch('/api/pid-control?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 loops.innerHTML=x.runs.map(loop).join('');
}
async function refreshLive(){
 try{
  const x=await fetch('/api/pid-control?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  for(const r of x.runs){
   if(e('pv'+r.run))e('pv'+r.run).textContent=r.pvGood?Number(r.pv).toFixed(3):'---';
   if(e('prpv'+r.run))e('prpv'+r.run).textContent=r.pressureGood?Number(r.pressurePV).toFixed(1):'---';
   if(e('fout'+r.run))e('fout'+r.run).textContent=Number(r.flowOutput).toFixed(1)+'%';
   if(e('out'+r.run))e('out'+r.run).textContent=Number(r.output).toFixed(1)+'%';
   if(e('pout'+r.run))e('pout'+r.run).textContent=Number(r.pressureOutput).toFixed(1)+'%';
   if(e('st'+r.run)){
    e('st'+r.run).textContent=r.overrideActive?'PRESSURE OVERRIDE':(r.modeName+(r.mode==2&&!r.inputGood?' / WAITING FLOW PV':''));
    e('st'+r.run).className=r.overrideActive?'warn':'';
   }
  }
 }catch(err){}
}
async function save(run){
 const p=new URLSearchParams({
  run:String(run),mode:e('m'+run).value,ao:e('a'+run).value,sp:e('s'+run).value,
  manualOut:e('o'+run).value,p:e('p'+run).value,i:e('i'+run).value,d:e('d'+run).value,
  outMin:e('n'+run).value,outMax:e('x'+run).value,
  pressureEnabled:e('pe'+run).value,pressureSP:e('ps'+run).value,
  pressureP:e('pp'+run).value,pressureI:e('pi'+run).value,pressureD:e('pd'+run).value
 });
 const r=await fetch('/api/pid-control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json()}catch(err){}
 msg.textContent=r.ok?'PID '+run+' saved':(x.error||'SAVE FAILED');
 msg.className='msg '+(r.ok?'good':'bad');
 if(r.ok)setTimeout(load,250);
}
load();setInterval(refreshLive,1000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/m31-transport-runtime",HTTP_POST,[](){
    if(!server.hasArg("mode")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"mode required: rtu or tcp\"}");
      return;
    }
    String mode=server.arg("mode"); mode.toLowerCase();
    if(mode=="tcp")m31TransportMode=M31_TRANSPORT_TCP;
    else if(mode=="rtu")m31TransportMode=M31_TRANSPORT_RTU;
    else{
      server.send(400,"application/json","{\"ok\":false,\"error\":\"mode must be rtu or tcp\"}");
      return;
    }
    if(server.hasArg("host") && server.arg("host").length())m31TcpHost=server.arg("host");
    if(server.hasArg("port")){
      long p=server.arg("port").toInt();
      if(p<1 || p>65535){server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid port\"}");return;}
      m31TcpPort=(uint16_t)p;
    }
    if(server.hasArg("unit")){
      long u=server.arg("unit").toInt();
      if(u<1 || u>247){server.send(400,"application/json","{\"ok\":false,\"error\":\"Modbus unit must be 1..247\"}");return;}
      m31UnitId=(uint8_t)u;
    }
    if(server.hasArg("baud")){
      long b=server.arg("baud").toInt();
      if(b!=1200 && b!=2400 && b!=4800 && b!=9600 && b!=19200 && b!=38400 && b!=57600 && b!=115200){
        server.send(400,"application/json","{\"ok\":false,\"error\":\"unsupported RTU baud\"}");return;
      }
      if((uint32_t)b!=m31RtuBaud){
        m31RtuBaud=(uint32_t)b;
        modbusSerial.end();
        delay(5);
        modbusSerial.begin(m31RtuBaud,SERIAL_8N1,MODBUS_RX_PIN,MODBUS_TX_PIN);
      }
    }
    modbusOnline=false;
    consecutiveFails=0;
    lastPoll=0;
    pollPhase=0;
    String j="{\"ok\":true,\"mode\":\""+String(m31TransportMode==M31_TRANSPORT_TCP?"tcp":"rtu")+"\"";
    j+=",\"host\":\""+m31TcpHost+"\",\"port\":"+String(m31TcpPort);
    j+=",\"unit\":"+String(m31UnitId)+",\"baud\":"+String(m31RtuBaud)+"}";
    if(!saveOperatorFlashConfig()){server.send(500,"application/json","{\"ok\":false,\"error\":\"flash config save failed\"}");return;}
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/m31-rack-status",HTTP_GET,[](){
    Ep32M31RackStatus s; const bool ok=ep32M31ReadRackStatus(s);
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:502,"application/json",ep32M31RackStatusJson(s));
  });

  server.on("/api/m31-rack-negotiate",HTTP_POST,[](){
    if(!server.hasArg("confirm") || server.arg("confirm")!="NEGOTIATE"){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"confirm=NEGOTIATE required\"}");
      return;
    }
    Ep32M31RackStatus before,after; bool transitionObserved=false; uint8_t stableReads=0; String error;
    const bool ok=ep32M31NegotiateRack(before,after,transitionObserved,stableReads,error);
    Serial.printf("M31 NEGOTIATE ok=%s transition=%s stable=%u complete=%u deviceError=%u pending=%u bitmap=0x%04X%04X error=%s\n",
      ok?"YES":"NO",transitionObserved?"YES":"NO",(unsigned)stableReads,(unsigned)after.complete,
      (unsigned)after.deviceError,(unsigned)after.pending,(unsigned)after.exceptionHi,(unsigned)after.exceptionLo,error.c_str());
    server.sendHeader("Cache-Control","no-store");
    server.send(ok?200:502,"application/json",ep32M31RackStatusJson(after,true,ok,error,transitionObserved,stableReads));
  });

  server.on("/api/modbus-comms",HTTP_POST,[](){
    if(!server.hasArg("enabled")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"enabled required\"}");
      return;
    }
    bool enable=server.arg("enabled")=="1" || server.arg("enabled")=="true" || server.arg("enabled")=="on";
    modbusCommsEnabled=enable;
    if(!enable){
      modbusOnline=false;
      consecutiveFails=0;
    }else{
      lastPoll=0;
      pollPhase=0;
    }
    saveModbusCommsConfig();
    epaperPendingRegionMask|=EPAPER_REGION_FOOTER;
    displayDirty=true;
    String j="{\"ok\":true,\"enabled\":"+String(modbusCommsEnabled?"true":"false")+"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/modbus-diag",HTTP_GET,[](){
    String j="{\"enabled\":"+String(modbusCommsEnabled?"true":"false");
    j+=",\"transport\":\""+String(m31TransportMode==M31_TRANSPORT_TCP?"TCP":"RTU")+"\"";
    j+=",\"tcpHost\":\""+m31TcpHost+"\",\"tcpPort\":"+String(m31TcpPort);
    j+=",\"online\":"+String(modbusOnline?"true":"false");
    j+=",\"good\":"+String(modbusGood)+",\"bad\":"+String(modbusBad);
    j+=",\"timeouts\":"+String(rawTimeouts)+",\"frames\":"+String(rawRxFrames);
    j+=",\"bytes\":"+String(rawRxBytes)+",\"crcFail\":"+String(rawCrcFails);
    j+=",\"lastTx\":\""+lastRawTx+"\"";
    j+=",\"lastWriteTx\":\""+lastWriteTx+"\"";
    j+=",\"lastWriteRx\":\""+lastWriteRx+"\"";
    j+=",\"ao_mA\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(ao_mA[i],3);}
    j+="]";
    j+=",\"aoForce\":[";
    for(uint8_t i=0;i<4;i++){
      if(i)j+=",";
      j+="{\"active\":"+String(aoForceActive[i]?"true":"false");
      j+=",\"percent\":"+String(aoForcePercent[i],1)+"}";
    }
    j+="]";
    j+=",\"diRaw\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(diRawState[i]?"true":"false");}
    j+="],\"diForce\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+="{\"active\":"+String(diForceActive[i]?"true":"false")+",\"state\":"+String(diForceValue[i]?"true":"false")+"}";}
    j+="],\"doForce\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+="{\"active\":"+String(doForceActive[i]?"true":"false")+",\"state\":"+String(doForceValue[i]?"true":"false")+"}";}
    j+="],\"di\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(diState[i]?"true":"false");}
    j+="],\"do\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(doState[i]?"true":"false");}
    j+="],\"ai_mA\":[";
    for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+=String(ai_mA[i],3);}
    j+="],\"aiEU\":[";
    for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+=String(aiEU[i],3);}
    j+="],\"aiName\":[";
    for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+="\""+String(aiCfg[i].name)+"\"";}
    j+="],\"aiUnits\":[";
    for(uint8_t i=0;i<ep32M31ActiveAiCount();i++){if(i)j+=",";j+="\""+String(aiCfg[i].units)+"\"";}
    j+="]";
    j+=",\"baud\":"+String(m31RtuBaud);
    j+=",\"slave\":"+String(m31UnitId);
    j+=",\"txPin\":"+String(MODBUS_TX_PIN);
    j+=",\"rxPin\":"+String(MODBUS_RX_PIN);
    j+=",\"phase\":"+String(pollPhase)+",\"lastRx\":\""+lastRawRx+"\"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/do-test",HTTP_POST,[](){
    if(!modbusOnline){
      server.send(503,"application/json","{\"ok\":false,\"error\":\"M31 offline\"}");
      return;
    }

    if(server.hasArg("allOff") && server.arg("allOff")=="1"){
      bool ok=true;
      for(uint8_t ch=0;ch<4;ch++){
        if(!writeM31Coil(ch,false))ok=false;
      }
      if(ok){
        epaperPendingRegionMask|=EPAPER_REGION_IO;
        displayDirty=true;
      }
      server.send(ok?200:502,"application/json",
        ok?"{\"ok\":true,\"allOff\":true}":"{\"ok\":false,\"error\":\"one or more DO writes failed\"}");
      return;
    }

    if(!server.hasArg("channel") || !server.hasArg("state")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"channel and state required\"}");
      return;
    }

    int ch=server.arg("channel").toInt();
    bool on=server.arg("state")=="1";
    if(ch<1 || ch>4){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid DO channel\"}");
      return;
    }

    bool ok=writeM31Coil((uint8_t)(ch-1),on);
    if(ok){
      epaperPendingRegionMask|=EPAPER_REGION_IO;
      displayDirty=true;
    }

    String j="{\"ok\":"+String(ok?"true":"false");
    j+=",\"channel\":"+String(ch);
    j+=",\"state\":"+String(on?"true":"false")+"}";
    server.send(ok?200:502,"application/json",j);
  });

  server.on("/api/control-output",HTTP_GET,[](){
    String j="{\"modbusOnline\":"+String(modbusOnline?"true":"false");
    j+=",\"runs\":[";
    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      if(run)j+=",";
      ManualControlOutput &o=manualControlOutput[run];
      j+="{\"run\":"+String(run+1);
      j+=",\"name\":\""+String(flowRunName[run])+"\"";
      j+=",\"lsd\":\""+String(flowRunLSD[run])+"\"";
      j+=",\"enabled\":"+String(flowRunEnabled[run]?"true":"false");
      j+=",\"type\":"+String((uint8_t)o.type);
      j+=",\"typeName\":\""+String(controlOutputTypeName(o.type))+"\"";
      j+=",\"channel\":"+String(o.channel+1);
      j+=",\"aoPercent\":"+String(o.aoPercent,1);
      j+=",\"ao_mA\":"+String(4.0f+constrain(o.aoPercent,0.0f,100.0f)*0.16f,3);
      j+=",\"doOn\":"+String(o.doOn?"true":"false");
      j+="}";
    }
    j+="],\"do\":[";
    for(uint8_t i=0;i<4;i++){if(i)j+=",";j+=String(doState[i]?"true":"false");}
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/control-output",HTTP_POST,[](){
    if(!server.hasArg("run") || !server.hasArg("type") || !server.hasArg("channel")){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"run, type and channel required\"}");
      return;
    }

    int runArg=server.arg("run").toInt();
    int typeArg=server.arg("type").toInt();
    int channelArg=server.arg("channel").toInt();
    if(runArg<1 || runArg>FLOW_RUN_COUNT ||
       typeArg<0 || typeArg>2 ||
       channelArg<1 || channelArg>4){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid output selection\"}");
      return;
    }

    uint8_t run=(uint8_t)(runArg-1);
    ControlOutputType type=(ControlOutputType)typeArg;
    uint8_t channel=(uint8_t)(channelArg-1);

    if(!flowRunEnabled[run]){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"flow run is disabled\"}");
      return;
    }
    if(controlOutputConflict(run,type,channel)){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"output channel already assigned to another run\"}");
      return;
    }

    ManualControlOutput &o=manualControlOutput[run];
    o.type=type;
    o.channel=channel;
    if(server.hasArg("aoPercent")){
      o.aoPercent=constrain(server.arg("aoPercent").toFloat(),0.0f,100.0f);
    }
    if(server.hasArg("doOn")){
      o.doOn=server.arg("doOn")=="1";
    }

    if(type==CONTROL_OUTPUT_NONE){
      server.send(200,"application/json","{\"ok\":true,\"applied\":false}");
      return;
    }
    if(!modbusOnline){
      server.send(503,"application/json","{\"ok\":false,\"error\":\"M31 offline - output not written\"}");
      return;
    }

    bool ok=applyManualControlOutput(run);
    if(!ok){
      server.send(502,"application/json","{\"ok\":false,\"error\":\"Modbus output write failed\"}");
      return;
    }

    server.send(200,"application/json","{\"ok\":true,\"applied\":true}");
  });

  server.on("/control-output",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>MultiFlow Control Outputs</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1100px}
a{color:#63e6be}.top{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}
.badge{padding:6px 10px;border-radius:16px;border:1px solid #555}.ok{color:#8f8}.bad{color:#ff8b8b}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:13px}
.muted{font-size:12px;color:#aaa}
.runTable{width:100%;border-collapse:collapse;background:#171717;border:1px solid #444;border-radius:9px;overflow:hidden}
.runTable th{background:#101415;color:#aaa;font-size:12px;text-align:left;padding:9px;border-bottom:1px solid #344}
.runTable td{padding:8px;border-bottom:1px solid #30383b;vertical-align:middle}
.runTable tr:last-child td{border-bottom:0}.runTable tr.disabled{opacity:.5}.runName{font-weight:700}.runMeta{font-size:11px;color:#999;margin-top:2px}
.runTable select,.runTable input,.runTable button{width:100%;padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:4px}
.runTable button{cursor:pointer;margin:0;white-space:nowrap}.cmdCell .muted{padding:7px 0}.result{min-height:18px;font-size:11px;margin-top:3px}
label{display:block;color:#aaa;font-size:12px;margin-bottom:4px}
input,select,button{padding:8px;background:#222;color:#fff;border:1px solid #666;border-radius:4px}
.warning{background:#2a2112;border:1px solid #806020;border-radius:7px;padding:10px;margin:10px 0;color:#ffd166}
.doTest{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin:10px 0}
.doBox{background:#101415;border:1px solid #344;border-radius:8px;padding:10px;text-align:center}
.doState{font-size:18px;font-weight:700;margin:5px 0 8px}.doButtons{display:grid;grid-template-columns:1fr 1fr;gap:5px}
.doButtons button{margin:0}.allOff{max-width:220px;background:#3a1c1c}
@media(max-width:760px){.doTest{grid-template-columns:repeat(2,1fr)}.runWrap{overflow-x:auto}.runTable{min-width:820px}}
</style></head><body>
<div class=top><div><h2 style="margin-bottom:4px">Manual Control Outputs</h2>
<div class=muted>Commissioning foundation only — no PID or automatic output writes.</div></div>
<div id=mb class=badge>Loading M31...</div></div>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>

<div class=card>
<h3 style="margin-top:0">DO Commissioning Test</h3>
<div class=muted>Direct temporary control of M31 DO1-DO4 for wiring and field testing. Nothing is saved.</div>
<div id=doTest class=doTest></div>
<button class=allOff onclick=allDOOff()>ALL DO OFF</button>
<div id=doMsg class=result></div>
</div>

<div class=warning>Flow-output assignments below change only when <b>APPLY OUTPUT</b> is pressed. Assignments are intentionally RAM-only in this test build and return to NONE after reboot.</div>
<div class=runWrap>
<table class=runTable>
<thead><tr><th style="width:24%">Run / Name</th><th style="width:16%">Output Type</th><th style="width:14%">Channel</th><th style="width:20%">Command</th><th style="width:12%">Apply</th><th>Status</th></tr></thead>
<tbody id=runs></tbody>
</table>
</div>
<script>
const e=id=>document.getElementById(id);
function title(r){let s=r.name||('Flow Run '+r.run);if(r.lsd)s+=' - '+r.lsd;return s}
function channelOpts(type,sel){
 let p=type==2?'DO':'AO',h='';
 for(let i=1;i<=4;i++)h+='<option value="'+i+'" '+(i==sel?'selected':'')+'>'+p+i+'</option>';
 return h;
}
function row(r){
 const disabled=!r.enabled;
 return '<tr class="'+(disabled?'disabled':'')+'" id=row'+r.run+'>'+
 '<td><div class=runName>'+title(r)+'</div><div class=runMeta>Run '+r.run+' · '+(r.enabled?'ENABLED':'DISABLED')+'</div></td>'+
 '<td><select id=t'+r.run+' onchange="changed('+r.run+')">'+
 '<option value=0 '+(r.type==0?'selected':'')+'>NONE</option>'+
 '<option value=1 '+(r.type==1?'selected':'')+'>AO 4-20mA</option>'+
 '<option value=2 '+(r.type==2?'selected':'')+'>DO ON/OFF</option></select></td>'+
 '<td><select id=c'+r.run+'>'+channelOpts(r.type,r.channel)+'</select></td>'+
 '<td class=cmdCell id=cmd'+r.run+'></td>'+
 '<td><button '+(disabled?'disabled':'')+' onclick="applyOut('+r.run+')">APPLY</button></td>'+
 '<td><div class=result id=m'+r.run+'></div></td></tr>';
}
function changed(run){
 const t=Number(e('t'+run).value),c=e('c'+run),cmd=e('cmd'+run);
 const old=Number(c.value)||1;
 c.innerHTML=channelOpts(t,old);
 if(t==1)cmd.innerHTML='<input id=v'+run+' type=number min=0 max=100 step=.1 value="0"><div class=muted>% (0=4mA, 100=20mA)</div>';
 else if(t==2)cmd.innerHTML='<select id=v'+run+'><option value=0>OFF</option><option value=1>ON</option></select>';
 else cmd.innerHTML='<div class=muted>Disabled</div>';
}
function renderDO(states){
 e('doTest').innerHTML=states.map((on,i)=>
   '<div class=doBox><div><b>DO'+(i+1)+'</b></div>'+
   '<div class="doState '+(on?'ok':'muted')+'">'+(on?'ON':'OFF')+'</div>'+
   '<div class=doButtons><button onclick="setDO('+(i+1)+',1)">ON</button>'+
   '<button onclick="setDO('+(i+1)+',0)">OFF</button></div></div>'
 ).join('');
}
async function setDO(ch,state){
 const p=new URLSearchParams({channel:String(ch),state:String(state)});
 e('doMsg').textContent='Writing DO'+ch+'...';
 const r=await fetch('/api/do-test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json()}catch(_){}
 e('doMsg').textContent=r.ok?('DO'+ch+' '+(state?'ON':'OFF')):('FAILED: '+(x.error||r.status));
 await load();
}
async function allDOOff(){
 const p=new URLSearchParams({allOff:'1'});
 e('doMsg').textContent='Turning all DO OFF...';
 const r=await fetch('/api/do-test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
 let x={};try{x=await r.json()}catch(_){}
 e('doMsg').textContent=r.ok?'All DO OFF':('FAILED: '+(x.error||r.status));
 await load();
}
async function load(){
 const x=await fetch('/api/control-output?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 renderDO(x.do||[false,false,false,false]);
 e('mb').className='badge '+(x.modbusOnline?'ok':'bad');
 e('mb').textContent=x.modbusOnline?'M31 ONLINE':'M31 OFFLINE';
 e('runs').innerHTML=x.runs.map(row).join('');
 for(const r of x.runs){
   changed(r.run);
   if(r.type==1 && e('v'+r.run))e('v'+r.run).value=r.aoPercent;
   if(r.type==2 && e('v'+r.run))e('v'+r.run).value=r.doOn?'1':'0';
 }
}
async function applyOut(run){
 const type=Number(e('t'+run).value);
 const p=new URLSearchParams({run:String(run),type:String(type),channel:e('c'+run).value});
 if(type==1)p.set('aoPercent',e('v'+run).value);
 if(type==2)p.set('doOn',e('v'+run).value);
 const m=e('m'+run);
 m.className='result muted';m.textContent='Writing...';
 try{
   const r=await fetch('/api/control-output',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});
   const x=await r.json();
   if(!r.ok)throw new Error(x.error||'write failed');
   m.className='result ok';
   if(type==1)m.textContent='Applied '+Number(e('v'+run).value).toFixed(1)+'% ('+(4+Number(e('v'+run).value)*.16).toFixed(3)+' mA)';
   else if(type==2)m.textContent='Applied DO '+(e('v'+run).value=='1'?'ON':'OFF');
   else m.textContent='Output assignment set to NONE';
 }catch(err){m.className='result bad';m.textContent=String(err.message||err)}
}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  // EP32 TAG QA GATE6A MQTT VISIBILITY EXPORT V1
  server.on("/api/tags",HTTP_GET,[](){
    String j;
    j.reserve(24000);
    j="{\"mqtt\":{";
    j+="\"enabled\":"+String(mqttCfg.enabled?"true":"false");
    j+=",\"connected\":"+String(mqttClient.connected()?"true":"false");
    j+=",\"baseTopic\":\""+mqttBaseTopicClean()+"\"";
    j+=",\"liveIntervalSec\":"+String(mqttCfg.liveIntervalSec);
    j+=",\"totalizerIntervalSec\":"+String(mqttCfg.totalizerIntervalSec);
    j+=",\"statusIntervalSec\":"+String(mqttCfg.statusIntervalSec);
    j+=",\"publishCount\":"+String(mqttPublishCount);
    j+=",\"publishFails\":"+String(mqttPublishFailCount);
    j+="},\"points\":[";

    bool first=true;
    auto addPoint=[&](const String& id,const String& name,const String& source,
                      const String& type,const String& units,float value,const String& quality,
                      const String& detail,const String& groupName,const String& mqttState,
                      const String& publishClass,const String& publishMode,uint32_t intervalSec,
                      uint32_t ttlSec,const String& topic,const String& field){
      if(!first)j+=",";
      first=false;
      String a=id,b=name,c=source,d=type,e=units,q=quality,z=detail,g=groupName;
      String ms=mqttState,pc=publishClass,pm=publishMode,tp=topic,fd=field;
      String* vals[]={&a,&b,&c,&d,&e,&q,&z,&g,&ms,&pc,&pm,&tp,&fd};
      for(String* v:vals){v->replace("\\","\\\\");v->replace("\"","\\\"");}
      j+="{\"id\":\""+a+"\",\"name\":\""+b+"\",\"source\":\""+c+"\",\"type\":\""+d+"\",\"units\":\""+e+"\"";
      j+=",\"value\":"+String(isfinite(value)?value:0.0f,6)+",\"quality\":\""+q+"\",\"detail\":\""+z+"\"";
      j+=",\"group\":\""+g+"\",\"mqtt\":\""+ms+"\",\"publishClass\":\""+pc+"\",\"publishMode\":\""+pm+"\"";
      j+=",\"intervalSec\":"+String(intervalSec)+",\"ttlSec\":"+String(ttlSec);
      j+=",\"topic\":\""+tp+"\",\"field\":\""+fd+"\"}";
    };

    String base=mqttBaseTopicClean();
    String mbQuality=!modbusCommsEnabled?"DISABLED":(modbusOnline?"GOOD":"BAD");

    String ioTopic=base+"/io/live";
    uint32_t ioTtl=mqttCfg.liveIntervalSec?mqttCfg.liveIntervalSec*3UL:0;
    for(uint8_t i=0;i<8;i++){
      bool pub=ioMqttAI[i] && i<ep32M31ActiveAiCount();
      addPoint("M31.AI"+String(i+1),String(aiCfg[i].name),"M31","Analog",
        String(aiCfg[i].units),aiEU[i],aiManual[i]?"MANUAL":mbQuality,
        aiManual[i]?"Manual override":("AI"+String(i+1)),"M31 I/O",pub?"YES":"NO",pub?"LIVE":"LOCAL",
        pub?"Periodic":"Local only",pub?mqttCfg.liveIntervalSec:0,pub?ioTtl:0,pub?ioTopic:"",pub?("ai."+String(i+1)+".value"):"");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttDI[i];
      addPoint("M31.DI"+String(i+1),"DI"+String(i+1),"M31","Digital","",
        diState[i]?1.0f:0.0f,diForceActive[i]?"FORCED":mbQuality,
        diForceActive[i]?"Forced effective DI":("Discrete Input "+String(i+1)),"M31 I/O",pub?"YES":"NO",pub?"LIVE":"LOCAL",
        pub?"Change + heartbeat":"Local only",pub?mqttCfg.liveIntervalSec:0,pub?ioTtl:0,pub?ioTopic:"",pub?("di."+String(i+1)+".value"):"");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttDO[i];
      addPoint("M31.DO"+String(i+1),"DO"+String(i+1),"M31","Digital","",
        doState[i]?1.0f:0.0f,doForceActive[i]?"FORCED":mbQuality,
        doForceActive[i]?"Forced output":("Discrete Output "+String(i+1)),"M31 I/O",pub?"YES":"NO",pub?"LIVE":"LOCAL",
        pub?"Change + heartbeat":"Local only",pub?mqttCfg.liveIntervalSec:0,pub?ioTtl:0,pub?ioTopic:"",pub?("do."+String(i+1)+".value"):"");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttAO[i] && i<ep32M31ActiveAoCount();
      addPoint("M31.AO"+String(i+1),"AO"+String(i+1),"M31","Analog Output","mA",
        ao_mA[i],aoForceActive[i]?"FORCED":mbQuality,
        aoForceActive[i]?"Forced analog output":("Analog Output "+String(i+1)),"M31 I/O",pub?"YES":"NO",pub?"LIVE":"LOCAL",
        pub?"Periodic":"Local only",pub?mqttCfg.liveIntervalSec:0,pub?ioTtl:0,pub?ioTopic:"",pub?("ao."+String(i+1)+".mA"):"");
    }

    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      String rn=flowRunDisplayName(run);
      String fq=!flowRunEnabled[run]?"DISABLED":(flowRunValid[run]?"GOOD":"BAD");
      String liveTopic=base+"/flow/"+String(run+1)+"/live";
      String totTopic=base+"/flow/"+String(run+1)+"/totalizer";
      uint32_t liveTtl=mqttCfg.liveIntervalSec?mqttCfg.liveIntervalSec*3UL:0;
      uint32_t totTtl=mqttCfg.totalizerIntervalSec?mqttCfg.totalizerIntervalSec*3UL:0;

      addPoint("FLOW"+String(run+1)+".RATE",rn+" Flow","Flow","Calculated Flow","E3 m3/day",
        flowRunValid[run]?flowRunFlow[run]:0.0f,fq,"Run "+String(run+1),
        "Flow Run "+String(run+1),"YES","LIVE","Periodic",mqttCfg.liveIntervalSec,liveTtl,
        liveTopic,"flow_E3m3_day");

      addPoint("FLOW"+String(run+1)+".TODAY",rn+" Today","Flow","Totalizer","E3 m3",
        (float)flowDay[run].volumeE3m3,flowRunEnabled[run]?"GOOD":"DISABLED","Running daily total",
        "Flow Run "+String(run+1),"YES","LIVE","Periodic",mqttCfg.totalizerIntervalSec,totTtl,
        totTopic,"flowToday_E3m3");

      addPoint("PID"+String(run+1)+".SP",rn+" PID SP","PID","Control","E3 m3/day",
        flowPIDCfg[run].flowSP,flowPIDCfg[run].mode==FLOW_CTRL_DISABLED?"DISABLED":"GOOD","Flow setpoint",
        "PID "+String(run+1),"NO","LOCAL","Local only",0,0,"","");

      addPoint("PID"+String(run+1)+".OUT",rn+" PID Output","PID","Control","%",
        flowPIDRt[run].output,flowPIDCfg[run].mode==FLOW_CTRL_DISABLED?"DISABLED":"GOOD",
        String(flowControlModeName(flowPIDCfg[run].mode)),
        "PID "+String(run+1),"NO","LOCAL","Local only",0,0,"","");
    }

    uint32_t statTtl=mqttCfg.statusIntervalSec?mqttCfg.statusIntervalSec*3UL:0;
    addPoint("SYS.BATTERY","Battery Voltage","System","Power","V",
      batteryTelemetryValid()?batteryVoltageV:0.0f,batteryTelemetryValid()?"GOOD":"BAD","Controller battery telemetry",
      "System","YES","STATUS","Periodic",mqttCfg.statusIntervalSec,statTtl,base+"/status","batteryV");
    addPoint("SYS.WIFI","WiFi Connected","System","Digital","",
      WiFi.status()==WL_CONNECTED?1.0f:0.0f,WiFi.status()==WL_CONNECTED?"GOOD":"BAD",activeIpString(),
      "System","NO","LOCAL","Local only",0,0,"","");
    addPoint("SYS.MQTT","MQTT Connected","System","Digital","",
      mqttClient.connected()?1.0f:0.0f,mqttCfg.enabled?(mqttClient.connected()?"GOOD":"BAD"):"DISABLED",mqttCfg.baseTopic,
      "System","NO","LOCAL","Local only",0,0,"","");

    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    if(server.hasArg("download") && server.arg("download")=="1")
      server.sendHeader("Content-Disposition","attachment; filename=riotflow_tags.json");
    server.send(200,"application/json",j);
  });

  // EP32 TAG QA GATE6A CSV QUOTE FIX V3
  server.on("/api/tags/export.csv",HTTP_GET,[](){
    String c;
    c.reserve(12000);
    c="Tag ID,Name,Source,Type,Units,Group,MQTT,Publish Class,Publish Mode,Interval Sec,TTL Sec,Topic,JSON Field,Notes\r\n";
    auto csv=[&](String v){
      v.replace("\"", "\"\"");
      if(v.indexOf(',')>=0 || v.indexOf('\"')>=0 || v.indexOf('\n')>=0)v="\""+v+"\"";
      return v;
    };
    auto row=[&](const String& id,const String& name,const String& source,const String& type,
                 const String& units,const String& groupName,const String& mqttState,
                 const String& publishClass,const String& publishMode,uint32_t intervalSec,uint32_t ttlSec,
                 const String& topic,const String& field,const String& notes){
      c+=csv(id)+","+csv(name)+","+csv(source)+","+csv(type)+","+csv(units)+","+csv(groupName)+","+csv(mqttState)+",";
      c+=csv(publishClass)+","+csv(publishMode)+","+String(intervalSec)+","+String(ttlSec)+","+csv(topic)+","+csv(field)+","+csv(notes)+"\r\n";
    };

    String base=mqttBaseTopicClean();
    String ioTopic=base+"/io/live";
    uint32_t li=mqttCfg.liveIntervalSec,ioTtl=li?li*3UL:0;
    for(uint8_t i=0;i<8;i++){
      bool pub=ioMqttAI[i] && i<ep32M31ActiveAiCount();
      row("M31.AI"+String(i+1),String(aiCfg[i].name),"M31","Analog",String(aiCfg[i].units),"M31 I/O",
          pub?"YES":"NO",pub?"LIVE":"LOCAL",pub?"Periodic":"Local only",pub?li:0,pub?ioTtl:0,
          pub?ioTopic:"",pub?("ai."+String(i+1)+".value"):"",pub?"Selected for I/O MQTT":"Not selected");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttDI[i];
      row("M31.DI"+String(i+1),"DI"+String(i+1),"M31","Digital","","M31 I/O",
          pub?"YES":"NO",pub?"LIVE":"LOCAL",pub?"Change + heartbeat":"Local only",pub?li:0,pub?ioTtl:0,
          pub?ioTopic:"",pub?("di."+String(i+1)+".value"):"",pub?"Selected for I/O MQTT":"Not selected");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttDO[i];
      row("M31.DO"+String(i+1),"DO"+String(i+1),"M31","Digital","","M31 I/O",
          pub?"YES":"NO",pub?"LIVE":"LOCAL",pub?"Change + heartbeat":"Local only",pub?li:0,pub?ioTtl:0,
          pub?ioTopic:"",pub?("do."+String(i+1)+".value"):"",pub?"Selected for I/O MQTT":"Not selected");
    }
    for(uint8_t i=0;i<4;i++){
      bool pub=ioMqttAO[i] && i<ep32M31ActiveAoCount();
      row("M31.AO"+String(i+1),"AO"+String(i+1),"M31","Analog Output","mA","M31 I/O",
          pub?"YES":"NO",pub?"LIVE":"LOCAL",pub?"Periodic":"Local only",pub?li:0,pub?ioTtl:0,
          pub?ioTopic:"",pub?("ao."+String(i+1)+".mA"):"",pub?"Selected for I/O MQTT":"Not selected");
    }

    for(uint8_t run=0;run<FLOW_RUN_COUNT;run++){
      String rn=flowRunDisplayName(run);
      uint32_t li=mqttCfg.liveIntervalSec,ti=mqttCfg.totalizerIntervalSec;
      row("FLOW"+String(run+1)+".RATE",rn+" Flow","Flow","Calculated Flow","E3 m3/day","Flow Run "+String(run+1),
          "YES","LIVE","Periodic",li,li?li*3UL:0,base+"/flow/"+String(run+1)+"/live","flow_E3m3_day","Enabled runs only");
      row("FLOW"+String(run+1)+".TODAY",rn+" Today","Flow","Totalizer","E3 m3","Flow Run "+String(run+1),
          "YES","LIVE","Periodic",ti,ti?ti*3UL:0,base+"/flow/"+String(run+1)+"/totalizer","flowToday_E3m3","Enabled runs only");
      row("PID"+String(run+1)+".SP",rn+" PID SP","PID","Control","E3 m3/day","PID "+String(run+1),"NO","LOCAL","Local only",0,0,"","","Not currently published");
      row("PID"+String(run+1)+".OUT",rn+" PID Output","PID","Control","%","PID "+String(run+1),"NO","LOCAL","Local only",0,0,"","","Not currently published");
    }
    uint32_t si=mqttCfg.statusIntervalSec;
    row("SYS.BATTERY","Battery Voltage","System","Power","V","System","YES","STATUS","Periodic",si,si?si*3UL:0,base+"/status","batteryV","");
    row("SYS.WIFI","WiFi Connected","System","Digital","","System","NO","LOCAL","Local only",0,0,"","","Not currently published as a tag");
    row("SYS.MQTT","MQTT Connected","System","Digital","","System","NO","LOCAL","Local only",0,0,"","","Not currently published as a tag");

    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Disposition","attachment; filename=riotflow_tag_interface.csv");
    server.send(200,"text/csv",c);
  });

  server.on("/tags",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Tag / SCADA QA</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px}a{color:#63e6be}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:12px;margin:10px 0}
.controls{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.controls input,.controls select,.btn{padding:8px;background:#222;color:#fff;border:1px solid #666;border-radius:6px;text-decoration:none}
.summary{display:grid;grid-template-columns:repeat(4,minmax(120px,1fr));gap:8px}.metric{background:#121718;border:1px solid #344;padding:9px;border-radius:7px}.metric b{font-size:1.25em}
.wrap{overflow:auto;border:1px solid #333;border-radius:8px;max-height:72vh}
table{width:100%;border-collapse:collapse;font-size:.88em;white-space:nowrap}th,td{padding:7px;border-bottom:1px solid #333;text-align:left}th{position:sticky;top:0;background:#1b1b1b}
.ok{color:#63e6be}.bad{color:#ff6b6b}.warn{color:#ffd166}.muted{color:#999}
@media(max-width:700px){.summary{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<h2>Riot-Flow Tag / SCADA QA</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
@media(max-width:600px){.rfNav{display:grid;grid-template-columns:1fr}.rfNav>a,.rfDrop,.rfDrop>button{width:100%}.rfDrop>button{text-align:left}.rfMenu{position:static;min-width:0;width:100%;margin-top:4px}}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu><a href="/flow-runs">Flow Runs</a><a href="/flow-setup">Flow Setup</a><a href="/aga-setup">AGA / Gas</a><a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a></div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu><a href="/io-setup">I/O Setup</a><a href="/pid-control">PID / Control</a><a href="/control-output">Manual Outputs</a><a href="/modbus-monitor">Modbus Monitor</a></div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu><a href="/rules">Rules</a><a href="/display-setup">Display</a><a href="/mqtt-setup">MQTT</a><a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a><a href="/process-setup">Process Setup</a></div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu><a href="/system-status">System Status</a><a href="/backup-restore">Backup / Restore</a><a href="/nvs-maintenance">NVS Storage</a><a href="/modbus-debug">Modbus Debug</a><a href="/tags">Tags</a><a href="/api-reference">API Reference</a></div></div>
</div>

<div class=card>
<b>SCADA / MQTT Interface Verification</b>
<p class=muted>This page reports what exists locally and what the current firmware actually publishes. It does not imply publication of local-only tags.</p>
<div class=summary>
<div class=metric>Tags<br><b id=nTags>-</b></div>
<div class=metric>MQTT Published<br><b id=nPub>-</b></div>
<div class=metric>Local Only<br><b id=nLocal>-</b></div>
<div class=metric>MQTT<br><b id=mqState>-</b></div>
</div>
<div id=mqInfo class=muted style="margin-top:9px"></div>
</div>

<div class=card>
<div class=controls>
<input id=q placeholder="Search tag, topic, field..." oninput=render()>
<select id=source onchange=render()><option value="">All Sources</option></select>
<select id=pub onchange=render()><option value="">All MQTT</option><option>YES</option><option>NO</option></select>
<select id=pclass onchange=render()><option value="">All Classes</option></select>
<a class=btn href="/api/tags/export.csv">Export CSV</a>
<a class=btn href="/api/tags?download=1">Export JSON</a>
</div>
<div id=stats class=muted style="margin-top:8px"></div>
</div>

<div class=wrap><table><thead><tr>
<th>Tag ID</th><th>Name</th><th>Group</th><th>Value</th><th>Units</th><th>Quality</th>
<th>MQTT</th><th>Class</th><th>Mode</th><th>Rate</th><th>TTL</th><th>Topic</th><th>Field</th><th>Detail</th>
</tr></thead><tbody id=body></tbody></table></div>
<script>
let points=[],meta={};const esc=s=>String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
function qc(x){return x==='GOOD'||x==='MANUAL'||x==='FORCED'?'ok':(x==='DISABLED'?'warn':'bad')}
function sec(x){return Number(x)>0?Number(x)+' s':'-'}
function filters(){
 const ss=[...new Set(points.map(x=>x.source))].sort(),cc=[...new Set(points.map(x=>x.publishClass))].sort();
 source.innerHTML='<option value="">All Sources</option>'+ss.map(x=>'<option>'+esc(x)+'</option>').join('');
 pclass.innerHTML='<option value="">All Classes</option>'+cc.map(x=>'<option>'+esc(x)+'</option>').join('');
}
function render(){
 const z=q.value.toLowerCase(),sf=source.value,mf=pub.value,cf=pclass.value;
 const a=points.filter(x=>(!sf||x.source===sf)&&(!mf||x.mqtt===mf)&&(!cf||x.publishClass===cf)&&(!z||(x.id+' '+x.name+' '+x.group+' '+x.topic+' '+x.field+' '+x.detail).toLowerCase().includes(z)));
 body.innerHTML=a.map(x=>'<tr><td><b>'+esc(x.id)+'</b></td><td>'+esc(x.name)+'</td><td>'+esc(x.group)+'</td><td>'+Number(x.value).toFixed(x.type==='Digital'?0:4)+'</td><td>'+esc(x.units)+'</td><td class="'+qc(x.quality)+'">'+esc(x.quality)+'</td><td class="'+(x.mqtt==='YES'?'ok':'muted')+'">'+esc(x.mqtt)+'</td><td>'+esc(x.publishClass)+'</td><td>'+esc(x.publishMode)+'</td><td>'+sec(x.intervalSec)+'</td><td>'+sec(x.ttlSec)+'</td><td>'+esc(x.topic||'-')+'</td><td>'+esc(x.field||'-')+'</td><td class=muted>'+esc(x.detail)+'</td></tr>').join('');
 stats.textContent=a.length+' shown / '+points.length+' total';
}
async function load(first=false){
 try{
  const x=await fetch('/api/tags?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  points=x.points||[];meta=x.mqtt||{};
  if(first)filters();
  nTags.textContent=points.length;nPub.textContent=points.filter(x=>x.mqtt==='YES').length;nLocal.textContent=points.filter(x=>x.mqtt==='NO').length;
  mqState.textContent=meta.enabled?(meta.connected?'CONNECTED':'OFFLINE'):'DISABLED';
  mqState.className=meta.enabled?(meta.connected?'ok':'bad'):'warn';
  mqInfo.textContent='Base topic: '+(meta.baseTopic||'-')+' | Live '+sec(meta.liveIntervalSec)+' | Totalizer '+sec(meta.totalizerIntervalSec)+' | Status '+sec(meta.statusIntervalSec)+' | Publishes '+(meta.publishCount||0)+' | Failures '+(meta.publishFails||0);
  render();
 }catch(e){stats.textContent='Tag database read failed'}
}
load(true);setInterval(()=>load(false),3000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/nvs-maintenance",HTTP_GET,[](){
    NVSStatsSnapshot st=getNVSStatsSnapshot();
    const bool critical=st.valid && (mqttDailyNvsWriteFailures>0 || (st.total>0 && st.free*100U<st.total*8U));
    const bool low=st.valid && !critical && st.total>0 && st.free*100U<st.total*15U;
    String h;h.reserve(3500);
    h+="<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Riot-Flow NVS Storage</title>";
    h+="<style>body{font-family:Arial;background:#111;color:#eee;margin:18px;max-width:850px}a{color:#63e6be}.card{background:#171717;border:1px solid #444;border-radius:9px;padding:15px;margin:12px 0}.ok{color:#63e6be}.warn{color:#ffd166}.bad{color:#ff6b6b}button{padding:10px 14px;background:#ffd166;color:#111;border:0;border-radius:7px;font-weight:700;cursor:pointer}</style></head><body>";
    h+="<h2>Riot-Flow NVS Storage</h2>";
    h+="<style>\n.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}\n.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}\n.rfDrop{position:relative}.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}\n.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}\n.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}\n@media(max-width:600px){.rfNav{display:grid;grid-template-columns:1fr}.rfNav>a,.rfDrop,.rfDrop>button{width:100%}.rfDrop>button{text-align:left}.rfMenu{position:static;min-width:0;width:100%;margin-top:4px}}\n</style>\n<div class=rfNav>\n<a href=\"/\">Dashboard</a>\n<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>\n<a href=\"/flow-runs\">Flow Runs</a><a href=\"/flow-setup\">Flow Setup</a><a href=\"/aga-setup\">AGA / Gas</a><a href=\"/station-history\">History</a><a href=\"/history-v2\">Alarms / Events</a>\n</div></div>\n<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>\n<a href=\"/io-setup\">I/O Setup</a><a href=\"/pid-control\">PID / Control</a><a href=\"/control-output\">Manual Outputs</a><a href=\"/modbus-monitor\">Modbus Monitor</a>\n</div></div>\n<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>\n<a href=\"/rules\">Rules</a><a href=\"/display-setup\">Display</a><a href=\"/mqtt-setup\">MQTT</a><a href=\"/wifi\">Network / WiFi</a><a href=\"/technician-access\">Technician Access</a><a href=\"/process-setup\">Process Setup</a>\n</div></div>\n<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>\n<a href=\"/system-status\">System Status</a><a href=\"/backup-restore\">Backup / Restore</a><a href=\"/nvs-maintenance\">NVS Storage</a><a href=\"/modbus-debug\">Modbus Debug</a><a href=\"/tags\">Tags</a><a href=\"/api-reference\">API Reference</a>\n</div></div>\n</div>";
    h+="<div class=card><b>NVS Health</b><br>";
    if(st.valid){
      h+="Used entries: "+String(st.used)+"<br>Free entries: "+String(st.free)+"<br>Total entries: "+String(st.total)+"<br>Status: <span class='";
      if(critical)h+="bad'>CRITICAL / COMPACT RECOMMENDED";
      else if(low)h+="warn'>LOW";
      else h+="ok'>OK";
      h+="</span>";
    }else h+="<span class=bad>Statistics unavailable</span>";
    h+="<br>MQTT daily NVS write failures this boot: "+String(mqttDailyNvsWriteFailures)+"</div>";
    h+="<div class=card><b>Compact NVS</b><p>Preserves all readable NVS records in RAM, creates and verifies a SPIFFS recovery backup, erases/reinitializes NVS, restores every preserved record, verifies the restored values, then reboots.</p>";
    h+="<form method='POST' action='/api/nvs-compact' onsubmit=\"return confirm('Compact NVS now? Controller will reboot after successful verification.')\"><button type=submit>Compact NVS</button></form></div>";
    h+="<p class=warn>Do not remove power while compaction is running.</p></body></html>";
    server.sendHeader("Cache-Control","no-store");server.send(200,"text/html",h);
  });

  server.on("/api/nvs-compact",HTTP_POST,[](){
    NVSCompactResult r=compactNVSNow();
    if(!r.ok){
      Serial.println("NVS COMPACT FAIL: "+r.error);
      server.send(500,"text/plain","NVS COMPACT FAIL: "+r.error);
      return;
    }
    mqttDailyNvsRetryAfterMs=0;
    String body="NVS COMPACT PASS\nrecords="+String((unsigned)r.preservedRecords);
    if(r.before.valid&&r.after.valid)body+="\nfree="+String((unsigned)r.before.free)+"->"+String((unsigned)r.after.free);
    body+="\nController rebooting...\n";
    server.send(200,"text/plain",body);
    commonBackupRebootAtMs=millis()+1500UL;
  });

  server.on("/maintenance",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Maintenance</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1100px}a{color:#63e6be}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.card{background:#171717;border:1px solid #444;border-radius:9px;padding:13px}
.card a{display:block;font-size:17px;font-weight:700;margin-bottom:5px}.muted{color:#999;font-size:12px}
@media(max-width:800px){.grid{grid-template-columns:repeat(2,1fr)}}@media(max-width:500px){.grid{grid-template-columns:1fr}}
</style></head><body>
<h2>Riot-Flow Maintenance</h2>
<!-- RIOT FIELD SERVICE LANDING GATE A7A V1 -->
<div class=card style="margin-bottom:12px">
<b>Field Service</b>
<div class=muted style="margin-top:5px">Technician-controlled maintenance over the site LAN/VPN. Approved release files are obtained from the public MultiFlowOTA repository, then applied from this browser to the controller.</div>
<div style="margin-top:10px"><b>Controller firmware:</b> <span id=fw>checking...</span></div>
<div style="margin-top:10px"><a href="/firmware-update#public-release">Open Public MultiFlowOTA</a><div class=muted>Public release source; no GitHub sign-in required.</div></div>
</div>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<!-- RIOT FIELD SERVICE CLEANUP GATE A7A3 V1 -->
<div class=grid>
<div class=card><a href="/system-status">System Status</a><div class=muted>Confirm controller identity, firmware, site, time and runtime health before and after service.</div></div>
<div class=card><a href="/technician-access">Technician Access / QR</a><div class=muted>Print the local Riot-XXXX walk-up access label with customer logo, QR and fallback instructions.</div></div>
<div class=card><a href="/firmware-update">Firmware Update</a><div class=muted>Apply an approved local .bin file or use the validated A6 OTA functions.</div></div>
<div class=card><a href="/backup-restore">Backup / Restore</a><div class=muted>Download the current .mfbak first, then inspect/restore approved configuration files using A5c controls.</div></div>
<div class=card><a href="/firmware-update#public-release">Public MultiFlowOTA</a><div class=muted>Technician download source for approved public firmware and future commissioning templates.</div></div>
<div class=card><a href="/nvs-maintenance">NVS Storage</a><div class=muted>Health, write-failure status and guarded NVS compaction.</div></div>
<div class=card><a href="/modbus-debug">Modbus Debug</a><div class=muted>Low-level protocol, counters, scheduler and raw-frame diagnostics.</div></div>
<div class=card><a href="/tags">Tag Database</a><div class=muted>Complete live Riot-Flow signal inventory for field diagnosis and rule building.</div></div>
<div class=card><a href="/api-reference">API Reference</a><div class=muted>Built-in REST API documentation for commissioning and debugging.</div></div>
</div>
<script>
(async()=>{
  try{
    const r=await fetch('/status?x='+Date.now(),{cache:'no-store'});
    const j=await r.json();
    document.getElementById('fw').textContent=j.firmware||'unknown';
  }catch(_){
    document.getElementById('fw').textContent='unavailable';
  }
})();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api",HTTP_GET,[](){
    server.sendHeader("Location","/api-reference",true);
    server.send(302,"text/plain","");
  });

  server.on("/api-reference",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow API Reference</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1250px}a{color:#63e6be}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:12px;margin:10px 0}.muted{color:#999}
.wrap{overflow:auto;border:1px solid #444;border-radius:8px;margin:8px 0 18px}table{border-collapse:collapse;width:100%;min-width:980px}
th,td{padding:8px;border-bottom:1px solid #333;text-align:left;vertical-align:top}th{background:#202020}
.get{color:#8f8;font-weight:700}.post{color:#ffd166;font-weight:700}.del{color:#ff8b8b;font-weight:700}.write{color:#ffb86b}
code{background:#0b0d0e;border:1px solid #333;border-radius:4px;padding:2px 4px;color:#d7f9f1}
</style></head><body>
<h2>Riot-Flow API Reference</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
@media(max-width:600px){.rfNav{display:grid;grid-template-columns:1fr}.rfNav>a,.rfDrop,.rfDrop>button{width:100%}.rfDrop>button{text-align:left}.rfMenu{position:static;min-width:0;width:100%;margin-top:4px}}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a><a href="/flow-setup">Flow Setup</a><a href="/aga-setup">AGA / Gas</a><a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a><a href="/pid-control">PID / Control</a><a href="/control-output">Manual Outputs</a><a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a><a href="/display-setup">Display</a><a href="/mqtt-setup">MQTT</a><a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a><a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a><a href="/backup-restore">Backup / Restore</a><a href="/nvs-maintenance">NVS Storage</a><a href="/modbus-debug">Modbus Debug</a><a href="/tags">Tags</a><a href="/api-reference">API Reference</a>
</div></div>
</div>
<div class=card><b>REST API / Debugging Reference</b><div class=muted>GET calls are normally read-only. POST and DELETE calls may change controller state or persistent configuration. Examples use PowerShell Invoke-RestMethod.</div></div>

<h3>System & Diagnostics</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/ping</code></td><td>Basic controller reachability test</td><td>Returns OK</td></tr>
<tr><td class=get>GET</td><td><code>/api/state</code></td><td>Main live controller state</td><td>Dashboard/status diagnostics</td></tr>
<tr><td class=get>GET</td><td><code>/api/test</code></td><td>Basic API test response</td><td>Diagnostic</td></tr>
<tr><td class=get>GET</td><td><code>/api/power</code></td><td>Power/battery telemetry</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/power-test</code></td><td>Power-loss test control</td><td class=write>Changes test state</td></tr>
<tr><td class=get>GET</td><td><code>/api/device-backup-info</code></td><td>Backup capability/info</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/device-backup</code></td><td>Generate/read device backup</td><td>Maintenance use</td></tr>
</table></div>

<h3>Display & Site Identity</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/api/site-identity</code></td><td>Read site name/LSD</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/site-identity</code></td><td>Save site name/LSD</td><td class=write>Persistent configuration</td></tr>
<tr><td class=get>GET</td><td><code>/api/display-config</code></td><td>Read display settings</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/display-config</code></td><td>Save display settings</td><td class=write>Persistent configuration</td></tr>
<tr><td class=post>POST</td><td><code>/api/epaper-refresh</code></td><td>Request e-paper refresh</td><td class=write>Display action only</td></tr>
<tr><td class=get>GET</td><td><code>/api/display-debug</code></td><td>Display diagnostic state</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/display-logo</code></td><td>Logo metadata/status</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/display-logo-bin</code></td><td>Stored logo binary</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/display-logo</code></td><td>Logo upload/control</td><td class=write>Persistent display data</td></tr>
<tr><td class=post>POST</td><td><code>/api/display-logo-data</code></td><td>Logo data upload</td><td class=write>Persistent display data</td></tr>
<tr><td class=del>DELETE</td><td><code>/api/display-logo</code></td><td>Delete stored logo</td><td class=write>Destructive</td></tr>
</table></div>

<h3>MQTT, Flow & I/O</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/api/mqtt-config</code></td><td>Read MQTT configuration</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/mqtt-config</code></td><td>Save MQTT configuration</td><td class=write>Persistent configuration</td></tr>
<tr><td class=get>GET</td><td><code>/api/mqtt-traffic</code></td><td>MQTT traffic counters</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/flow-runs</code></td><td>Read flow-run configuration/live data</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/flow-run-save</code></td><td>Save one flow run</td><td class=write><code>run</code> plus flow fields</td></tr>
<tr><td class=get>GET</td><td><code>/api/run1-bindings</code></td><td>Read Run 1 input bindings</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/run1-bindings</code></td><td>Save Run 1 input bindings</td><td class=write>Persistent configuration</td></tr>
<tr><td class=get>GET</td><td><code>/api/io-config</code></td><td>Read I/O configuration</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/io-save</code></td><td>Save I/O scaling/configuration</td><td class=write>Persistent configuration</td></tr>
<tr><td class=post>POST</td><td><code>/api/io-ao-force</code></td><td>Force analog output for commissioning</td><td class=write>Physical output change</td></tr>
<tr><td class=get>GET</td><td><code>/api/process-config</code></td><td>Process configuration/status</td><td>Read-only</td></tr>
</table></div>

<h3>AGA / Gas</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/api/aga-config</code></td><td>Read AGA meter configuration</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/aga-save</code></td><td>Save AGA configuration</td><td class=write>Persistent configuration</td></tr>
<tr><td class=get>GET</td><td><code>/api/aga-gas</code></td><td>Read gas analysis for a run</td><td><code>run</code></td></tr>
<tr><td class=post>POST</td><td><code>/api/aga-gas-save</code></td><td>Save gas composition</td><td class=write><code>run</code>, component values</td></tr>
<tr><td class=post>POST</td><td><code>/api/aga-gas-copy</code></td><td>Copy gas analysis between runs</td><td class=write>Persistent configuration</td></tr>
<tr><td class=get>GET</td><td><code>/api/gas-library</code></td><td>Read saved gas library</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/gas-library-save</code></td><td>Save gas library entry</td><td class=write>Persistent configuration</td></tr>
<tr><td class=post>POST</td><td><code>/api/gas-library-load</code></td><td>Load gas library entry</td><td class=write>Changes active analysis</td></tr>
<tr><td class=post>POST</td><td><code>/api/gas-library-standard</code></td><td>Save/use standard gas entry</td><td class=write>Configuration action</td></tr>
<tr><td class=post>POST</td><td><code>/api/gas-standard-load</code></td><td>Load standard gas composition</td><td class=write>Changes active analysis</td></tr>
<tr><td class=post>POST</td><td><code>/api/golden-reference</code></td><td>Golden-reference calculation/test</td><td>Commissioning/test</td></tr>
</table></div>

<h3>PID, Modbus & Outputs</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/api/pid-control</code></td><td>Read PID configuration/live state</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/pid-control</code></td><td>Save/control PID loop</td><td class=write>Can alter control behavior</td></tr>
<tr><td class=post>POST</td><td><code>/api/modbus-comms</code></td><td>Enable/disable Modbus communications</td><td class=write>Changes communications state</td></tr>
<tr><td class=get>GET</td><td><code>/api/modbus-diag</code></td><td>Modbus diagnostics/counters</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/modbus-monitor</code></td><td>Live Modbus monitor data</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/modbus-monitor-clear</code></td><td>Clear Modbus monitor buffer/counters</td><td class=write>Diagnostic state</td></tr>
<tr><td class=post>POST</td><td><code>/api/do-test</code></td><td>Digital output test</td><td class=write>Physical output change</td></tr>
<tr><td class=get>GET</td><td><code>/api/control-output</code></td><td>Read control output state</td><td>Read-only</td></tr>
<tr><td class=post>POST</td><td><code>/api/control-output</code></td><td>Control/test output</td><td class=write>Physical output/control change</td></tr>
<tr><td class=get>GET</td><td><code>/api/tags</code></td><td>Complete live tag database</td><td>Read-only</td></tr>
</table></div>

<h3>Rules & History</h3><div class=wrap><table><tr><th>Method</th><th>Endpoint</th><th>Purpose</th><th>Parameters / Notes</th></tr>
<tr><td class=get>GET</td><td><code>/api/rules</code></td><td>Read rule configuration/runtime state</td><td>Read-only</td></tr>
<tr><td class=get>GET</td><td><code>/api/rules-persist</code></td><td>Rule persistence diagnostic</td><td>Shows effective SPIFFS enabled mask</td></tr>
<tr><td class=post>POST</td><td><code>/api/rules-master</code></td><td>Enable/disable rules engine</td><td class=write><code>enabled=0|1</code></td></tr>
<tr><td class=post>POST</td><td><code>/api/rule-enabled</code></td><td>Enable/disable one rule</td><td class=write><code>index=0..7</code>, <code>enabled=0|1</code></td></tr>
<tr><td class=post>POST</td><td><code>/api/rule-save</code></td><td>Save full rule configuration</td><td class=write><code>index</code> plus rule fields</td></tr>
<tr><td class=post>POST</td><td><code>/api/rule-ack</code></td><td>Acknowledge one active local alarm</td><td class=write><code>index</code>; does not clear alarm condition</td></tr>\n<tr><td class=post>POST</td><td><code>/api/rules-ack-all</code></td><td>Acknowledge all active unacknowledged local alarms</td><td class=write>Writes one ACK ALL history record</td></tr>\n<tr><td class=post>POST</td><td><code>/api/rule-reset</code></td><td>Reset/clear one rule runtime state</td><td class=write><code>index</code>; generates CLEAR history when applicable</td></tr>
<tr><td class=get>GET</td><td><code>/api/flow-history-day</code></td><td>Read persistent daily flow rollover history</td><td><code>limit=1..10</code>, <code>offset</code>, optional <code>run=1..6</code></td></tr>
<tr><td class=get>GET</td><td><code>/api/flow-history</code></td><td>Read persistent hourly flow history</td><td><code>limit=1..10</code>, <code>offset</code>, optional <code>run=1..6</code></td></tr>
<tr><td class=get>GET</td><td><code>/api/history-v2</code></td><td>Read persistent alarm/event history</td><td><code>limit=1..10</code>, <code>offset</code>, optional <code>type=0..4</code></td></tr>
<tr><td class=post>POST</td><td><code>/api/history-v2-test</code></td><td>Append one TEST history record</td><td class=write>Diagnostic only</td></tr>
<tr><td class=post>POST</td><td><code>/api/history-v2-clear</code></td><td>Protected audit log erase endpoint</td><td>403 - Factory Reset / Recommission only</td></tr>
</table></div>

<h3>PowerShell Examples</h3>
<div class=card>
<div><code>Invoke-RestMethod http://192.168.1.134/api/state</code></div><br>
<div><code>Invoke-RestMethod "http://192.168.1.134/api/history-v2?limit=10&amp;offset=0"</code></div><br>
<div><code>Invoke-RestMethod -Method POST -Uri http://192.168.1.134/api/rule-enabled -ContentType "application/x-www-form-urlencoded" -Body "index=0&amp;enabled=1"</code></div>
</div>
</body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/rule-config-storage",HTTP_GET,[](){
    static RiotRuleFileV2 f;
    memset(&f,0,sizeof(f));
    bool nowOK=readRiotRuleFlashFileV2(RIOT_RULE_FILE_PATH,f);
    uint8_t enabledCount=0;
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++)if(riotRules[i].enabled)enabledCount++;
    String j="{\"loaded\":"+String(riotRuleFlashLoaded?"true":"false");
    j+=",\"bootLoadOK\":"+String(riotRuleFlashBootLoadOK?"true":"false");
    j+=",\"lastSaveOK\":"+String(riotRuleFlashLastSaveOK?"true":"false");
    j+=",\"saveCount\":"+String(riotRuleFlashSaveCount);
    j+=",\"status\":\""+riotRuleFlashStatus+"\"";
    j+=",\"path\":\""+String(RIOT_RULE_FILE_PATH)+"\"";
    j+=",\"bytes\":"+String((unsigned)sizeof(RiotRuleFileV2));
    j+=",\"version\":2";
    j+=",\"fileNowOK\":"+String(nowOK?"true":"false");
    j+=",\"ruleCount\":"+String((unsigned)RIOT_RULE_COUNT);
    j+=",\"enabledCount\":"+String(enabledCount);
    j+=",\"masterEnabled\":"+String(riotRulesMasterEnabled?"true":"false");
    j+=",\"elseSchemaReady\":true,\"dualActionSchemaReady\":true}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/rules-persist",HTTP_GET,[](){
    uint8_t fileMask=0;
    bool haveFileMask=loadRiotRuleEnableMask(fileMask);
    String j="{\"master\":"+String(riotRulesMasterEnabled?"true":"false");
    j+=",\"haveFileMask\":"+String(haveFileMask?"true":"false")+",\"fileMask\":"+String(fileMask)+",\"rules\":[";
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      if(i)j+=",";
      bool enabled=haveFileMask?((fileMask&(1U<<i))!=0):riotRules[i].enabled;
      j+="{\"index\":"+String(i)+",\"enabled\":"+String(enabled?"true":"false")+"}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/rules",HTTP_GET,[](){
    String j="{\"masterEnabled\":"+String(riotRulesMasterEnabled?"true":"false");
    j+=",\"doWriteGood\":"+String(riotRuleDOWriteGood)+",\"doWriteBad\":"+String(riotRuleDOWriteBad)+",\"rules\":[";
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      if(i)j+=",";
      RiotRuleConfig &r=riotRules[i];RiotRuleRuntime &rt=riotRuleRt[i];
      String nm=r.name,msg=r.message,st=rt.status;nm.replace("\"","\\\"");msg.replace("\"","\\\"");st.replace("\"","\\\"");
      j+="{\"index\":"+String(i)+",\"enabled\":"+String(r.enabled?"true":"false")+",\"name\":\""+nm+"\"";
      j+=",\"tagIndex\":"+String(r.tagIndex)+",\"compare\":"+String(r.compare)+",\"threshold\":"+String(r.threshold,6);
      j+=",\"delayMs\":"+String(r.delayMs)+",\"latch\":"+String(r.latch?"true":"false")+",\"action\":"+String(r.action)+",\"target\":"+String(r.target);
      j+=",\"action2\":"+String(r.action2)+",\"target2\":"+String(r.target2);
      j+=",\"elseAction\":"+String(r.elseAction)+",\"elseTarget\":"+String(r.elseTarget);
      j+=",\"elseAction2\":"+String(r.elseAction2)+",\"elseTarget2\":"+String(r.elseTarget2);
      j+=",\"message\":\""+msg+"\",\"condition\":"+String(rt.condition?"true":"false")+",\"timing\":"+String(rt.timing?"true":"false");
      j+=",\"active\":"+String(rt.active?"true":"false")+",\"latched\":"+String(rt.latched?"true":"false")+",\"alarm\":"+String(rt.alarm?"true":"false")+",\"acknowledged\":"+String(rt.acknowledged?"true":"false");
      float liveValue=0.0f;String liveQuality;riotRuleTagValue(r.tagIndex,liveValue,liveQuality);
      String liveUnits=riotRuleTagUnits(r.tagIndex);
      liveQuality.replace("\"","\\\"");liveUnits.replace("\"","\\\"");
      String firedAt=rt.firedAt;firedAt.replace("\"","\\\"");
      j+=",\"fireCount\":"+String(rt.fireCount)+",\"status\":\""+st+"\"";
      j+=",\"value\":"+String(isfinite(liveValue)?liveValue:0.0f,6)+",\"quality\":\""+liveQuality+"\",\"units\":\""+liveUnits+"\"";
      j+=",\"fireValue\":"+String(isfinite(rt.fireValue)?rt.fireValue:0.0f,6)+",\"firedAt\":\""+firedAt+"\"}";
    }
    j+="]}";server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
  });
  server.on("/api/rules-master",HTTP_POST,[](){
    riotRulesMasterEnabled=server.hasArg("enabled") && server.arg("enabled")=="1";saveRiotRulesMaster();
    if(!riotRulesMasterEnabled)for(uint8_t i=0;i<RIOT_RULE_COUNT;i++)if(!riotRuleRt[i].latched)resetRiotRuleRuntime(i);
    server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/api/rule-enabled",HTTP_POST,[](){
    int i=server.hasArg("index")?server.arg("index").toInt():-1;
    if(i<0||i>=RIOT_RULE_COUNT||!server.hasArg("enabled")){
      server.send(400,"application/json","{\"ok\":false}");
      return;
    }
    riotRules[i].enabled=server.arg("enabled")=="1";
    saveRiotRule((uint8_t)i);
    server.send(200,"application/json","{\"ok\":true,\"index\":"+String(i)+",\"enabled\":"+String(riotRules[i].enabled?"true":"false")+"}");
  });

  server.on("/api/rule-ack",HTTP_POST,[](){
    int i=server.hasArg("index")?server.arg("index").toInt():-1;
    if(i<0||i>=RIOT_RULE_COUNT){server.send(400,"application/json","{\"ok\":false}");return;}
    RiotRuleRuntime &rt=riotRuleRt[i];
    RiotRuleConfig &r=riotRules[i];
    if(!(rt.alarm||rt.latched)){
      server.send(409,"application/json","{\"ok\":false,\"error\":\"not active alarm\"}");
      return;
    }
    if(!rt.acknowledged){
      rt.acknowledged=true;
      RiotHistoryStore::Entry rec={};
      time_t now=time(nullptr);
      rec.epoch=(now>1000000000)?(uint32_t)now:0U;
      rec.type=RiotHistoryStore::ENTRY_ACK;
      rec.sourceIndex=(uint8_t)i;
      rec.value=rt.fireValue;
      rec.threshold=r.threshold;
      snprintf(rec.label,sizeof(rec.label),"%s",r.name);
      snprintf(rec.message,sizeof(rec.message),"Local ACK");
      RiotHistoryStore::append(SPIFFS,rec);
    }
    server.send(200,"application/json","{\"ok\":true,\"index\":"+String(i)+",\"acknowledged\":true}");
  });

  server.on("/api/rules-ack-all",HTTP_POST,[](){
    uint8_t acked=0;
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      RiotRuleRuntime &rt=riotRuleRt[i];
      if((rt.alarm||rt.latched) && !rt.acknowledged){
        rt.acknowledged=true;
        acked++;
      }
    }
    if(acked){
      RiotHistoryStore::Entry rec={};
      time_t now=time(nullptr);
      rec.epoch=(now>1000000000)?(uint32_t)now:0U;
      rec.type=RiotHistoryStore::ENTRY_ACK;
      rec.sourceIndex=255;
      rec.value=(float)acked;
      rec.threshold=0.0f;
      snprintf(rec.label,sizeof(rec.label),"ACK ALL");
      snprintf(rec.message,sizeof(rec.message),"%u alarms acknowledged locally",(unsigned)acked);
      RiotHistoryStore::append(SPIFFS,rec);
    }
    server.send(200,"application/json","{\"ok\":true,\"acknowledged\":"+String(acked)+"}");
  });

  server.on("/api/rule-reset",HTTP_POST,[](){
    int i=server.hasArg("index")?server.arg("index").toInt():-1;if(i<0||i>=RIOT_RULE_COUNT){server.send(400,"application/json","{\"ok\":false}");return;}

    RiotRuleConfig &r=riotRules[i];
    RiotRuleRuntime &rt=riotRuleRt[i];
    const bool hadState=rt.alarm||rt.latched||rt.active||rt.firedThisCondition;
    const float clearedValue=rt.fireValue;

    if(hadState){
      RiotHistoryStore::Entry rec={};
      rec.sequence=(uint32_t)RiotHistoryStore::count(SPIFFS)+1U;
      time_t now=time(nullptr);
      rec.epoch=(now>1000000000)?(uint32_t)now:0U;
      rec.type=RiotHistoryStore::ENTRY_CLEAR;
      rec.sourceIndex=(uint8_t)i;
      rec.value=clearedValue;
      rec.threshold=r.threshold;
      snprintf(rec.label,sizeof(rec.label),"%s",r.name);
      snprintf(rec.message,sizeof(rec.message),"Rule reset / cleared");
      if(!RiotHistoryStore::append(SPIFFS,rec)){
        Serial.println("RULE CLEAR history append failed");
      }
    }

    resetRiotRuleRuntime((uint8_t)i);server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/api/rule-save",HTTP_POST,[](){
    int i=server.hasArg("index")?server.arg("index").toInt():-1;if(i<0||i>=RIOT_RULE_COUNT){server.send(400,"application/json","{\"ok\":false,\"error\":\"bad index\"}");return;}
    RiotRuleConfig &r=riotRules[i];
    if(server.hasArg("enabled"))r.enabled=server.arg("enabled")=="1";
    if(server.hasArg("name")){String v=server.arg("name");v.trim();if(!v.length())v="Rule "+String(i+1);v.toCharArray(r.name,sizeof(r.name));}
    if(server.hasArg("tagIndex"))r.tagIndex=(uint8_t)constrain(server.arg("tagIndex").toInt(),0,46);
    if(server.hasArg("compare"))r.compare=(uint8_t)constrain(server.arg("compare").toInt(),0,5);
    if(server.hasArg("threshold"))r.threshold=server.arg("threshold").toFloat();
    if(server.hasArg("delayMs"))r.delayMs=(uint32_t)constrain(server.arg("delayMs").toInt(),0,86400000);
    if(server.hasArg("latch"))r.latch=server.arg("latch")=="1";
    if(server.hasArg("action")){int a=server.arg("action").toInt();r.action=(a==255)?RIOT_ACT_NONE:(uint8_t)constrain(a,0,4);}
    if(server.hasArg("target"))r.target=(uint8_t)constrain(server.arg("target").toInt(),1,6);
    if(server.hasArg("action2")){int a=server.arg("action2").toInt();r.action2=(a==255)?RIOT_ACT_NONE:(uint8_t)constrain(a,0,4);}
    if(server.hasArg("target2"))r.target2=(uint8_t)constrain(server.arg("target2").toInt(),1,6);
    if(server.hasArg("elseAction")){
      int a=server.arg("elseAction").toInt();
      if(a==0){server.send(400,"application/json","{\"ok\":false,\"error\":\"OTHERWISE cannot raise an alarm\"}");return;}
      r.elseAction=(a==255)?RIOT_ACT_NONE:(uint8_t)constrain(a,1,4);
    }
    if(server.hasArg("elseTarget"))r.elseTarget=(uint8_t)constrain(server.arg("elseTarget").toInt(),1,6);
    if(server.hasArg("elseAction2")){
      int a=server.arg("elseAction2").toInt();
      if(a==0){server.send(400,"application/json","{\"ok\":false,\"error\":\"OTHERWISE second action cannot raise an alarm\"}");return;}
      r.elseAction2=(a==255)?RIOT_ACT_NONE:(uint8_t)constrain(a,1,4);
    }
    if(server.hasArg("elseTarget2"))r.elseTarget2=(uint8_t)constrain(server.arg("elseTarget2").toInt(),1,6);
    if(server.hasArg("message")){String v=server.arg("message");v.toCharArray(r.message,sizeof(r.message));}
    auto ctrlKey=[](uint8_t a,uint8_t t)->int{
      if(a==RIOT_ACT_DO_OFF||a==RIOT_ACT_DO_ON)return 100+(int)t;
      if(a==RIOT_ACT_PID_MANUAL)return 200+(int)t;
      return 0;
    };
    int k1=ctrlKey(r.action,r.target),k2=ctrlKey(r.action2,r.target2);
    int e1=ctrlKey(r.elseAction,r.elseTarget),e2=ctrlKey(r.elseAction2,r.elseTarget2);
    if(k1 && k1==k2){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"DO side cannot command the same DO/PID target twice\"}");
      return;
    }
    if(e1 && e1==e2){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"OTHERWISE side cannot command the same DO/PID target twice\"}");
      return;
    }

    resetRiotRuleRuntime((uint8_t)i);saveRiotRule((uint8_t)i);server.send(200,"application/json","{\"ok\":true}");
  });

  // EP32 RULE ENGINE GATE5G3 ONE-LINE UI V1
  // EP32 RULE ENGINE GATE5G4 OPERATOR PROOFING V1
  server.on("/api/rules-disable-all",HTTP_POST,[](){
    for(uint8_t i=0;i<RIOT_RULE_COUNT;i++){
      riotRules[i].enabled=false;
      resetRiotRuleRuntime(i);
    }
    riotRulesMasterEnabled=false;
    bool ok=saveRiotRulesFlash();
    server.send(ok?200:507,"application/json",ok?
      "{\"ok\":true,\"masterEnabled\":false,\"enabledCount\":0}":
      "{\"ok\":false,\"error\":\"rule flash save failed\"}");
  });

  server.on("/api/rule-duplicate",HTTP_POST,[](){
    int src=server.hasArg("source")?server.arg("source").toInt():-1;
    int dst=server.hasArg("dest")?server.arg("dest").toInt():-1;
    if(src<0||src>=RIOT_RULE_COUNT||dst<0||dst>=RIOT_RULE_COUNT||src==dst){
      server.send(400,"application/json","{\"ok\":false,\"error\":\"invalid source/destination\"}");
      return;
    }
    RiotRuleConfig copy=riotRules[src];
    copy.enabled=false; // safe default: duplicated rule never becomes active automatically
    String nm=String(copy.name)+" Copy";
    nm.toCharArray(copy.name,sizeof(copy.name));
    riotRules[dst]=copy;
    resetRiotRuleRuntime((uint8_t)dst);
    bool ok=saveRiotRulesFlash();
    if(!ok){
      server.send(507,"application/json","{\"ok\":false,\"error\":\"rule flash save failed\"}");
      return;
    }
    server.send(200,"application/json","{\"ok\":true,\"enabled\":false}");
  });

  server.on("/rules",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Rules</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:14px;max-width:1680px}a{color:#63e6be}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:9px 10px;margin:7px 0}
.head{display:flex;justify-content:space-between;gap:8px;align-items:center;flex-wrap:wrap}
.master{padding:10px}.master input{width:auto;transform:scale(1.25)}
.good{color:#8f8}.warn{color:#ffd166}.bad{color:#ff8b8b}.muted{color:#999}
.logic{display:flex;align-items:end;gap:4px;flex-wrap:nowrap;margin-top:7px;overflow-x:auto;padding-bottom:2px}
.word{font-weight:800;color:#63e6be;white-space:nowrap;padding:0 1px 9px}.and{color:#ffd166}
.f{min-width:68px}.tag{min-width:170px}.cmp{min-width:105px}.num{width:82px;min-width:82px}.act{min-width:118px}.target{width:74px;min-width:74px}
label{display:block;color:#aaa;font-size:10px;margin-bottom:3px}
input,select,button{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:5px;font-size:13px}
input,select{width:100%}button{width:auto;cursor:pointer;white-space:nowrap}
.details{display:flex;gap:6px;align-items:end;flex-wrap:wrap;margin-top:6px}.details .name{min-width:180px;flex:1}.details .msg{min-width:220px;flex:2}.details .small{width:105px}
.preview{margin-top:6px;padding:7px 9px;background:#0d1213;border-left:4px solid #63e6be;font-size:13px}
.current{white-space:nowrap}.proof{margin-top:6px;display:flex;gap:8px;flex-wrap:wrap;align-items:center}.badge{border:1px solid #555;border-radius:16px;padding:4px 8px;font-size:12px}.danger{color:#ff8b8b;border-color:#8a4040}.safe{color:#8f8;border-color:#385c38}.conflict{margin-top:6px;padding:7px 9px;border:1px solid #8a6030;border-radius:6px;color:#ffd166;background:#231b0f}.toolbar{display:flex;gap:7px;flex-wrap:wrap;margin-top:7px}
@media(max-width:1180px){.logic{flex-wrap:wrap;overflow:visible}.word{padding-bottom:8px}}
@media(max-width:560px){.logic{display:grid;grid-template-columns:auto 1fr;align-items:end}.word{padding-bottom:8px}.f,.tag,.cmp,.num,.act,.target{width:100%;min-width:0}.details>*{width:100%!important;min-width:100%!important}}
</style></head><body>
<h2>Riot-Flow Rules</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div class="card master"><div class=head><div><input id=master type=checkbox onchange=setMaster()> <b>Rules Enabled</b> <span id=masterState class=warn>OFF</span></div><div>DO good <b id=dg>0</b> / bad <b id=db>0</b></div></div>
<div class=muted style="margin-top:5px">Build each rule as one plain-English line: WHEN ... FOR ... DO ... AND ... OTHERWISE ... AND ...</div><div class=toolbar><button onclick=disableAll()>Disable All Rules</button><span class=muted>Turns master OFF and disables every rule.</span></div></div>
<div id=alarms class=card style="display:none"></div>
<div id=rules>Loading...</div><p id=msg></p>
<script>
let tags=[],cfg=[];
const cmpText=['is greater than','is less than','is at least','is at most','is equal to','is not equal to'];
const acts=[[0,'Raise Alarm'],[1,'Log Event'],[2,'Turn OFF'],[3,'Turn ON'],[4,'Set Manual'],[255,'Do nothing']];
const elseActs=[[1,'Log Event'],[2,'Turn OFF'],[3,'Turn ON'],[4,'Set Manual'],[255,'Do nothing']];
const e=id=>document.getElementById(id),esc=s=>String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
function tagOpts(sel){return tags.map((t,i)=>'<option value="'+i+'" '+(i==sel?'selected':'')+'>'+esc(t.name||t.id)+'</option>').join('')}
function actionOpts(sel,list=acts){return list.map(a=>'<option value="'+a[0]+'" '+(a[0]==sel?'selected':'')+'>'+a[1]+'</option>').join('')}
function targetOpts(action,sel){action=+action;let n=action==2||action==3?4:(action==4?6:0),prefix=action==2||action==3?'DO':'PID';if(!n)return '<option value=1>--</option>';let h='';for(let x=1;x<=n;x++)h+='<option value="'+x+'" '+(x==sel?'selected':'')+'>'+prefix+x+'</option>';return h}
function isDigitalTag(i){const t=tags[+e('tg'+i).value]||{};return t.type==='Digital'}
function statusClass(r){return r.latched||r.alarm?'bad':(r.timing?'warn':(r.active?'good':'muted'))}
function valText(r){const u=r.units?(' '+r.units):'';return Number(r.value).toFixed(3)+u}
function renderAlarms(rr){const a=(rr||[]).filter(r=>r.alarm||r.latched);if(!a.length){alarms.style.display='none';alarms.innerHTML='';return}alarms.style.display='block';alarms.innerHTML='<b>ACTIVE ALARMS</b>'+a.map(r=>'<div class=bad style="margin-top:6px"><b>'+esc(r.message||r.name)+'</b> - '+esc(valText(r))+' <span class=muted>(Rule '+(r.index+1)+')</span></div>').join('')}
function card(r){const digital=(tags[r.tagIndex]||{}).type==='Digital';return '<div class=card>'+
'<div class=head><div><b>Rule '+(r.index+1)+' - '+esc(r.name)+'</b> &nbsp;<span id=st'+r.index+' class="'+statusClass(r)+'">'+esc(r.status)+'</span></div><div class=current id=cv'+r.index+'>Current '+esc(valText(r))+' &nbsp;|&nbsp; Fires <b>'+r.fireCount+'</b></div></div>'+
'<div class=logic><span class=word>WHEN</span><div class="f tag"><label>Signal</label><select id=tg'+r.index+'>'+tagOpts(r.tagIndex)+'</select></div>'+
'<div class="f cmp" id=analogCmp'+r.index+' style="'+(digital?'display:none':'')+'"><label>Condition</label><select id=cp'+r.index+'>'+cmpText.map((x,j)=>'<option value='+j+' '+(j==r.compare?'selected':'')+'>'+x+'</option>').join('')+'</select></div>'+
'<div class="f num" id=analogVal'+r.index+' style="'+(digital?'display:none':'')+'"><label>Value</label><input id=th'+r.index+' type=number step=.001 value="'+r.threshold+'"></div>'+
'<div class="f cmp" id=digitalVal'+r.index+' style="'+(!digital?'display:none':'')+'"><label>State</label><select id=ds'+r.index+'><option value=1 '+(r.threshold>=0.5?'selected':'')+'>is ON</option><option value=0 '+(r.threshold<0.5?'selected':'')+'>is OFF</option></select></div>'+
'<span class=word>FOR</span><div class="f num"><label>Seconds</label><input id=dl'+r.index+' type=number min=0 step=.1 value="'+(r.delayMs/1000)+'"></div>'+
'<span class=word>DO</span><div class="f act"><label>Action 1</label><select id=ac'+r.index+'>'+actionOpts(r.action)+'</select></div><div class="f target"><label>Target</label><select id=tr'+r.index+'>'+targetOpts(r.action,r.target)+'</select></div>'+
'<span class="word and">AND</span><div class="f act"><label>Action 2</label><select id=a2'+r.index+'>'+actionOpts(r.action2)+'</select></div><div class="f target"><label>Target</label><select id=t2'+r.index+'>'+targetOpts(r.action2,r.target2)+'</select></div>'+
'<span class=word>OTHERWISE</span><div class="f act"><label>Action 1</label><select id=ea'+r.index+'>'+actionOpts(r.elseAction,elseActs)+'</select></div><div class="f target"><label>Target</label><select id=et'+r.index+'>'+targetOpts(r.elseAction,r.elseTarget)+'</select></div>'+
'<span class="word and">AND</span><div class="f act"><label>Action 2</label><select id=e2'+r.index+'>'+actionOpts(r.elseAction2,elseActs)+'</select></div><div class="f target"><label>Target</label><select id=u2'+r.index+'>'+targetOpts(r.elseAction2,r.elseTarget2)+'</select></div>'+
'<button onclick=saveRule('+r.index+')>Save</button></div>'+
'<div class=details><div class=name><label>Rule Name</label><input id=nm'+r.index+' maxlength=24 value="'+esc(r.name)+'"></div><div class=msg><label>Alarm / Event Message</label><input id=ms'+r.index+' maxlength=48 value="'+esc(r.message)+'"></div><div class=small><label>Enabled</label><select id=en'+r.index+' onchange="setRuleEnabled('+r.index+')"><option value=0 '+(!r.enabled?'selected':'')+'>NO</option><option value=1 '+(r.enabled?'selected':'')+'>YES</option></select></div><div class=small><label>Latch Alarm</label><select id=la'+r.index+'><option value=0 '+(!r.latch?'selected':'')+'>NO</option><option value=1 '+(r.latch?'selected':'')+'>YES</option></select></div><button onclick=resetRule('+r.index+')>Reset</button><button onclick=duplicateRule('+r.index+')>Duplicate</button></div>'+
'<div class=preview id=pv'+r.index+'></div><div class=proof id=proof'+r.index+'></div><div class=conflict id=conflict'+r.index+' style="display:none"></div></div>'}
function actionPhrase(a,target){a=+a;target=+target;if(a==2)return 'turn DO'+target+' OFF';if(a==3)return 'turn DO'+target+' ON';if(a==4)return 'set PID'+target+' to Manual';if(a==1)return 'log an event';if(a==0)return 'raise an alarm';return 'do nothing'}
function controlledTarget(a,t){a=+a;t=+t;if(a==2||a==3)return 'DO'+t;if(a==4)return 'PID'+t;return ''}
function sideTargets(i,otherwise){return otherwise?[controlledTarget(e('ea'+i).value,e('et'+i).value),controlledTarget(e('e2'+i).value,e('u2'+i).value)].filter(Boolean):[controlledTarget(e('ac'+i).value,e('tr'+i).value),controlledTarget(e('a2'+i).value,e('t2'+i).value)].filter(Boolean)}
function localSideError(i,otherwise){const x=sideTargets(i,otherwise);return x.length==2&&x[0]==x[1]?(otherwise?'OTHERWISE':'DO')+' side cannot command '+x[0]+' twice':''}
function conflictText(i){const mine=[...sideTargets(i,false),...sideTargets(i,true)];if(!mine.length)return '';const hits=[];cfg.forEach(r=>{if(r.index==i||!r.enabled)return;[[r.action,r.target],[r.action2,r.target2],[r.elseAction,r.elseTarget],[r.elseAction2,r.elseTarget2]].forEach(z=>{const x=controlledTarget(z[0],z[1]);if(x&&mine.includes(x))hits.push('Rule '+(r.index+1)+' also controls '+x)})});return [...new Set(hits)].join(' | ')}
function updateProof(i){const r=cfg.find(x=>x.index==i)||{},ctl=[...new Set([...sideTargets(i,false),...sideTargets(i,true)])],p=e('proof'+i),c=e('conflict'+i);if(p){let h='<span class="badge '+(r.quality==='GOOD'||r.quality==='MANUAL'?'safe':'danger')+'">SOURCE '+esc(r.quality||'--')+'</span>';h+='<span class="badge '+(r.condition?'safe':'')+'">CONDITION '+(r.condition?'TRUE':'FALSE')+'</span>';h+='<span class=badge>STATUS '+esc(r.status||'--')+'</span>';if(ctl.length)h+='<span class="badge danger">CONTROLS '+esc(ctl.join(', '))+'</span>';p.innerHTML=h}let x=localSideError(i,false)||localSideError(i,true)||conflictText(i);if(c){c.style.display=x?'block':'none';c.textContent=x?('WARNING: '+x):''}}
function syncRule(i){const digital=isDigitalTag(i);e('analogCmp'+i).style.display=digital?'none':'';e('analogVal'+i).style.display=digital?'none':'';e('digitalVal'+i).style.display=digital?'':'none';[['ac','tr'],['a2','t2'],['ea','et'],['e2','u2']].forEach(z=>{const a=+e(z[0]+i).value,old=+e(z[1]+i).value||1;e(z[1]+i).innerHTML=targetOpts(a,old)});preview(i);updateProof(i)}
function preview(i){const tag=tags[+e('tg'+i).value]||{},digital=isDigitalTag(i),sec=+e('dl'+i).value||0;let cond=digital?(+e('ds'+i).value?'is ON':'is OFF'):(cmpText[+e('cp'+i).value]+' '+e('th'+i).value+(tag.units?(' '+tag.units):''));let s='<b>WHEN</b> '+esc(tag.name||tag.id)+' '+esc(cond);if(sec)s+=' <b>FOR</b> '+sec+' sec';s+=' <b>DO</b> '+esc(actionPhrase(e('ac'+i).value,e('tr'+i).value));if(+e('a2'+i).value!=255)s+=' <b>AND</b> '+esc(actionPhrase(e('a2'+i).value,e('t2'+i).value));s+=' <b>OTHERWISE</b> '+esc(actionPhrase(e('ea'+i).value,e('et'+i).value));if(+e('e2'+i).value!=255)s+=' <b>AND</b> '+esc(actionPhrase(e('e2'+i).value,e('u2'+i).value));if(e('la'+i).value=='1')s+=' &nbsp; <b>LATCH UNTIL RESET</b>';e('pv'+i).innerHTML=s}
function wire(i){['tg','cp','th','ds','dl','ac','tr','a2','t2','ea','et','e2','u2','la'].forEach(p=>{const x=e(p+i);if(x)x.addEventListener('change',()=>syncRule(i))});syncRule(i)}
async function load(){const [t,r]=await Promise.all([fetch('/api/tags?x='+Date.now(),{cache:'no-store'}).then(x=>x.json()),fetch('/api/rules?x='+Date.now(),{cache:'no-store'}).then(x=>x.json())]);tags=t.points||[];cfg=r.rules||[];master.checked=!!r.masterEnabled;masterState.textContent=r.masterEnabled?'ON':'OFF';masterState.className=r.masterEnabled?'good':'warn';dg.textContent=r.doWriteGood||0;db.textContent=r.doWriteBad||0;renderAlarms(cfg);rules.innerHTML=cfg.map(card).join('');cfg.forEach(x=>wire(x.index))}
async function setMaster(){await fetch('/api/rules-master',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({enabled:master.checked?'1':'0'})});await refresh()}
async function setRuleEnabled(i){const p=new URLSearchParams({index:String(i),enabled:e('en'+i).value});const r=await fetch('/api/rule-enabled',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});msg.textContent=r.ok?'Rule '+(i+1)+' enabled state saved':'ENABLE SAVE FAILED'}
async function saveRule(i){const err=localSideError(i,false)||localSideError(i,true);if(err){msg.textContent='SAVE BLOCKED: '+err;updateProof(i);return}const digital=isDigitalTag(i);const p=new URLSearchParams({index:i,enabled:e('en'+i).value,name:e('nm'+i).value,tagIndex:e('tg'+i).value,compare:digital?'4':e('cp'+i).value,threshold:digital?e('ds'+i).value:e('th'+i).value,delayMs:String(Math.round((+e('dl'+i).value||0)*1000)),action:e('ac'+i).value,target:e('tr'+i).value,action2:e('a2'+i).value,target2:e('t2'+i).value,elseAction:e('ea'+i).value,elseTarget:e('et'+i).value,elseAction2:e('e2'+i).value,elseTarget2:e('u2'+i).value,latch:e('la'+i).value,message:e('ms'+i).value});const r=await fetch('/api/rule-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});let body={};try{body=await r.json()}catch(_){}msg.textContent=r.ok?'Rule '+(i+1)+' saved':('SAVE FAILED'+(body.error?': '+body.error:''));if(r.ok)setTimeout(load,150)}
async function resetRule(i){await fetch('/api/rule-reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({index:i})});await refresh()}
function duplicateRule(i){let dest=-1;for(let n=0;n<16;n++){if(n!=i&&!cfg[n]?.enabled&&/^Rule \d+$/.test(cfg[n]?.name||'')){dest=n;break}}if(dest<0){for(let n=0;n<16;n++){if(n!=i&&!cfg[n]?.enabled){dest=n;break}}}if(dest<0){msg.textContent='No disabled rule slot available for duplicate';return}if(!confirm('Duplicate Rule '+(i+1)+' into Rule '+(dest+1)+'? The copy will be DISABLED.'))return;fetch('/api/rule-duplicate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({source:i,dest})}).then(async r=>{msg.textContent=r.ok?'Rule '+(i+1)+' duplicated to Rule '+(dest+1)+' (disabled)':'DUPLICATE FAILED';if(r.ok)setTimeout(load,150)})}
async function disableAll(){if(!confirm('Disable ALL rules and turn Rules Enabled OFF?'))return;const r=await fetch('/api/rules-disable-all',{method:'POST'});msg.textContent=r.ok?'All rules disabled':'DISABLE ALL FAILED';if(r.ok)setTimeout(load,150)}
async function refresh(){try{const r=await fetch('/api/rules?x='+Date.now(),{cache:'no-store'}).then(x=>x.json());master.checked=!!r.masterEnabled;masterState.textContent=r.masterEnabled?'ON':'OFF';masterState.className=r.masterEnabled?'good':'warn';dg.textContent=r.doWriteGood||0;db.textContent=r.doWriteBad||0;renderAlarms(r.rules||[]);(r.rules||[]).forEach(x=>{if(e('st'+x.index)){e('st'+x.index).textContent=(x.alarm?'ALARM - '+valText(x):x.status);e('st'+x.index).className=statusClass(x)}if(e('cv'+x.index))e('cv'+x.index).innerHTML='Current '+esc(valText(x))+' &nbsp;|&nbsp; Fires <b>'+x.fireCount+'</b>';const k=cfg.findIndex(z=>z.index==x.index);if(k>=0)cfg[k]=x;updateProof(x.index)})}catch(_){}}
load();setInterval(refresh,1000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");server.send_P(200,"text/html",PAGE);
  });

  server.on("/api/modbus-monitor",HTTP_GET,[](){
    String j;
    j.reserve(10000);
    j="{\"sequence\":"+String(modbusMonitorSequence);
    j+=",\"enabled\":"+String(modbusCommsEnabled?"true":"false");
    j+=",\"online\":"+String(modbusOnline?"true":"false");
    j+=",\"records\":[";
    uint8_t start=(modbusMonitorHead+MODBUS_MONITOR_COUNT-modbusMonitorUsed)%MODBUS_MONITOR_COUNT;
    for(uint8_t n=0;n<modbusMonitorUsed;n++){
      if(n)j+=",";
      uint8_t i=(start+n)%MODBUS_MONITOR_COUNT;
      ModbusMonitorRecord &r=modbusMonitor[i];
      String txt=r.text;txt.replace("\\","\\\\");txt.replace("\"","\\\"");
      j+="{\"ms\":"+String(r.ms)+",\"dir\":\""+String(r.dir)+"\",\"text\":\""+txt+"\"}";
    }
    j+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/api/modbus-monitor-clear",HTTP_POST,[](){
    modbusMonitorHead=0;
    modbusMonitorUsed=0;
    modbusMonitorSequence++;
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/modbus-monitor",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Modbus Monitor</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#0b0b0b;color:#eee;margin:12px}
.top{position:sticky;top:0;background:#0b0b0b;padding:8px 0;z-index:2;display:flex;gap:7px;align-items:center;flex-wrap:wrap}
button,select{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}a{color:#63e6be;font-weight:bold}
.badge{padding:6px 10px;border:1px solid #555;border-radius:16px}.ok{color:#8f8}.bad{color:#ff8b8b}.muted{color:#aaa}
pre{background:#000;color:#eee;border:1px solid #333;padding:12px;height:calc(100vh - 125px);overflow:auto;white-space:pre-wrap;font-family:Consolas,Monaco,monospace;font-size:15px;line-height:1.35}
</style></head><body>
<div class=top>
<b>Riot-Flow Modbus Monitor</b>
<a href="/">Dashboard</a><a href="/modbus-debug">Debug</a>
<button onclick=togglePause() id=pauseBtn>Pause</button>
<button onclick=clearMon()>Clear</button>
<select id=filter onchange=render()><option value="">All Traffic</option><option value=TX>TX</option><option value=RX>RX</option><option value=ERR>Errors Only</option></select>
<button id=commsBtn onclick=toggleComms()>Loading...</button>
<span id=state class=badge>Loading...</span><span id=info class=muted></span>
</div>
<pre id=terminal>Waiting for traffic...</pre>
<script>
let paused=false,records=[],commsEnabled=true;
function togglePause(){paused=!paused;pauseBtn.textContent=paused?'Resume':'Pause'}
function render(){
 let r=records.slice().reverse(),f=filter.value;if(f)r=r.filter(x=>x.dir===f);
 terminal.textContent=r.map(x=>String(x.ms).padStart(10,' ')+'  M31 RTU  '+x.dir.padEnd(4,' ')+'  '+x.text).join('\n');
 terminal.scrollTop=0;info.textContent=' newest first | '+r.length+' shown';
}
async function update(){
 if(paused)return;
 try{
  const x=await fetch('/api/modbus-monitor?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
  records=x.records||[];commsEnabled=!!x.enabled;
  commsBtn.textContent=commsEnabled?'Disable Comms':'Enable Comms';
  state.className='badge '+(!commsEnabled?'muted':(x.online?'ok':'bad'));
  state.textContent=!commsEnabled?'M31 DISABLED':(x.online?'M31 ONLINE':'M31 OFFLINE');
  render();
 }catch(e){info.textContent=' monitor read error'}
}
async function clearMon(){await fetch('/api/modbus-monitor-clear',{method:'POST'});records=[];render()}
async function toggleComms(){
 const p=new URLSearchParams({enabled:commsEnabled?'0':'1'});
 await fetch('/api/modbus-comms',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});await update();
}
update();setInterval(update,750);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  server.on("/modbus-debug",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Modbus Debug</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1180px}a{color:#63e6be}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}.card{background:#171717;border:1px solid #444;border-radius:9px;padding:11px}
.k{font-size:11px;color:#999}.v{font-size:18px;font-weight:700;margin-top:4px}.ok{color:#8f8}.bad{color:#ff8b8b}.muted{color:#aaa}
pre{background:#050505;border:1px solid #333;padding:10px;overflow:auto;white-space:pre-wrap}
button{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}
@media(max-width:800px){.grid{grid-template-columns:repeat(2,1fr)}}@media(max-width:480px){.grid{grid-template-columns:1fr}}
</style></head><body>
<h2>Riot-Flow Modbus Debug</h2>
<p><a href="/">Dashboard</a> | <a href="/modbus-monitor">Traffic Monitor</a> | <a href="/io-setup">I/O Setup</a></p>
<div class=grid>
<div class=card><div class=k>STATE</div><div id=state class=v>--</div></div>
<div class=card><div class=k>GOOD / BAD</div><div id=counts class=v>--</div></div>
<div class=card><div class=k>TIMEOUT / CRC</div><div id=errors class=v>--</div></div>
<div class=card><div class=k>RX FRAMES / BYTES</div><div id=rxstats class=v>--</div></div>
</div>
<div class=card style="margin-top:10px">
<div class=k>RTU PROFILE</div><div id=profile class=v>--</div>
<div class=k style="margin-top:8px">SCHEDULER PHASE</div><div id=phase>--</div>
</div>
<div class=card style="margin-top:10px"><div class=k>LAST WRITE TX</div><pre id=wtx>--</pre><div class=k>LAST WRITE RX</div><pre id=wrx>--</pre></div>
<div class=card style="margin-top:10px"><div class=k>LAST POLL TX</div><pre id=tx>--</pre><div class=k>LAST POLL RX</div><pre id=rx>--</pre></div>
<script>
const e=id=>document.getElementById(id),phases=['READ DI','READ DO','READ AI1-4','READ AI5-8','READ AO1-4'];
async function refresh(){
 const x=await fetch('/api/modbus-diag?x='+Date.now(),{cache:'no-store'}).then(r=>r.json());
 e('state').textContent=!x.enabled?'M31 DISABLED':(x.online?'M31 ONLINE':'M31 OFFLINE');e('state').className='v '+(!x.enabled?'muted':(x.online?'ok':'bad'));
 e('counts').textContent=x.good+' / '+x.bad;e('errors').textContent=x.timeouts+' / '+x.crcFail;e('rxstats').textContent=x.frames+' / '+x.bytes;
 e('profile').textContent=x.baud+' baud 8N1 | Slave '+x.slave+' | TX GPIO'+x.txPin+' | RX GPIO'+x.rxPin;
 e('phase').textContent=phases[x.phase]||('PHASE '+x.phase);e('wtx').textContent=x.lastWriteTx||'--';e('wrx').textContent=x.lastWriteRx||'--';e('tx').textContent=x.lastTx||'--';e('rx').textContent=x.lastRx||'--';
}
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  // History v2 gate 2: manual one-record storage test only.
  // Not connected to rules, boot, dashboard, e-paper, or automatic logging.
  server.on("/api/history-v2-test",HTTP_POST,[](){
    RiotHistoryStore::Entry rec={};
    rec.sequence=(uint32_t)RiotHistoryStore::count(SPIFFS)+1U;
    time_t now=time(nullptr);
    rec.epoch=(now>1000000000)?(uint32_t)now:0U;
    rec.type=RiotHistoryStore::ENTRY_TEST;
    rec.sourceIndex=0;
    rec.value=123.456f;
    rec.threshold=100.0f;
    snprintf(rec.label,sizeof(rec.label),"History v2 test");
    snprintf(rec.message,sizeof(rec.message),"Manual storage test record");

    bool ok=RiotHistoryStore::append(SPIFFS,rec);
    uint16_t stored=RiotHistoryStore::count(SPIFFS);
    String j="{\"ok\":";
    j+=ok?"true":"false";
    j+=",\"count\":"+String(stored)+"}";
    server.send(ok?200:507,"application/json",j);
  });

  server.on("/api/flow-history-day",HTTP_GET,[](){
    RiotDailyFlowHistoryStore::Stats s={};
    RiotDailyFlowHistoryStore::stats(SPIFFS,s);
    uint16_t limit=10,offset=0;
    int runFilter=-1;
    if(server.hasArg("limit")){
      int v=server.arg("limit").toInt(); if(v<1)v=1; if(v>10)v=10; limit=(uint16_t)v;
    }
    if(server.hasArg("offset")){
      int v=server.arg("offset").toInt(); if(v>0)offset=(uint16_t)constrain(v,0,(int)RiotDailyFlowHistoryStore::MAX_RECORDS);
    }
    if(server.hasArg("run")){
      int v=server.arg("run").toInt(); if(v>=1&&v<=FLOW_RUN_COUNT)runFilter=v-1;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json","");
    server.sendContent("{\"count\":"+String(s.count)+",\"capacity\":"+String(s.capacity)+",\"rolloverCount\":"+String(s.rolloverCount)+",\"records\":[");
    uint16_t matched=0,returned=0;
    for(uint16_t physical=0;physical<s.count && returned<limit;physical++){
      RiotDailyFlowHistoryStore::Entry r={};
      if(!RiotDailyFlowHistoryStore::readNewest(SPIFFS,physical,r))continue;
      if(runFilter>=0 && r.run!=(uint8_t)runFilter)continue;
      if(matched<offset){matched++;continue;}
      matched++;
      if(returned)server.sendContent(",");
      String row="{\"sequence\":"+String(r.sequence);
      row+=",\"date\":"+String(r.date);
      row+=",\"run\":"+String(r.run+1);
      row+=",\"flow_E3m3\":"+String(r.volumeE3m3,6);
      row+=",\"flowHours\":"+String(r.flowHours,6)+"}";
      server.sendContent(row);
      returned++;
    }
    server.sendContent("],\"returned\":"+String(returned)+"}");
    server.sendContent("");
  });

  server.on("/api/flow-history",HTTP_GET,[](){
    RiotFlowHistoryStore::Stats s={};
    RiotFlowHistoryStore::stats(SPIFFS,s);
    uint16_t limit=10,offset=0;
    int runFilter=-1;
    if(server.hasArg("limit")){
      int v=server.arg("limit").toInt(); if(v<1)v=1; if(v>10)v=10; limit=(uint16_t)v;
    }
    if(server.hasArg("offset")){
      int v=server.arg("offset").toInt(); if(v>0)offset=(uint16_t)constrain(v,0,(int)RiotFlowHistoryStore::MAX_RECORDS);
    }
    if(server.hasArg("run")){
      int v=server.arg("run").toInt(); if(v>=1&&v<=FLOW_RUN_COUNT)runFilter=v-1;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json","");
    server.sendContent("{\"count\":"+String(s.count)+",\"capacity\":"+String(s.capacity)+",\"rolloverCount\":"+String(s.rolloverCount)+",\"records\":[");
    uint16_t matched=0,returned=0;
    for(uint16_t physical=0;physical<s.count && returned<limit;physical++){
      RiotFlowHistoryStore::Entry r={};
      if(!RiotFlowHistoryStore::readNewest(SPIFFS,physical,r))continue;
      if(runFilter>=0 && r.run!=(uint8_t)runFilter)continue;
      if(matched<offset){matched++;continue;}
      matched++;
      if(returned)server.sendContent(",");
      String row="{\"sequence\":"+String(r.sequence);
      row+=",\"hour\":"+String(r.hourKey);
      row+=",\"run\":"+String(r.run+1);
      row+=",\"flow_E3m3\":"+String(r.volumeE3m3,6);
      row+=",\"flowHours\":"+String(r.flowHours,6);
      row+=",\"avgDP_kPa\":"+String(r.avgDP,3);
      row+=",\"avgStatic_kPa\":"+String(r.avgStatic,3);
      row+=",\"avgTemp_C\":"+String(r.avgTemp,3)+"}";
      server.sendContent(row);
      returned++;
    }
    server.sendContent("],\"returned\":"+String(returned)+"}");
    server.sendContent("");
  });

  // History v2 bounded paging API.
  // Never returns more than 10 records and never builds full history in RAM.
  server.on("/api/history-v2",HTTP_GET,[](){
    if(!riotFsMounted){
      server.sendHeader("Cache-Control","no-store");
      server.send(503,"application/json",
        "{\"storageOK\":false,\"protected\":true,\"erasePolicy\":\"FACTORY_RESET_ONLY\",\"error\":\"SPIFFS mount failed; audit log not formatted\"}");
      return;
    }
    RiotHistoryStore::Stats historyStats={};
    if(!RiotHistoryStore::stats(SPIFFS,historyStats)){
      server.sendHeader("Cache-Control","no-store");
      server.send(503,"application/json",
        "{\"storageOK\":false,\"protected\":true,\"erasePolicy\":\"FACTORY_RESET_ONLY\",\"error\":\"Audit log metadata/read failure\"}");
      return;
    }
    const uint16_t stored=historyStats.count;
    uint16_t limit=10;
    uint16_t offset=0;
    int typeFilter=-1;

    if(server.hasArg("limit")){
      int requested=server.arg("limit").toInt();
      if(requested<1)requested=1;
      if(requested>10)requested=10;
      limit=(uint16_t)requested;
    }
    if(server.hasArg("offset")){
      int requested=server.arg("offset").toInt();
      if(requested>0)offset=(uint16_t)constrain(requested,0,(int)RiotHistoryStore::MAX_RECORDS);
    }
    if(server.hasArg("type")){
      int requested=server.arg("type").toInt();
      if(requested>=0 && requested<=4)typeFilter=requested;
    }

    uint16_t typeCounts[5]={0,0,0,0,0};
    uint16_t matchedCount=0;
    for(uint16_t physical=0;physical<stored;physical++){
      RiotHistoryStore::Entry rec={};
      if(!RiotHistoryStore::readNewest(SPIFFS,physical,rec))continue;
      if(rec.type<=4)typeCounts[rec.type]++;
      if(typeFilter<0 || rec.type==(uint8_t)typeFilter)matchedCount++;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json","");

    String meta="{\"storageOK\":true,\"protected\":true,\"erasePolicy\":\"FACTORY_RESET_ONLY\"";
    meta+=",\"count\":"+String(stored)+",\"capacity\":"+String(historyStats.capacity);
    meta+=",\"oldestSequence\":"+String(historyStats.oldestSequence);
    meta+=",\"newestSequence\":"+String(historyStats.newestSequence)+",\"rolloverCount\":"+String(historyStats.rolloverCount);
    meta+=",\"matchedCount\":"+String(matchedCount);
    meta+=",\"alarmCount\":"+String(typeCounts[0])+",\"eventCount\":"+String(typeCounts[1]);
    meta+=",\"clearCount\":"+String(typeCounts[2])+",\"testCount\":"+String(typeCounts[3])+",\"ackCount\":"+String(typeCounts[4]);
    meta+=",\"offset\":"+String(offset)+",\"limit\":"+String(limit)+",\"records\":[";
    server.sendContent(meta);
    uint16_t matchedSeen=0;
    uint16_t returned=0;
    uint16_t nextOffset=offset;
    for(uint16_t physical=0;physical<stored && returned<limit;physical++){
      RiotHistoryStore::Entry rec={};
      if(!RiotHistoryStore::readNewest(SPIFFS,physical,rec))continue;
      if(typeFilter>=0 && rec.type!=(uint8_t)typeFilter)continue;
      if(matchedSeen<offset){matchedSeen++;continue;}
      matchedSeen++;
      nextOffset=matchedSeen;

      String label=rec.label;
      String message=rec.message;
      label.replace("\\","\\\\");label.replace("\"","\\\"");
      message.replace("\\","\\\\");message.replace("\"","\\\"");

      if(returned)server.sendContent(",");
      String row="{\"sequence\":"+String(rec.sequence);
      row+=",\"epoch\":"+String(rec.epoch);
      row+=",\"type\":"+String(rec.type);
      row+=",\"sourceIndex\":"+String(rec.sourceIndex);
      row+=",\"value\":"+String(rec.value,3);
      row+=",\"threshold\":"+String(rec.threshold,3);
      row+=",\"label\":\""+label+"\"";
      row+=",\"message\":\""+message+"\"}";
      server.sendContent(row);
      returned++;
    }
    server.sendContent("],\"returned\":"+String(returned)+",\"nextOffset\":"+String(nextOffset)+"}");
    server.sendContent("");
  });

  // A8b: audit history cannot be erased through normal web/API maintenance.
  // RiotHistoryStore::clear() remains internal for a future deliberate
  // Factory Reset / Recommission workflow.
  server.on("/api/history-v2-clear",HTTP_POST,[](){
    server.send(403,"application/json",
      "{\"ok\":false,\"protected\":true,\"erasePolicy\":\"FACTORY_RESET_ONLY\",\"error\":\"Alarm/event audit log can only be erased by Factory Reset / Recommission\"}");
  });

  // RIOT A8A SPLIT HISTORY EVENTS V1
  // Station History is process/flow data. Alarm/event audit records remain
  // available separately at /history-v2.
  server.on("/station-history",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Station History</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1180px}a{color:#63e6be}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:12px;margin:10px 0}.muted{color:#999}
.controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.wrap{overflow:auto;border:1px solid #444;border-radius:8px}
table{border-collapse:collapse;width:100%;min-width:860px;background:#151515}th,td{padding:8px;border-bottom:1px solid #333;text-align:left}
th{background:#202020}button,select{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}
.good{color:#8f8}.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
@media(max-width:600px){.rfNav{display:grid;grid-template-columns:1fr}.rfNav>a,.rfDrop,.rfDrop>button{width:100%}.rfDrop>button{text-align:left}.rfMenu{position:static;min-width:0;width:100%;margin-top:4px}}
</style></head><body>
<h2>Riot-Flow Station History</h2>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/maintenance">Maintenance Home</a>
<a href="/firmware-update">Firmware Update</a>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>

<div class=card>
<b>Station process history</b>
<div class=muted style="margin-top:6px">Process and flow records are kept separate from alarm/event audit records.</div>
<div class=controls style="margin-top:10px">
<label>Interval <select id=interval onchange="firstPage()"><option value=hour>Hourly</option><option value=day>Daily</option></select></label>
<label>Run <select id=run onchange="firstPage()"><option value="">All Runs</option><option value=1>Run 1</option><option value=2>Run 2</option><option value=3>Run 3</option><option value=4>Run 4</option><option value=5>Run 5</option><option value=6>Run 6</option></select></label>
<button onclick="firstPage()">Newest</button>
<button id=prevBtn onclick="prevPage()">Previous</button>
<button id=nextBtn onclick="nextPage()">Next</button>
<span id=stat class=muted>Loading...</span>
</div>
<div id=summary class=muted style="margin-top:7px">Loading...</div>
</div>

<div class=wrap>
<table>
<thead id=head></thead>
<tbody id=body></tbody>
</table>
</div>

<script>
let offset=0,lastReturned=0,totalCount=0;
const esc=s=>String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
function hourLabel(v){
 const s=String(v??'');
 if(s.length!==10)return s||'--';
 return s.slice(0,4)+'-'+s.slice(4,6)+'-'+s.slice(6,8)+' '+s.slice(8,10)+':00';
}
function dayLabel(v){
 const s=String(v??'');
 if(s.length!==8)return s||'--';
 return s.slice(0,4)+'-'+s.slice(4,6)+'-'+s.slice(6,8);
}
async function load(){
 stat.textContent='Loading...';
 const daily=interval.value==='day';
 const q=new URLSearchParams({limit:'10',offset:String(offset),x:String(Date.now())});
 if(run.value!=='')q.set('run',run.value);
 const url=(daily?'/api/flow-history-day?':'/api/flow-history?')+q.toString();
 try{
  const x=await fetch(url,{cache:'no-store'}).then(r=>r.json());
  const rows=x.records||[];
  lastReturned=rows.length;
  totalCount=Number(x.count)||0;

  if(daily){
   head.innerHTML='<tr><th>Date</th><th>Run</th><th>Volume E3 m3</th><th>Flow Hours</th><th>Seq</th></tr>';
   body.innerHTML=rows.map(r=>'<tr><td>'+esc(dayLabel(r.date))+'</td><td>'+r.run+'</td><td>'+Number(r.flow_E3m3).toFixed(3)+'</td><td>'+Number(r.flowHours).toFixed(2)+'</td><td>'+r.sequence+'</td></tr>').join('');
  }else{
   head.innerHTML='<tr><th>Hour</th><th>Run</th><th>Volume E3 m3</th><th>Flow Hours</th><th>Avg DP kPa</th><th>Avg P kPa</th><th>Avg T C</th><th>Seq</th></tr>';
   body.innerHTML=rows.map(r=>'<tr><td>'+esc(hourLabel(r.hour))+'</td><td>'+r.run+'</td><td>'+Number(r.flow_E3m3).toFixed(3)+'</td><td>'+Number(r.flowHours).toFixed(2)+'</td><td>'+Number(r.avgDP_kPa).toFixed(3)+'</td><td>'+Number(r.avgStatic_kPa).toFixed(3)+'</td><td>'+Number(r.avgTemp_C).toFixed(3)+'</td><td>'+r.sequence+'</td></tr>').join('');
  }

  const page=rows.length?Math.floor(offset/10)+1:0;
  stat.textContent='Page '+page+' | '+rows.length+' shown';
  summary.textContent=totalCount+'/'+x.capacity+' retained | Rollover '+x.rolloverCount+(run.value?' | Run '+run.value:' | All runs');
  prevBtn.disabled=offset===0;
  nextBtn.disabled=rows.length<10;
 }catch(err){
  stat.textContent='Station history read error';
  body.innerHTML='';
 }
}
function firstPage(){offset=0;load()}
function nextPage(){if(lastReturned<10)return;offset+=10;load()}
function prevPage(){offset=Math.max(0,offset-10);load()}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  // History v2 operator browser. UI paging remains capped at 10 rows per request.
  server.on("/history-v2",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Alarms / Events</title>
<style>
*{box-sizing:border-box}body{font-family:Arial;background:#111;color:#eee;margin:16px;max-width:1180px}a{color:#63e6be}
.card{background:#171717;border:1px solid #444;border-radius:9px;padding:12px;margin:10px 0}.muted{color:#999}
.controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.wrap{overflow:auto;border:1px solid #444;border-radius:8px}
table{border-collapse:collapse;width:100%;min-width:900px;background:#151515}th,td{padding:8px;border-bottom:1px solid #333;text-align:left}
th{background:#202020}.msg{white-space:normal;min-width:180px}button,select{padding:7px;background:#222;color:#fff;border:1px solid #666;border-radius:5px}
.bad{color:#ff8b8b}.warn{color:#ffd166}.good{color:#8f8}
</style></head><body>
<h2>Riot-Flow Alarms / Events</h2>
<style>
.rfNav{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 12px;padding:7px;background:#13191a;border:1px solid #354143;border-radius:8px}
.rfNav>a,.rfDrop>button{color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:6px;padding:7px 10px;text-decoration:none;font-weight:600;font-size:15px;cursor:pointer}
.rfDrop{position:relative}
.rfMenu{display:none;position:absolute;z-index:9999;top:100%;left:0;min-width:190px;padding:5px;background:#121718;border:1px solid #455;border-radius:7px}
.rfDrop:hover .rfMenu,.rfDrop:focus-within .rfMenu{display:block}
.rfMenu a{display:block;color:#63e6be;background:#1d2526;border:1px solid #3e4b4d;border-radius:5px;padding:7px 9px;margin:2px 0;text-decoration:none;font-weight:600;white-space:nowrap}
</style>
<div class=rfNav>
<a href="/">Dashboard</a>
<div class=rfDrop tabindex=0><button type=button>Flow &#9662;</button><div class=rfMenu>
<a href="/flow-runs">Flow Runs</a>
<a href="/flow-setup">Flow Setup</a>
<a href="/aga-setup">AGA / Gas</a>
<a href="/station-history">History</a>
<a href="/history-v2">Alarms / Events</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>I/O &#9662;</button><div class=rfMenu>
<a href="/io-setup">I/O Setup</a>
<a href="/pid-control">PID / Control</a>
<a href="/control-output">Manual Outputs</a>
<a href="/modbus-monitor">Modbus Monitor</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Setup &#9662;</button><div class=rfMenu>
<a href="/rules">Rules</a>
<a href="/display-setup">Display</a>
<a href="/mqtt-setup">MQTT</a>
<a href="/wifi">Network / WiFi</a>
<a href="/technician-access">Technician Access</a>
<a href="/process-setup">Process Setup</a>
</div></div>
<div class=rfDrop tabindex=0><button type=button>Maintenance &#9662;</button><div class=rfMenu>
<a href="/system-status">System Status</a>
<a href="/backup-restore">Backup / Restore</a>
<a href="/nvs-maintenance">NVS Storage</a>
<a href="/modbus-debug">Modbus Debug</a>
<a href="/tags">Tags</a>
<a href="/api-reference">API Reference</a>
</div></div>
</div>
<div class=card>
<div class=controls>
<label>Type <select id=type onchange="firstPage()"><option value="">All</option><option value=0>ALARM</option><option value=1>EVENT</option><option value=2>CLEAR</option><option value=3>TEST</option><option value=4>ACK</option></select></label>
<button onclick="firstPage()">Newest</button>
<button id=prevBtn onclick="prevPage()">Previous</button>
<button id=nextBtn onclick="nextPage()">Next</button>
<span class=good><b>Protected audit log</b> - records age out only by circular rollover; erase requires Factory Reset / Recommission.</span>
<span id=stat class=muted>Loading...</span>
</div>
<div id=summary class=muted style="margin-top:7px">Loading counts...</div>
<div class=muted style="margin-top:5px">Newest first. Reads are limited to 10 records per request.</div>
</div>
<div class=wrap><table><thead><tr><th>Time</th><th>Type</th><th>Seq</th><th>Rule</th><th>Label</th><th>Message</th><th>Value</th><th>Threshold</th></tr></thead><tbody id=body></tbody></table></div>
<script>
let offset=0,lastReturned=0,lastMatched=0;
const esc=s=>String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
const typeName=t=>['ALARM','EVENT','CLEAR','TEST','ACK'][Number(t)]||('TYPE '+t);
const tc=t=>Number(t)===0?'bad':(Number(t)===1?'warn':(Number(t)===2?'good':(Number(t)===4?'good':'muted')));
const when=e=>{if(!e)return 'TIME INVALID';const d=new Date(Number(e)*1000);return isNaN(d.getTime())?'TIME INVALID':d.toLocaleString()};
async function load(){
 stat.textContent='Loading...';
 const q=new URLSearchParams({limit:'10',offset:String(offset),x:String(Date.now())});
 if(type.value!=='')q.set('type',type.value);
 try{
  const x=await fetch('/api/history-v2?'+q.toString(),{cache:'no-store'}).then(r=>r.json());
  const rows=x.records||[];lastReturned=rows.length;lastMatched=Number(x.matchedCount)||0;
  body.innerHTML=rows.map(r=>'<tr><td>'+esc(when(r.epoch))+'</td><td class="'+tc(r.type)+'"><b>'+esc(typeName(r.type))+'</b></td><td>'+r.sequence+'</td><td>'+(Number(r.sourceIndex)+1)+'</td><td>'+esc(r.label)+'</td><td class=msg>'+esc(r.message)+'</td><td>'+Number(r.value).toFixed(3)+'</td><td>'+Number(r.threshold).toFixed(3)+'</td></tr>').join('');
  const page=lastMatched?Math.floor(offset/10)+1:0;
  const pages=lastMatched?Math.ceil(lastMatched/10):0;
  stat.textContent='Page '+page+' / '+pages+' | '+lastMatched+' matching';
  summary.textContent=x.count+'/'+x.capacity+' retained | '+x.alarmCount+' Alarm | '+x.eventCount+' Event | '+x.clearCount+' Clear | '+x.ackCount+' Ack | '+x.testCount+' Test | Rollover '+x.rolloverCount;
  prevBtn.disabled=offset===0;
  nextBtn.disabled=(offset+rows.length)>=lastMatched || rows.length===0;
 }catch(e){stat.textContent='History read error'}
}
function firstPage(){offset=0;load()}
function nextPage(){if((offset+lastReturned)>=lastMatched)return;offset+=10;load()}
function prevPage(){offset=Math.max(0,offset-10);load()}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  // RIOT A9A TECHNICIAN ACCESS QR V1
  server.on("/api/technician-access",HTTP_GET,[](){
    String j="{\"ssid\":\""+riotFallbackApSsid+"\"";
    j+=",\"apIp\":\""+WiFi.softAPIP().toString()+"\"";
    j+=",\"deviceId\":\""+riotDeviceId()+"\"";
    j+=",\"openAp\":true";
    j+=",\"qrPayload\":\"WIFI:T:nopass;S:"+riotFallbackApSsid+";;\"";
    j+="}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.on("/technician-qr.svg",HTTP_GET,[](){
    static constexpr uint8_t QR_VERSION=3;
    static uint8_t qrData[256];
    const size_t required=qrcode_getBufferSize(QR_VERSION);
    if(required>sizeof(qrData)){
      server.send(500,"text/plain","QR buffer size error");
      return;
    }

    String payload="WIFI:T:nopass;S:"+riotFallbackApSsid+";;";
    QRCode qr={};
    qrcode_initText(&qr,qrData,QR_VERSION,ECC_LOW,payload.c_str());

    const int quiet=4;
    const int side=(int)qr.size+quiet*2;
    String svg;
    svg.reserve(12000);
    svg="<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    svg+="<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "+String(side)+" "+String(side)+"\" shape-rendering=\"crispEdges\">";
    svg+="<rect width=\"100%\" height=\"100%\" fill=\"white\"/><path d=\"";
    for(uint8_t y=0;y<qr.size;y++){
      for(uint8_t x=0;x<qr.size;x++){
        if(!qrcode_getModule(&qr,x,y))continue;
        svg+="M"+String((int)x+quiet)+" "+String((int)y+quiet)+"h1v1h-1z";
      }
    }
    svg+="\" fill=\"black\"/></svg>";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"image/svg+xml",svg);
  });

  server.on("/technician-access",HTTP_GET,[](){
    static const char PAGE[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Riot-Flow Technician Access</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial;background:#111;color:#eee;margin:18px;max-width:1050px}
a{color:#63e6be}.card{background:#171717;border:1px solid #444;border-radius:10px;padding:16px;margin:12px 0}
.label{background:white;color:#111;width:410px;max-width:100%;margin:0 auto;padding:18px;border:2px solid #111;border-radius:10px;text-align:center}
.logoWrap{height:66px;display:flex;align-items:center;justify-content:center;margin-bottom:4px}
#ownerLogo{display:none;max-width:180px;max-height:56px;image-rendering:pixelated}
.brand{font-size:28px;font-weight:800;letter-spacing:1px}.access{font-size:18px;font-weight:700;margin:3px 0 9px}
#qr{width:270px;height:270px;max-width:90%;display:block;margin:0 auto}
.scan{font-size:16px;font-weight:700;margin:8px 0 12px}
.info{text-align:left;border-top:1px solid #bbb;padding-top:10px;font-size:15px;line-height:1.5}
.info b{display:inline-block;min-width:82px}.fallback{font-size:12px;margin-top:10px;padding-top:8px;border-top:1px solid #ccc}
.controls{text-align:center;margin-top:14px}.controls button{padding:10px 16px;font-size:16px;background:#222;color:#fff;border:1px solid #666;border-radius:6px;cursor:pointer}
.muted{color:#aaa;font-size:13px}
@media print{
 @page{margin:8mm}
 body{background:white;color:black;margin:0;max-width:none}
 .noPrint{display:none!important}
 .card{border:0;padding:0;margin:0;background:white}
 .label{width:95mm;max-width:95mm;border:1.5px solid #000;border-radius:5mm;padding:5mm;page-break-inside:avoid}
 #qr{width:58mm;height:58mm}
 .brand{font-size:22pt}.access{font-size:13pt}.info{font-size:10pt}.fallback{font-size:8.5pt}
}
</style></head><body>
<div class=noPrint><p><a href="/">Dashboard</a> | <a href="/wifi">Network / WiFi</a> | <a href="/maintenance">Maintenance</a></p>
<h2>Technician Access / QR Label</h2>
<div class=card><b>Walk-up commissioning access</b><div class=muted style="margin-top:6px">This QR joins the controller's local Riot-XXXX Wi-Fi. The captive portal then opens Riot-Flow locally; no site LAN address or Internet service is required.</div></div></div>

<div class=card>
<div class=label id=printLabel>
 <div class=logoWrap><canvas id=ownerLogo width=1 height=1></canvas><div id=logoFallback class=brand>RIOT-FLOW</div></div>
 <div class=access>TECHNICIAN ACCESS</div>
 <img id=qr src="/technician-qr.svg" alt="Riot-Flow Wi-Fi QR">
 <div class=scan>Scan to connect directly to this controller</div>
 <div class=info>
  <div><b>Site:</b> <span id=site>--</span></div>
  <div><b>LSD:</b> <span id=lsd>--</span></div>
  <div><b>AP:</b> <span id=ssid>--</span></div>
  <div><b>Device:</b> <span id=device>--</span></div>
  <div class=fallback>No QR? Join Wi-Fi "<span id=ssid2>Riot-XXXX</span>" then browse to <b>192.168.4.1</b></div>
 </div>
</div>
<div class="controls noPrint"><button onclick="window.print()">Print Access Label</button></div>
</div>

<script>
const e=id=>document.getElementById(id);
async function loadLogo(){
 try{
  const r=await fetch('/api/display-logo-bin?x='+Date.now(),{cache:'no-store'});
  if(!r.ok)return;
  const b=new Uint8Array(await r.arrayBuffer());
  if(b.length<5)return;
  const w=b[0]|(b[1]<<8),h=b[2]|(b[3]<<8);
  if(!w||!h||w>180||h>56)return;
  let setCount=0,pixels=w*h;
  for(let bit=0;bit<pixels;bit++)if(b[4+(bit>>3)]&(0x80>>(bit&7)))setCount++;
  const drawSet=setCount<=pixels/2;
  const cv=e('ownerLogo');cv.width=w;cv.height=h;
  const cx=cv.getContext('2d');cx.clearRect(0,0,w,h);cx.fillStyle='#000';
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){
    const bit=y*w+x,set=(b[4+(bit>>3)]&(0x80>>(bit&7)))!==0;
    if(set===drawSet)cx.fillRect(x,y,1,1);
  }
  cv.style.display='block';e('logoFallback').style.display='none';
 }catch(_){}
}
async function load(){
 try{
  const [a,s]=await Promise.all([
   fetch('/api/technician-access?x='+Date.now(),{cache:'no-store'}).then(r=>r.json()),
   fetch('/status?x='+Date.now(),{cache:'no-store'}).then(r=>r.json())
  ]);
  e('ssid').textContent=a.ssid||'Riot-XXXX';
  e('ssid2').textContent=a.ssid||'Riot-XXXX';
  e('device').textContent=a.deviceId||'--';
  e('site').textContent=s.siteName||'--';
  e('lsd').textContent=s.siteLSD||'--';
 }catch(_){}
 await loadLogo();
}
load();
</script></body></html>)HTML";
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html",PAGE);
  });

  // RIOT A9A CAPTIVE LANDING REPAIR V1
  auto riotCaptiveHome=[](){
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Location","http://192.168.4.1/",true);
    server.send(302,"text/plain","");
  };
  server.on("/generate_204",HTTP_GET,riotCaptiveHome);
  server.on("/gen_204",HTTP_GET,riotCaptiveHome);
  server.on("/hotspot-detect.html",HTTP_GET,riotCaptiveHome);
  server.on("/library/test/success.html",HTTP_GET,riotCaptiveHome);
  server.on("/connecttest.txt",HTTP_GET,riotCaptiveHome);
  server.on("/ncsi.txt",HTTP_GET,riotCaptiveHome);
  server.on("/fwlink",HTTP_GET,riotCaptiveHome);
  server.on("/redirect",HTTP_GET,riotCaptiveHome);

  server.onNotFound([](){
    if(captivePortalActive){
      server.sendHeader("Location","http://192.168.4.1/",true);
      server.send(302,"text/plain","");
    }else{
      server.send(404,"text/plain","Not found");
    }
  });

  server.begin();
}

void setup(){
  Serial.begin(115200);
  pinMode(RIOT_KEY1_PIN,INPUT_PULLUP);
  riotKey1LastRaw=digitalRead(RIOT_KEY1_PIN);
  riotKey1Stable=riotKey1LastRaw;
  riotKey1ChangedMs=millis();
  Serial.printf("KEY1 demo button: GPIO%u active-low initial=%s\n",
    (unsigned)RIOT_KEY1_PIN,riotKey1Stable==LOW?"PRESSED":"RELEASED");
  beginBatteryTelemetry();
  sampleBatteryTelemetry(true);
  delay(1000);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("Riot-Flow Controller Bring-Up v1");
  Serial.println("==========================================");
  Serial.printf("Chip: %s\n",ESP.getChipModel());
  Serial.printf("Flash: %u\n",ESP.getFlashChipSize());
  Serial.printf("PSRAM: %u\n",ESP.getPsramSize());

  // ePaper initialization occurs after display config load so HEADLESS can
  // avoid touching display hardware entirely.

  modbusSerial.begin(m31RtuBaud,SERIAL_8N1,MODBUS_RX_PIN,MODBUS_TX_PIN);
  Serial.printf("M31 UART: TX D6/GPIO%d / RX D9/GPIO%d @ %lu\n",MODBUS_TX_PIN,MODBUS_RX_PIN,(unsigned long)m31RtuBaud);

  riotFsMounted=SPIFFS.begin(false);
  bool fsOK=riotFsMounted;
  if(!fsOK){
    Serial.println("SPIFFS mount failed; checking whether partition is demonstrably virgin");
    if(riotProvisionVirginSpiffs()){
      riotFsMounted=true;
      fsOK=true;
      Serial.println("SPIFFS FIRST-BOOT PROVISIONING COMPLETE");
    }else{
      Serial.println("AUDIT/STORAGE FAULT: SPIFFS mount failed; protected auto-format refused");
    }
  }
  if(POWER_GOOD_SENSE_ENABLED)pinMode(POWER_GOOD_PIN,INPUT);
  powerInputCandidate=requestedPowerGoodState();
  powerGood=powerInputCandidate;
  powerInputCandidateSinceMs=millis();
  Serial.printf("POWER_GOOD: pin=%d physical=%s state=%s\n",
    POWER_GOOD_PIN,
    POWER_GOOD_SENSE_ENABLED?"ENABLED":"DISABLED",
    powerGood?"ON":"OFF");
  Serial.printf("SPIFFS: %s total=%u used=%u\n",
    fsOK?"OK":"FAIL",
    fsOK?(unsigned)SPIFFS.totalBytes():0U,
    fsOK?(unsigned)SPIFFS.usedBytes():0U);

  printNVSStats("boot before config load");
  loadOperatorFlashConfig();
  loadIOMQTTConfig();
  modbusSerial.end();
  delay(5);
  modbusSerial.begin(m31RtuBaud,SERIAL_8N1,MODBUS_RX_PIN,MODBUS_TX_PIN);
  Serial.printf("M31 UART active baud after flash config: %lu\n",(unsigned long)m31RtuBaud);
  loadDisplayConfig();
  epaperRuntimeEnabled=riotResolveEpaperRuntime();
#ifdef EPAPER_ENABLE
  if(epaperRuntimeEnabled){
    Serial.printf("DISPLAY MODE %s -> ePaper runtime ENABLED\n",riotDisplayModeName(displayCfg.displayMode));
    epaper.begin();
  }else{
    Serial.printf("DISPLAY MODE %s -> HEADLESS runtime, ePaper initialization skipped\n",riotDisplayModeName(displayCfg.displayMode));
  }
#else
  epaperRuntimeEnabled=false;
  Serial.printf("DISPLAY MODE %s -> HEADLESS runtime, EPAPER_ENABLE not compiled\n",riotDisplayModeName(displayCfg.displayMode));
#endif
  loadAGAFlashConfig();
  loadFlowRunsFlashConfig();
  loadFlowTotalizers();
  loadHourlyHistoryQueue();
  loadDailyRolloverQueue();
  loadMQTTDailyUsage();
  cleanupLegacyHistoryNVS();
  loadMQTTConfig();
  loadFlowPIDConfig();
  Serial.println("RULECFG BOOT ENTER");
  loadRiotRules();
  Serial.printf("RULECFG BOOT EXIT status=%s bootLoadOK=%s saveCount=%lu\n",riotRuleFlashStatus.c_str(),riotRuleFlashBootLoadOK?"YES":"NO",(unsigned long)riotRuleFlashSaveCount);
  printNVSStats("boot after config load");
  updateEngineeringUnits();
  startAGATask();
  loadNetworkConfig();
  startFallbackAP();
  beginStationConnect();

  setupWeb();

  if(wifiStaSsid.length()){
    uint32_t started=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-started<WIFI_CONNECT_TIMEOUT_MS){
      server.handleClient();
      if(captivePortalActive)dnsServer.processNextRequest();
      delay(50);
    }
  }

  if(WiFi.status()==WL_CONNECTED){
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    startTimeService();
  }else{
    Serial.println("WiFi station not connected; fallback AP remains active.");
  }

  if(epaperRuntimeEnabled)drawDashboard();
}

// EP32 WEB DIAG GATE5A LOOP EPAPER TIMING V1
void loop(){
  static uint32_t diagLastLoopMs=0;
  uint32_t diagLoopNow=millis();
  if(diagLastLoopMs){
    uint32_t diagGap=diagLoopNow-diagLastLoopMs;
    if(diagGap>=250UL){
      Serial.printf("WEB/LOOP GAP %lu ms\n",(unsigned long)diagGap);
    }
  }
  diagLastLoopMs=diagLoopNow;
  if(commonBackupRebootAtMs && (int32_t)(millis()-commonBackupRebootAtMs)>=0){ESP.restart();}
  if(riotOtaRebootAtMs && (int32_t)(millis()-riotOtaRebootAtMs)>=0){ESP.restart();}
  sampleBatteryTelemetry();
  servicePowerGood();
  serviceRiotDemoKey();
  server.handleClient();
  if(captivePortalActive)dnsServer.processNextRequest();
  serviceWiFi();
  serviceM31();
  serviceAGA();
  serviceFlowPID();
  serviceRiotRules();
  serviceFlowTotalizers();
  serviceMQTT();

  uint32_t now=millis();
  static bool displayFpReady=false;
  static uint32_t lastDisplayEval=0;
  static uint32_t fpHeader=0,fpFlow=0,fpIO=0,fpFooter=0,fpAlarm=0;

  if(epaperRuntimeEnabled && (!riotDemoQrActive || powerOutageActive) && now-lastDisplayEval>=500UL){
    lastDisplayEval=now;

    static int8_t lastResolvedTheme=-1;
    int8_t resolvedTheme=epaperThemeIsDark()?1:0;
    if(lastResolvedTheme<0)lastResolvedTheme=resolvedTheme;
    else if(resolvedTheme!=lastResolvedTheme){
      Serial.printf("EPAPER THEME CHANGE old=%d new=%d mode=%u light=%u dark=%u\n",
        (int)lastResolvedTheme,(int)resolvedTheme,
        (unsigned)displayCfg.themeMode,
        (unsigned)displayCfg.lightHour,
        (unsigned)displayCfg.darkHour);
      lastResolvedTheme=resolvedTheme;
      requestFullEPaperRefresh();
    }

    uint32_t newHeader=displayHeaderFingerprint();
    uint32_t newFlow=displayFlowFingerprint();
    uint32_t newIO=displayIOFingerprint();
    uint32_t newFooter=displayFooterFingerprint();
    uint32_t newAlarm=displayAlarmFingerprint();

    if(!displayFpReady){
      fpHeader=newHeader;
      fpFlow=newFlow;
      fpIO=newIO;
      fpFooter=newFooter;
      fpAlarm=newAlarm;
      displayFpReady=true;
      displayDirty=false;
    }else{
      uint8_t changed=epaperPendingRegionMask;
      if(newHeader!=fpHeader)changed|=EPAPER_REGION_HEADER;
      if(newFlow!=fpFlow)changed|=EPAPER_REGION_FLOW;
      if(newIO!=fpIO)changed|=EPAPER_REGION_IO;
      if(newFooter!=fpFooter)changed|=EPAPER_REGION_FOOTER;
      if(newAlarm!=fpAlarm)changed|=EPAPER_REGION_ALARM;

      // ePaper is not an LCD. Keep calculations/MQTT fast, but limit the
      // frequently changing process region to a calm operator-display cadence.
      if((changed&EPAPER_REGION_FLOW) &&
         epaperLastFlowUpdateMs &&
         now-epaperLastFlowUpdateMs<EPAPER_FLOW_MIN_UPDATE_MS){
        changed&=~EPAPER_REGION_FLOW;
      }

      if(changed!=EPAPER_REGION_NONE || epaperForceFullRefresh){
        epaperPartialRegionMask=changed;
        epaperPendingRegionMask=EPAPER_REGION_NONE;
        const bool forceRequested=epaperForceFullRefresh;
        const bool darkTheme=epaperThemeIsDark();
        const uint16_t generalCleanupLimit=
          darkTheme?EPAPER_DARK_FULL_CLEANUP_EVERY:EPAPER_FULL_CLEANUP_EVERY;
        const uint16_t flowCleanupLimit=
          darkTheme?EPAPER_DARK_FLOW_CLEANUP_EVERY:EPAPER_FLOW_CLEANUP_EVERY;
        const bool generalCleanup=epaperPartialRefreshes>=generalCleanupLimit;
        const bool flowCleanup=epaperFlowPartialRefreshes>=flowCleanupLimit;
        bool cleanupRefresh =
          forceRequested ||
          generalCleanup ||
          flowCleanup;

        Serial.printf(
          "EPAPER REFRESH type=%s changed=0x%02X force=%u generalCleanup=%u flowCleanup=%u partial=%u flowPartial=%u theme=%s\n",
          cleanupRefresh?"FULL":"PARTIAL",
          (unsigned)changed,
          forceRequested?1U:0U,
          generalCleanup?1U:0U,
          flowCleanup?1U:0U,
          (unsigned)epaperPartialRefreshes,
          (unsigned)epaperFlowPartialRefreshes,
          epaperThemeIsDark()?"DARK":"LIGHT"
        );

        lastDisplay=now;
        displayDirty=false;
        epaperForceFullRefresh=false;
        if(changed&EPAPER_REGION_HEADER)fpHeader=newHeader;
        if(changed&EPAPER_REGION_FLOW){
          fpFlow=newFlow;
          epaperLastFlowUpdateMs=now;
        }
        if(changed&EPAPER_REGION_IO)fpIO=newIO;
        if(changed&EPAPER_REGION_FOOTER)fpFooter=newFooter;
        if(changed&EPAPER_REGION_ALARM)fpAlarm=newAlarm;
        if(cleanupRefresh){
          fpHeader=newHeader;
          fpFlow=newFlow;
          fpIO=newIO;
          fpFooter=newFooter;
          fpAlarm=newAlarm;
          epaperLastFlowUpdateMs=now;
        }
        if(!cleanupRefresh && (changed&EPAPER_REGION_IO))scrubEpaperIOBand();
        uint32_t epaperDiagStartMs=millis();
        Serial.printf("EPAPER DRAW START type=%s changed=0x%02X ms=%lu\n",
          cleanupRefresh?"FULL":"PARTIAL",
          (unsigned)changed,
          (unsigned long)epaperDiagStartMs);
        drawDashboard(cleanupRefresh);
        uint32_t epaperDiagDurMs=millis()-epaperDiagStartMs;
        Serial.printf("EPAPER DRAW END type=%s duration=%lu ms\n",
          cleanupRefresh?"FULL":"PARTIAL",
          (unsigned long)epaperDiagDurMs);
      }
    }
  }

  static uint32_t lastSerial=0;
  if(now-lastSerial>3000){
    lastSerial=now;
    Serial.printf("M31 %s good=%lu bad=%lu timeout=%lu rxFrames=%lu rxBytes=%lu crcFail=%lu | AI1 %.3f AI2 %.3f AI3 %.3f AI4 %.3f\n",
      modbusOnline?"ONLINE":"OFFLINE",
      (unsigned long)modbusGood,(unsigned long)modbusBad,
      (unsigned long)rawTimeouts,(unsigned long)rawRxFrames,
      (unsigned long)rawRxBytes,(unsigned long)rawCrcFails,
      ai_mA[0],ai_mA[1],ai_mA[2],ai_mA[3]);
    Serial.printf("Battery %.3f V raw=%u samples=%lu cal=%.4f\n",
      isfinite(batteryVoltageV)?batteryVoltageV:0.0f,
      (unsigned)batteryAdcRaw,
      (unsigned long)batterySampleCounter,
      BATTERY_ADC_CALIBRATION);
  }
}
