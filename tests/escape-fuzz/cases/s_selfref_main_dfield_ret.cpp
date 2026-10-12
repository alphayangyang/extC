#include <iostream>
struct node { long long value; node *self; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->self = nullptr;
    return n;
}
int main() {
    node *a0 = new node;
    node *a = a0;
    node *b0 = new node;
    node *b = b0;
    a0->value = 1;
    b0->value = 2;
    a0->self = nullptr;
    b0->self = nullptr;
    if (a != nullptr) { a->self = b; }
    if (b != nullptr) { b->self = a; }
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
    std::cout << t << "\n";
    return 0;
}
