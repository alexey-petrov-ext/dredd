// The function is straight-line code, so that a call reaches all of its
// mutants.

int CxxWork(int x) {
  int a = x * 3 + 7;
  int b = a - x / 2;
  return (a ^ b) + (a & 15);
}
