#include <iostream>
struct node { long long value; node *next; };
struct inner { node *next; };
struct outer { inner mid; };
long long burn() {
    outer bx; bx.mid.next = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
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
