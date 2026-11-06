int Read(const int a, const int b) {
  int sum = a * b + 2;
  return sum;
}

int ReadInCondition(const int a) {
  if (int next = a + 1) {
    return 1;
  }
  return 0;
}

int ReadInDecltype(const int a) {
  int next = a + 1;
  decltype(next) other = 2;
  return other;
}

class Guard {
 public:
  explicit Guard(int level);
  ~Guard();
};

void ClassType(const int a) {
  [[maybe_unused]] Guard guard(a + 1);
}

void Volatile(const int a) {
  [[maybe_unused]] volatile int next = a + 1;
}

void Reference(const int a) {
  [[maybe_unused]] const int& next = a + 1;
}
