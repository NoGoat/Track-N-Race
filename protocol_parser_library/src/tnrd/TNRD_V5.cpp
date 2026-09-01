#include "TNRD_V5.h"
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
#include <exception>
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

constexpr size_t HEADER_SIZE = 112;
constexpr size_t LAP_ENTRY_SIZE = 24;
constexpr size_t CHUNK_ENTRY_SIZE = 88;
constexpr size_t ROW_INDEX_ENTRY_SIZE = 24;
constexpr uint16_t HOT_ROW_INDEX_STRIDE = 64;
constexpr size_t FOOTER_SIZE = 40;
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
const std::array<uint8_t, 8> MAGIC{{'T','N','R','D','_','V','5','\0'}};

struct DecompressionContext {
    ZSTD_DCtx* value{ZSTD_createDCtx()};
    ~DecompressionContext() { if (value) ZSTD_freeDCtx(value); }
};

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
struct RowSeek { float firstTime{},lastTime{};uint32_t offset{},length{},ordinal{};uint16_t count{},reserved{}; };
struct Chunk {
    uint32_t lap{};uint16_t type{},flags{};uint64_t offset{},compressed{},plain{};uint32_t rows{},crc{};uint64_t sequence{};
    float firstTime{},lastTime{},prefixMaxLastTime{},minDistance{std::numeric_limits<float>::quiet_NaN()},maxDistance{std::numeric_limits<float>::quiet_NaN()};
    uint64_t rowIndexOffset{};uint32_t rowIndexCount{};uint16_t rowIndexStride{1},rowIndexEntrySize{ROW_INDEX_ENTRY_SIZE};
    std::vector<RowSeek> rowIndex;
};

std::vector<uint8_t> makeHeader(uint64_t metaOff,uint64_t metaSize,uint64_t lapOff,uint32_t lapCount,
                                uint64_t chunkOff,uint32_t chunkCount,uint64_t footerOff,
                                uint64_t summaryOff,uint64_t summarySize,uint64_t rowIndexOff,uint64_t rowIndexSize){
    std::vector<uint8_t>b;b.insert(b.end(),MAGIC.begin(),MAGIC.end());put16(b,5);put16(b,(uint16_t)HEADER_SIZE);put32(b,1);
    put64(b,metaOff);put64(b,metaSize);put64(b,lapOff);put32(b,lapCount);put32(b,(uint32_t)LAP_ENTRY_SIZE);
    put64(b,chunkOff);put32(b,chunkCount);put32(b,(uint32_t)CHUNK_ENTRY_SIZE);put64(b,footerOff);
    put64(b,summaryOff);put64(b,summarySize);put64(b,rowIndexOff);put64(b,rowIndexSize);put32(b,0);put32(b,0);
    const uint32_t crc=(uint32_t)::crc32(0,b.data(),104);b[104]=(uint8_t)crc;b[105]=(uint8_t)(crc>>8);b[106]=(uint8_t)(crc>>16);b[107]=(uint8_t)(crc>>24);return b;
}

uint32_t controlCrc(std::string_view summary,const std::vector<uint8_t>& laps,const std::vector<uint8_t>& chunks,const std::vector<uint8_t>& rowIndex){
    uint32_t crc=(uint32_t)::crc32(0,(const Bytef*)summary.data(),(uInt)summary.size());crc=(uint32_t)::crc32(crc,laps.data(),(uInt)laps.size());crc=(uint32_t)::crc32(crc,chunks.data(),(uInt)chunks.size());return (uint32_t)::crc32(crc,rowIndex.data(),(uInt)rowIndex.size());
}

bool splitRows(std::string_view plain,uint8_t expectedType,uint64_t sequence,std::vector<V5TimedRow>& out,uint32_t expectedCount,std::string* errorOut,const std::vector<float>* inferredTimes=nullptr,bool filter=false,float from=0,float to=0,bool latestOnly=false,float* firstOut=nullptr,float* lastOut=nullptr){
    size_t pos=0;uint32_t count=0;float first=std::numeric_limits<float>::infinity(),last=-std::numeric_limits<float>::infinity(),bestTime=-std::numeric_limits<float>::infinity();std::string_view bestLine;uint32_t bestOffset{};bool haveBest=false;
    while(pos<plain.size()){
        size_t nl=plain.find('\n',pos);if(nl==std::string_view::npos)nl=plain.size();
        if(nl>pos){auto line=plain.substr(pos,nl-pos);const uint8_t actual=rowType(line);if(actual!=expectedType){fail(errorOut,"V5 chunk contains the wrong row type");return false;}float time=scanTime(line);if(time<0&&inferredTimes&&count<inferredTimes->size())time=(*inferredTimes)[count];if(std::isfinite(time)){first=std::min(first,time);last=std::max(last,time);}if(!filter||(time>=from&&time<=to)){if(latestOnly){if(!haveBest||time>bestTime){bestTime=time;bestLine=line;bestOffset=(uint32_t)pos;haveBest=true;}}else out.push_back({time,actual,sequence,std::string(line),(uint32_t)pos});}++count;}
        if(nl==plain.size())break;pos=nl+1;
    }
    if(count!=expectedCount){fail(errorOut,"V5 chunk row count mismatch");return false;}if(latestOnly&&haveBest)out.push_back({bestTime,expectedType,sequence,std::string(bestLine),bestOffset});if(firstOut)*firstOut=first;if(lastOut)*lastOut=last;return true;
}

void sortRows(std::vector<V5TimedRow>& rows){std::stable_sort(rows.begin(),rows.end(),[](const auto&a,const auto&b){if(a.sessionTime!=b.sessionTime)return a.sessionTime<b.sessionTime;return a.sequence<b.sequence;});}
void mergeRowGroups(std::vector<std::vector<V5TimedRow>>& groups,std::vector<V5TimedRow>& out){
    struct Cursor{size_t group{},row{};};auto later=[&](const Cursor&a,const Cursor&b){const auto&x=groups[a.group][a.row];const auto&y=groups[b.group][b.row];if(x.sessionTime!=y.sessionTime)return x.sessionTime>y.sessionTime;if(x.sequence!=y.sequence)return x.sequence>y.sequence;if(a.group!=b.group)return a.group>b.group;return a.row>b.row;};
    size_t total=0;std::priority_queue<Cursor,std::vector<Cursor>,decltype(later)> heap(later);for(size_t i=0;i<groups.size();++i){sortRows(groups[i]);total+=groups[i].size();if(!groups[i].empty())heap.push({i,0});}out.clear();out.reserve(total);while(!heap.empty()){auto cursor=heap.top();heap.pop();auto& group=groups[cursor.group];out.push_back(std::move(group[cursor.row]));if(++cursor.row<group.size())heap.push(cursor);}
}
}

struct TnrdV5Writer::Impl {
    struct Builder{std::string plain;std::vector<RowSeek> rowIndex;float firstTime{std::numeric_limits<float>::infinity()},lastTime{-std::numeric_limits<float>::infinity()},minDistance{std::numeric_limits<float>::infinity()},maxDistance{-std::numeric_limits<float>::infinity()};uint32_t rows{};uint16_t stride{1};bool dirty{};};
    std::FILE* file{};uint64_t metadataOffset{HEADER_SIZE+METADATA_PREFIX_SIZE},metadataSize{};uint32_t currentLap{},highestLap{};float lastTime{};bool haveTime{},timedSession{},timedLapActive{};
    std::map<uint32_t,Lap> laps;std::map<std::pair<uint32_t,uint16_t>,Builder> builders;std::vector<Chunk> chunks;
    V5ControlSummary summary;std::map<uint32_t,V5LapStatusSummary> statusByLap;uint64_t nextSequence{1};

    static bool hotType(uint16_t type){return type==1||type==11||type==12||type==13;}
    static void rebuildIndex(Builder& builder,uint16_t type){
        builder.rowIndex.clear();builder.firstTime=std::numeric_limits<float>::infinity();builder.lastTime=-std::numeric_limits<float>::infinity();builder.minDistance=std::numeric_limits<float>::infinity();builder.maxDistance=-std::numeric_limits<float>::infinity();builder.rows=0;builder.stride=hotType(type)?HOT_ROW_INDEX_STRIDE:1;
        size_t pos=0;while(pos<builder.plain.size()){size_t nl=builder.plain.find('\n',pos);if(nl==std::string::npos)nl=builder.plain.size();if(nl>pos){const auto line=std::string_view(builder.plain).substr(pos,nl-pos);const float time=scanTime(line);if(builder.rows%builder.stride==0)builder.rowIndex.push_back({time,time,(uint32_t)pos,0,builder.rows,0,0});auto& entry=builder.rowIndex.back();entry.firstTime=std::min(entry.firstTime,time);entry.lastTime=std::max(entry.lastTime,time);entry.length=builder.stride==1?(uint32_t)(nl-pos):(uint32_t)((nl<builder.plain.size()?nl+1:nl)-entry.offset);++entry.count;++builder.rows;if(std::isfinite(time)){builder.firstTime=std::min(builder.firstTime,time);builder.lastTime=std::max(builder.lastTime,time);}const float distance=(float)scanDouble(line,"\"lap_distance_m\":",std::numeric_limits<double>::quiet_NaN());if(std::isfinite(distance)){builder.minDistance=std::min(builder.minDistance,distance);builder.maxDistance=std::max(builder.maxDistance,distance);}}if(nl==builder.plain.size())break;pos=nl+1;}
    }

