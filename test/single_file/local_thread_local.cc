int Count(int n) {
  thread_local int calls = 0;
  calls = calls + n;
  return calls * 2;
}

int main() {
  return Count(1) + Count(2);
}
