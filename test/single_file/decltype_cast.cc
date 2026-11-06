void foo() {
  unsigned x = (decltype(1)) 'a';
  (void)x;
}
