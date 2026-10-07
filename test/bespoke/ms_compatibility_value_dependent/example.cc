template <typename T> int Size() { return static_cast<int>(sizeof(T)) + 1; }

template <typename T> void Construct() { T(sizeof(T)); }

int main() { return Size<int>() - 5; }
