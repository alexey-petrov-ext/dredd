bool Write(int* file);

int Count();

class List {
 public:
  int Extent() const;
};

void MemberCall(const List& list) {
  [[maybe_unused]] int extent = list.Extent();
}

void BoolCall(int* file) {
  [[maybe_unused]] bool ok = Write(file);
}

void CallInOperand(const int a) {
  [[maybe_unused]] int scaled = Count() * a;
}

void CallsInBothOperands() {
  [[maybe_unused]] int total = Count() + Count();
}

void ShortCircuit(int* file) {
  [[maybe_unused]] bool both = Write(file) && Write(file);
}

void Increment(int& counter) {
  [[maybe_unused]] int before = counter++;
}

void ReadOfVariable(int& value) {
  [[maybe_unused]] int copy = value;
}
