#include <iostream>
struct node { long long value; node *next; };
int main() {
    node *head = nullptr;
    node *cur = nullptr;
    long long i = 0;
    while (i < 4) {
        node *p = new node;
        p->value = i + 1;
        p->next = nullptr;
        if (head == nullptr) {
            head = p;
        } else {
    node *ocur = cur;
    if (ocur != nullptr) {
            ocur->next = p;
        }
        }
        cur = p;
        i++;
    }
    node *c = head;
    long long t = 0;
    while (c != nullptr) {
        t += c->value;
        c = c->next;
    }
    std::cout << t << "\n";
    return 0;
}
