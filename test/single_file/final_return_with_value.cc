int Twice(int x) {
  x = x * 2;
  return x;
}

int AddOne(int x) {
  auto add_one = [](int y) { return y + 1; };
  return add_one(x);
  ;
}
