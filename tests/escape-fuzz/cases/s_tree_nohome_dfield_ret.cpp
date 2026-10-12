#include <iostream>
struct node { long long value; node *left; node *right; node *parent; };
node *mk(long long v) {
    node *n = new node;
    n->value = v;
    n->left = nullptr;
    n->right = nullptr;
    n->parent = nullptr;
    return n;
}
long long burn() {
    node *root0 = new node;
    node *root = root0;
    root0->value = 1;
    root0->left = nullptr;
    root0->right = nullptr;
    root0->parent = nullptr;
    node *l = mk(2);
    node *r = mk(3);
    if (root != nullptr) {
        root->left = l;
        root->right = r;
    }
    l->parent = root;
    r->parent = root;
    node *c = root;
    long long t = 0;
    if (c != nullptr) {
        t += c->value;
        if (c->left != nullptr) { t += c->left->value; }
        if (c->right != nullptr) { t += c->right->value; }
    }
    return t;
}
int main() {
    std::cout << burn() << "\n";
    return 0;
}
