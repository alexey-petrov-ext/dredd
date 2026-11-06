int SafeDivide(int a, int b) {
  try {
    if (b == 0) {
      throw b;
    }
    return a / b;
  } catch (int) {
    a = 0;
    return a;
  }
}

int Parse(int x) try {
  if (x < 0) {
    throw x;
  }
  return x;
} catch (int) {
  return -1;
}
