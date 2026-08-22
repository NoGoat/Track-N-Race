#include "TnrdV4.h"
#include "TnrdCodec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <zlib.h>
#include <zstd.h>

namespace tnrp::detail {
namespace {

constexpr size_t HEADER_SIZE = 96;
constexpr size_t LAP_ENTRY_SIZE = 24;
constexpr size_t CHUNK_ENTRY_SIZE = 48;
constexpr size_t FOOTER_SIZE = 32;
constexpr size_t METADATA_PREFIX_SIZE = 16;
constexpr size_t CHUNK_PREFIX_SIZE = 32;
constexpr uint32_t CHUNK_MAGIC = 0x344B4843u;
constexpr uint32_t FOOTER_MAGIC = 0x34444E45u;
constexpr uint32_t METADATA_MAGIC = 0x3454454Du;
constexpr uint32_t MAX_LAPS = 100'000;
constexpr uint32_t MAX_CHUNKS = 1'000'000;
constexpr uint64_t MAX_CHUNK_PLAIN = 512ull * 1024ull * 1024ull;
constexpr uint64_t MAX_METADATA_BYTES = 16ull * 1024ull * 1024ull;
constexpr uint64_t MAX_SUMMARY_BYTES = 64ull * 1024ull * 1024ull;
constexpr size_t DEFAULT_CACHE_BYTES = 64ull * 1024ull * 1024ull;
constexpr size_t MAX_PARALLEL_CHUNKS = 8;
const std::array<uint8_t, 8> MAGIC{{'T','N','R','D','_','V','4','\0'}};

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i=0;i<4;++i) b.push_back((uint8_t)(v >> (8*i))); }
void put64(std::vector<uint8_t>& b, uint64_t v) { for (int i=0;i<8;++i) b.push_back((uint8_t)(v >> (8*i))); }
void putFloat(std::vector<uint8_t>& b, float v) { uint32_t u{}; std::memcpy(&u,&v,4); put32(b,u); }
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
uint64_t get64(const uint8_t* p) { uint64_t v=0; for(int i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i); return v; }
float getFloat(const uint8_t* p) { uint32_t u=get32(p); float v{}; std::memcpy(&v,&u,4); return v; }

void fail(std::string* out, std::string message) { if(out)*out=std::move(message); }
bool writeAll(std::FILE* f,const void* p,size_t n){return n==0||std::fwrite(p,1,n,f)==n;}
bool seek(std::FILE* f,uint64_t off){
#ifdef _WIN32
    return off<=(uint64_t)std::numeric_limits<__int64>::max()&&_fseeki64(f,(__int64)off,SEEK_SET)==0;
#else
    return off<=(uint64_t)std::numeric_limits<off_t>::max()&&fseeko(f,(off_t)off,SEEK_SET)==0;
#endif
}
uint64_t tell(std::FILE* f){
#ifdef _WIN32
    const auto p=_ftelli64(f); return p<0?UINT64_MAX:(uint64_t)p;
#else
    const auto p=ftello(f); return p<0?UINT64_MAX:(uint64_t)p;
#endif
}
bool seekEnd(std::FILE* f){
#ifdef _WIN32
    return _fseeki64(f,0,SEEK_END)==0;
#else
    return fseeko(f,0,SEEK_END)==0;
#endif
}
bool readAt(std::FILE* f,uint64_t off,void* p,size_t n){return seek(f,off)&&(n==0||std::fread(p,1,n,f)==n);}
bool rangeOk(uint64_t size,uint64_t off,uint64_t bytes){return off<=size&&bytes<=size-off;}

uint8_t rowType(std::string_view s){
    auto p=s.find("\"type\":\"");if(p==s.npos)return 0;p+=8;
    auto is=[&](std::string_view x){return s.substr(p,x.size())==x;};
    if(is("telemetry"))return 1;if(is("status"))return 2;if(is("damage"))return 3;if(is("lap\""))return 4;
    if(is("session\""))return 5;if(is("race_event"))return 6;if(is("timing"))return 7;if(is("participants"))return 8;
    if(is("all_status"))return 9;if(is("tyre_sets"))return 10;if(is("motion\""))return 11;if(is("motion_ex"))return 12;
    if(is("positions"))return 13;if(is("session_history_fastest"))return 14;return 0;
}
float scanTime(std::string_view s){auto p=s.find("\"session_time\":");if(p==s.npos)return -1;p+=15;return std::strtof(s.data()+p,nullptr);}
int scanInt(std::string_view s,std::string_view key,int fallback=0){auto p=s.find(key);if(p==s.npos)return fallback;p+=key.size();return (int)std::strtol(s.data()+p,nullptr,10);}
double scanDouble(std::string_view s,std::string_view key,double fallback=0){auto p=s.find(key);if(p==s.npos)return fallback;p+=key.size();return std::strtod(s.data()+p,nullptr);}
std::string withSessionTime(std::string_view source,float time){std::string stored(source);if(time<0||scanTime(stored)>=0)return stored;const size_t objectStart=stored.find('{');if(objectStart==std::string::npos)return stored;char number[32];std::snprintf(number,sizeof(number),"%.9g",(double)time);stored.insert(objectStart+1,std::string("\"session_time\":")+number+",");return stored;}

struct Lap { uint32_t num{};float start{},end{};uint32_t timeMs{},flags{}; };
struct Chunk { uint32_t lap{};uint16_t type{},flags{};uint64_t offset{},compressed{},plain{};uint32_t rows{},crc{};uint64_t sequence{}; };

std::vector<uint8_t> makeHeader(uint64_t metaOff,uint64_t metaSize,uint64_t lapOff,uint32_t lapCount,
                                uint64_t chunkOff,uint32_t chunkCount,uint64_t footerOff,
                                uint64_t summaryOff,uint64_t summarySize){
    std::vector<uint8_t>b;b.insert(b.end(),MAGIC.begin(),MAGIC.end());put16(b,4);put16(b,(uint16_t)HEADER_SIZE);put32(b,0);
    put64(b,metaOff);put64(b,metaSize);put64(b,lapOff);put32(b,lapCount);put32(b,(uint32_t)LAP_ENTRY_SIZE);
    put64(b,chunkOff);put32(b,chunkCount);put32(b,(uint32_t)CHUNK_ENTRY_SIZE);put64(b,footerOff);
    put64(b,summaryOff);put64(b,summarySize);put32(b,0);put32(b,0);
    const uint32_t crc=(uint32_t)::crc32(0,b.data(),88);b[88]=(uint8_t)crc;b[89]=(uint8_t)(crc>>8);b[90]=(uint8_t)(crc>>16);b[91]=(uint8_t)(crc>>24);return b;
}

uint32_t controlCrc(std::string_view summary,const std::vector<uint8_t>& laps,const std::vector<uint8_t>& chunks){
    uint32_t crc=(uint32_t)::crc32(0,(const Bytef*)summary.data(),(uInt)summary.size());crc=(uint32_t)::crc32(crc,laps.data(),(uInt)laps.size());return (uint32_t)::crc32(crc,chunks.data(),(uInt)chunks.size());
}

