int g(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    // Dredd disable all: The loop body only counts.
    s = s + i;
    // Dredd restore all
  }
  return s;
}
