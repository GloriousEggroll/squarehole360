#include <rex/memory/proc_maps_parser.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
using rex::memory::detail::LinuxMapEntry;
using rex::memory::detail::ParseProcMapsLine;
namespace {
void Check(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
bool Legacy(const std::string& line,LinuxMapEntry& out){
  out={};unsigned long long begin=0,end=0;char permissions[5]{};
  if(std::sscanf(line.c_str(),"%llx-%llx %4s",&begin,&end,permissions)<3)return false;
  out.start=uintptr_t(begin);out.end=uintptr_t(end);std::memcpy(out.perms,permissions,5);
  return out.start<out.end;
}
bool Same(const LinuxMapEntry& a,const LinuxMapEntry& b){
  return a.start==b.start&&a.end==b.end&&!std::memcmp(a.perms,b.perms,5);
}
std::string Hex(uintptr_t value,unsigned width,bool upper){
  std::ostringstream text;if(upper)text<<std::uppercase;
  text<<std::hex<<std::setw(width)<<std::setfill('0')<<value;return text.str();
}
}
int main(int argc,char** argv){try{
  std::vector<std::string> corpus;size_t actual_lines=0;
  if(argc>2)throw std::runtime_error("usage: parser-test [maps-corpus]");
  if(argc==2){std::ifstream input(argv[1]);Check(input.is_open(),"could not read actual-format corpus");
    std::string line;while(std::getline(input,line)){corpus.push_back(line);++actual_lines;}
    Check(actual_lines!=0,"empty actual-format corpus");}
  const uintptr_t maximum=std::numeric_limits<uintptr_t>::max();
  for(uintptr_t base:{uintptr_t(0),uintptr_t(1),uintptr_t(0x1000),uintptr_t(0x7FFF0000),maximum-0x2000})
    for(unsigned permissions=0;permissions<16;++permissions)
      for(unsigned width:{0u,8u,unsigned(sizeof(uintptr_t)*2)})
        for(bool upper:{false,true}){
          std::string flags="----";flags[0]=permissions&1?'r':'-';flags[1]=permissions&2?'w':'-';
          flags[2]=permissions&4?'x':'-';flags[3]=permissions&8?'s':'p';
          corpus.push_back(Hex(base,width,upper)+"-"+Hex(base+0x1000,width,upper)+" "+flags+" 00001000 00:00 0 /file with spaces (deleted)");
          corpus.push_back("\t "+Hex(base,width,upper)+"-"+Hex(base+0x1000,width,upper)+"\t"+flags);
        }
  size_t parity=0;
  for(const auto& line:corpus){LinuxMapEntry old{},fresh{};
    Check(Legacy(line,old)&&ParseProcMapsLine(line,fresh)&&Same(old,fresh),"valid proc-format differential mismatch");++parity;}
  std::vector<std::string> invalid={""," ","1","1-","-1-2 rw-p","+1-2 rw-p","0x1-2 rw-p","1-0x2 rw-p",
    "1-1 rw-p","2-1 rw-p","1 2 rw-p","1--2 rw-p","1-2rw-p","1-2 rw-","1-2 rw-pp","1-2 rw-pX",
    "1-2 RW-p","1-2 rr-p","1-2 rw-q","1-2 -z-p","1-2 --z-","1-2 ----","1-2",
    Hex(maximum,0,false)+"0-"+Hex(maximum,0,false)+"1 rw-p","1-"+Hex(maximum,0,false)+"0 rw-p"};
  for(const auto& line:invalid){LinuxMapEntry value{17,29,{'r','w','x','p',0}};
    Check(!ParseProcMapsLine(line,value),"malformed/overflow proc prefix accepted");Check(Same(value,{}),"failed parser left output authority");}
  std::string backing="00001000-00002000 rw-pTRAILER";
  LinuxMapEntry bounded{};
  Check(ParseProcMapsLine(std::string_view(backing).substr(0,backing.find("TRAILER")),bounded)&&bounded.start==0x1000&&bounded.end==0x2000,
        "string_view parser read beyond explicit bound");
  std::string nul="1-2 rw-p";nul.push_back('\0');nul+='x';
  Check(!ParseProcMapsLine(nul,bounded),"embedded non-whitespace permission suffix accepted");
  constexpr unsigned repeats=128;
  const auto time=[&](auto parse){uint64_t checksum=0;
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<repeats;++i)for(const auto& line:corpus){LinuxMapEntry value{};
      Check(parse(line,value),"benchmark input rejected");checksum+=uint64_t(value.start)^uint64_t(value.end)^uint64_t(value.perms[0]);}
    return std::pair{std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),checksum};};
  const auto before=time(Legacy),after=time([](const std::string& line,LinuxMapEntry& out){return ParseProcMapsLine(line,out);});
  Check(before.second==after.second,"parser benchmark outputs differ");
  std::cout<<"{\"valid_parity_cases\":"<<parity<<",\"actual_lines\":"<<actual_lines<<",\"malformed_cases\":"<<invalid.size()
    <<",\"benchmark_calls_each\":"<<corpus.size()*repeats<<",\"legacy_ms\":"<<before.first<<",\"checked_ms\":"<<after.first
    <<",\"checksum\":"<<after.second<<"}\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
