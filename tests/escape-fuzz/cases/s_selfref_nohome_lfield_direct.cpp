#include <iostream>
struct node { long long value; node *self; };
long long burn() {
    node *a = new node;
    node *b = new node;
    a->value = 1;
    b->value = 2;
    a->self = nullptr;
    b->self = nullptr;
    a->self = b;
    b->self = a;
    node *c = a;
    long long t = 0;
    long long s = 0;
    while (s < 4) {
        if (c != nullptr) {
            t += c->value;
            c = c->self;
        }
        s++;
    }
    return t;
}
int main() {
    std::cout << burn() << "\n";
    return 0;
}
