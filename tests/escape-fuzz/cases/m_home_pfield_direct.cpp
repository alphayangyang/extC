#include <iostream>
struct node { long long value; node *next; };
void link3(node *out, node *q) {
    out->next = q;
}
node *build() {
    node *a = new node;
    a->value = 0;
    a->next = nullptr;
    node *head = a;
    long long i = 1;
    while (i <= 4) {
        node *p = new node;
        p->value = i;
        p->next = nullptr;
        link3(a, p);
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
