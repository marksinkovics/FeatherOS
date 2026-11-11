// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/map"
#else
#include <map>
#endif

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE::__detail
{

namespace
{

using Base = TreeNodeBase;

void RotateLeft(Base* node, Base*& root) noexcept
{
    Base* const child = node->right;
    node->right = child->left;
    if (child->left != nullptr)
    {
        child->left->parent = node;
    }
    child->parent = node->parent;
    if (node == root)
    {
        root = child;
    }
    else if (node == node->parent->left)
    {
        node->parent->left = child;
    }
    else
    {
        node->parent->right = child;
    }
    child->left = node;
    node->parent = child;
}

void RotateRight(Base* node, Base*& root) noexcept
{
    Base* const child = node->left;
    node->left = child->right;
    if (child->right != nullptr)
    {
        child->right->parent = node;
    }
    child->parent = node->parent;
    if (node == root)
    {
        root = child;
    }
    else if (node == node->parent->right)
    {
        node->parent->right = child;
    }
    else
    {
        node->parent->left = child;
    }
    child->right = node;
    node->parent = child;
}

Base* Minimum(Base* node) noexcept
{
    while (node->left != nullptr)
    {
        node = node->left;
    }
    return node;
}

Base* Maximum(Base* node) noexcept
{
    while (node->right != nullptr)
    {
        node = node->right;
    }
    return node;
}

bool IsBlack(const Base* node) noexcept
{
    return node == nullptr || node->black;
}

} // namespace

Base* TreeIncrement(const Base* node) noexcept
{
    auto* current = const_cast<Base*>(node);
    if (current->right != nullptr)
    {
        return Minimum(current->right);
    }
    Base* parent = current->parent;
    while (current == parent->right)
    {
        current = parent;
        parent = parent->parent;
    }
    // Past the last node of a tree whose root has no right child, current is the header and
    // parent the root, whose right is not the header.
    return current->right != parent ? parent : current;
}

Base* TreeDecrement(const Base* node) noexcept
{
    auto* current = const_cast<Base*>(node);
    // Only the header is red and its parent's parent; the root, whose parent's parent is also
    // itself, is black.
    if (!current->black && current->parent->parent == current)
    {
        return current->right;
    }
    if (current->left != nullptr)
    {
        return Maximum(current->left);
    }
    Base* parent = current->parent;
    while (current == parent->left)
    {
        current = parent;
        parent = parent->parent;
    }
    return parent;
}

void TreeInsertAndRebalance(bool insert_left, Base* node, Base* parent, Base& header) noexcept
{
    Base*& root = header.parent;
    node->parent = parent;
    node->left = nullptr;
    node->right = nullptr;
    node->black = false;
    if (insert_left)
    {
        parent->left = node;
        if (parent == &header)
        {
            header.parent = node;
            header.right = node;
        }
        else if (parent == header.left)
        {
            header.left = node;
        }
    }
    else
    {
        parent->right = node;
        if (parent == header.right)
        {
            header.right = node;
        }
    }

    while (node != root && !node->parent->black)
    {
        Base* const grandparent = node->parent->parent;
        if (node->parent == grandparent->left)
        {
            Base* const uncle = grandparent->right;
            if (!IsBlack(uncle))
            {
                node->parent->black = true;
                uncle->black = true;
                grandparent->black = false;
                node = grandparent;
                continue;
            }
            if (node == node->parent->right)
            {
                node = node->parent;
                RotateLeft(node, root);
            }
            node->parent->black = true;
            grandparent->black = false;
            RotateRight(grandparent, root);
        }
        else
        {
            Base* const uncle = grandparent->left;
            if (!IsBlack(uncle))
            {
                node->parent->black = true;
                uncle->black = true;
                grandparent->black = false;
                node = grandparent;
                continue;
            }
            if (node == node->parent->left)
            {
                node = node->parent;
                RotateRight(node, root);
            }
            node->parent->black = true;
            grandparent->black = false;
            RotateLeft(grandparent, root);
        }
    }
    root->black = true;
}

void TreeRebalanceForErase(Base* removed, Base& header) noexcept
{
    Base*& root = header.parent;
    Base*& leftmost = header.left;
    Base*& rightmost = header.right;

    // spliced is the node that leaves its place: removed itself, or its successor, which then
    // takes removed's place and colour. child takes spliced's place.
    Base* spliced = removed;
    Base* child = nullptr;
    Base* child_parent = nullptr;
    if (spliced->left == nullptr)
    {
        child = spliced->right;
    }
    else if (spliced->right == nullptr)
    {
        child = spliced->left;
    }
    else
    {
        spliced = Minimum(spliced->right);
        child = spliced->right;
    }

    bool removed_black = false;
    if (spliced != removed)
    {
        removed->left->parent = spliced;
        spliced->left = removed->left;
        if (spliced != removed->right)
        {
            child_parent = spliced->parent;
            if (child != nullptr)
            {
                child->parent = spliced->parent;
            }
            spliced->parent->left = child;
            spliced->right = removed->right;
            removed->right->parent = spliced;
        }
        else
        {
            child_parent = spliced;
        }
        if (root == removed)
        {
            root = spliced;
        }
        else if (removed->parent->left == removed)
        {
            removed->parent->left = spliced;
        }
        else
        {
            removed->parent->right = spliced;
        }
        spliced->parent = removed->parent;
        removed_black = spliced->black;
        spliced->black = removed->black;
    }
    else
    {
        child_parent = spliced->parent;
        if (child != nullptr)
        {
            child->parent = spliced->parent;
        }
        if (root == removed)
        {
            root = child;
        }
        else if (removed->parent->left == removed)
        {
            removed->parent->left = child;
        }
        else
        {
            removed->parent->right = child;
        }
        if (leftmost == removed)
        {
            leftmost = removed->right == nullptr ? removed->parent : Minimum(child);
        }
        if (rightmost == removed)
        {
            rightmost = removed->left == nullptr ? removed->parent : Maximum(child);
        }
        removed_black = removed->black;
    }

    if (!removed_black)
    {
        return;
    }
    while (child != root && IsBlack(child))
    {
        if (child == child_parent->left)
        {
            Base* sibling = child_parent->right;
            if (!sibling->black)
            {
                sibling->black = true;
                child_parent->black = false;
                RotateLeft(child_parent, root);
                sibling = child_parent->right;
            }
            if (IsBlack(sibling->left) && IsBlack(sibling->right))
            {
                sibling->black = false;
                child = child_parent;
                child_parent = child_parent->parent;
                continue;
            }
            if (IsBlack(sibling->right))
            {
                sibling->left->black = true;
                sibling->black = false;
                RotateRight(sibling, root);
                sibling = child_parent->right;
            }
            sibling->black = child_parent->black;
            child_parent->black = true;
            if (sibling->right != nullptr)
            {
                sibling->right->black = true;
            }
            RotateLeft(child_parent, root);
            break;
        }
        Base* sibling = child_parent->left;
        if (!sibling->black)
        {
            sibling->black = true;
            child_parent->black = false;
            RotateRight(child_parent, root);
            sibling = child_parent->left;
        }
        if (IsBlack(sibling->left) && IsBlack(sibling->right))
        {
            sibling->black = false;
            child = child_parent;
            child_parent = child_parent->parent;
            continue;
        }
        if (IsBlack(sibling->left))
        {
            sibling->right->black = true;
            sibling->black = false;
            RotateLeft(sibling, root);
            sibling = child_parent->left;
        }
        sibling->black = child_parent->black;
        child_parent->black = true;
        if (sibling->left != nullptr)
        {
            sibling->left->black = true;
        }
        RotateRight(child_parent, root);
        break;
    }
    if (child != nullptr)
    {
        child->black = true;
    }
}

} // namespace FEATHEROS_STD_NAMESPACE::__detail
// NOLINTEND(bugprone-std-namespace-modification)
