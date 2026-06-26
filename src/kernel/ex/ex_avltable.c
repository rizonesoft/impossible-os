/* ============================================================================
 * ex_avltable.c -- RTL_AVL_TABLE: balanced (AVL) generic ordered table (S7).
 *
 * Stores caller elements keyed by a caller compare routine, kept height-balanced
 * so lookup/insert/delete are O(log n). Per-element nodes (RTL_BALANCED_LINKS
 * header + a COPY of the element) come from the caller's allocate/free pair, so
 * the table never hides an allocation. CALLER-SERIALIZED (not thread-safe),
 * matching Windows Rtl semantics.
 *
 * Height-cached AVL: each node caches its subtree height; rotations recompute
 * height from the children in O(1), avoiding the running balance-factor
 * bookkeeping that is the classic source of AVL bugs. Deletion uses node
 * RELINKING (the in-order successor takes the deleted node's slot by pointer
 * surgery) rather than copying the element body down, so it is correct for
 * variable-size elements and never overflows a smaller node.
 *
 * Arch-neutral: no inline asm, no arch headers; recursion depth is bounded by
 * the tree height (<= ~1.44*log2(n+2), ~21 for a million elements).
 * ============================================================================ */

#include "kernel/ex.h"
#include "libc/string.h"   /* memset / memcpy */

typedef RTL_BALANCED_LINKS node_t;

/* The element body sits immediately after the node header (8-byte aligned). */
static inline void *avl_body(node_t *n)
{
    return (void *)((uint8_t *)n + sizeof(RTL_BALANCED_LINKS));
}

static inline int32_t avl_height(node_t *n)
{
    return n ? n->Height : 0;
}

static inline int32_t avl_max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static inline void avl_update_height(node_t *n)
{
    n->Height = 1 + avl_max(avl_height(n->LeftChild), avl_height(n->RightChild));
}

static inline int32_t avl_balance(node_t *n)
{
    return n ? (avl_height(n->RightChild) - avl_height(n->LeftChild)) : 0;
}

/* Rotate left around x; returns the new subtree root y. Fixes the parents of the
 * moved nodes EXCEPT the returned root (the caller relinks it into its parent). */
static node_t *avl_rotate_left(node_t *x)
{
    node_t *y = x->RightChild;
    node_t *t2 = y->LeftChild;

    y->LeftChild = x;
    x->Parent = y;
    x->RightChild = t2;
    if (t2)
        t2->Parent = x;
    avl_update_height(x);
    avl_update_height(y);
    return y;
}

static node_t *avl_rotate_right(node_t *x)
{
    node_t *y = x->LeftChild;
    node_t *t2 = y->RightChild;

    y->RightChild = x;
    x->Parent = y;
    x->LeftChild = t2;
    if (t2)
        t2->Parent = x;
    avl_update_height(x);
    avl_update_height(y);
    return y;
}

/* Recompute height and rebalance the subtree rooted at n; returns the (possibly
 * new) subtree root whose Parent the caller sets. */
static node_t *avl_rebalance(node_t *n)
{
    int32_t bf;

    avl_update_height(n);
    bf = avl_balance(n);

    if (bf < -1) {                              /* left heavy */
        if (avl_balance(n->LeftChild) > 0) {    /* left-right */
            n->LeftChild = avl_rotate_left(n->LeftChild);
            n->LeftChild->Parent = n;
        }
        return avl_rotate_right(n);
    }
    if (bf > 1) {                               /* right heavy */
        if (avl_balance(n->RightChild) < 0) {   /* right-left */
            n->RightChild = avl_rotate_right(n->RightChild);
            n->RightChild->Parent = n;
        }
        return avl_rotate_left(n);
    }
    return n;
}

void RtlInitializeGenericTableAvl(RTL_AVL_TABLE *table,
                                  RTL_AVL_COMPARE_ROUTINE compare,
                                  RTL_AVL_ALLOCATE_ROUTINE allocate,
                                  RTL_AVL_FREE_ROUTINE free, void *context)
{
    if (!table)
        return;
    table->Root = (node_t *)0;
    table->NumberOfElements = 0;
    table->_pad = 0;
    table->CompareRoutine = compare;
    table->AllocateRoutine = allocate;
    table->FreeRoutine = free;
    table->TableContext = context;
    table->EnumNext = (node_t *)0;
}

static node_t *avl_find(RTL_AVL_TABLE *table, void *key)
{
    node_t *n = table->Root;
    while (n) {
        RTL_GENERIC_COMPARE_RESULTS c = table->CompareRoutine(table, key, avl_body(n));
        if (c == RtlGenericLessThan)
            n = n->LeftChild;
        else if (c == RtlGenericGreaterThan)
            n = n->RightChild;
        else
            return n;
    }
    return (node_t *)0;
}

void *RtlLookupElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer)
{
    node_t *n;
    if (!table || !buffer)
        return (void *)0;
    n = avl_find(table, buffer);
    return n ? avl_body(n) : (void *)0;
}

/* Recursive BST insert of an already-built node `nn`, rebalancing on the way up.
 * Returns the new subtree root (Parent set by the caller). The key cannot equal
 * an existing element here -- the public entry point pre-checks for duplicates. */