    bool writeChunk(const std::pair<uint32_t,uint16_t>& key,const Builder& builder,std::string* errorOut){
        const std::string& plain=builder.plain;
        if(plain.empty())return true;if(plain.size()>MAX_CHUNK_PLAIN){fail(errorOut,"V5 chunk exceeds safety limit");return false;}if(chunks.size()>=MAX_CHUNKS){fail(errorOut,"V5 chunk count exceeds safety limit");return false;}
        std::vector<uint8_t> compressed(ZSTD_compressBound(plain.size()));ZSTD_CCtx* ctx=ZSTD_createCCtx();
        if(!ctx){fail(errorOut,"could not allocate V5 compression context");return false;}ZSTD_CCtx_setParameter(ctx,ZSTD_c_compressionLevel,3);ZSTD_CCtx_setParameter(ctx,ZSTD_c_checksumFlag,1);
        const size_t n=ZSTD_compress2(ctx,compressed.data(),compressed.size(),plain.data(),plain.size());ZSTD_freeCCtx(ctx);
        if(ZSTD_isError(n)){fail(errorOut,ZSTD_getErrorName(n));return false;}if(!seekEnd(file)){fail(errorOut,"could not seek to append V5 chunk");return false;}
        Chunk c;c.lap=key.first;c.type=key.second;c.offset=tell(file)+CHUNK_PREFIX_SIZE;c.compressed=n;c.plain=plain.size();c.rows=builder.rows;c.crc=(uint32_t)::crc32(0,(const Bytef*)plain.data(),(uInt)plain.size());c.sequence=nextSequence++;c.firstTime=builder.firstTime;c.lastTime=builder.lastTime;c.minDistance=std::isfinite(builder.minDistance)?builder.minDistance:std::numeric_limits<float>::quiet_NaN();c.maxDistance=std::isfinite(builder.maxDistance)?builder.maxDistance:std::numeric_limits<float>::quiet_NaN();c.rowIndex=builder.rowIndex;c.rowIndexCount=(uint32_t)c.rowIndex.size();c.rowIndexStride=builder.stride;
        std::vector<uint8_t> prefix;put32(prefix,CHUNK_MAGIC);put32(prefix,c.lap);put16(prefix,c.type);put16(prefix,0);put64(prefix,c.compressed);put64(prefix,c.plain);put32(prefix,c.rows);
        if(!writeAll(file,prefix.data(),prefix.size())||!writeAll(file,compressed.data(),n)){fail(errorOut,"failed while appending V5 chunk");return false;}chunks.push_back(c);return true;
    }

    bool writeSnapshot(std::string* errorOut){
        if(!seekEnd(file)){fail(errorOut,"could not seek to append V5 checkpoint");return false;}
        if(laps.size()>MAX_LAPS||chunks.size()>MAX_CHUNKS){fail(errorOut,"V5 control table exceeds its format limit");return false;}
        summary.lapStatus.clear();for(const auto& [lap,status]:statusByLap)summary.lapStatus.push_back(status);
        const std::string summaryJson=writeJson(summary);const uint64_t summaryOffset=tell(file);
        if(summaryJson.size()>MAX_SUMMARY_BYTES||summaryJson.size()>UINT32_MAX){fail(errorOut,"V5 control summary exceeds its format limit");return false;}
        if(!writeAll(file,summaryJson.data(),summaryJson.size())){fail(errorOut,"failed to append V5 control summary");return false;}
        const uint64_t lapOffset=tell(file);std::vector<uint8_t> lapBytes;
        for(const auto& [n,l]:laps){put32(lapBytes,n);putFloat(lapBytes,l.start);putFloat(lapBytes,l.end);put32(lapBytes,l.timeMs);put32(lapBytes,l.flags);put32(lapBytes,0);}
        if(!writeAll(file,lapBytes.data(),lapBytes.size())){fail(errorOut,"failed to append V5 lap table");return false;}
        const uint64_t chunkOffset=tell(file);const uint64_t rowIndexOffset=chunkOffset+chunks.size()*CHUNK_ENTRY_SIZE;std::vector<uint8_t> dir,rowIndexBytes;std::array<float,15> prefixMax;prefixMax.fill(-std::numeric_limits<float>::infinity());uint64_t nextRowIndexOffset=rowIndexOffset;std::vector<size_t> order(chunks.size());for(size_t i=0;i<order.size();++i)order[i]=i;std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){const auto& x=chunks[a];const auto& y=chunks[b];if(x.type!=y.type)return x.type<y.type;if(x.firstTime!=y.firstTime)return x.firstTime<y.firstTime;return x.sequence<y.sequence;});
        // Sparse entries are exact locators and are serialized in seek order;
        // their physical ordinals retain payload order for validation. Hot
        // blocks stay in physical order because their min/max bounds may overlap.
        for(size_t index:order){auto& c=chunks[index];c.prefixMaxLastTime=prefixMax[c.type]=std::max(prefixMax[c.type],c.lastTime);c.rowIndexOffset=nextRowIndexOffset;c.rowIndexCount=(uint32_t)c.rowIndex.size();put32(dir,c.lap);put16(dir,c.type);put16(dir,c.flags);put64(dir,c.offset);put64(dir,c.compressed);put64(dir,c.plain);put32(dir,c.rows);put32(dir,c.crc);put64(dir,c.sequence);putFloat(dir,c.firstTime);putFloat(dir,c.lastTime);putFloat(dir,c.prefixMaxLastTime);putFloat(dir,c.minDistance);putFloat(dir,c.maxDistance);put64(dir,c.rowIndexOffset);put32(dir,c.rowIndexCount);put16(dir,c.rowIndexStride);put16(dir,c.rowIndexEntrySize);put32(dir,0);std::vector<RowSeek> sortedRows;if(c.rowIndexStride==1){sortedRows=c.rowIndex;std::stable_sort(sortedRows.begin(),sortedRows.end(),[](const RowSeek& a,const RowSeek& b){return a.firstTime<b.firstTime||(a.firstTime==b.firstTime&&a.offset<b.offset);});}const auto& storedRows=sortedRows.empty()?c.rowIndex:sortedRows;for(const auto& row:storedRows){putFloat(rowIndexBytes,row.firstTime);putFloat(rowIndexBytes,row.lastTime);put32(rowIndexBytes,row.offset);put32(rowIndexBytes,row.length);put32(rowIndexBytes,row.ordinal);put16(rowIndexBytes,row.count);put16(rowIndexBytes,row.reserved);}nextRowIndexOffset+=c.rowIndex.size()*ROW_INDEX_ENTRY_SIZE;}
        if(!writeAll(file,dir.data(),dir.size())){fail(errorOut,"failed to append V5 chunk directory");return false;}
        if(!writeAll(file,rowIndexBytes.data(),rowIndexBytes.size())){fail(errorOut,"failed to append V5 row index");return false;}
        const uint64_t footerOffset=tell(file);std::vector<uint8_t> footer;put32(footer,FOOTER_MAGIC);put16(footer,5);put16(footer,FOOTER_SIZE);put64(footer,lapOffset);put64(footer,chunkOffset);put64(footer,rowIndexOffset);put32(footer,controlCrc(summaryJson,lapBytes,dir,rowIndexBytes));put32(footer,(uint32_t)summaryJson.size());
        if(!writeAll(file,footer.data(),footer.size())||std::fflush(file)!=0){fail(errorOut,"failed to commit V5 checkpoint footer");return false;}
        const auto header=makeHeader(metadataOffset,metadataSize,lapOffset,(uint32_t)laps.size(),chunkOffset,(uint32_t)chunks.size(),footerOffset,summaryOffset,summaryJson.size(),rowIndexOffset,rowIndexBytes.size());
        if(!seek(file,0)||!writeAll(file,header.data(),header.size())||std::fflush(file)!=0){fail(errorOut,"failed to commit V5 checkpoint header");return false;}return seekEnd(file);
    }

    bool decode(const Chunk& c,std::string& plain,std::string* errorOut){
        std::vector<uint8_t> compressed((size_t)c.compressed);plain.assign((size_t)c.plain,'\0');if(!readAt(file,c.offset,compressed.data(),compressed.size())){fail(errorOut,"could not read V5 chunk during flashback");return false;}
        const size_t got=ZSTD_decompress(plain.data(),plain.size(),compressed.data(),compressed.size());if(ZSTD_isError(got)||got!=c.plain){fail(errorOut,"could not decompress V5 chunk during flashback");return false;}return true;
    }
};

TnrdV5Writer::TnrdV5Writer():impl_(std::make_unique<Impl>()){}
TnrdV5Writer::~TnrdV5Writer(){if(impl_&&impl_->file){std::string ignored;(void)finish(&ignored);}}
bool TnrdV5Writer::isOpen()const{return impl_&&impl_->file;}

bool TnrdV5Writer::open(const std::string& path,const HeaderRow& sourceHeader,std::string* errorOut){
    if(isOpen()){fail(errorOut,"V5 writer is already open");return false;}
    // A writer object may be reused after finish(). Do not carry directory,
    // lap-summary, or segment-sequence state into the next recording.
    impl_=std::make_unique<Impl>();impl_->file=openTnrdFile(path,"w+b");if(!impl_->file){const int openError=errno;fail(errorOut,std::string("could not create V5 file")+(openError?": "+std::string(std::strerror(openError)):std::string{}));return false;}
    HeaderRow header=sourceHeader;header.magic="TNRD_V5";header.compression="zstd";impl_->timedSession=header.session_type>=1&&header.session_type<=14;const std::string metadata=writeJson(header);if(metadata.size()>MAX_METADATA_BYTES){fail(errorOut,"V5 session metadata exceeds its format limit");std::fclose(impl_->file);impl_->file=nullptr;return false;}impl_->metadataSize=metadata.size();std::vector<uint8_t> zero(HEADER_SIZE),prefix;put32(prefix,METADATA_MAGIC);put32(prefix,(uint32_t)::crc32(0,(const Bytef*)metadata.data(),(uInt)metadata.size()));put64(prefix,metadata.size());
    if(!writeAll(impl_->file,zero.data(),zero.size())||!writeAll(impl_->file,prefix.data(),prefix.size())||!writeAll(impl_->file,metadata.data(),metadata.size())||!impl_->writeSnapshot(errorOut)){std::fclose(impl_->file);impl_->file=nullptr;return false;}return true;
}

