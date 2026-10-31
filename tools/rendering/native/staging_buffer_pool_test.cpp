#include "renderer/native/staging_buffer_pool.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
struct FakeBuffer {uint64_t size;};
using Pool=StagingBufferPool<FakeBuffer>;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() try {
    Check(Pool::SizeClass(0)==0&&Pool::SizeClass(1)==64*1024&&Pool::SizeClass(64*1024)==64*1024,"minimum class");
    Check(Pool::SizeClass(64*1024+1)==128*1024&&Pool::SizeClass(Pool::kMaxClass)==Pool::kMaxClass,"power-of-two class");
    Check(Pool::SizeClass(Pool::kMaxClass+1)==0,"oversized request pooled");
    Pool pool(1024*1024);
    Check(!pool.Take(1000),"empty pool returned a buffer");
    pool.Return(std::make_unique<FakeBuffer>(FakeBuffer{64*1024}),64*1024);
    pool.Return(std::make_unique<FakeBuffer>(FakeBuffer{512*1024}),512*1024);
    Check(!pool.Take(200*1024),"wrong class reused");
    auto reused=pool.Take(300*1024);
    Check(reused&&reused->size==512*1024,"matching class not reused");
    Check(pool.statistics().idle_bytes==64*1024,"idle accounting");
    pool.Return(std::make_unique<FakeBuffer>(FakeBuffer{100}),100);
    Check(pool.statistics().drops==1&&pool.statistics().idle_bytes==64*1024,"exact-size buffer kept");
    pool.Return(std::move(reused),512*1024);
    pool.Return(std::make_unique<FakeBuffer>(FakeBuffer{512*1024}),512*1024);
    Check(pool.statistics().drops==2&&pool.statistics().idle_bytes==576*1024,"idle bound exceeded");
    std::cout<<"PASS: staging pool classes, reuse, exact-size refusal and idle bound\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