bool splitRows(std::string_view plain,uint8_t expectedType,uint64_t sequence,std::vector<V4TimedRow>& out,uint32_t expectedCount,std::string* errorOut,const std::vector<float>* inferredTimes=nullptr){
    size_t pos=0;uint32_t count=0;
    while(pos<plain.size()){
        size_t nl=plain.find('\n',pos);if(nl==std::string_view::npos)nl=plain.size();
        if(nl>pos){auto line=plain.substr(pos,nl-pos);const uint8_t actual=rowType(line);if(actual!=expectedType){fail(errorOut,"V4 chunk contains the wrong row type");return false;}float time=scanTime(line);if(time<0&&inferredTimes&&count<inferredTimes->size())time=(*inferredTimes)[count];out.push_back({time,actual,sequence,std::string(line)});++count;}
        if(nl==plain.size())break;pos=nl+1;
    }
    if(count!=expectedCount){fail(errorOut,"V4 chunk row count mismatch");return false;}return true;
}

void sortRows(std::vector<V4TimedRow>& rows){std::stable_sort(rows.begin(),rows.end(),[](const auto&a,const auto&b){if(a.sessionTime!=b.sessionTime)return a.sessionTime<b.sessionTime;return a.sequence<b.sequence;});}
}

struct TnrdV4Writer::Impl {
    struct Builder{std::string plain;bool dirty{};};
    std::FILE* file{};uint64_t metadataOffset{HEADER_SIZE+METADATA_PREFIX_SIZE},metadataSize{};uint32_t currentLap{},highestLap{};float lastTime{};bool haveTime{},timedSession{},timedLapActive{};
    std::map<uint32_t,Lap> laps;std::map<std::pair<uint32_t,uint16_t>,Builder> builders;std::vector<Chunk> chunks;
    V4ControlSummary summary;std::map<uint32_t,V4LapStatusSummary> statusByLap;uint64_t nextSequence{1};

    bool writeChunk(const std::pair<uint32_t,uint16_t>& key,const std::string& plain,std::string* errorOut){
        if(plain.empty())return true;if(plain.size()>MAX_CHUNK_PLAIN){fail(errorOut,"V4 chunk exceeds safety limit");return false;}if(chunks.size()>=MAX_CHUNKS){fail(errorOut,"V4 chunk count exceeds safety limit");return false;}
        std::vector<uint8_t> compressed(ZSTD_compressBound(plain.size()));ZSTD_CCtx* ctx=ZSTD_createCCtx();
        if(!ctx){fail(errorOut,"could not allocate V4 compression context");return false;}ZSTD_CCtx_setParameter(ctx,ZSTD_c_compressionLevel,3);ZSTD_CCtx_setParameter(ctx,ZSTD_c_checksumFlag,1);
        const size_t n=ZSTD_compress2(ctx,compressed.data(),compressed.size(),plain.data(),plain.size());ZSTD_freeCCtx(ctx);
        if(ZSTD_isError(n)){fail(errorOut,ZSTD_getErrorName(n));return false;}if(!seekEnd(file)){fail(errorOut,"could not seek to append V4 chunk");return false;}
        Chunk c;c.lap=key.first;c.type=key.second;c.offset=tell(file)+CHUNK_PREFIX_SIZE;c.compressed=n;c.plain=plain.size();c.rows=(uint32_t)std::count(plain.begin(),plain.end(),'\n');c.crc=(uint32_t)::crc32(0,(const Bytef*)plain.data(),(uInt)plain.size());c.sequence=nextSequence++;
        std::vector<uint8_t> prefix;put32(prefix,CHUNK_MAGIC);put32(prefix,c.lap);put16(prefix,c.type);put16(prefix,0);put64(prefix,c.compressed);put64(prefix,c.plain);put32(prefix,c.rows);
        if(!writeAll(file,prefix.data(),prefix.size())||!writeAll(file,compressed.data(),n)){fail(errorOut,"failed while appending V4 chunk");return false;}chunks.push_back(c);return true;
    }

    bool writeSnapshot(std::string* errorOut){
        if(!seekEnd(file)){fail(errorOut,"could not seek to append V4 checkpoint");return false;}
        if(laps.size()>MAX_LAPS||chunks.size()>MAX_CHUNKS){fail(errorOut,"V4 control table exceeds its format limit");return false;}
        summary.lapStatus.clear();for(const auto& [lap,status]:statusByLap)summary.lapStatus.push_back(status);
        const std::string summaryJson=writeJson(summary);const uint64_t summaryOffset=tell(file);
        if(summaryJson.size()>MAX_SUMMARY_BYTES||summaryJson.size()>UINT32_MAX){fail(errorOut,"V4 control summary exceeds its format limit");return false;}
        if(!writeAll(file,summaryJson.data(),summaryJson.size())){fail(errorOut,"failed to append V4 control summary");return false;}
        const uint64_t lapOffset=tell(file);std::vector<uint8_t> lapBytes;
        for(const auto& [n,l]:laps){put32(lapBytes,n);putFloat(lapBytes,l.start);putFloat(lapBytes,l.end);put32(lapBytes,l.timeMs);put32(lapBytes,l.flags);put32(lapBytes,0);}
        if(!writeAll(file,lapBytes.data(),lapBytes.size())){fail(errorOut,"failed to append V4 lap table");return false;}
        const uint64_t chunkOffset=tell(file);std::vector<uint8_t> dir;
        for(const auto& c:chunks){put32(dir,c.lap);put16(dir,c.type);put16(dir,c.flags);put64(dir,c.offset);put64(dir,c.compressed);put64(dir,c.plain);put32(dir,c.rows);put32(dir,c.crc);put64(dir,c.sequence);}
        if(!writeAll(file,dir.data(),dir.size())){fail(errorOut,"failed to append V4 chunk directory");return false;}
        const uint64_t footerOffset=tell(file);std::vector<uint8_t> footer;put32(footer,FOOTER_MAGIC);put16(footer,4);put16(footer,FOOTER_SIZE);put64(footer,lapOffset);put64(footer,chunkOffset);put32(footer,controlCrc(summaryJson,lapBytes,dir));put32(footer,(uint32_t)summaryJson.size());
        if(!writeAll(file,footer.data(),footer.size())||std::fflush(file)!=0){fail(errorOut,"failed to commit V4 checkpoint footer");return false;}
        const auto header=makeHeader(metadataOffset,metadataSize,lapOffset,(uint32_t)laps.size(),chunkOffset,(uint32_t)chunks.size(),footerOffset,summaryOffset,summaryJson.size());
        if(!seek(file,0)||!writeAll(file,header.data(),header.size())||std::fflush(file)!=0){fail(errorOut,"failed to commit V4 checkpoint header");return false;}return seekEnd(file);
    }

    bool decode(const Chunk& c,std::string& plain,std::string* errorOut){
        std::vector<uint8_t> compressed((size_t)c.compressed);plain.assign((size_t)c.plain,'\0');if(!readAt(file,c.offset,compressed.data(),compressed.size())){fail(errorOut,"could not read V4 chunk during flashback");return false;}
        const size_t got=ZSTD_decompress(plain.data(),plain.size(),compressed.data(),compressed.size());if(ZSTD_isError(got)||got!=c.plain){fail(errorOut,"could not decompress V4 chunk during flashback");return false;}return true;
    }
};

TnrdV4Writer::TnrdV4Writer():impl_(std::make_unique<Impl>()){}
TnrdV4Writer::~TnrdV4Writer(){if(impl_&&impl_->file){std::string ignored;(void)finish(&ignored);}}
bool TnrdV4Writer::isOpen()const{return impl_&&impl_->file;}

