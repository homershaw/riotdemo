#include "RiotFlowHistoryStore.h"

#include <stddef.h>
#include <string.h>

namespace RiotFlowHistoryStore {

namespace {

static constexpr uint32_t META_MAGIC = 0x5246484DUL; // RFHM
static constexpr uint16_t META_VERSION = 1;

struct Meta {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint16_t count;
  uint16_t nextIndex;
  uint32_t nextSequence;
  uint32_t rolloverCount;
  uint32_t checksum;
};

static uint32_t fnv1a(const uint8_t* p,size_t n){
  uint32_t h=2166136261UL;
  for(size_t i=0;i<n;i++){h^=p[i];h*=16777619UL;}
  return h;
}

static uint32_t entryChecksum(const Entry& e){
  return fnv1a(reinterpret_cast<const uint8_t*>(&e),offsetof(Entry,checksum));
}

static uint32_t metaChecksum(const Meta& m){
  return fnv1a(reinterpret_cast<const uint8_t*>(&m),offsetof(Meta,checksum));
}

static String metaPathFor(const char* path){return String(path)+".meta";}

static bool metaValid(const Meta& m){
  if(m.magic!=META_MAGIC||m.version!=META_VERSION||m.size!=sizeof(Meta))return false;
  if(m.count>MAX_RECORDS)return false;
  if(m.count<MAX_RECORDS && m.nextIndex!=m.count)return false;
  if(m.count==MAX_RECORDS && m.nextIndex>=MAX_RECORDS)return false;
  if(m.nextSequence==0)return false;
  return m.checksum==metaChecksum(m);
}

static bool saveMeta(fs::FS& fs,Meta& m,const char* path){
  m.magic=META_MAGIC;m.version=META_VERSION;m.size=sizeof(Meta);m.checksum=metaChecksum(m);
  String target=metaPathFor(path),tmp=target+".tmp";
  if(fs.exists(tmp))fs.remove(tmp);
  File out=fs.open(tmp,FILE_WRITE);
  if(!out)return false;
  size_t n=out.write(reinterpret_cast<const uint8_t*>(&m),sizeof(Meta));
  out.flush();out.close();
  if(n!=sizeof(Meta)){fs.remove(tmp);return false;}
  if(fs.exists(target))fs.remove(target);
  if(!fs.rename(tmp,target)){fs.remove(tmp);return false;}
  return true;
}

static bool readMeta(fs::FS& fs,Meta& m,const char* path){
  String mp=metaPathFor(path);
  File in=fs.open(mp,FILE_READ);
  if(!in||in.size()!=sizeof(Meta)){if(in)in.close();return false;}
  size_t n=in.read(reinterpret_cast<uint8_t*>(&m),sizeof(Meta));
  in.close();
  return n==sizeof(Meta)&&metaValid(m);
}

static bool readPhysical(File& in,uint16_t index,Entry& e){
  size_t pos=(size_t)index*sizeof(Entry);
  if(!in.seek(pos,SeekSet))return false;
  size_t n=in.read(reinterpret_cast<uint8_t*>(&e),sizeof(Entry));
  return n==sizeof(Entry)&&valid(e);
}

static bool rebuildMeta(fs::FS& fs,Meta& m,const char* path){
  memset(&m,0,sizeof(m));
  m.magic=META_MAGIC;m.version=META_VERSION;m.size=sizeof(Meta);m.nextSequence=1;

  File in=fs.open(path,FILE_READ);
  if(!in){m.nextIndex=0;m.checksum=metaChecksum(m);return true;}

  size_t physical=min((size_t)MAX_RECORDS,in.size()/sizeof(Entry));
  uint32_t newest=0;
  uint16_t validCount=0;
  for(uint16_t i=0;i<physical;i++){
    Entry e={};
    if(!readPhysical(in,i,e))continue;
    validCount++;
    if(e.sequence>newest)newest=e.sequence;
  }
  in.close();

  m.count=validCount;
  m.nextSequence=newest?newest+1U:1U;
  if(m.count<MAX_RECORDS){m.nextIndex=m.count;m.rolloverCount=0;}
  else{
    m.nextIndex=newest?(uint16_t)(newest%MAX_RECORDS):0;
    m.rolloverCount=newest>m.count?newest-m.count:0;
  }
  m.checksum=metaChecksum(m);
  saveMeta(fs,m,path);
  return true;
}

static bool loadMeta(fs::FS& fs,Meta& m,const char* path){
  if(readMeta(fs,m,path))return true;
  return rebuildMeta(fs,m,path);
}

} // namespace

void prepare(Entry& e){
  e.magic=ENTRY_MAGIC;e.version=ENTRY_VERSION;e.size=sizeof(Entry);
  e.checksum=entryChecksum(e);
}

bool valid(const Entry& e){
  return e.magic==ENTRY_MAGIC &&
         e.version==ENTRY_VERSION &&
         e.size==sizeof(Entry) &&
         e.checksum==entryChecksum(e);
}

bool stats(fs::FS& fs,Stats& out,const char* path){
  Meta m={};if(!loadMeta(fs,m,path))return false;
  out.count=m.count;out.capacity=MAX_RECORDS;
  out.newestSequence=m.nextSequence>1?m.nextSequence-1U:0U;
  out.rolloverCount=m.rolloverCount;
  return true;
}

uint16_t count(fs::FS& fs,const char* path){
  Stats s={};return stats(fs,s,path)?s.count:0;
}

bool append(fs::FS& fs,Entry& e,const char* path){
  Meta m={};if(!loadMeta(fs,m,path))return false;
  e.sequence=m.nextSequence;
  prepare(e);

  bool ok=false;
  if(m.count<MAX_RECORDS){
    File out=fs.open(path,FILE_APPEND);
    if(!out)return false;
    size_t n=out.write(reinterpret_cast<const uint8_t*>(&e),sizeof(Entry));
    out.flush();out.close();ok=n==sizeof(Entry);
  }else{
    File out=fs.open(path,"r+");
    if(!out)return false;
    size_t pos=(size_t)m.nextIndex*sizeof(Entry);
    if(out.seek(pos,SeekSet)){
      size_t n=out.write(reinterpret_cast<const uint8_t*>(&e),sizeof(Entry));
      out.flush();ok=n==sizeof(Entry);
    }
    out.close();
  }
  if(!ok)return false;

  m.nextSequence++;
  if(m.count<MAX_RECORDS){
    m.count++;
    m.nextIndex=m.count<MAX_RECORDS?m.count:0;
  }else{
    m.nextIndex=(uint16_t)((m.nextIndex+1U)%MAX_RECORDS);
    m.rolloverCount++;
  }
  return saveMeta(fs,m,path);
}

bool readNewest(fs::FS& fs,uint16_t newestOffset,Entry& e,const char* path){
  Meta m={};if(!loadMeta(fs,m,path))return false;
  if(m.count==0||newestOffset>=m.count)return false;

  uint32_t newest=m.nextSequence-1U;
  uint32_t target=newest-newestOffset;
  uint16_t physical=(uint16_t)((target-1U)%MAX_RECORDS);

  File in=fs.open(path,FILE_READ);
  if(!in)return false;
  bool ok=readPhysical(in,physical,e);
  in.close();
  return ok&&e.sequence==target;
}

bool clear(fs::FS& fs,const char* path){
  bool ok=true;
  if(fs.exists(path))ok=fs.remove(path)&&ok;
  String mp=metaPathFor(path),tmp=mp+".tmp";
  if(fs.exists(mp))ok=fs.remove(mp)&&ok;
  if(fs.exists(tmp))ok=fs.remove(tmp)&&ok;
  return ok;
}

} // namespace RiotFlowHistoryStore
