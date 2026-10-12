#include <iostream>
struct node { long long value; node *next; };
node *build() {
    node *head = nullptr;
    node *cur = nullptr;
    long long i = 0;
    while (i < 4) {
        node *p = new node;
        p->value = i + 1;
        p->next = nullptr;
        node *pt = p;
        if (head == nullptr) {
            head = pt;
        } else {
    if (cur != nullptr) {
            cur->next = pt;
        }
        }
        cur = pt;
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