bool TnrdV4Writer::open(const std::string& path,const HeaderRow& sourceHeader,std::string* errorOut){
    if(isOpen()){fail(errorOut,"V4 writer is already open");return false;}
    // A writer object may be reused after finish(). Do not carry directory,
    // lap-summary, or segment-sequence state into the next recording.
    impl_=std::make_unique<Impl>();impl_->file=openTnrdFile(path,"w+b");if(!impl_->file){const int openError=errno;fail(errorOut,std::string("could not create V4 file")+(openError?": "+std::string(std::strerror(openError)):std::string{}));return false;}
    HeaderRow header=sourceHeader;header.magic="TNRD_V4";header.compression="zstd";impl_->timedSession=header.session_type>=1&&header.session_type<=14;const std::string metadata=writeJson(header);if(metadata.size()>MAX_METADATA_BYTES){fail(errorOut,"V4 session metadata exceeds its format limit");std::fclose(impl_->file);impl_->file=nullptr;return false;}impl_->metadataSize=metadata.size();std::vector<uint8_t> zero(HEADER_SIZE),prefix;put32(prefix,METADATA_MAGIC);put32(prefix,(uint32_t)::crc32(0,(const Bytef*)metadata.data(),(uInt)metadata.size()));put64(prefix,metadata.size());
    if(!writeAll(impl_->file,zero.data(),zero.size())||!writeAll(impl_->file,prefix.data(),prefix.size())||!writeAll(impl_->file,metadata.data(),metadata.size())||!impl_->writeSnapshot(errorOut)){std::fclose(impl_->file);impl_->file=nullptr;return false;}return true;
}

bool TnrdV4Writer::append(const std::vector<V4SourceRow>& rows,std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V4 writer is not open");return false;}
    for(const auto&r:rows){std::string_view source=r.line;while(!source.empty()&&(source.back()=='\n'||source.back()=='\r'))source.remove_suffix(1);if(source.empty())continue;const float t=r.sessionTime>=0?r.sessionTime:scanTime(source);const std::string stored=withSessionTime(source,t);const std::string_view line=stored;const uint8_t type=rowType(line);if(t>=0){impl_->lastTime=std::max(impl_->lastTime,t);if(!impl_->haveTime){impl_->summary.startSessionTime=t;impl_->haveTime=true;}impl_->summary.totalSessionTime=std::max(impl_->summary.totalSessionTime,t);}
        if(type==4){const int n=scanInt(line,"\"lap_num\":",(int)impl_->currentLap),driver=scanInt(line,"\"driver_status\":",-1);const bool garageAware=impl_->timedSession&&driver>=0;if(garageAware&&driver!=1){if(impl_->currentLap)impl_->laps[impl_->currentLap].end=std::max(impl_->laps[impl_->currentLap].end,t);impl_->currentLap=0;impl_->timedLapActive=false;}else if(n>0){const uint32_t next=(uint32_t)n;const bool mayAdvance=next>impl_->highestLap;const bool mayStartTimed=garageAware&&!impl_->timedLapActive&&next>=impl_->highestLap;if((mayAdvance||mayStartTimed)&&(next!=impl_->currentLap||!impl_->timedLapActive)){float start=t;const int curMs=scanInt(line,"\"current_lap_ms\":",0);if(curMs>0)start=t-curMs/1000.0f;if(impl_->currentLap&&next>impl_->currentLap)impl_->laps[impl_->currentLap].end=start;impl_->currentLap=next;impl_->highestLap=std::max(impl_->highestLap,next);impl_->timedLapActive=garageAware;auto& lap=impl_->laps[next];lap.num=next;lap.start=start;lap.end=t;lap.timeMs=0;lap.flags=0;const int lastMs=scanInt(line,"\"last_lap_ms\":",0);auto prev=impl_->laps.find(next-1);if(prev!=impl_->laps.end()&&lastMs>0){prev->second.timeMs=(uint32_t)lastMs;prev->second.flags|=1;}}}}
        if(type==2){auto& s=impl_->statusByLap[impl_->currentLap];s.lapNumber=impl_->currentLap;s.sessionTime=t;s.ersPct=scanDouble(line,"\"ers_pct\":",s.ersPct);s.tyreCompound=scanInt(line,"\"tyre_compound\":",s.tyreCompound);s.visualCompound=scanInt(line,"\"visual_compound\":",s.visualCompound);if(impl_->summary.initialFuelKg<0)impl_->summary.initialFuelKg=scanDouble(line,"\"fuel_kg\":",-1);}
        if(type==14){const int lapNum=scanInt(line,"\"latest_lap_num\":",0),lapMs=scanInt(line,"\"latest_lap_time_ms\":",0);auto lap=impl_->laps.find((uint32_t)std::max(lapNum,0));if(lap!=impl_->laps.end()&&lapMs>0){lap->second.timeMs=(uint32_t)lapMs;lap->second.flags|=1;}}
        if(type==6)impl_->summary.events.emplace_back(line);
        auto& b=impl_->builders[{impl_->currentLap,(uint16_t)type}];b.plain.append(line);if(b.plain.empty()||b.plain.back()!='\n')b.plain.push_back('\n');b.dirty=true;if(impl_->currentLap)impl_->laps[impl_->currentLap].end=std::max(impl_->laps[impl_->currentLap].end,t);
    }return true;
}

bool TnrdV4Writer::checkpoint(std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V4 writer is not open");return false;}
    for(auto it=impl_->builders.begin();it!=impl_->builders.end();){if(it->second.dirty&&!impl_->writeChunk(it->first,it->second.plain,errorOut))return false;it=impl_->builders.erase(it);}return impl_->writeSnapshot(errorOut);
}

bool TnrdV4Writer::rewind(float sessionTime,std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V4 writer is not open");return false;}uint32_t targetLap=0;for(const auto&[n,l]:impl_->laps)if(l.start<=sessionTime)targetLap=n;
    auto filter=[&](std::string_view input){std::string kept;size_t p=0;while(p<input.size()){size_t nl=input.find('\n',p);if(nl==std::string_view::npos)nl=input.size();auto line=input.substr(p,nl-p);if(!line.empty()&&scanTime(line)<=sessionTime){kept.append(line);kept.push_back('\n');}if(nl==input.size())break;p=nl+1;}return kept;};
    std::map<std::pair<uint32_t,uint16_t>,std::string> recovered;for(auto it=impl_->chunks.begin();it!=impl_->chunks.end();){if(it->lap>targetLap){it=impl_->chunks.erase(it);continue;}if(it->lap==targetLap){std::string plain;if(!impl_->decode(*it,plain,errorOut))return false;recovered[{it->lap,it->type}]+=plain;it=impl_->chunks.erase(it);continue;}++it;}for(auto&[key,plain]:recovered){auto&b=impl_->builders[key];b.plain=std::move(plain)+b.plain;b.dirty=true;}
    for(auto it=impl_->builders.begin();it!=impl_->builders.end();){if(it->first.first>targetLap){it=impl_->builders.erase(it);continue;}if(it->first.first==targetLap){it->second.plain=filter(it->second.plain);it->second.dirty=true;}++it;}
    for(auto it=impl_->laps.upper_bound(targetLap);it!=impl_->laps.end();)it=impl_->laps.erase(it);for(auto it=impl_->statusByLap.upper_bound(targetLap);it!=impl_->statusByLap.end();)it=impl_->statusByLap.erase(it);
    impl_->statusByLap.erase(targetLap);auto statusBuilder=impl_->builders.find(std::make_pair(targetLap,(uint16_t)2));if(statusBuilder!=impl_->builders.end()){size_t pos=0;while(pos<statusBuilder->second.plain.size()){size_t nl=statusBuilder->second.plain.find('\n',pos);if(nl==std::string::npos)nl=statusBuilder->second.plain.size();auto line=std::string_view(statusBuilder->second.plain).substr(pos,nl-pos);if(!line.empty()){auto&s=impl_->statusByLap[targetLap];s.lapNumber=targetLap;s.sessionTime=scanTime(line);s.ersPct=scanDouble(line,"\"ers_pct\":",s.ersPct);s.tyreCompound=scanInt(line,"\"tyre_compound\":",s.tyreCompound);s.visualCompound=scanInt(line,"\"visual_compound\":",s.visualCompound);}if(nl==statusBuilder->second.plain.size())break;pos=nl+1;}}
    impl_->summary.events.erase(std::remove_if(impl_->summary.events.begin(),impl_->summary.events.end(),[&](const std::string&e){return scanTime(e)>sessionTime;}),impl_->summary.events.end());impl_->summary.totalSessionTime=sessionTime;if(targetLap)impl_->laps[targetLap].end=sessionTime;impl_->currentLap=targetLap;impl_->highestLap=targetLap;impl_->timedLapActive=false;impl_->lastTime=sessionTime;return checkpoint(errorOut);
}

