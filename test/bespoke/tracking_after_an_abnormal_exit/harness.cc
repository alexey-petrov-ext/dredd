#include <cstdio>
#include <cstdlib>

int CxxWork(int x);

extern "C" {
int CWork(int x);
}

int main() {
  std::printf("%d\n", CxxWork(4) + CWork(5));
  // End as a crash does: _Exit flushes no stream of the C library and calls no
  // exit handler.
  std::_Exit(3);
}