bool TnrdV5Writer::append(const std::vector<V5SourceRow>& rows,std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V5 writer is not open");return false;}
    for(const auto&r:rows){std::string_view source=r.line;while(!source.empty()&&(source.back()=='\n'||source.back()=='\r'))source.remove_suffix(1);if(source.empty())continue;const float t=r.sessionTime>=0?r.sessionTime:scanTime(source);const std::string stored=withSessionTime(source,t);const std::string_view line=stored;const uint8_t type=rowType(line);if(t>=0){impl_->lastTime=std::max(impl_->lastTime,t);if(!impl_->haveTime){impl_->summary.startSessionTime=t;impl_->haveTime=true;}impl_->summary.totalSessionTime=std::max(impl_->summary.totalSessionTime,t);}
        if(type==4){const int n=scanInt(line,"\"lap_num\":",(int)impl_->currentLap),driver=scanInt(line,"\"driver_status\":",-1);const bool garageAware=impl_->timedSession&&driver>=0;if(garageAware&&driver!=1){if(impl_->currentLap)impl_->laps[impl_->currentLap].end=std::max(impl_->laps[impl_->currentLap].end,t);impl_->currentLap=0;impl_->timedLapActive=false;}else if(n>0){const uint32_t next=(uint32_t)n;const bool mayAdvance=next>impl_->highestLap;const bool mayStartTimed=garageAware&&!impl_->timedLapActive&&next>=impl_->highestLap;if((mayAdvance||mayStartTimed)&&(next!=impl_->currentLap||!impl_->timedLapActive)){float start=t;const int curMs=scanInt(line,"\"current_lap_ms\":",0);if(curMs>0)start=t-curMs/1000.0f;if(impl_->currentLap&&next>impl_->currentLap)impl_->laps[impl_->currentLap].end=start;impl_->currentLap=next;impl_->highestLap=std::max(impl_->highestLap,next);impl_->timedLapActive=garageAware;auto& lap=impl_->laps[next];lap.num=next;lap.start=start;lap.end=t;lap.timeMs=0;lap.flags=0;const int lastMs=scanInt(line,"\"last_lap_ms\":",0);auto prev=impl_->laps.find(next-1);if(prev!=impl_->laps.end()&&lastMs>0){prev->second.timeMs=(uint32_t)lastMs;prev->second.flags|=1;}}}}
        if(type==2){auto& s=impl_->statusByLap[impl_->currentLap];s.lapNumber=impl_->currentLap;s.sessionTime=t;s.ersPct=scanDouble(line,"\"ers_pct\":",s.ersPct);s.tyreCompound=scanInt(line,"\"tyre_compound\":",s.tyreCompound);s.visualCompound=scanInt(line,"\"visual_compound\":",s.visualCompound);if(impl_->summary.initialFuelKg<0)impl_->summary.initialFuelKg=scanDouble(line,"\"fuel_kg\":",-1);}
        if(type==14){const int lapNum=scanInt(line,"\"latest_lap_num\":",0),lapMs=scanInt(line,"\"latest_lap_time_ms\":",0);auto lap=impl_->laps.find((uint32_t)std::max(lapNum,0));if(lap!=impl_->laps.end()&&lapMs>0){lap->second.timeMs=(uint32_t)lapMs;lap->second.flags|=1;}}
        if(type==6)impl_->summary.events.emplace_back(line);
        auto& b=impl_->builders[{impl_->currentLap,(uint16_t)type}];if(!b.rows)b.stride=Impl::hotType(type)?HOT_ROW_INDEX_STRIDE:1;const float storedTime=scanTime(line);const uint32_t rowOffset=(uint32_t)b.plain.size();if(b.rows%b.stride==0)b.rowIndex.push_back({storedTime,storedTime,rowOffset,0,b.rows,0,0});b.plain.append(line);const uint32_t rowLength=(uint32_t)b.plain.size()-rowOffset;if(b.plain.empty()||b.plain.back()!='\n')b.plain.push_back('\n');auto& indexEntry=b.rowIndex.back();indexEntry.firstTime=std::min(indexEntry.firstTime,storedTime);indexEntry.lastTime=std::max(indexEntry.lastTime,storedTime);indexEntry.length=b.stride==1?rowLength:(uint32_t)b.plain.size()-indexEntry.offset;++indexEntry.count;++b.rows;if(std::isfinite(storedTime)){b.firstTime=std::min(b.firstTime,storedTime);b.lastTime=std::max(b.lastTime,storedTime);}const float distance=(float)scanDouble(line,"\"lap_distance_m\":",std::numeric_limits<double>::quiet_NaN());if(std::isfinite(distance)){b.minDistance=std::min(b.minDistance,distance);b.maxDistance=std::max(b.maxDistance,distance);}b.dirty=true;if(impl_->currentLap)impl_->laps[impl_->currentLap].end=std::max(impl_->laps[impl_->currentLap].end,t);
    }return true;
}

bool TnrdV5Writer::checkpoint(std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V5 writer is not open");return false;}
    for(auto it=impl_->builders.begin();it!=impl_->builders.end();){if(it->second.dirty&&!impl_->writeChunk(it->first,it->second,errorOut))return false;it=impl_->builders.erase(it);}return impl_->writeSnapshot(errorOut);
}

bool TnrdV5Writer::rewind(float sessionTime,std::string* errorOut){
    if(!isOpen()){fail(errorOut,"V5 writer is not open");return false;}uint32_t targetLap=0;for(const auto&[n,l]:impl_->laps)if(l.start<=sessionTime)targetLap=n;
    auto filter=[&](std::string_view input){std::string kept;size_t p=0;while(p<input.size()){size_t nl=input.find('\n',p);if(nl==std::string_view::npos)nl=input.size();auto line=input.substr(p,nl-p);if(!line.empty()&&scanTime(line)<=sessionTime){kept.append(line);kept.push_back('\n');}if(nl==input.size())break;p=nl+1;}return kept;};
    std::map<std::pair<uint32_t,uint16_t>,std::string> recovered;for(auto it=impl_->chunks.begin();it!=impl_->chunks.end();){if(it->lap>targetLap){it=impl_->chunks.erase(it);continue;}if(it->lap==targetLap){std::string plain;if(!impl_->decode(*it,plain,errorOut))return false;recovered[{it->lap,it->type}]+=plain;it=impl_->chunks.erase(it);continue;}++it;}for(auto&[key,plain]:recovered){auto&b=impl_->builders[key];b.plain=std::move(plain)+b.plain;b.dirty=true;Impl::rebuildIndex(b,key.second);}
    for(auto it=impl_->builders.begin();it!=impl_->builders.end();){if(it->first.first>targetLap){it=impl_->builders.erase(it);continue;}if(it->first.first==targetLap){it->second.plain=filter(it->second.plain);it->second.dirty=true;Impl::rebuildIndex(it->second,it->first.second);}++it;}
    for(auto it=impl_->laps.upper_bound(targetLap);it!=impl_->laps.end();)it=impl_->laps.erase(it);for(auto it=impl_->statusByLap.upper_bound(targetLap);it!=impl_->statusByLap.end();)it=impl_->statusByLap.erase(it);
    impl_->statusByLap.erase(targetLap);auto statusBuilder=impl_->builders.find(std::make_pair(targetLap,(uint16_t)2));if(statusBuilder!=impl_->builders.end()){size_t pos=0;while(pos<statusBuilder->second.plain.size()){size_t nl=statusBuilder->second.plain.find('\n',pos);if(nl==std::string::npos)nl=statusBuilder->second.plain.size();auto line=std::string_view(statusBuilder->second.plain).substr(pos,nl-pos);if(!line.empty()){auto&s=impl_->statusByLap[targetLap];s.lapNumber=targetLap;s.sessionTime=scanTime(line);s.ersPct=scanDouble(line,"\"ers_pct\":",s.ersPct);s.tyreCompound=scanInt(line,"\"tyre_compound\":",s.tyreCompound);s.visualCompound=scanInt(line,"\"visual_compound\":",s.visualCompound);}if(nl==statusBuilder->second.plain.size())break;pos=nl+1;}}
    impl_->summary.events.erase(std::remove_if(impl_->summary.events.begin(),impl_->summary.events.end(),[&](const std::string&e){return scanTime(e)>sessionTime;}),impl_->summary.events.end());impl_->summary.totalSessionTime=sessionTime;if(targetLap)impl_->laps[targetLap].end=sessionTime;impl_->currentLap=targetLap;impl_->highestLap=targetLap;impl_->timedLapActive=false;impl_->lastTime=sessionTime;return checkpoint(errorOut);
}

bool TnrdV5Writer::finish(std::string* errorOut){if(!isOpen())return true;const bool ok=checkpoint(errorOut);const bool closeOk=std::fclose(impl_->file)==0;impl_->file=nullptr;if(!closeOk&&ok)fail(errorOut,"failed to close V5 file");return ok&&closeOk;}
bool writeTnrdV5(const std::string& path,const HeaderRow& header,const std::vector<V5SourceRow>& rows,std::string* errorOut){TnrdV5Writer writer;return writer.open(path,header,errorOut)&&writer.append(rows,errorOut)&&writer.finish(errorOut);}

bool TNRD_V5::load(const std::string& path,V5LoadResult& result,std::string& error){result=V5LoadResult{};result.archive=std::make_unique<TnrdV5Archive>();if(result.archive->open(path,result.header,&error))return true;result.archive.reset();if(error.empty())error="The V5 recording could not be read.";return false;}