bool TnrdV4Writer::finish(std::string* errorOut){if(!isOpen())return true;const bool ok=checkpoint(errorOut);const bool closeOk=std::fclose(impl_->file)==0;impl_->file=nullptr;if(!closeOk&&ok)fail(errorOut,"failed to close V4 file");return ok&&closeOk;}
bool writeTnrdV4(const std::string& path,const HeaderRow& header,const std::vector<V4SourceRow>& rows,std::string* errorOut){TnrdV4Writer writer;return writer.open(path,header,errorOut)&&writer.append(rows,errorOut)&&writer.finish(errorOut);}

struct TnrdV4Archive::Impl {
    struct CacheEntry{std::shared_ptr<std::string> plain;std::list<size_t>::iterator lru;};
    struct TimeBounds{float first{};float last{};bool known{};};
    struct ChunkResult{bool ok{};bool permanentError{};std::shared_ptr<std::string> plain;std::string error;};
    struct RowsResult{bool ok{true};std::string error;std::vector<V4TimedRow> rows;};
    class Executor {
    public:
        ~Executor(){stop();}
        template<class Fn> auto submit(const std::string& path,Fn&& fn){
            using R=std::invoke_result_t<Fn,std::FILE*>;
            auto task=std::make_shared<std::packaged_task<R(std::FILE*)>>(std::forward<Fn>(fn));
            auto future=task->get_future();ensureStarted(path);
            {std::lock_guard<std::mutex> lock(mutex_);jobs_.push([task](std::FILE* file){(*task)(file);});}
            cv_.notify_one();return future;
        }
        void stop(){
            {std::lock_guard<std::mutex> lock(mutex_);if(workers_.empty())return;stopping_=true;}
            cv_.notify_all();for(auto& worker:workers_)if(worker.joinable())worker.join();
            std::lock_guard<std::mutex> lock(mutex_);workers_.clear();while(!jobs_.empty())jobs_.pop();stopping_=false;
        }
    private:
        void ensureStarted(const std::string& path){
            std::lock_guard<std::mutex> lock(mutex_);if(!workers_.empty())return;workers_.reserve(MAX_PARALLEL_CHUNKS);
            for(size_t i=0;i<MAX_PARALLEL_CHUNKS;++i)workers_.emplace_back([this,path]{
                std::FILE* file=openTnrdFile(path,"rb");
                for(;;){std::function<void(std::FILE*)> job;{std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[this]{return stopping_||!jobs_.empty();});if(stopping_&&jobs_.empty())break;job=std::move(jobs_.front());jobs_.pop();}job(file);}
                if(file)std::fclose(file);
            });
        }
        std::mutex mutex_;std::condition_variable cv_;bool stopping_{};
        std::queue<std::function<void(std::FILE*)>> jobs_;std::vector<std::thread> workers_;
    };
    std::FILE* file{};uint64_t fileSize{};HeaderRow header;std::vector<V4LapInfo> laps;std::vector<V4ChunkInfo> chunks;V4ControlSummary summary;
    std::map<std::pair<uint32_t,uint16_t>,std::vector<size_t>> chunkIndex;
    std::vector<TimeBounds> chunkTimeBounds;
    std::string path;Executor executor;mutable std::mutex stateMutex;
    size_t cacheLimit{DEFAULT_CACHE_BYTES},cacheBytes{},activeLoads{},peakActiveLoads{};uint64_t decompressions{};std::list<size_t> lru;std::unordered_map<size_t,CacheEntry> cache;
    std::unordered_map<size_t,std::shared_future<ChunkResult>> inFlight;std::unordered_map<size_t,std::string> failedChunks;

    void clear(){
        executor.stop();if(file)std::fclose(file);file=nullptr;fileSize=0;path.clear();laps.clear();chunks.clear();summary={};chunkIndex.clear();chunkTimeBounds.clear();
        std::lock_guard<std::mutex> lock(stateMutex);cache.clear();lru.clear();inFlight.clear();failedChunks.clear();cacheBytes=0;activeLoads=0;peakActiveLoads=0;decompressions=0;
    }
    bool loadChunk(size_t index,std::FILE* reader,std::shared_ptr<std::string>& result,std::string* errorOut){
        std::shared_future<ChunkResult> shared;std::shared_ptr<std::promise<ChunkResult>> promise;bool producer=false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            auto hit=cache.find(index);if(hit!=cache.end()){lru.erase(hit->second.lru);lru.push_front(index);hit->second.lru=lru.begin();result=hit->second.plain;return true;}
            auto failed=failedChunks.find(index);if(failed!=failedChunks.end()){fail(errorOut,failed->second);return false;}
            auto active=inFlight.find(index);if(active!=inFlight.end())shared=active->second;
            else{promise=std::make_shared<std::promise<ChunkResult>>();shared=promise->get_future().share();inFlight.emplace(index,shared);producer=true;}
        }
        if(!producer){const auto loaded=shared.get();if(!loaded.ok){fail(errorOut,loaded.error);return false;}result=loaded.plain;return true;}
        {std::lock_guard<std::mutex> lock(stateMutex);++activeLoads;peakActiveLoads=std::max(peakActiveLoads,activeLoads);}
        ChunkResult loaded;
        if(!reader)loaded.error="could not open a parallel V4 reader handle";
        else if(index>=chunks.size()){loaded.error="V4 chunk index is out of bounds";loaded.permanentError=true;}
        else{
            const auto& c=chunks[index];std::array<uint8_t,CHUNK_PREFIX_SIZE> prefix{};
            if(!readAt(reader,c.offset-CHUNK_PREFIX_SIZE,prefix.data(),prefix.size())||get32(prefix.data())!=CHUNK_MAGIC||get32(prefix.data()+4)!=c.lapNumber||get16(prefix.data()+8)!=c.rowType||get16(prefix.data()+10)!=c.flags||get64(prefix.data()+12)!=c.compressedSize||get64(prefix.data()+20)!=c.uncompressedSize||get32(prefix.data()+28)!=c.rowCount){loaded.error="V4 chunk prefix does not match its directory entry";loaded.permanentError=true;}
            else{
                std::vector<uint8_t> compressed((size_t)c.compressedSize);auto plain=std::make_shared<std::string>((size_t)c.uncompressedSize,'\0');
                if(!readAt(reader,c.offset,compressed.data(),compressed.size())){loaded.error="truncated V4 chunk at lap "+std::to_string(c.lapNumber);loaded.permanentError=true;}
                else{
                    {std::lock_guard<std::mutex> lock(stateMutex);++decompressions;}
                    const size_t got=ZSTD_decompress(plain->data(),plain->size(),compressed.data(),compressed.size());
                    if(ZSTD_isError(got)||got!=c.uncompressedSize||(uint32_t)::crc32(0,(const Bytef*)plain->data(),(uInt)plain->size())!=c.checksum){loaded.error="V4 chunk integrity check failed for lap "+std::to_string(c.lapNumber)+" type "+std::to_string(c.rowType);loaded.permanentError=true;}
                    else{loaded.ok=true;loaded.plain=std::move(plain);}
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            --activeLoads;
            if(loaded.ok&&loaded.plain->size()<=cacheLimit){while(cacheBytes+loaded.plain->size()>cacheLimit&&!lru.empty()){const size_t old=lru.back();lru.pop_back();auto it=cache.find(old);cacheBytes-=it->second.plain->size();cache.erase(it);}lru.push_front(index);cache[index]={loaded.plain,lru.begin()};cacheBytes+=loaded.plain->size();}
            else if(!loaded.ok&&loaded.permanentError)failedChunks[index]=loaded.error;
        }
        promise->set_value(loaded);{std::lock_guard<std::mutex> lock(stateMutex);inFlight.erase(index);}
        if(!loaded.ok){fail(errorOut,loaded.error);return false;}result=std::move(loaded.plain);return true;
    }
    bool splitChunk(size_t index,std::FILE* reader,std::vector<V4TimedRow>& out,std::string* errorOut,std::shared_ptr<std::string> plain={}){
        const auto& c=chunks[index];if(!plain&&!loadChunk(index,reader,plain,errorOut))return false;
        const size_t firstOutput=out.size();
        const std::string_view plainView(*plain);const size_t firstEnd=plainView.find('\n');const std::string_view first=plainView.substr(0,firstEnd);
        if(scanTime(first)>=0){if(!splitRows(*plain,(uint8_t)c.rowType,c.sequence,out,c.rowCount,errorOut))return false;rememberBounds(index,out,firstOutput);return true;}

        // Early V4 writers kept the external packet timestamp only in memory.
        // Recover those files lazily from the matching timed hot-row segment.
        // Positions are emitted from Motion packets, so equal-sized Motion
        // chunks provide exact per-row times; other sparse families use the
        // nearest Telemetry segment and interpolate within its time bounds.
        const uint16_t referenceType=c.rowType==13?11:1;size_t best=SIZE_MAX;bool bestExact=false;uint64_t bestDistance=UINT64_MAX;
        for(size_t i=0;i<chunks.size();++i){const auto& candidate=chunks[i];if(candidate.lapNumber!=c.lapNumber||candidate.rowType!=referenceType)continue;const bool exact=candidate.rowCount==c.rowCount;const uint64_t distance=candidate.sequence>c.sequence?candidate.sequence-c.sequence:c.sequence-candidate.sequence;if(best==SIZE_MAX||(exact&&!bestExact)||(exact==bestExact&&distance<bestDistance)){best=i;bestExact=exact;bestDistance=distance;}}
        std::vector<float> referenceTimes;if(best!=SIZE_MAX){std::shared_ptr<std::string> reference;if(!loadChunk(best,reader,reference,errorOut))return false;size_t pos=0;while(pos<reference->size()){size_t nl=reference->find('\n',pos);if(nl==std::string::npos)nl=reference->size();if(nl>pos){const float time=scanTime(std::string_view(*reference).substr(pos,nl-pos));if(time>=0)referenceTimes.push_back(time);}if(nl==reference->size())break;pos=nl+1;}}
        std::vector<float> inferred(c.rowCount);if(referenceTimes.size()==c.rowCount)inferred=std::move(referenceTimes);else{float from=0,to=0;if(!referenceTimes.empty()){from=referenceTimes.front();to=referenceTimes.back();}else{auto lap=std::find_if(laps.begin(),laps.end(),[&](const auto& value){return value.lapNumber==c.lapNumber;});if(lap!=laps.end()){from=lap->startSessionTime;to=lap->endSessionTime;}}for(uint32_t i=0;i<c.rowCount;++i)inferred[i]=c.rowCount>1?from+(to-from)*(float)i/(float)(c.rowCount-1):from;}
        if(!splitRows(*plain,(uint8_t)c.rowType,c.sequence,out,c.rowCount,errorOut,&inferred))return false;rememberBounds(index,out,firstOutput);return true;
    }
    void rememberBounds(size_t index,const std::vector<V4TimedRow>& rows,size_t firstOutput){
        if(index>=chunkTimeBounds.size()||firstOutput>=rows.size())return;float first=std::numeric_limits<float>::infinity(),last=-std::numeric_limits<float>::infinity();for(size_t i=firstOutput;i<rows.size();++i)if(std::isfinite(rows[i].sessionTime)){first=std::min(first,rows[i].sessionTime);last=std::max(last,rows[i].sessionTime);}if(std::isfinite(first)&&std::isfinite(last)){std::lock_guard<std::mutex> lock(stateMutex);chunkTimeBounds[index]={first,last,true};}
    }
    TimeBounds bounds(size_t index)const{std::lock_guard<std::mutex> lock(stateMutex);return chunkTimeBounds[index];}
    template<class Fn>std::vector<RowsResult> runChunkJobs(const std::vector<size_t>& indices,Fn fn){
        using Pending=std::pair<size_t,std::future<RowsResult>>;std::deque<Pending> pending;std::vector<RowsResult> results;results.reserve(indices.size());size_t next=0;
        auto schedule=[&](size_t ordinal){const size_t index=indices[ordinal];pending.emplace_back(ordinal,executor.submit(path,[fn,index](std::FILE* file){return fn(file,index);}));};
        while(next<indices.size()&&pending.size()<MAX_PARALLEL_CHUNKS)schedule(next++);
        while(!pending.empty()){results.push_back(pending.front().second.get());pending.pop_front();if(next<indices.size())schedule(next++);}
        return results;
    }
};

