template <typename T>
auto Twice(T x) {
  x = x + x;
  return x;
}

template <typename T>
auto Increment(T& x) {
  x = x + 1;
}

int Apply(int x) {
  auto triple = [](auto y) { return y * 3; };
  auto bump = [](auto& y) { y = y + 1; };
  bump(x);
  Increment(x);
  return triple(Twice(x));
}