struct TnrdV5Archive::Impl {
    struct RowSpan{uint32_t offset{};uint32_t length{};float firstTime{},lastTime{};uint32_t ordinal{};uint16_t count{};};
    struct ChunkMeta{float first{},last{},prefixMax{},minDistance{},maxDistance{};uint64_t rowIndexOffset{};uint32_t rowIndexCount{};uint16_t rowIndexStride{},rowIndexEntrySize{};size_t rowBegin{};};
    struct FamilyIndex{std::vector<size_t> chunks;std::vector<float> prefixMax;};
    struct CacheEntry{std::shared_ptr<std::string> plain;std::list<size_t>::iterator lru;size_t bytes{};};
    struct ChunkResult{bool ok{};bool permanentError{};std::shared_ptr<std::string> plain;std::string error;};
    struct RowsResult{bool ok{true};std::string error;std::vector<V5TimedRow> rows;};
    class Executor {
    public:
        ~Executor(){stop();}
        template<class Fn> auto submit(const std::string& path,Fn&& fn,bool foreground=true){
            using R=std::invoke_result_t<Fn,std::FILE*>;
            auto task=std::make_shared<std::packaged_task<R(std::FILE*)>>(std::forward<Fn>(fn));
            auto future=task->get_future();ensureStarted(path);
            {std::lock_guard<std::mutex> lock(mutex_);if(foreground)foregroundJobs_.push([task](std::FILE* file){(*task)(file);});else if(prefetchJobs_.size()<MAX_PARALLEL_CHUNKS)prefetchJobs_.push([task](std::FILE* file){(*task)(file);});}
            cv_.notify_one();return future;
        }
        void cancelPrefetch(){std::lock_guard<std::mutex> lock(mutex_);while(!prefetchJobs_.empty())prefetchJobs_.pop();}
        void stop(){
            {std::lock_guard<std::mutex> lock(mutex_);if(workers_.empty())return;stopping_=true;while(!prefetchJobs_.empty())prefetchJobs_.pop();}
            cv_.notify_all();for(auto& worker:workers_)if(worker.joinable())worker.join();
            std::lock_guard<std::mutex> lock(mutex_);workers_.clear();while(!foregroundJobs_.empty())foregroundJobs_.pop();while(!prefetchJobs_.empty())prefetchJobs_.pop();stopping_=false;
        }
    private:
        void ensureStarted(const std::string& path){
            std::lock_guard<std::mutex> lock(mutex_);if(!workers_.empty())return;workers_.reserve(MAX_PARALLEL_CHUNKS);
            for(size_t i=0;i<MAX_PARALLEL_CHUNKS;++i)workers_.emplace_back([this,path]{
                std::FILE* file=openTnrdFile(path,"rb");
                for(;;){std::function<void(std::FILE*)> job;{std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[this]{return stopping_||!foregroundJobs_.empty()||!prefetchJobs_.empty();});if(stopping_&&foregroundJobs_.empty()&&prefetchJobs_.empty())break;if(!foregroundJobs_.empty()){job=std::move(foregroundJobs_.front());foregroundJobs_.pop();}else{job=std::move(prefetchJobs_.front());prefetchJobs_.pop();}}job(file);}
                if(file)std::fclose(file);
            });
        }
        std::mutex mutex_;std::condition_variable cv_;bool stopping_{};
        std::queue<std::function<void(std::FILE*)>> foregroundJobs_,prefetchJobs_;std::vector<std::thread> workers_;
    };
    std::FILE* file{};uint64_t fileSize{};HeaderRow header;std::vector<V5LapInfo> laps;std::vector<V5ChunkInfo> chunks;V5ControlSummary summary;
    std::map<std::pair<uint32_t,uint16_t>,FamilyIndex> chunkIndex;std::array<FamilyIndex,15> typeIndex;std::vector<ChunkMeta> chunkMetadata;std::vector<RowSpan> rowSpans;
    std::string path;Executor executor;mutable std::mutex stateMutex;
    size_t cacheLimit{DEFAULT_CACHE_BYTES},cacheBytes{},activeLoads{},peakActiveLoads{};uint64_t decompressions{};std::list<size_t> lru;std::unordered_map<size_t,CacheEntry> cache;
    std::unordered_map<size_t,std::shared_future<ChunkResult>> inFlight;std::unordered_map<size_t,std::string> failedChunks;

    void clear(){
        executor.stop();if(file)std::fclose(file);file=nullptr;fileSize=0;path.clear();laps.clear();chunks.clear();summary={};chunkIndex.clear();for(auto& family:typeIndex)family={};chunkMetadata.clear();rowSpans.clear();
        std::lock_guard<std::mutex> lock(stateMutex);cache.clear();lru.clear();inFlight.clear();failedChunks.clear();cacheBytes=0;activeLoads=0;peakActiveLoads=0;decompressions=0;
    }
    bool loadChunk(size_t index,std::FILE* reader,std::shared_ptr<std::string>& result,std::string* errorOut){
        std::shared_future<ChunkResult> shared;std::shared_ptr<std::promise<ChunkResult>> promise;bool producer=false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            auto hit=cache.find(index);if(hit!=cache.end()){lru.splice(lru.begin(),lru,hit->second.lru);hit->second.lru=lru.begin();result=hit->second.plain;return true;}
            auto failed=failedChunks.find(index);if(failed!=failedChunks.end()){fail(errorOut,failed->second);return false;}
            auto active=inFlight.find(index);if(active!=inFlight.end())shared=active->second;
            else{promise=std::make_shared<std::promise<ChunkResult>>();shared=promise->get_future().share();inFlight.emplace(index,shared);producer=true;}
        }
        if(!producer){const auto loaded=shared.get();if(!loaded.ok){fail(errorOut,loaded.error);return false;}result=loaded.plain;return true;}
        {std::lock_guard<std::mutex> lock(stateMutex);++activeLoads;peakActiveLoads=std::max(peakActiveLoads,activeLoads);}
        ChunkResult loaded;
        if(!reader)loaded.error="could not open a parallel V5 reader handle";
        else if(index>=chunks.size()){loaded.error="V5 chunk index is out of bounds";loaded.permanentError=true;}
        else{
            const auto& c=chunks[index];thread_local std::vector<uint8_t> input;input.resize(CHUNK_PREFIX_SIZE+(size_t)c.compressedSize);const uint8_t* prefix=input.data();
            if(!readAt(reader,c.offset-CHUNK_PREFIX_SIZE,input.data(),input.size())){loaded.error="truncated V5 chunk at lap "+std::to_string(c.lapNumber);loaded.permanentError=true;}
            else if(get32(prefix)!=CHUNK_MAGIC||get32(prefix+4)!=c.lapNumber||get16(prefix+8)!=c.rowType||get16(prefix+10)!=c.flags||get64(prefix+12)!=c.compressedSize||get64(prefix+20)!=c.uncompressedSize||get32(prefix+28)!=c.rowCount){loaded.error="V5 chunk prefix does not match its directory entry";loaded.permanentError=true;}
            else{
                thread_local DecompressionContext decompressor;auto plain=std::make_shared<std::string>((size_t)c.uncompressedSize,'\0');
                {std::lock_guard<std::mutex> lock(stateMutex);++decompressions;}
                const size_t got=decompressor.value?ZSTD_decompressDCtx(decompressor.value,plain->data(),plain->size(),input.data()+CHUNK_PREFIX_SIZE,(size_t)c.compressedSize):ZSTD_CONTENTSIZE_ERROR;
                if(!decompressor.value||ZSTD_isError(got)||got!=c.uncompressedSize||(uint32_t)::crc32(0,(const Bytef*)plain->data(),(uInt)plain->size())!=c.checksum){loaded.error="V5 chunk integrity check failed for lap "+std::to_string(c.lapNumber)+" type "+std::to_string(c.rowType);loaded.permanentError=true;}
                else{loaded.ok=true;loaded.plain=std::move(plain);}
            }
            if(input.capacity()>16ull*1024ull*1024ull){std::vector<uint8_t> release;input.swap(release);}
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            --activeLoads;
            const size_t loadedBytes=loaded.ok?loaded.plain->size():0;
            if(loaded.ok&&loadedBytes<=cacheLimit){while(cacheBytes+loadedBytes>cacheLimit&&!lru.empty()){const size_t old=lru.back();lru.pop_back();auto it=cache.find(old);cacheBytes-=it->second.bytes;cache.erase(it);}lru.push_front(index);cache[index]={loaded.plain,lru.begin(),loadedBytes};cacheBytes+=loadedBytes;}
            else if(!loaded.ok&&loaded.permanentError)failedChunks[index]=loaded.error;
        }
        promise->set_value(loaded);{std::lock_guard<std::mutex> lock(stateMutex);inFlight.erase(index);}
        if(!loaded.ok){fail(errorOut,loaded.error);return false;}result=std::move(loaded.plain);return true;
    }
    bool splitChunk(size_t index,std::FILE* reader,std::vector<V5TimedRow>& out,std::string* errorOut,std::shared_ptr<std::string> plain={},bool filter=false,float from=0,float to=0,bool latestOnly=false,const IndexedCancelCheck& cancelled={}){
        if(cancelled&&cancelled()){fail(errorOut,"indexed read cancelled");return false;}if(index>=chunks.size()||index>=chunkMetadata.size()){fail(errorOut,"V5 chunk index is out of bounds");return false;}const auto& c=chunks[index];const auto& metadata=chunkMetadata[index];if(!plain&&!loadChunk(index,reader,plain,errorOut))return false;
        auto spanBegin=rowSpans.begin()+(ptrdiff_t)metadata.rowBegin,spanEnd=spanBegin+(ptrdiff_t)metadata.rowIndexCount;
        if(metadata.rowIndexStride==1&&filter){spanBegin=std::lower_bound(spanBegin,spanEnd,from,[](const RowSpan& row,float value){return row.firstTime<value;});spanEnd=std::upper_bound(spanBegin,spanEnd,to,[](float value,const RowSpan& row){return value<row.firstTime;});if(latestOnly&&spanBegin!=spanEnd)spanBegin=std::prev(spanEnd);}
        if(!latestOnly){if(!filter)out.reserve(c.rowCount);else{size_t estimate=0;for(auto it=spanBegin;it!=spanEnd;++it)if(!(it->lastTime<from||it->firstTime>to))estimate+=it->count;out.reserve(estimate);}}
        float bestTime=-std::numeric_limits<float>::infinity();uint32_t bestOffset{},bestLength{};bool haveBest=false;size_t spanOrdinal=0;for(auto spanIt=spanBegin;spanIt!=spanEnd;++spanIt,++spanOrdinal){if((spanOrdinal&63u)==0&&cancelled&&cancelled()){fail(errorOut,"indexed read cancelled");return false;}const auto& span=*spanIt;if(span.offset>plain->size()||span.length>plain->size()-span.offset){fail(errorOut,"V5 row index points outside its chunk");return false;}if(filter&&(span.lastTime<from||span.firstTime>to))continue;if(metadata.rowIndexStride==1){const std::string_view line(plain->data()+span.offset,span.length);const float time=scanTime(line);if(rowType(line)!=(uint8_t)c.rowType||time!=span.firstTime||time!=span.lastTime){fail(errorOut,"V5 row index does not match its chunk payload");return false;}if(filter&&(time<from||time>to))continue;if(latestOnly){if(!haveBest||time>=bestTime){haveBest=true;bestTime=time;bestOffset=span.offset;bestLength=span.length;}}else out.push_back({time,(uint8_t)c.rowType,c.sequence,std::string(line),span.offset});continue;}
            const size_t end=span.offset+span.length;size_t pos=span.offset;uint16_t count=0;float blockFirst=std::numeric_limits<float>::infinity(),blockLast=-std::numeric_limits<float>::infinity();while(pos<end){if((count&63u)==0&&cancelled&&cancelled()){fail(errorOut,"indexed read cancelled");return false;}size_t nl=plain->find('\n',pos);if(nl==std::string::npos||nl>end)nl=end;if(nl>pos){const std::string_view line(plain->data()+pos,nl-pos);const float time=scanTime(line);if(rowType(line)!=(uint8_t)c.rowType||!std::isfinite(time)){fail(errorOut,"V5 hot-row block does not match its chunk payload");return false;}blockFirst=std::min(blockFirst,time);blockLast=std::max(blockLast,time);if(!filter||(time>=from&&time<=to)){if(latestOnly){if(!haveBest||time>=bestTime){haveBest=true;bestTime=time;bestOffset=(uint32_t)pos;bestLength=(uint32_t)(nl-pos);}}else out.push_back({time,(uint8_t)c.rowType,c.sequence,std::string(line),(uint32_t)pos});}++count;}if(nl==end)break;pos=nl+1;}if(count!=span.count||blockFirst!=span.firstTime||blockLast!=span.lastTime){fail(errorOut,"V5 hot-row block metadata mismatch");return false;}}
        if(latestOnly&&haveBest)out.push_back({bestTime,(uint8_t)c.rowType,c.sequence,plain->substr(bestOffset,bestLength),bestOffset});return true;
    }
    void appendFamily(FamilyIndex& family,size_t index){
        family.chunks.push_back(index);family.prefixMax.push_back(std::max(family.prefixMax.empty()?-std::numeric_limits<float>::infinity():family.prefixMax.back(),chunkMetadata[index].last));
    }
    void selectRange(const FamilyIndex& family,float from,float to,std::vector<size_t>& out)const{
        auto first=std::lower_bound(family.prefixMax.begin(),family.prefixMax.end(),from);size_t pos=(size_t)(first-family.prefixMax.begin());for(;pos<family.chunks.size();++pos){const size_t index=family.chunks[pos];const auto& meta=chunkMetadata[index];if(meta.first>to)break;if(meta.last>=from)out.push_back(index);}
    }
    template<class Fn>std::vector<RowsResult> runChunkJobs(const std::vector<size_t>& indices,Fn fn,const IndexedCancelCheck& cancelled={}){
        struct Completed{std::mutex mutex;std::condition_variable cv;std::deque<std::pair<size_t,RowsResult>> ready;};
        if(indices.empty())return {};auto completed=std::make_shared<Completed>();std::vector<RowsResult> results(indices.size());std::vector<size_t> order(indices.size());for(size_t i=0;i<order.size();++i)order[i]=i;std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return chunks[indices[a]].offset<chunks[indices[b]].offset;});
        size_t next=0,active=0,done=0;bool wasCancelled=false;
        auto schedule=[&](size_t ordinal){const size_t index=indices[ordinal];++active;(void)executor.submit(path,[fn,index,ordinal,cancelled,completed](std::FILE* file){RowsResult result;try{if(cancelled&&cancelled()){result.ok=false;result.error="indexed read cancelled";}else result=fn(file,index);}catch(const std::exception& e){result.ok=false;result.error=e.what();}catch(...){result.ok=false;result.error="indexed read failed";}{std::lock_guard<std::mutex> lock(completed->mutex);completed->ready.emplace_back(ordinal,std::move(result));}completed->cv.notify_one();});};
        while(next<order.size()&&active<MAX_PARALLEL_CHUNKS&&(!cancelled||!cancelled()))schedule(order[next++]);
        while(active){std::unique_lock<std::mutex> lock(completed->mutex);completed->cv.wait(lock,[&]{return !completed->ready.empty();});while(!completed->ready.empty()){auto ready=std::move(completed->ready.front());completed->ready.pop_front();results[ready.first]=std::move(ready.second);--active;++done;}lock.unlock();if(cancelled&&cancelled())wasCancelled=true;while(!wasCancelled&&next<order.size()&&active<MAX_PARALLEL_CHUNKS)schedule(order[next++]);}
        if(wasCancelled||done!=indices.size())return {};return results;
    }
};