TnrdV4Archive::TnrdV4Archive():impl_(std::make_unique<Impl>()){}
TnrdV4Archive::~TnrdV4Archive(){close();}
void TnrdV4Archive::close(){impl_->clear();}
bool TnrdV4Archive::isOpen()const{return impl_->file!=nullptr;}
const std::vector<V4LapInfo>& TnrdV4Archive::laps()const{return impl_->laps;}
const std::vector<V4ChunkInfo>& TnrdV4Archive::chunks()const{return impl_->chunks;}
const V4ControlSummary& TnrdV4Archive::summary()const{return impl_->summary;}
float TnrdV4Archive::startTime()const{return impl_->summary.startSessionTime;}
float TnrdV4Archive::totalTime()const{return std::max(impl_->summary.totalSessionTime,impl_->laps.empty()?0.0f:impl_->laps.back().endSessionTime);}
int TnrdV4Archive::lapAt(float time)const{int found=0;for(const auto&lap:impl_->laps){if(lap.startSessionTime>time)break;found=(int)lap.lapNumber;}return found;}

bool TnrdV4Archive::open(const std::string& path,HeaderRow& header,std::string* errorOut){
    close();impl_->file=openTnrdFile(path,"rb");if(!impl_->file){fail(errorOut,"could not open V4 file");return false;}impl_->path=path;if(!seekEnd(impl_->file)){fail(errorOut,"could not size V4 file");close();return false;}impl_->fileSize=tell(impl_->file);
    std::array<uint8_t,HEADER_SIZE> h{};if(!readAt(impl_->file,0,h.data(),h.size())||!std::equal(MAGIC.begin(),MAGIC.end(),h.begin())||get16(h.data()+8)!=4||get16(h.data()+10)!=HEADER_SIZE){fail(errorOut,"invalid V4 header");close();return false;}
    const uint32_t stored=get32(h.data()+88);auto copy=h;std::fill(copy.begin()+88,copy.begin()+92,0);const bool headerCrcOk=stored==(uint32_t)::crc32(0,copy.data(),88);
    if(headerCrcOk&&(get32(h.data()+12)!=0||get32(h.data()+92)!=0)){fail(errorOut,"V4 file uses unsupported feature flags");close();return false;}
    uint64_t mo=get64(h.data()+16),ms=get64(h.data()+24);const uint64_t so=get64(h.data()+72),ss=get64(h.data()+80);uint64_t lo=get64(h.data()+32),co=get64(h.data()+48),fo=get64(h.data()+64);uint32_t lc=get32(h.data()+40),cc=get32(h.data()+56);
    if(headerCrcOk&&(get32(h.data()+44)!=LAP_ENTRY_SIZE||get32(h.data()+60)!=CHUNK_ENTRY_SIZE)){fail(errorOut,"V4 header table sizes are invalid");close();return false;}
    auto validateFooter=[&](uint64_t offset,uint64_t& lapOff,uint32_t& lapCount,uint64_t& chunkOff,uint32_t& chunkCount,uint64_t& summaryOff,uint64_t& summarySize,uint64_t& recoveredMetadataSize,uint32_t& expectedCrc,bool checkTables)->bool{
        std::array<uint8_t,FOOTER_SIZE> f{};if(!rangeOk(impl_->fileSize,offset,FOOTER_SIZE)||!readAt(impl_->file,offset,f.data(),f.size())||get32(f.data())!=FOOTER_MAGIC||get16(f.data()+4)!=4||get16(f.data()+6)!=FOOTER_SIZE)return false;
        lapOff=get64(f.data()+8);chunkOff=get64(f.data()+16);if(chunkOff<lapOff||offset<chunkOff||(chunkOff-lapOff)%LAP_ENTRY_SIZE||(offset-chunkOff)%CHUNK_ENTRY_SIZE)return false;
        const uint64_t derivedLaps=(chunkOff-lapOff)/LAP_ENTRY_SIZE,derivedChunks=(offset-chunkOff)/CHUNK_ENTRY_SIZE;if(derivedLaps>MAX_LAPS||derivedChunks>MAX_CHUNKS)return false;lapCount=(uint32_t)derivedLaps;chunkCount=(uint32_t)derivedChunks;
        summarySize=get32(f.data()+28);if(summarySize>MAX_SUMMARY_BYTES||summarySize>lapOff)return false;summaryOff=lapOff-summarySize;
        std::array<uint8_t,METADATA_PREFIX_SIZE> prefix{};if(!readAt(impl_->file,HEADER_SIZE,prefix.data(),prefix.size())||get32(prefix.data())!=METADATA_MAGIC)return false;recoveredMetadataSize=get64(prefix.data()+8);if(recoveredMetadataSize>MAX_METADATA_BYTES||!rangeOk(impl_->fileSize,HEADER_SIZE+METADATA_PREFIX_SIZE,recoveredMetadataSize)||summaryOff<HEADER_SIZE+METADATA_PREFIX_SIZE+recoveredMetadataSize)return false;
        expectedCrc=get32(f.data()+24);if(!checkTables)return true;std::string summary((size_t)summarySize,'\0');std::vector<uint8_t> l((size_t)(chunkOff-lapOff)),c((size_t)(offset-chunkOff));if(!readAt(impl_->file,summaryOff,summary.data(),summary.size())||!readAt(impl_->file,lapOff,l.data(),l.size())||!readAt(impl_->file,chunkOff,c.data(),c.size()))return false;return expectedCrc==controlCrc(summary,l,c);
    };
    uint64_t vlo{},vco{},vso{},vss{},vms{};uint32_t vlc{},vcc{},expectedTablesCrc{};bool footerOk=headerCrcOk&&ms<=MAX_METADATA_BYTES&&rangeOk(impl_->fileSize,mo,ms)&&validateFooter(fo,vlo,vlc,vco,vcc,vso,vss,vms,expectedTablesCrc,true)&&vlo==lo&&vlc==lc&&vco==co&&vcc==cc&&vso==so&&vss==ss&&vms==ms;
    if(!footerOk){
        bool recovered=false;constexpr uint64_t BLOCK=1024ull*1024ull;uint64_t end=impl_->fileSize;
        while(end>HEADER_SIZE&&!recovered){const uint64_t begin=end>BLOCK?std::max<uint64_t>(HEADER_SIZE,end-BLOCK):HEADER_SIZE;std::vector<uint8_t> scan((size_t)(end-begin));if(!readAt(impl_->file,begin,scan.data(),scan.size()))break;for(size_t i=scan.size();i-- >0;){if(i+4>scan.size()||get32(scan.data()+i)!=FOOTER_MAGIC)continue;const uint64_t pos=begin+i;if(validateFooter(pos,vlo,vlc,vco,vcc,vso,vss,vms,expectedTablesCrc,true)){lo=vlo;lc=vlc;co=vco;cc=vcc;fo=pos;mo=HEADER_SIZE+METADATA_PREFIX_SIZE;ms=vms;recovered=true;break;}}if(begin==HEADER_SIZE)break;end=begin+3;}
        if(!recovered){fail(errorOut,headerCrcOk?"V4 commit footer is invalid":"V4 header checksum mismatch and no recoverable checkpoint exists");close();return false;}
    }
    // A crash can occur after a checkpoint footer is flushed but before the
    // fixed header is patched. If bytes follow the header's valid snapshot,
    // inspect only that tail and prefer its newest fully checksummed footer.
    if(footerOk&&impl_->fileSize>fo+FOOTER_SIZE){bool newer=false;constexpr uint64_t BLOCK=1024ull*1024ull;uint64_t end=impl_->fileSize;while(end>fo+FOOTER_SIZE&&!newer){const uint64_t begin=end>BLOCK?std::max<uint64_t>(fo+FOOTER_SIZE,end-BLOCK):fo+FOOTER_SIZE;std::vector<uint8_t> scan((size_t)(end-begin));if(!readAt(impl_->file,begin,scan.data(),scan.size()))break;for(size_t i=scan.size();i-- >0;){if(i+4>scan.size()||get32(scan.data()+i)!=FOOTER_MAGIC)continue;const uint64_t pos=begin+i;if(pos<=fo)continue;if(validateFooter(pos,vlo,vlc,vco,vcc,vso,vss,vms,expectedTablesCrc,true)){lo=vlo;lc=vlc;co=vco;cc=vcc;fo=pos;mo=HEADER_SIZE+METADATA_PREFIX_SIZE;ms=vms;newer=true;footerOk=false;break;}}if(begin==fo+FOOTER_SIZE)break;end=begin+3;}}
    std::array<uint8_t,METADATA_PREFIX_SIZE> metadataPrefix{};if(!readAt(impl_->file,HEADER_SIZE,metadataPrefix.data(),metadataPrefix.size())||get32(metadataPrefix.data())!=METADATA_MAGIC||get64(metadataPrefix.data()+8)!=ms||mo!=HEADER_SIZE+METADATA_PREFIX_SIZE){fail(errorOut,"invalid V4 metadata prefix");close();return false;}
    std::string metadata((size_t)ms,'\0');if(!readAt(impl_->file,mo,metadata.data(),metadata.size())||get32(metadataPrefix.data()+4)!=(uint32_t)::crc32(0,(const Bytef*)metadata.data(),(uInt)metadata.size())||glz::read_json(impl_->header,metadata)||impl_->header.magic!="TNRD_V4"||!impl_->header.compression||*impl_->header.compression!="zstd"){fail(errorOut,"invalid V4 session metadata");close();return false;}header=impl_->header;
    const uint64_t activeSummaryOffset=footerOk?so:vso,activeSummarySize=footerOk?ss:vss;
    std::string summaryJson;
    if(activeSummaryOffset||activeSummarySize){if(!rangeOk(impl_->fileSize,activeSummaryOffset,activeSummarySize)){fail(errorOut,"V4 control summary bounds are invalid");close();return false;}summaryJson.assign((size_t)activeSummarySize,'\0');if(!readAt(impl_->file,activeSummaryOffset,summaryJson.data(),summaryJson.size())||glz::read_json(impl_->summary,summaryJson)){fail(errorOut,"invalid V4 control summary");close();return false;}}
    std::vector<uint8_t> lapBytes((size_t)lc*LAP_ENTRY_SIZE),dir((size_t)cc*CHUNK_ENTRY_SIZE);if(!readAt(impl_->file,lo,lapBytes.data(),lapBytes.size())||!readAt(impl_->file,co,dir.data(),dir.size())||expectedTablesCrc!=controlCrc(summaryJson,lapBytes,dir)){fail(errorOut,"could not validate V4 control tables");close();return false;}
    std::set<uint32_t> lapKeys;float previousStart=-std::numeric_limits<float>::infinity(),previousEnd=-std::numeric_limits<float>::infinity();uint32_t previousNumber=0;for(uint32_t i=0;i<lc;++i){const uint8_t*p=lapBytes.data()+i*LAP_ENTRY_SIZE;V4LapInfo lap{get32(p),getFloat(p+4),getFloat(p+8),get32(p+12),get32(p+16)};const bool lapTimeInvalid=((lap.flags&1u)!=0)!=(lap.lapTimeMs>0);if(!lap.lapNumber||lap.lapNumber<=previousNumber||(lap.flags&~1u)||get32(p+20)!=0||lapTimeInvalid||!std::isfinite(lap.startSessionTime)||!std::isfinite(lap.endSessionTime)||lap.endSessionTime<lap.startSessionTime||lap.startSessionTime<previousStart||lap.startSessionTime<previousEnd||!lapKeys.insert(lap.lapNumber).second){fail(errorOut,"invalid V4 lap table");close();return false;}previousNumber=lap.lapNumber;previousStart=lap.startSessionTime;previousEnd=lap.endSessionTime;impl_->laps.push_back(lap);}
    if(!std::isfinite(impl_->summary.initialFuelKg)||!std::isfinite(impl_->summary.startSessionTime)||!std::isfinite(impl_->summary.totalSessionTime)||impl_->summary.totalSessionTime<impl_->summary.startSessionTime){fail(errorOut,"invalid V4 control summary values");close();return false;}std::set<uint32_t> summaryLaps;for(const auto&s:impl_->summary.lapStatus)if((s.lapNumber&&!lapKeys.count(s.lapNumber))||!std::isfinite(s.sessionTime)||!std::isfinite(s.ersPct)||!summaryLaps.insert(s.lapNumber).second){fail(errorOut,"invalid V4 lap-status summary");close();return false;}
    auto overlaps=[](uint64_t a,uint64_t an,uint64_t b,uint64_t bn){return an<=UINT64_MAX-a&&bn<=UINT64_MAX-b&&a<b+bn&&b<a+an;};
    const uint64_t maxCompressed=ZSTD_compressBound((size_t)MAX_CHUNK_PLAIN);std::vector<std::pair<uint64_t,uint64_t>> payloadRanges;std::set<std::tuple<uint32_t,uint16_t,uint64_t>> keys;for(uint32_t i=0;i<cc;++i){const uint8_t*p=dir.data()+i*CHUNK_ENTRY_SIZE;V4ChunkInfo c{get32(p),get16(p+4),get16(p+6),get64(p+8),get64(p+16),get64(p+24),get32(p+32),get32(p+36),get64(p+40)};const bool payloadOverflow=c.compressedSize>UINT64_MAX-CHUNK_PREFIX_SIZE;const uint64_t payloadStart=c.offset>=CHUNK_PREFIX_SIZE?c.offset-CHUNK_PREFIX_SIZE:0,payloadSize=payloadOverflow?0:c.compressedSize+CHUNK_PREFIX_SIZE;const bool controlOverlap=!payloadOverflow&&(overlaps(payloadStart,payloadSize,0,HEADER_SIZE)||overlaps(payloadStart,payloadSize,HEADER_SIZE,METADATA_PREFIX_SIZE)||overlaps(payloadStart,payloadSize,mo,ms)||(activeSummarySize&&overlaps(payloadStart,payloadSize,activeSummaryOffset,activeSummarySize))||overlaps(payloadStart,payloadSize,lo,(uint64_t)lc*LAP_ENTRY_SIZE)||overlaps(payloadStart,payloadSize,co,(uint64_t)cc*CHUNK_ENTRY_SIZE)||overlaps(payloadStart,payloadSize,fo,FOOTER_SIZE));if((c.lapNumber&&!lapKeys.count(c.lapNumber))||c.rowType>14||c.flags||!c.sequence||!c.compressedSize||!c.uncompressedSize||!c.rowCount||c.uncompressedSize>MAX_CHUNK_PLAIN||c.compressedSize>maxCompressed||c.compressedSize>SIZE_MAX||payloadOverflow||!rangeOk(impl_->fileSize,c.offset,c.compressedSize)||c.offset<CHUNK_PREFIX_SIZE||controlOverlap||!keys.emplace(c.lapNumber,c.rowType,c.sequence).second){fail(errorOut,"invalid or duplicate V4 chunk entry");close();return false;}payloadRanges.emplace_back(payloadStart,payloadStart+payloadSize);impl_->chunks.push_back(c);}std::sort(payloadRanges.begin(),payloadRanges.end());for(size_t i=1;i<payloadRanges.size();++i)if(payloadRanges[i].first<payloadRanges[i-1].second){fail(errorOut,"V4 chunk payload ranges overlap");close();return false;}impl_->chunkTimeBounds.resize(impl_->chunks.size());for(size_t i=0;i<impl_->chunks.size();++i)impl_->chunkIndex[{impl_->chunks[i].lapNumber,impl_->chunks[i].rowType}].push_back(i);for(auto&[key,indices]:impl_->chunkIndex)std::sort(indices.begin(),indices.end(),[&](size_t a,size_t b){return impl_->chunks[a].sequence<impl_->chunks[b].sequence;});
    return true;
}

