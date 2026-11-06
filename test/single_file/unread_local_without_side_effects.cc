void Arithmetic(const int a, const int b) {
  [[maybe_unused]] int sum = a * b + 2;
}

void Logical(const int a, const int b) {
  [[maybe_unused]] bool in_order = a < b && b != 0;
}

void Conversion(const int a) {
  [[maybe_unused]] double half = -static_cast<double>(a) / 2.0;
}

void Conditional(const int a, const int b) {
  [[maybe_unused]] int smaller = a < b ? a : b;
}

void Braces(const int a) {
  [[maybe_unused]] int next{a + 1};
}

void Constant() {
  [[maybe_unused]] unsigned mask = 0xffu;
}