TnrdV5Archive::TnrdV5Archive():impl_(std::make_unique<Impl>()){}
TnrdV5Archive::~TnrdV5Archive(){close();}
void TnrdV5Archive::close(){impl_->clear();}
bool TnrdV5Archive::isOpen()const{return impl_->file!=nullptr;}
const std::vector<V5LapInfo>& TnrdV5Archive::laps()const{return impl_->laps;}
const std::vector<V5ChunkInfo>& TnrdV5Archive::chunks()const{return impl_->chunks;}
const V5ControlSummary& TnrdV5Archive::summary()const{return impl_->summary;}
float TnrdV5Archive::startTime()const{return impl_->summary.startSessionTime;}
float TnrdV5Archive::totalTime()const{return std::max(impl_->summary.totalSessionTime,impl_->laps.empty()?0.0f:impl_->laps.back().endSessionTime);}
int TnrdV5Archive::lapAt(float time)const{const auto it=std::upper_bound(impl_->laps.begin(),impl_->laps.end(),time,[](float value,const V5LapInfo& lap){return value<lap.startSessionTime;});return it==impl_->laps.begin()?0:(int)std::prev(it)->lapNumber;}

void TnrdV5Archive::chunkIndicesForLap(uint32_t lap,V5RowTypeMask mask,std::vector<size_t>& out)const{
    out.clear();auto it=impl_->chunkIndex.lower_bound({lap,0});for(;it!=impl_->chunkIndex.end()&&it->first.first==lap;++it)if(mask&v5TypeBit((uint8_t)it->first.second))out.insert(out.end(),it->second.chunks.begin(),it->second.chunks.end());
}
bool TnrdV5Archive::chunkTimeBounds(size_t index,float& firstOut,float& lastOut)const{
    if(index>=impl_->chunkMetadata.size())return false;firstOut=impl_->chunkMetadata[index].first;lastOut=impl_->chunkMetadata[index].last;return true;
}
void TnrdV5Archive::prefetchChunk(size_t index){
    if(index>=impl_->chunks.size())return;(void)impl_->executor.submit(impl_->path,[state=impl_.get(),index](std::FILE* file){std::shared_ptr<std::string> plain;std::string error;return state->loadChunk(index,file,plain,&error);},false);
}
void TnrdV5Archive::cancelPrefetch(){impl_->executor.cancelPrefetch();}

