#include <cstdio>
extern "C" int run_policy_fixture(unsigned);
int main() {
  long sum=0;
  for(unsigned x=0;x<32;++x)sum+=run_policy_fixture(x);
  std::printf("fixture sum=%ld\n",sum);
  return sum>0?0:1;
}
