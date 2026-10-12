#include <iostream>
struct node { long long value; node *next; };
struct inner { node *next; };
struct outer { inner mid; };
node *build() {
    outer bx; bx.mid.next = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    bx.mid.next = p;
    return bx.mid.next;
}
int main() {
    node *h = build();
    node *c = h;
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    std::cout << t << "\n";
    return 0;
}
