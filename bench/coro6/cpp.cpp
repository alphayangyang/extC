#include <cstdio>
#include <cstdint>
#include <coroutine>
struct Gen {
  struct promise_type {
    int64_t val;
    Gen get_return_object(){ return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
    std::suspend_always initial_suspend(){ return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void(){}
    std::suspend_always yield_value(int64_t v){ val=v; return {}; }
    void unhandled_exception(){}
  };
  std::coroutine_handle<promise_type> h;
  ~Gen(){ if(h) h.destroy(); }
  bool next(){ h.resume(); return !h.done(); }
  int64_t value(){ return h.promise().val; }
};
static Gen gen(int64_t id, int64_t n, int64_t salt){
  for(int64_t i=0;i<n;i++) co_yield (i%65536)*65521 + id*40503 + salt;
}
static int64_t run(int64_t k, int64_t m, int64_t salt){
  int64_t s=0;
  for(int64_t id=0; id<k; id++){ auto g=gen(id,m,salt); while(g.next()) s+=g.value(); }
  return s;
}
int main(){ int64_t t=run(8,2000,0); for(int64_t r=0;r<R_ROUNDS;r++) t+=run(512,20000,r);
  printf("%lld\n",(long long)t); return 0; }