bool TnrdV5Archive::open(const std::string& path,HeaderRow& header,std::string* errorOut){
    close();impl_->file=openTnrdFile(path,"rb");if(!impl_->file){fail(errorOut,"could not open V5 file");return false;}impl_->path=path;if(!seekEnd(impl_->file)){fail(errorOut,"could not size V5 file");close();return false;}impl_->fileSize=tell(impl_->file);
    std::array<uint8_t,HEADER_SIZE> h{};if(!readAt(impl_->file,0,h.data(),h.size())||!std::equal(MAGIC.begin(),MAGIC.end(),h.begin())||get16(h.data()+8)!=5||get16(h.data()+10)!=HEADER_SIZE){fail(errorOut,"invalid V5 header");close();return false;}
    const uint32_t stored=get32(h.data()+104);auto copy=h;std::fill(copy.begin()+104,copy.begin()+108,0);const bool headerCrcOk=stored==(uint32_t)::crc32(0,copy.data(),104);
    if(headerCrcOk&&(get32(h.data()+12)!=1||get32(h.data()+108)!=0)){fail(errorOut,"V5 file uses unsupported feature flags");close();return false;}
    uint64_t mo=get64(h.data()+16),ms=get64(h.data()+24);const uint64_t so=get64(h.data()+72),ss=get64(h.data()+80);uint64_t ro=get64(h.data()+88),rs=get64(h.data()+96);uint64_t lo=get64(h.data()+32),co=get64(h.data()+48),fo=get64(h.data()+64);uint32_t lc=get32(h.data()+40),cc=get32(h.data()+56);
    if(headerCrcOk&&(get32(h.data()+44)!=LAP_ENTRY_SIZE||get32(h.data()+60)!=CHUNK_ENTRY_SIZE)){fail(errorOut,"V5 header table sizes are invalid");close();return false;}
    uint64_t validatedFooter=UINT64_MAX;std::string validatedSummary;std::vector<uint8_t> validatedLaps,validatedChunks,validatedRows;
    auto validateFooter=[&](uint64_t offset,uint64_t& lapOff,uint32_t& lapCount,uint64_t& chunkOff,uint32_t& chunkCount,uint64_t& rowOff,uint64_t& rowSize,uint64_t& summaryOff,uint64_t& summarySize,uint64_t& recoveredMetadataSize,uint32_t& expectedCrc,bool checkTables)->bool{
        std::array<uint8_t,FOOTER_SIZE> f{};if(!rangeOk(impl_->fileSize,offset,FOOTER_SIZE)||!readAt(impl_->file,offset,f.data(),f.size())||get32(f.data())!=FOOTER_MAGIC||get16(f.data()+4)!=5||get16(f.data()+6)!=FOOTER_SIZE)return false;
        lapOff=get64(f.data()+8);chunkOff=get64(f.data()+16);rowOff=get64(f.data()+24);if(chunkOff<lapOff||rowOff<chunkOff||offset<rowOff||(chunkOff-lapOff)%LAP_ENTRY_SIZE||(rowOff-chunkOff)%CHUNK_ENTRY_SIZE||(offset-rowOff)%ROW_INDEX_ENTRY_SIZE)return false;
        const uint64_t derivedLaps=(chunkOff-lapOff)/LAP_ENTRY_SIZE,derivedChunks=(rowOff-chunkOff)/CHUNK_ENTRY_SIZE;if(derivedLaps>MAX_LAPS||derivedChunks>MAX_CHUNKS)return false;lapCount=(uint32_t)derivedLaps;chunkCount=(uint32_t)derivedChunks;rowSize=offset-rowOff;
        summarySize=get32(f.data()+36);if(summarySize>MAX_SUMMARY_BYTES||summarySize>lapOff)return false;summaryOff=lapOff-summarySize;
        std::array<uint8_t,METADATA_PREFIX_SIZE> prefix{};if(!readAt(impl_->file,HEADER_SIZE,prefix.data(),prefix.size())||get32(prefix.data())!=METADATA_MAGIC)return false;recoveredMetadataSize=get64(prefix.data()+8);if(recoveredMetadataSize>MAX_METADATA_BYTES||!rangeOk(impl_->fileSize,HEADER_SIZE+METADATA_PREFIX_SIZE,recoveredMetadataSize)||summaryOff<HEADER_SIZE+METADATA_PREFIX_SIZE+recoveredMetadataSize)return false;
        expectedCrc=get32(f.data()+32);if(!checkTables)return true;std::string summary((size_t)summarySize,'\0');std::vector<uint8_t> l((size_t)(chunkOff-lapOff)),c((size_t)(rowOff-chunkOff)),r((size_t)rowSize);if(!readAt(impl_->file,summaryOff,summary.data(),summary.size())||!readAt(impl_->file,lapOff,l.data(),l.size())||!readAt(impl_->file,chunkOff,c.data(),c.size())||!readAt(impl_->file,rowOff,r.data(),r.size())||expectedCrc!=controlCrc(summary,l,c,r))return false;validatedFooter=offset;validatedSummary=std::move(summary);validatedLaps=std::move(l);validatedChunks=std::move(c);validatedRows=std::move(r);return true;
    };
    uint64_t vlo{},vco{},vro{},vrs{},vso{},vss{},vms{};uint32_t vlc{},vcc{},expectedTablesCrc{};bool footerOk=headerCrcOk&&ms<=MAX_METADATA_BYTES&&rangeOk(impl_->fileSize,mo,ms)&&validateFooter(fo,vlo,vlc,vco,vcc,vro,vrs,vso,vss,vms,expectedTablesCrc,true)&&vlo==lo&&vlc==lc&&vco==co&&vcc==cc&&vro==ro&&vrs==rs&&vso==so&&vss==ss&&vms==ms;
    if(!footerOk){
        bool recovered=false;constexpr uint64_t BLOCK=1024ull*1024ull;uint64_t end=impl_->fileSize;
        while(end>HEADER_SIZE&&!recovered){const uint64_t begin=end>BLOCK?std::max<uint64_t>(HEADER_SIZE,end-BLOCK):HEADER_SIZE;std::vector<uint8_t> scan((size_t)(end-begin));if(!readAt(impl_->file,begin,scan.data(),scan.size()))break;for(size_t i=scan.size();i-- >0;){if(i+4>scan.size()||get32(scan.data()+i)!=FOOTER_MAGIC)continue;const uint64_t pos=begin+i;if(validateFooter(pos,vlo,vlc,vco,vcc,vro,vrs,vso,vss,vms,expectedTablesCrc,true)){lo=vlo;lc=vlc;co=vco;cc=vcc;ro=vro;rs=vrs;fo=pos;mo=HEADER_SIZE+METADATA_PREFIX_SIZE;ms=vms;recovered=true;break;}}if(begin==HEADER_SIZE)break;end=begin+3;}
        if(!recovered){fail(errorOut,headerCrcOk?"V5 commit footer is invalid":"V5 header checksum mismatch and no recoverable checkpoint exists");close();return false;}
    }
    // A crash can occur after a checkpoint footer is flushed but before the
    // fixed header is patched. If bytes follow the header's valid snapshot,
    // inspect only that tail and prefer its newest fully checksummed footer.
    if(footerOk&&impl_->fileSize>fo+FOOTER_SIZE){bool newer=false;constexpr uint64_t BLOCK=1024ull*1024ull;uint64_t end=impl_->fileSize;while(end>fo+FOOTER_SIZE&&!newer){const uint64_t begin=end>BLOCK?std::max<uint64_t>(fo+FOOTER_SIZE,end-BLOCK):fo+FOOTER_SIZE;std::vector<uint8_t> scan((size_t)(end-begin));if(!readAt(impl_->file,begin,scan.data(),scan.size()))break;for(size_t i=scan.size();i-- >0;){if(i+4>scan.size()||get32(scan.data()+i)!=FOOTER_MAGIC)continue;const uint64_t pos=begin+i;if(pos<=fo)continue;if(validateFooter(pos,vlo,vlc,vco,vcc,vro,vrs,vso,vss,vms,expectedTablesCrc,true)){lo=vlo;lc=vlc;co=vco;cc=vcc;ro=vro;rs=vrs;fo=pos;mo=HEADER_SIZE+METADATA_PREFIX_SIZE;ms=vms;newer=true;footerOk=false;break;}}if(begin==fo+FOOTER_SIZE)break;end=begin+3;}}
    std::array<uint8_t,METADATA_PREFIX_SIZE> metadataPrefix{};if(!readAt(impl_->file,HEADER_SIZE,metadataPrefix.data(),metadataPrefix.size())||get32(metadataPrefix.data())!=METADATA_MAGIC||get64(metadataPrefix.data()+8)!=ms||mo!=HEADER_SIZE+METADATA_PREFIX_SIZE){fail(errorOut,"invalid V5 metadata prefix");close();return false;}
    std::string metadata((size_t)ms,'\0');if(!readAt(impl_->file,mo,metadata.data(),metadata.size())||get32(metadataPrefix.data()+4)!=(uint32_t)::crc32(0,(const Bytef*)metadata.data(),(uInt)metadata.size())||glz::read_json(impl_->header,metadata)||impl_->header.magic!="TNRD_V5"||!impl_->header.compression||*impl_->header.compression!="zstd"){fail(errorOut,"invalid V5 session metadata");close();return false;}header=impl_->header;
    const uint64_t activeSummaryOffset=footerOk?so:vso,activeSummarySize=footerOk?ss:vss;
    if(validatedFooter!=fo||validatedSummary.size()!=activeSummarySize||validatedLaps.size()!=(size_t)lc*LAP_ENTRY_SIZE||validatedChunks.size()!=(size_t)cc*CHUNK_ENTRY_SIZE||validatedRows.size()!=rs){fail(errorOut,"could not retain V5 control tables");close();return false;}
    std::string summaryJson=std::move(validatedSummary);if((activeSummaryOffset||activeSummarySize)&&glz::read_json(impl_->summary,summaryJson)){fail(errorOut,"invalid V5 control summary");close();return false;}
    std::vector<uint8_t> lapBytes=std::move(validatedLaps),dir=std::move(validatedChunks),rowIndexBytes=std::move(validatedRows);impl_->laps.reserve(lc);impl_->chunks.reserve(cc);impl_->chunkMetadata.reserve(cc);impl_->rowSpans.reserve(rowIndexBytes.size()/ROW_INDEX_ENTRY_SIZE);
    std::set<uint32_t> lapKeys;float previousStart=-std::numeric_limits<float>::infinity(),previousEnd=-std::numeric_limits<float>::infinity();uint32_t previousNumber=0;for(uint32_t i=0;i<lc;++i){const uint8_t*p=lapBytes.data()+i*LAP_ENTRY_SIZE;V5LapInfo lap{get32(p),getFloat(p+4),getFloat(p+8),get32(p+12),get32(p+16)};const bool lapTimeInvalid=((lap.flags&1u)!=0)!=(lap.lapTimeMs>0);if(!lap.lapNumber||lap.lapNumber<=previousNumber||(lap.flags&~1u)||get32(p+20)!=0||lapTimeInvalid||!std::isfinite(lap.startSessionTime)||!std::isfinite(lap.endSessionTime)||lap.endSessionTime<lap.startSessionTime||lap.startSessionTime<previousStart||lap.startSessionTime<previousEnd||!lapKeys.insert(lap.lapNumber).second){fail(errorOut,"invalid V5 lap table");close();return false;}previousNumber=lap.lapNumber;previousStart=lap.startSessionTime;previousEnd=lap.endSessionTime;impl_->laps.push_back(lap);}
    if(!std::isfinite(impl_->summary.initialFuelKg)||!std::isfinite(impl_->summary.startSessionTime)||!std::isfinite(impl_->summary.totalSessionTime)||impl_->summary.totalSessionTime<impl_->summary.startSessionTime){fail(errorOut,"invalid V5 control summary values");close();return false;}std::set<uint32_t> summaryLaps;for(const auto&s:impl_->summary.lapStatus)if((s.lapNumber&&!lapKeys.count(s.lapNumber))||!std::isfinite(s.sessionTime)||!std::isfinite(s.ersPct)||!summaryLaps.insert(s.lapNumber).second){fail(errorOut,"invalid V5 lap-status summary");close();return false;}
    auto overlaps=[](uint64_t a,uint64_t an,uint64_t b,uint64_t bn){return an<=UINT64_MAX-a&&bn<=UINT64_MAX-b&&a<b+bn&&b<a+an;};
    const uint64_t maxCompressed=ZSTD_compressBound((size_t)MAX_CHUNK_PLAIN);std::vector<std::pair<uint64_t,uint64_t>> payloadRanges;payloadRanges.reserve(cc);std::vector<Impl::RowSpan> parsedRows;std::set<std::tuple<uint32_t,uint16_t,uint64_t>> keys;std::array<float,15> familyPrefix;familyPrefix.fill(-std::numeric_limits<float>::infinity());bool havePreviousChunk=false;uint16_t previousChunkType{};float previousChunkFirst{};uint64_t previousChunkSequence{};
    for(uint32_t i=0;i<cc;++i){
        const uint8_t*p=dir.data()+i*CHUNK_ENTRY_SIZE;V5ChunkInfo c{get32(p),get16(p+4),get16(p+6),get64(p+8),get64(p+16),get64(p+24),get32(p+32),get32(p+36),get64(p+40)};Impl::ChunkMeta meta{getFloat(p+48),getFloat(p+52),getFloat(p+56),getFloat(p+60),getFloat(p+64),get64(p+68),get32(p+76),get16(p+80),get16(p+82)};
        const bool payloadOverflow=c.compressedSize>UINT64_MAX-CHUNK_PREFIX_SIZE;const uint64_t payloadStart=c.offset>=CHUNK_PREFIX_SIZE?c.offset-CHUNK_PREFIX_SIZE:0,payloadSize=payloadOverflow?0:c.compressedSize+CHUNK_PREFIX_SIZE;const bool controlOverlap=!payloadOverflow&&(overlaps(payloadStart,payloadSize,0,HEADER_SIZE)||overlaps(payloadStart,payloadSize,HEADER_SIZE,METADATA_PREFIX_SIZE)||overlaps(payloadStart,payloadSize,mo,ms)||(activeSummarySize&&overlaps(payloadStart,payloadSize,activeSummaryOffset,activeSummarySize))||overlaps(payloadStart,payloadSize,lo,(uint64_t)lc*LAP_ENTRY_SIZE)||overlaps(payloadStart,payloadSize,co,(uint64_t)cc*CHUNK_ENTRY_SIZE)||overlaps(payloadStart,payloadSize,ro,rs)||overlaps(payloadStart,payloadSize,fo,FOOTER_SIZE));
        const bool distancesValid=(std::isnan(meta.minDistance)&&std::isnan(meta.maxDistance))||(std::isfinite(meta.minDistance)&&std::isfinite(meta.maxDistance)&&meta.minDistance<=meta.maxDistance);const bool indexOverflow=meta.rowIndexCount>UINT64_MAX/ROW_INDEX_ENTRY_SIZE;const uint64_t indexBytes=indexOverflow?0:(uint64_t)meta.rowIndexCount*ROW_INDEX_ENTRY_SIZE;const float expectedPrefix=c.rowType<=14?std::max(familyPrefix[c.rowType],meta.last):meta.last;
        const uint16_t expectedStride=(c.rowType==1||c.rowType==11||c.rowType==12||c.rowType==13)?HOT_ROW_INDEX_STRIDE:1;
        const bool directoryOrdered=!havePreviousChunk||c.rowType>previousChunkType||(c.rowType==previousChunkType&&(meta.first>previousChunkFirst||(meta.first==previousChunkFirst&&c.sequence>=previousChunkSequence)));
        if((c.lapNumber&&!lapKeys.count(c.lapNumber))||c.rowType>14||c.flags||!c.sequence||!c.compressedSize||!c.uncompressedSize||!c.rowCount||c.uncompressedSize>MAX_CHUNK_PLAIN||c.compressedSize>maxCompressed||c.compressedSize>SIZE_MAX||payloadOverflow||!rangeOk(impl_->fileSize,c.offset,c.compressedSize)||c.offset<CHUNK_PREFIX_SIZE||controlOverlap||!keys.emplace(c.lapNumber,c.rowType,c.sequence).second||!std::isfinite(meta.first)||!std::isfinite(meta.last)||meta.last<meta.first||!std::isfinite(meta.prefixMax)||meta.prefixMax!=expectedPrefix||!distancesValid||!meta.rowIndexCount||meta.rowIndexStride!=expectedStride||meta.rowIndexEntrySize!=ROW_INDEX_ENTRY_SIZE||get32(p+84)!=0||indexOverflow||meta.rowIndexOffset<ro||!rangeOk(ro+rs,meta.rowIndexOffset,indexBytes)||!directoryOrdered){
            fail(errorOut,"invalid or duplicate V5 chunk entry");close();return false;
        }
        havePreviousChunk=true;previousChunkType=c.rowType;previousChunkFirst=meta.first;previousChunkSequence=c.sequence;familyPrefix[c.rowType]=expectedPrefix;meta.rowBegin=impl_->rowSpans.size();const size_t relative=(size_t)(meta.rowIndexOffset-ro);const bool exact=meta.rowIndexStride==1;uint32_t previousEnd=0,totalIndexedRows=0;float indexedFirst=std::numeric_limits<float>::infinity(),indexedLast=-std::numeric_limits<float>::infinity();parsedRows.clear();parsedRows.reserve(meta.rowIndexCount);
        for(uint32_t row=0;row<meta.rowIndexCount;++row){const uint8_t*q=rowIndexBytes.data()+relative+(size_t)row*ROW_INDEX_ENTRY_SIZE;const float firstTime=getFloat(q),lastTime=getFloat(q+4);const uint32_t rowOffset=get32(q+8),rowLength=get32(q+12),ordinal=get32(q+16);const uint16_t count=get16(q+20),reserved=get16(q+22);const bool physicalBlockValid=exact||(ordinal==totalIndexedRows&&(!row||rowOffset>previousEnd));if(!std::isfinite(firstTime)||!std::isfinite(lastTime)||lastTime<firstTime||!rowLength||rowOffset>c.uncompressedSize||rowLength>c.uncompressedSize-rowOffset||!count||count>meta.rowIndexStride||reserved||!physicalBlockValid||(exact&&(count!=1||ordinal>=c.rowCount))){fail(errorOut,"invalid V5 row index");close();return false;}previousEnd=rowOffset+rowLength-1;totalIndexedRows+=count;indexedFirst=std::min(indexedFirst,firstTime);indexedLast=std::max(indexedLast,lastTime);parsedRows.push_back({rowOffset,rowLength,firstTime,lastTime,ordinal,count});}
        if(totalIndexedRows!=c.rowCount||indexedFirst!=meta.first||indexedLast!=meta.last){fail(errorOut,"V5 row index time bounds mismatch");close();return false;}if(exact){std::sort(parsedRows.begin(),parsedRows.end(),[](const auto& a,const auto& b){return a.ordinal<b.ordinal;});previousEnd=0;for(size_t row=0;row<parsedRows.size();++row){const auto& span=parsedRows[row];if(span.ordinal!=row||(row&&span.offset<=previousEnd)){fail(errorOut,"invalid V5 exact row index coverage");close();return false;}previousEnd=span.offset+span.length-1;}std::stable_sort(parsedRows.begin(),parsedRows.end(),[](const auto& a,const auto& b){return a.firstTime<b.firstTime||(a.firstTime==b.firstTime&&a.offset<b.offset);});}impl_->rowSpans.insert(impl_->rowSpans.end(),parsedRows.begin(),parsedRows.end());
        payloadRanges.emplace_back(payloadStart,payloadStart+payloadSize);impl_->chunks.push_back(c);impl_->chunkMetadata.push_back(std::move(meta));
    }
    std::sort(payloadRanges.begin(),payloadRanges.end());for(size_t i=1;i<payloadRanges.size();++i)if(payloadRanges[i].first<payloadRanges[i-1].second){fail(errorOut,"V5 chunk payload ranges overlap");close();return false;}for(size_t i=0;i<impl_->chunks.size();++i){const auto& chunk=impl_->chunks[i];impl_->appendFamily(impl_->typeIndex[chunk.rowType],i);impl_->appendFamily(impl_->chunkIndex[{chunk.lapNumber,chunk.rowType}],i);}
    return true;
}

