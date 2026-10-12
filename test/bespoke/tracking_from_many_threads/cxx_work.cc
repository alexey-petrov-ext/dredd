// Each function is straight-line code, so that every call reaches all of its
// mutants. The harness calls these functions, and those of c_work.c, from
// several threads at the same time.

int CxxWork0(int x) {
  int a = x * 3 + 7;
  int b = a - x / 2;
  int c = (a ^ b) + (a & 15);
  return c * 2 - b;
}

int CxxWork1(int x) {
  int a = (x << 2) | 5;
  int b = a % 11 + (x >> 1);
  a += b;
  b -= x;
  return a * b + (a > b);
}

int CxxWork2(int x) {
  unsigned a = static_cast<unsigned>(x) * 5u + 3u;
  unsigned b = ~a & 255u;
  unsigned c = a % 7u + b / 3u;
  return static_cast<int>(c - (a < b));
}

int CxxWork3(int x) {
  int a = -x + 100;
  int b = a * a - 4 * x;
  int c = (b != a) + (b >= 0) - (a <= x);
  return b + c * 3;
}

int CxxWork4(int x) {
  int a = x + 1;
  ++a;
  a *= 3;
  int b = a-- - x;
  b |= 8;
  return a ^ b;
}

int CxxWork5(int x) {
  long a = x * 9L + 2;
  long b = a / 3 - a % 3;
  long c = (a & 7) + (b | 1) - (a == b);
  return static_cast<int>(c + b);
}

int CxxWork6(int x) {
  int a = x * x % 97;
  int b = (a + 13) * (x + 2);
  int c = b - (a << 1) + (b > a);
  return c / 4;
}

int CxxWork7(int x) {
  double a = x * 0.5 + 1.25;
  double b = a * a - 3.0;
  int c = static_cast<int>(b) + (a < b) * 2;
  return c - x;
}
