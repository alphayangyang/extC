#include <iostream>
struct node { long long value; node *next; node *prev; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->next = nullptr;
    n->prev = nullptr;
    return n;
}
node *build() {
    node *a = new node;
    a->value = 0;
    a->next = nullptr;
    a->prev = nullptr;
    node *head = a;
    long long i = 1;
    while (i <= 4) {
        node *p = mk(i);
        a->next = p;
        a = p;
        i++;
    }
    return head;
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
