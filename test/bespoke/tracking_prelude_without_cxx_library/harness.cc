#include <stdio.h>

int Reached(int x, int y);

int main() {
  // The second call reaches the same mutants again.
  printf("%d\n", Reached(4, 5));
  printf("%d\n", Reached(4, 5));
  return 0;
}
