#include <iostream>
struct node { long long value; node *next; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->next = nullptr;
    return n;
}
void link3(node *out, node *q) {
    out->next = q;
}
long long burn() {
    node *a = new node;
    a->value = 0;
    a->next = nullptr;
    node *head = a;
    long long i = 1;
    while (i <= 4) {
        node *p = mk(i);
        link3(a, p);
        a = p;
        i++;
    }
    node *c = head;
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
