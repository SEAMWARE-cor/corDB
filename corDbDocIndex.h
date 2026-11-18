#ifndef CORDB_CORDBDOCINDEX_H_
#define CORDB_CORDBDOCINDEX_H_

//
// FILE            corDbDocIndex.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                         // CorNode



struct CorDbStore;



// -----------------------------------------------------------------------------
//
// The document store's id index - one per collection, as corDbIndex.c is for the entities: a document id
// maps to the document's PREDECESSOR in the collection (the collection's array for the first), so that a
// lookup, an append and an unlink are all O(1). Built on a collection's first use, from its list.
//
// ⚠️ Nothing else may link or unlink a document of a collection: these functions are the only ways. A
// document's CONTENT is replaced in place (corDbDocIndexSwap) - its node stays, so its entry stays.
//
// The caller holds the store's lock: a read lock for a lookup of an indexed collection, else the write lock
// (a lookup may have to build the index - corDbDocIndexLookup's 'build').
//
typedef struct CorDbDocIndex
{
  CorNode*               collP;                       // the collection (its array node in the store's "docs")
  struct CorHashTable*   table;                       // id -> the document's predecessor
  struct CorHashTable*   old;                         // a growth in progress: the table before, being moved
  int                    oldMoved;                    // how many of old's slots have been moved
  int                    slots;
  int                    count;
  struct CorDbDocIndex*  next;
} CorDbDocIndex;



// -----------------------------------------------------------------------------
//
// corDbDocIndexLookup - the document of a collection with this id, NULL if none
//
// 'build': make the collection's index if it has none (needs the write lock). Without it, a collection not
// yet indexed is walked - a read under a read lock never changes the store.
//
extern CorNode* corDbDocIndexLookup(struct CorDbStore* storeP, CorNode* collP, const char* docId, bool build);



// -----------------------------------------------------------------------------
//
// corDbDocIndexAppend - a document (its "id" its first member) linked at the end of a collection
//
extern void corDbDocIndexAppend(struct CorDbStore* storeP, CorNode* collP, CorNode* docP);



// -----------------------------------------------------------------------------
//
// corDbDocIndexUnlink - a document taken out of its collection (not freed)
//
extern void corDbDocIndexUnlink(struct CorDbStore* storeP, CorNode* collP, CorNode* docP);



// -----------------------------------------------------------------------------
//
// corDbDocIndexSwap - the stored document's content replaced by newP's, in place; newP freed
//
extern void corDbDocIndexSwap(CorNode* storedP, CorNode* newP);



// -----------------------------------------------------------------------------
//
// corDbDocIndexFree - every collection's index of a store released (the store emptied or freed)
//
extern void corDbDocIndexFree(struct CorDbStore* storeP);

#endif  // CORDB_CORDBDOCINDEX_H_
