void Copy(int& dst, int src) { dst = src; }

void ElseWithoutBraces(int& x, int& y, int c) {if (c == 4)x = 1; else Copy(x, c);y = c;}

void IfWithoutBraces(int& x, int& y, int c) {if (c)x = 1;y = c;}

void IfWithBraces(int& x, int& y, int c) {if (c){x = 1;}y = c;}

void Sequence(int& x, int& y, int c) {x = c;y = c;}
