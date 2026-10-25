// The function is straight-line code, so that a call reaches all of its
// mutants.

int CWork(int x) {
  int a = x * 5 + 3;
  int b = a - x / 3;
  return (a | b) - (b & 31);
}
