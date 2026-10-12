#include <iostream>
struct node { long long value; node *next; };
node *build() {
    node *q1 = new node;
    q1->value = 1;
    q1->next = nullptr;
    node *q2 = new node;
    q2->value = 2;
    q2->next = nullptr;
    node *q3 = new node;
    q3->value = 3;
    q3->next = nullptr;
    node *a = q1;
    node *b = q2;
    node *cc0 = q3;
    if (a != nullptr) { a->next = b; }
    if (b != nullptr) { b->next = cc0; }
    if (cc0 != nullptr) { cc0->next = a; }
    return a;
}
int main() {
    node *h = build();
    node *c = h;
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
