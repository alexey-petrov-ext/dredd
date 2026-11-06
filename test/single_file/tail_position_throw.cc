[[noreturn]] void Fail();

int Checked(int x) {
  if (x >= 0) {
    return x;
  }
  throw x;
}

int CheckedOrFail(int x) {
  if (x >= 0) {
    return x;
  }
  Fail();
}

int FirstPositive(const int* values) {
  for (int i = 0;; i++) {
    if (values[i] > 0) {
      return values[i];
    }
  }
}
