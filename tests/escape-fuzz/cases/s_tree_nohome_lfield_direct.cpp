#include <iostream>
struct node { long long value; node *left; node *right; node *parent; };
long long burn() {
    node *root = new node;
    root->value = 1;
    root->left = nullptr;
    root->right = nullptr;
    root->parent = nullptr;
    node *l = new node;
    l->value = 2;
    l->left = nullptr;
    l->right = nullptr;
    l->parent = nullptr;
    node *r = new node;
    r->value = 3;
    r->left = nullptr;
    r->right = nullptr;
    r->parent = nullptr;
    root->left = l;
    root->right = r;
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
