#include <iostream>
struct node { long long value; node *next; };
struct inner { node *next; };
struct outer { inner mid; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->next = nullptr;
    return n;
}
long long burn() {
    outer bx; bx.mid.next = nullptr;
    node *p = mk(7);
    bx.mid.next = p;
    node *c = bx.mid.next;
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    return t;
}
int main() {
    std::cout << burn() << "\n";
    return 0;
}
