#include <iostream>
struct node { long long value; node *next; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->next = nullptr;
    return n;
}
long long burn() {
    node *a = mk(1);
    node *b = mk(2);
    node *ca = mk(3);
    a->next = b;
    b->next = ca;
    ca->next = a;
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
    return t;
}
int main() {
    std::cout << burn() << "\n";
    return 0;
}
