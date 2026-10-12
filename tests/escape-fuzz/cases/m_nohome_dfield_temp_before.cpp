#include <iostream>
struct node { long long value; node *next; };
long long burn() {
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
            cur = pt;
        } else {
    if (cur != nullptr) {
            cur->next = pt;
        }
            cur = pt;
        }
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
