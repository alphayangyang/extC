#include <iostream>
struct node { long long value; node *next; };
#include <vector>
int main() {
    std::vector<node *> v;
    long long i = 0;
    while (i < 4) {
        node *p = new node;
        p->value = i + 1;
        p->next = nullptr;
        v.push_back(p);
        i++;
    }
    node *head = nullptr;
    node *cur = nullptr;
    long long k = 0;
    while (k < (long long)v.size()) {
        node *q = v[k];
        if (head == nullptr) {
            head = q;
            cur = q;
        } else {
            if (cur != nullptr) { cur->next = q; }
            cur = q;
        }
        k++;
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
