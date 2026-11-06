long bar() {
  return 1LL;
}

void foo() {
  unsigned x = (decltype(bar())) 'a';
  (void)x;
}
