#include <iostream>
struct node { long long value; node *next; node *prev; };
long long burn() {
    node *head = nullptr;
    node *cur = nullptr;
    long long i = 0;
    while (i < 4) {
        node *p = new node;
        p->value = i + 1;
        p->next = nullptr;
        p->prev = nullptr;
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
