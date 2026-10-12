#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int CxxWork0(int x);
int CxxWork1(int x);
int CxxWork2(int x);
int CxxWork3(int x);
int CxxWork4(int x);
int CxxWork5(int x);
int CxxWork6(int x);
int CxxWork7(int x);

extern "C" {
int CWork0(int x);
int CWork1(int x);
int CWork2(int x);
int CWork3(int x);
int CWork4(int x);
int CWork5(int x);
int CWork6(int x);
int CWork7(int x);
}

namespace {

int (*const kWork[])(int) = {
    CxxWork0, CWork0, CxxWork1, CWork1, CxxWork2, CWork2, CxxWork3, CWork3,
    CxxWork4, CWork4, CxxWork5, CWork5, CxxWork6, CWork6, CxxWork7, CWork7};
const int kNumWork = sizeof(kWork) / sizeof(kWork[0]);
const int kNumRounds = 10;

}  // namespace

// Usage: harness <number of threads>
int main(int argc, char** argv) {
  const int num_threads = argc > 1 ? std::atoi(argv[1]) : 1;
  std::atomic<int> waiting(num_threads);
  std::atomic<long long> total(0);
  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; t++) {
    threads.emplace_back([t, &waiting, &total]() {
      // Start the threads together, so that they reach new mutants at the same
      // time.
      waiting--;
      while (waiting.load() > 0) {
        std::this_thread::yield();
      }
      long long sum = 0;
      for (int round = 0; round < kNumRounds; round++) {
        for (int i = 0; i < kNumWork; i++) {
          // Each thread starts with another function, and each round calls the
          // functions in another order: an odd stride visits each of the 16
          // functions once.
          sum += kWork[(t + i * (2 * round + 1)) % kNumWork](i + round);
        }
      }
      total += sum;
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  std::printf("%lld\n", total.load());
  return 0;
}
