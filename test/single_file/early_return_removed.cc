int Clamp(int x) {
  if (x > 10) {
    return 10;
  }
  return x;
}

int FindFirstNegative(const int* values, int count) {
  for (int i = 0; i < count; i++) {
    if (values[i] < 0) {
      return i;
    }
  }
  return -1;
}