static node_t *avl_insert_rec(RTL_AVL_TABLE *table, node_t *root, node_t *nn)
{
    RTL_GENERIC_COMPARE_RESULTS c;
    if (!root)
        return nn;

    c = table->CompareRoutine(table, avl_body(nn), avl_body(root));
    if (c == RtlGenericLessThan) {
        root->LeftChild = avl_insert_rec(table, root->LeftChild, nn);
        root->LeftChild->Parent = root;
    } else {
        root->RightChild = avl_insert_rec(table, root->RightChild, nn);
        root->RightChild->Parent = root;
    }
    return avl_rebalance(root);
}

void *RtlInsertElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer,
                                      uint32_t size, bool *new_element)
{
    node_t *existing, *nn;

    if (new_element)
        *new_element = false;
    if (!table || !buffer || size == 0 || !table->CompareRoutine ||
        !table->AllocateRoutine || !table->FreeRoutine)
        return (void *)0;
    /* header + size must not wrap the uint32 allocate-size argument, or the
     * allocator would get a small wrapped size while the memcpy below copies the
     * full (large) element -- a heap overflow. Reject before allocating. */
    if (size > 0xFFFFFFFFu - (uint32_t)sizeof(RTL_BALANCED_LINKS))
        return (void *)0;

    existing = avl_find(table, buffer);
    if (existing)
        return avl_body(existing);   /* equal element already present */

    nn = (node_t *)table->AllocateRoutine(table,
                                          (uint32_t)sizeof(RTL_BALANCED_LINKS) + size);
    if (!nn)
        return (void *)0;            /* allocate failure */

    nn->Parent = (node_t *)0;
    nn->LeftChild = (node_t *)0;
    nn->RightChild = (node_t *)0;
    nn->Height = 1;
    nn->_pad = 0;
    memcpy(avl_body(nn), buffer, size);

    table->Root = avl_insert_rec(table, table->Root, nn);
    table->Root->Parent = (node_t *)0;
    table->NumberOfElements++;

    if (new_element)
        *new_element = true;
    return avl_body(nn);
}

static node_t *avl_min(node_t *n)
{
    while (n && n->LeftChild)
        n = n->LeftChild;
    return n;
}

bool RtlDeleteElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer)
{
    node_t *z, *y, *x, *p, *rebalance_from;

    if (!table || !buffer)
        return false;
    z = avl_find(table, buffer);
    if (!z)
        return false;

    /* y = the node physically spliced out: z itself when z has < 2 children,
     * else z's in-order successor (which has no left child). */
    if (z->LeftChild && z->RightChild)
        y = avl_min(z->RightChild);
    else
        y = z;

    x = y->LeftChild ? y->LeftChild : y->RightChild;   /* y's only child or NULL */
    p = y->Parent;

    /* Splice y out, putting x in its place under p (or as root). */
    if (x)
        x->Parent = p;
    if (!p)
        table->Root = x;
    else if (p->LeftChild == y)
        p->LeftChild = x;
    else
        p->RightChild = x;

    /* The shrink begins at p (the parent of the spliced node). */
    rebalance_from = p;

    /* Two-children case: relink y into z's slot (pointer surgery, no element
     * copy -- correct for any element size). */
    if (y != z) {
        y->Parent = z->Parent;
        y->LeftChild = z->LeftChild;
        y->RightChild = z->RightChild;
        y->Height = z->Height;
        if (z->LeftChild)
            z->LeftChild->Parent = y;
        if (z->RightChild)
            z->RightChild->Parent = y;
        if (!z->Parent)
            table->Root = y;
        else if (z->Parent->LeftChild == z)
            z->Parent->LeftChild = y;
        else
            z->Parent->RightChild = y;
        /* If p was z (y was z's direct right child), the shrink point moved to
         * y, which now occupies z's slot. */
        if (rebalance_from == z)
            rebalance_from = y;
    }

    table->FreeRoutine(table, z);

    /* Retrace to the root: deletion can require rotations at multiple levels, so
     * unlike insert we do NOT stop after the first. */
    while (rebalance_from) {
        node_t *parent = rebalance_from->Parent;
        node_t *new_sub = avl_rebalance(rebalance_from);
        if (new_sub != rebalance_from) {
            if (!parent)
                table->Root = new_sub;
            else if (parent->LeftChild == rebalance_from)
                parent->LeftChild = new_sub;
            else
                parent->RightChild = new_sub;
            new_sub->Parent = parent;
        }
        rebalance_from = parent;
    }

    table->NumberOfElements--;
    table->EnumNext = (node_t *)0;   /* invalidate any in-flight enumeration */
    return true;
}

/* In-order successor via parent pointers. */
static node_t *avl_successor(node_t *n)
{
    if (n->RightChild)
        return avl_min(n->RightChild);
    while (n->Parent && n->Parent->RightChild == n)
        n = n->Parent;
    return n->Parent;
}

void *RtlEnumerateGenericTableAvl(RTL_AVL_TABLE *table, bool restart)
{
    node_t *cur;
    if (!table)
        return (void *)0;
    if (restart)
        table->EnumNext = avl_min(table->Root);
    cur = table->EnumNext;
    if (!cur)
        return (void *)0;
    table->EnumNext = avl_successor(cur);
    return avl_body(cur);
}

uint32_t RtlNumberGenericTableElementsAvl(const RTL_AVL_TABLE *table)
{
    return table ? table->NumberOfElements : 0;
}

bool RtlIsGenericTableEmptyAvl(const RTL_AVL_TABLE *table)
{
    return !table || table->NumberOfElements == 0;
}