bool TnrdV5Archive::rowsForChunks(const std::vector<size_t>& indices,
                                  std::vector<std::vector<V5TimedRow>>& out,
                                  std::string* errorOut){
    out.clear();for(size_t index:indices)if(index>=impl_->chunks.size()){fail(errorOut,"V5 chunk index is out of bounds");return false;}
    auto results=impl_->runChunkJobs(indices,[state=impl_.get()](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error))result.ok=false;return result;});
    for(const auto& result:results)if(!result.ok){fail(errorOut,result.error);return false;}
    out.reserve(results.size());for(auto& result:results){for(auto& row:result.rows)if(scanTime(row.json)<0)row.json=withSessionTime(row.json,row.sessionTime);sortRows(result.rows);out.push_back(std::move(result.rows));}return true;
}
bool TnrdV5Archive::rowsForChunksRange(const std::vector<size_t>& indices,float from,float to,
                                       std::vector<std::vector<V5TimedRow>>& out,
                                       std::string* errorOut,const IndexedCancelCheck& cancelled){
    out.clear();if(std::isnan(from)||std::isnan(to)||to<from){fail(errorOut,"invalid V5 time range");return false;}for(size_t index:indices)if(index>=impl_->chunks.size()){fail(errorOut,"V5 chunk index is out of bounds");return false;}
    auto results=impl_->runChunkJobs(indices,[state=impl_.get(),from,to,cancelled](std::FILE* file,size_t index){Impl::RowsResult result;const auto& meta=state->chunkMetadata[index];if(meta.last<from||meta.first>to)return result;if(!state->splitChunk(index,file,result.rows,&result.error,{},true,from,to,false,cancelled))result.ok=false;return result;},cancelled);
    if(results.size()!=indices.size()){fail(errorOut,"indexed read cancelled");return false;}for(const auto& result:results)if(!result.ok){fail(errorOut,result.error);return false;}
    out.reserve(results.size());for(auto& result:results){for(auto& row:result.rows)if(scanTime(row.json)<0)row.json=withSessionTime(row.json,row.sessionTime);sortRows(result.rows);out.push_back(std::move(result.rows));}return true;
}

