// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin Core developers
// Copyright (c) 2016-2023 The Hush developers
// Distributed under the GPLv3 software license, see the accompanying
// file COPYING or https://www.gnu.org/licenses/gpl-3.0.en.html

#ifndef HUSH_SUPPORT_ALLOCATORS_SECURE_H
#define HUSH_SUPPORT_ALLOCATORS_SECURE_H

#include "support/pagelocker.h"
#include <string>
// Allocator that locks its contents from being paged
// out of memory and clears its contents before deletion.
template <typename T>
struct secure_allocator : public std::allocator<T> {
    // MSVC8 default copy constructor is broken
    typedef std::allocator<T> base;
    typedef typename base::size_type size_type;
    typedef typename base::difference_type difference_type;
    typedef typename base::pointer pointer;
    typedef typename base::const_pointer const_pointer;
    typedef typename base::reference reference;
    typedef typename base::const_reference const_reference;
    typedef typename base::value_type value_type;
    secure_allocator() throw() {}
    secure_allocator(const secure_allocator& a) throw() : base(a) {}
    template <typename U>
    secure_allocator(const secure_allocator<U>& a) throw() : base(a)
    {
    }
    ~secure_allocator() throw() {}
    template <typename _Other>
    struct rebind {
        typedef secure_allocator<_Other> other;
    };

    T* allocate(std::size_t n, const void* hint = 0)
    {
        T* p;
        p = std::allocator<T>::allocate(n, hint);
        if (p != NULL)
            LockedPageManager::Instance().LockRange(p, sizeof(T) * n);
        return p;
    }

    void deallocate(T* p, std::size_t n)
    {
        if (p != NULL) {
            memory_cleanse(p, sizeof(T) * n);
            LockedPageManager::Instance().UnlockRange(p, sizeof(T) * n);
        }
        std::allocator<T>::deallocate(p, n);
    }
};

// This is exactly like std::string, but with a custom allocator.
typedef std::basic_string<char, std::char_traits<char>, secure_allocator<char> > SecureString;

#endif // HUSH_SUPPORT_ALLOCATORS_SECURE_H
