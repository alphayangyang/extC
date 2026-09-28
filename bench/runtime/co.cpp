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
  bool next(){ h.resume(); return !h.done(); }
  int64_t value(){ return h.promise().val; }
};
static Gen gen(int64_t n){ for(int64_t i=0;i<n;i++) co_yield i; }
int main(){ auto g=gen(1000000); int64_t s=0; while(g.next()) s+=g.value(); printf("%lld\n",(long long)s); return 0; }
