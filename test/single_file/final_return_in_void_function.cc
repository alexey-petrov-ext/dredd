static int counter = 0;

void Increment(int x) {
  counter = counter + x;
  return;
}

int main() {
  auto reset = []() {
    counter = 0;
    return;
  };
  reset();
  Increment(1);
  return counter;
}
