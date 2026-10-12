// Each function is straight-line code, so that every call reaches all of its
// mutants. The harness calls these functions, and those of cxx_work.cc, from
// several threads at the same time.

int CWork0(int x) {
  int a = x * 5 + 3;
  int b = a - x / 3;
  int c = (a | b) - (b & 31);
  return c * 3 + b;
}

int CWork1(int x) {
  int a = (x << 3) ^ 9;
  int b = a % 13 + (x >> 2);
  a -= b;
  b += x;
  return a * b - (a < b);
}

int CWork2(int x) {
  unsigned a = (unsigned)x * 7u + 1u;
  unsigned b = ~a | 128u;
  unsigned c = a % 5u + b / 9u;
  return (int)(c + (a > b));
}

int CWork3(int x) {
  int a = -x + 80;
  int b = a * a - 3 * x;
  int c = (b == a) + (b <= 0) - (a >= x);
  return b - c * 5;
}

int CWork4(int x) {
  int a = x + 2;
  --a;
  a *= 5;
  int b = a++ + x;
  b &= 63;
  return a | b;
}

int CWork5(int x) {
  long a = x * 7L + 4;
  long b = a / 5 + a % 5;
  long c = (a | 3) - (b & 6) + (a != b);
  return (int)(c - b);
}

int CWork6(int x) {
  int a = x * x % 89;
  int b = (a + 11) * (x + 3);
  int c = b + (a >> 1) - (b < a);
  return c / 3;
}

int CWork7(int x) {
  double a = x * 0.25 + 2.5;
  double b = a * a + 1.0;
  int c = (int)b - (a > b) * 3;
  return c + x;
}
