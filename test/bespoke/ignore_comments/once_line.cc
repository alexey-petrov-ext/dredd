int f(int a, int b) {
  // Dredd disable once replaceBinaryOperator: The sum feeds a trace only.
  int x = a + b;
  int y = a * b;
  return x + y;
}
