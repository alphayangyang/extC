#include <iostream>
struct node { long long value; node *next; };
struct inner { node *next; };
struct outer { inner mid; };
int main() {
    outer bx; bx.mid.next = nullptr;
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    node *pt = p;
    bx.mid.next = pt;
    node *c = bx.mid.next;
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    std::cout << t << "\n";
    return 0;
}
