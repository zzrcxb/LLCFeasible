#include "cache/evchain.h"

evchain *evchain_build(u8 *addrs[], size_t size) {
    size_t i;
    evchain *ptr, *next, *prev;

    if (size == 0) {
        return NULL;
    }

    for (i = 0; i < size; i++) {
        ptr = (evchain *)addrs[i];
        next = (evchain *)addrs[(i + 1) % size];
        if (i > 0) prev = (evchain *)addrs[i - 1];
        else prev = (evchain *)addrs[size - 1];

        ptr->next = next;
        ptr->prev = prev;
    }
    return (evchain *)addrs[0];
}

evchain *evchain_stride(u8 *start, ptrdiff_t stride, size_t size) {
    size_t i = 0;
    evchain *head = (evchain *)start, *cur = head, *next;
    if (size == 0 || stride < sizeof(evchain)) {
        return NULL;
    }

    for (i = 1; i < size; i++) {
        next = (evchain *)((u8 *)cur + stride);
        cur->next = next;
        next->prev = cur;
        cur = next;
    }

    cur->next = head;
    head->prev = cur;
    return head;
}