bool TnrdV5Archive::rowsForLap(uint32_t lap,V5RowTypeMask mask,std::vector<V5TimedRow>& out,std::string* errorOut){
    std::vector<size_t> selected;auto it=impl_->chunkIndex.lower_bound({lap,0});for(;it!=impl_->chunkIndex.end()&&it->first.first==lap;++it)if(mask&v5TypeBit((uint8_t)it->first.second))selected.insert(selected.end(),it->second.chunks.begin(),it->second.chunks.end());
    auto results=impl_->runChunkJobs(selected,[state=impl_.get()](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error))result.ok=false;return result;});
    std::vector<std::vector<V5TimedRow>> groups;groups.reserve(results.size());for(auto& result:results){if(!result.ok){fail(errorOut,result.error);return false;}groups.push_back(std::move(result.rows));}mergeRowGroups(groups,out);return true;
}
bool TnrdV5Archive::rowsForLapRange(uint32_t lap,float from,float to,V5RowTypeMask mask,std::vector<V5TimedRow>& out,std::string* errorOut,const IndexedCancelCheck& cancelled){
    if(std::isnan(from)||std::isnan(to)||to<from){fail(errorOut,"invalid V5 time range");return false;}std::vector<size_t> selected;auto it=impl_->chunkIndex.lower_bound({lap,0});for(;it!=impl_->chunkIndex.end()&&it->first.first==lap;++it)if(mask&v5TypeBit((uint8_t)it->first.second))impl_->selectRange(it->second,from,to,selected);
    auto results=impl_->runChunkJobs(selected,[state=impl_.get(),from,to,cancelled](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error,{},true,from,to,false,cancelled))result.ok=false;return result;},cancelled);
    if(results.size()!=selected.size()){fail(errorOut,"indexed read cancelled");return false;}
    std::vector<std::vector<V5TimedRow>> groups;groups.reserve(results.size());for(auto& result:results){if(!result.ok){fail(errorOut,result.error);return false;}groups.push_back(std::move(result.rows));}mergeRowGroups(groups,out);return true;
}
bool TnrdV5Archive::rowsForRange(float from,float to,V5RowTypeMask mask,std::vector<V5TimedRow>& out,std::string* errorOut,const IndexedCancelCheck& cancelled){
    if(std::isnan(from)||std::isnan(to)||to<from){fail(errorOut,"invalid V5 time range");return false;}std::vector<size_t> selected;for(uint8_t type=0;type<impl_->typeIndex.size();++type)if(mask&v5TypeBit(type))impl_->selectRange(impl_->typeIndex[type],from,to,selected);
    auto results=impl_->runChunkJobs(selected,[state=impl_.get(),from,to,cancelled](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error,{},true,from,to,false,cancelled))result.ok=false;return result;},cancelled);
    if(results.size()!=selected.size()){fail(errorOut,"indexed read cancelled");return false;}
    std::vector<std::vector<V5TimedRow>> groups;groups.reserve(results.size());for(auto& result:results){if(!result.ok){fail(errorOut,result.error);return false;}groups.push_back(std::move(result.rows));}mergeRowGroups(groups,out);return true;
}
bool TnrdV5Archive::latestRows(float at,const std::vector<uint8_t>& types,std::vector<V5TimedRow>& out,std::string* errorOut,const IndexedCancelCheck& cancelled){
    if(cancelled&&cancelled()){fail(errorOut,"indexed read cancelled");return false;}if(std::isnan(at)){fail(errorOut,"invalid V5 latest-row time");return false;}struct Candidate{uint8_t type{};size_t index{};};std::vector<Candidate> candidates;std::array<bool,15> seen{};
    for(uint8_t type:types){if(type>=impl_->typeIndex.size()||seen[type])continue;seen[type]=true;const auto& family=impl_->typeIndex[type];const auto upper=std::upper_bound(family.chunks.begin(),family.chunks.end(),at,[&](float value,size_t index){return value<impl_->chunkMetadata[index].first;});const size_t end=(size_t)(upper-family.chunks.begin());if(!end)continue;
        const bool exact=impl_->chunkMetadata[family.chunks.front()].rowIndexStride==1;if(exact){size_t bestChunk=SIZE_MAX;float bestTime=-std::numeric_limits<float>::infinity();uint64_t bestSequence=0;for(size_t pos=end;pos-- >0;){if(bestChunk!=SIZE_MAX&&family.prefixMax[pos]<bestTime)break;const size_t index=family.chunks[pos];const auto& meta=impl_->chunkMetadata[index];auto begin=impl_->rowSpans.begin()+(ptrdiff_t)meta.rowBegin,finish=begin+(ptrdiff_t)meta.rowIndexCount;const auto row=std::upper_bound(begin,finish,at,[](float value,const Impl::RowSpan& span){return value<span.firstTime;});if(row==begin)continue;const float time=std::prev(row)->firstTime;const uint64_t sequence=impl_->chunks[index].sequence;if(bestChunk==SIZE_MAX||time>bestTime||(time==bestTime&&sequence>bestSequence)){bestChunk=index;bestTime=time;bestSequence=sequence;}}if(bestChunk!=SIZE_MAX)candidates.push_back({type,bestChunk});continue;}
        const float bestUpper=std::min(at,family.prefixMax[end-1]);if(bestUpper==at){const auto first=std::lower_bound(family.prefixMax.begin(),family.prefixMax.begin()+(ptrdiff_t)end,at);for(size_t pos=(size_t)(first-family.prefixMax.begin());pos<end;++pos){const size_t index=family.chunks[pos];if(impl_->chunkMetadata[index].last>=at)candidates.push_back({type,index});}}else{for(size_t pos=end;pos-- >0;){if(family.prefixMax[pos]<bestUpper)break;const size_t index=family.chunks[pos];if(impl_->chunkMetadata[index].last==bestUpper)candidates.push_back({type,index});}}
    }
    std::vector<size_t> selected;selected.reserve(candidates.size());for(const auto& candidate:candidates)selected.push_back(candidate.index);auto results=impl_->runChunkJobs(selected,[state=impl_.get(),at,cancelled](std::FILE* file,size_t index){Impl::RowsResult result;if(!state->splitChunk(index,file,result.rows,&result.error,{},true,-std::numeric_limits<float>::infinity(),at,true,cancelled))result.ok=false;return result;},cancelled);if(results.size()!=selected.size()){fail(errorOut,"indexed read cancelled");return false;}std::array<V5TimedRow,15> best{};std::array<bool,15> have{};for(size_t i=0;i<results.size();++i){auto& result=results[i];if(!result.ok){fail(errorOut,result.error);return false;}for(auto& row:result.rows){const uint8_t type=candidates[i].type;if(!have[type]||row.sessionTime>best[type].sessionTime||(row.sessionTime==best[type].sessionTime&&row.sequence>best[type].sequence)){best[type]=std::move(row);have[type]=true;}}}out.clear();for(uint8_t type=0;type<have.size();++type)if(have[type])out.push_back(std::move(best[type]));sortRows(out);return true;
}
bool TnrdV5Archive::forEachChunk(V5RowTypeMask mask,const std::function<bool(const V5ChunkInfo&,std::string_view)>& callback,std::string* errorOut){
    struct Prepared{bool ok{};std::string error;std::shared_ptr<std::string> plain;};
    std::vector<size_t> selected;for(size_t i=0;i<impl_->chunks.size();++i)if(mask&v5TypeBit((uint8_t)impl_->chunks[i].rowType))selected.push_back(i);std::sort(selected.begin(),selected.end(),[&](size_t a,size_t b){const auto&x=impl_->chunks[a];const auto&y=impl_->chunks[b];if(x.rowType!=y.rowType)return x.rowType<y.rowType;if(x.lapNumber!=y.lapNumber)return x.lapNumber<y.lapNumber;return x.sequence<y.sequence;});
    struct Completed{std::mutex mutex;std::condition_variable cv;std::deque<std::pair<size_t,Prepared>> ready;};if(selected.empty())return true;auto completed=std::make_shared<Completed>();std::vector<std::unique_ptr<Prepared>> prepared(selected.size());size_t nextSchedule=0,nextEmit=0,active=0;bool stopped=false,callbackStopped=false;
    auto schedule=[&](size_t ordinal){const size_t index=selected[ordinal];++active;(void)impl_->executor.submit(impl_->path,[state=impl_.get(),index,ordinal,completed](std::FILE* file){Prepared result;try{std::shared_ptr<std::string> plain;if(state->loadChunk(index,file,plain,&result.error)){const std::string_view view(*plain);const size_t firstEnd=view.find('\n');if(scanTime(view.substr(0,firstEnd))>=0){result.ok=true;result.plain=std::move(plain);}else{std::vector<V5TimedRow> rows;if(state->splitChunk(index,file,rows,&result.error,plain)){auto normalized=std::make_shared<std::string>();normalized->reserve(plain->size()+rows.size()*24);for(const auto& row:rows){*normalized+=withSessionTime(row.json,row.sessionTime);normalized->push_back('\n');}result.ok=true;result.plain=std::move(normalized);}}}}catch(const std::exception& e){result.error=e.what();}catch(...){result.error="indexed chunk iteration failed";}{std::lock_guard<std::mutex> lock(completed->mutex);completed->ready.emplace_back(ordinal,std::move(result));}completed->cv.notify_one();});};
    constexpr size_t LOOKAHEAD=MAX_PARALLEL_CHUNKS*2;while(nextSchedule<selected.size()&&active<MAX_PARALLEL_CHUNKS)schedule(nextSchedule++);
    while(active){std::unique_lock<std::mutex> lock(completed->mutex);completed->cv.wait(lock,[&]{return !completed->ready.empty();});while(!completed->ready.empty()){auto ready=std::move(completed->ready.front());completed->ready.pop_front();prepared[ready.first]=std::make_unique<Prepared>(std::move(ready.second));--active;}lock.unlock();while(!stopped&&nextEmit<prepared.size()&&prepared[nextEmit]){auto current=std::move(prepared[nextEmit]);if(!current->ok){fail(errorOut,current->error);stopped=true;}else if(!callback(impl_->chunks[selected[nextEmit]],*current->plain)){stopped=true;callbackStopped=true;}++nextEmit;}while(!stopped&&nextSchedule<selected.size()&&active<MAX_PARALLEL_CHUNKS&&nextSchedule<nextEmit+LOOKAHEAD)schedule(nextSchedule++);}
    if(callbackStopped)return false;if(stopped)return false;return nextEmit==selected.size();
}
void TnrdV5Archive::setCacheLimitBytes(size_t bytes){std::lock_guard<std::mutex> lock(impl_->stateMutex);impl_->cacheLimit=bytes;while(impl_->cacheBytes>bytes&&!impl_->lru.empty()){const size_t old=impl_->lru.back();impl_->lru.pop_back();auto it=impl_->cache.find(old);impl_->cacheBytes-=it->second.bytes;impl_->cache.erase(it);}}
size_t TnrdV5Archive::cacheBytes()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->cacheBytes;}
uint64_t TnrdV5Archive::decompressedChunkCount()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->decompressions;}
size_t TnrdV5Archive::peakConcurrentChunkLoads()const{std::lock_guard<std::mutex> lock(impl_->stateMutex);return impl_->peakActiveLoads;}

} // namespace tnrp::detail
