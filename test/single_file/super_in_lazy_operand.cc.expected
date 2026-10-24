class Base {
 public:
  bool Ok();
};

// __super exists only with Microsoft extensions.
#ifdef _MSC_VER
class Derived : public Base {
 public:
  bool Initializer() {
    bool valid = __super::Ok();
    return valid;
  }

  bool Operand(bool flag) { return flag && __super::Ok(); }

  bool Return() { return __super::Ok(); }
};

struct Name {
  int n;
};

bool Use(Name name);

class NameBase {
 public:
  Name Get();
};

// The class name after a colon, in an operand that is not mutated itself.
class AfterColon : public NameBase {
 public:
  bool Operand(bool flag, Name name) {
    return flag && Use(flag ? name :__super::Get());
  }
};

namespace {
class Base {
 public:
  bool Ok();
};

// No name from the global scope names the base class, so the operand stays
// out of a lambda.
class InUnnamedNamespace : public Base {
 public:
  bool Operand(bool flag) { return flag && __super::Ok(); }
};
}  // namespace
#endif
