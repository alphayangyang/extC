#include <iostream>
struct node { long long value; node *next; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->next = nullptr;
    return n;
}
int main() {
    node *q1 = mk(1);
    node *q2 = mk(2);
    node *q3 = mk(3);
    node *a = q1;
    node *b = q2;
    node *cc0 = q3;
    if (a != nullptr) { a->next = b; }
    if (b != nullptr) { b->next = cc0; }
    if (cc0 != nullptr) { cc0->next = a; }
    node *c = a;
    long long t = 0;
    long long s = 0;
    while (s < 6) {
        if (c != nullptr) {
            t += c->value;
            c = c->next;
        }
        s++;
    }
    std::cout << t << "\n";
    return 0;
}