bool TnrdV4Archive::rowsForLap(uint32_t lap,V4RowTypeMask mask,std::vector<V4TimedRow>& out,std::string* errorOut){
    std::vector<size_t> selected;auto it=impl_->chunkIndex.lower_bound({lap,0});for(;it!=impl_->chunkIndex.end()&&it->first.first==lap;++it)if(mask&v4TypeBit((uint8_t)it->first.second))selected.insert(selected.end(),it->second.begin(),it->second.end());
    auto results=impl_->runChunkJobs(selected,[state=impl_.get()](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error))result.ok=false;return result;});
    out.clear();for(const auto& result:results)if(!result.ok){fail(errorOut,result.error);return false;}for(auto& result:results)for(auto& row:result.rows)out.push_back(std::move(row));sortRows(out);return true;
}
bool TnrdV4Archive::rowsForRange(float from,float to,V4RowTypeMask mask,std::vector<V4TimedRow>& out,std::string* errorOut){
    std::vector<size_t> selected;
    for(const auto&[key,indices]:impl_->chunkIndex){const auto[lapNumber,rowType]=key;if(!(mask&v4TypeBit((uint8_t)rowType)))continue;if(lapNumber){auto lap=std::lower_bound(impl_->laps.begin(),impl_->laps.end(),lapNumber,[](const auto&value,uint32_t number){return value.lapNumber<number;});if(lap!=impl_->laps.end()&&lap->lapNumber==lapNumber&&(lap->endSessionTime<from||lap->startSessionTime>to))continue;}selected.insert(selected.end(),indices.begin(),indices.end());}
    auto results=impl_->runChunkJobs(selected,[state=impl_.get(),from,to](std::FILE* file,size_t index){Impl::RowsResult result;const auto bounds=state->bounds(index);if(bounds.known&&(bounds.last<from||bounds.first>to))return result;std::vector<V4TimedRow> rows;if(!state->splitChunk(index,file,rows,&result.error)){result.ok=false;return result;}for(auto& row:rows)if(row.sessionTime>=from&&row.sessionTime<=to)result.rows.push_back(std::move(row));return result;});
    out.clear();for(const auto& result:results)if(!result.ok){fail(errorOut,result.error);return false;}for(auto& result:results)for(auto& row:result.rows)out.push_back(std::move(row));sortRows(out);return true;
}
bool TnrdV4Archive::latestRows(float at,const std::vector<uint8_t>& types,std::vector<V4TimedRow>& out,std::string* errorOut){
    struct Search{uint8_t type{};int lap{};bool done{};bool found{};V4TimedRow best;};struct Span{size_t search{};size_t begin{};size_t end{};};
    const int targetLap=lapAt(at);std::vector<Search> searches;searches.reserve(types.size());for(uint8_t type:types)searches.push_back({type,targetLap});
    for(;;){
        std::vector<size_t> selected;std::vector<Span> spans;
        for(size_t i=0;i<searches.size();++i){auto& search=searches[i];if(search.done)continue;decltype(impl_->chunkIndex)::iterator bucket;for(;;){if(search.lap<0){search.done=true;break;}bucket=impl_->chunkIndex.find({(uint32_t)search.lap,search.type});if(bucket!=impl_->chunkIndex.end())break;--search.lap;}if(search.done)continue;const size_t begin=selected.size();selected.insert(selected.end(),bucket->second.begin(),bucket->second.end());spans.push_back({i,begin,selected.size()});}
        if(selected.empty())break;
        auto results=impl_->runChunkJobs(selected,[state=impl_.get(),at](std::FILE* file,size_t index){Impl::RowsResult result;const auto bounds=state->bounds(index);if(bounds.known&&bounds.first>at)return result;if(!state->splitChunk(index,file,result.rows,&result.error))result.ok=false;return result;});
        for(const auto& span:spans){auto& search=searches[span.search];for(size_t i=span.begin;i<span.end;++i){const auto& result=results[i];if(!result.ok){fail(errorOut,result.error);return false;}for(const auto& row:result.rows)if(row.sessionTime<=at&&(!search.found||row.sessionTime>search.best.sessionTime||(row.sessionTime==search.best.sessionTime&&row.sequence>search.best.sequence))){search.best=row;search.found=true;}}if(search.found)search.done=true;else--search.lap;}
    }
    out.clear();for(auto& search:searches)if(search.found)out.push_back(std::move(search.best));sortRows(out);return true;
}
bool TnrdV4Archive::forEachChunk(V4RowTypeMask mask,const std::function<bool(const V4ChunkInfo&,std::string_view)>& callback,std::string* errorOut){
    struct Prepared{bool ok{};std::string error;std::shared_ptr<std::string> plain;};
    std::vector<size_t> selected;for(size_t i=0;i<impl_->chunks.size();++i)if(mask&v4TypeBit((uint8_t)impl_->chunks[i].rowType))selected.push_back(i);std::sort(selected.begin(),selected.end(),[&](size_t a,size_t b){const auto&x=impl_->chunks[a];const auto&y=impl_->chunks[b];if(x.rowType!=y.rowType)return x.rowType<y.rowType;if(x.lapNumber!=y.lapNumber)return x.lapNumber<y.lapNumber;return x.sequence<y.sequence;});
    using Pending=std::pair<size_t,std::future<Prepared>>;std::deque<Pending> pending;size_t next=0;
    auto schedule=[&](size_t index){pending.emplace_back(index,impl_->executor.submit(impl_->path,[state=impl_.get(),index](std::FILE* file){Prepared result;std::shared_ptr<std::string> plain;if(!state->loadChunk(index,file,plain,&result.error))return result;const std::string_view view(*plain);const size_t firstEnd=view.find('\n');if(scanTime(view.substr(0,firstEnd))>=0){result.ok=true;result.plain=std::move(plain);return result;}std::vector<V4TimedRow> rows;if(!state->splitChunk(index,file,rows,&result.error,plain))return result;auto normalized=std::make_shared<std::string>();normalized->reserve(plain->size()+rows.size()*24);for(const auto& row:rows){*normalized+=withSessionTime(row.json,row.sessionTime);normalized->push_back('\n');}result.ok=true;result.plain=std::move(normalized);return result;}));};
    while(next<selected.size()&&pending.size()<MAX_PARALLEL_CHUNKS)schedule(selected[next++]);
    while(!pending.empty()){const size_t index=pending.front().first;auto prepared=pending.front().second.get();pending.pop_front();if(!prepared.ok){while(!pending.empty()){pending.front().second.wait();pending.pop_front();}fail(errorOut,prepared.error);return false;}if(!callback(impl_->chunks[index],*prepared.plain)){while(!pending.empty()){pending.front().second.wait();pending.pop_front();}return false;}if(next<selected.size())schedule(selected[next++]);}
    return true;
}
void TnrdV4Archive::setCacheLimitBytes(size_t bytes){std::lock_guard<std::mutex> lock(impl_->stateMutex);impl_->cacheLimit=bytes;while(impl_->cacheBytes>bytes&&!impl_->lru.empty()){const size_t old=impl_->lru.back();impl_->lru.pop_back();auto it=impl_->cache.find(old);impl_->cacheBytes-=it->second.plain->size();impl_->cache.erase(it);}}
size_t TnrdV4Archive::cacheBytes()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->cacheBytes;}
uint64_t TnrdV4Archive::decompressedChunkCount()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->decompressions;}
size_t TnrdV4Archive::peakConcurrentChunkLoads()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->peakActiveLoads;}

} // namespace tnrp::detail
