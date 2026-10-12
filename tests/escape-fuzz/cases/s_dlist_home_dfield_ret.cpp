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
    node *head = nullptr;
    node *cur = nullptr;
    long long i = 0;
    while (i < 4) {
        node *p = mk(i + 1);
        if (head == nullptr) {
            head = p;
            cur = p;
        } else {
    if (cur != nullptr) {
            cur->next = p;
            p->prev = cur;
        }
            cur = p;
        }
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
