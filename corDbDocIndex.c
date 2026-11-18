//
// FILE            corDbDocIndex.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp

#include "corHash/corHash.h"                          // CorHashTable, corHash*
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeFree.h"                      // corTreeFree
#include "corTree/corTreeBuilder.h"                   // corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corDB/corDbStore.h"                         // CorDbStore
#include "corDB/corDbDocIndex.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// Why - a Service Execution is a document (the broker's "serviceExecutions" collection) kept 24 hours after it
// ended, and it is written three or four times (created, executing + progress, ended). Found by a walk of the
// list, every write cost O(documents): 2 840 executions/s over 3 s, 1 710/s averaged over 20 s (2026-11-18).
//
// Sizes and growth as corDbIndex.c's for the entities - see there why the move is a few slots per write and
// never all at once.
//
#define DOC_INDEX_SLOTS   256
#define DOC_INDEX_LOAD      4
#define DOC_INDEX_GROWTH    8
#define DOC_INDEX_MOVE     32



// -----------------------------------------------------------------------------
//
// docAfter - the document after a predecessor: the collection's (an array) first, a document's next
//
static inline CorNode* docAfter(CorNode* prevP)
{
  return (prevP->type == CorArray) ? prevP->value.head : prevP->next;
}



// -----------------------------------------------------------------------------
//
// docId - a document's id: its first member, as corDbDoc.c stores it (else looked up)
//
static const char* docId(CorNode* docP)
{
  if (docP == NULL)
    return NULL;

  CorNode* idP = docP->value.head;

  if ((idP == NULL) || (idP->name == NULL) || (strcmp(idP->name, "id") != 0))
    idP = corTreeLookup(docP, "id");

  return ((idP != NULL) && (idP->type == CorString)) ? idP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// idHash / idCompare - djb2; is the document after this predecessor the one with this id?
//
// As in corDbIndex.c, the table keeps no key: the compare reads the id of the predecessor's successor, so
// the list must agree with the table whenever a lookup or a remove runs - the unlink below orders its steps
// for that.
//
static unsigned int idHash(const char* name)
{
  unsigned int hash = 5381;

  while (*name != '\0')
  {
    hash = ((hash << 5) + hash) + (unsigned char) *name;
    name++;
  }

  return hash;
}

static int idCompare(const char* name, void* itemP)
{
  const char* id = docId(docAfter((CorNode*) itemP));

  return (id != NULL) ? strcmp(name, id) : 1;
}



// -----------------------------------------------------------------------------
//
// migrate - up to 'slots' of the old table's slots relinked into the current one (all: -1)
//
static void migrate(CorDbDocIndex* ixP, int slots)
{
  CorHashTable* oldP = ixP->old;

  if (oldP == NULL)
    return;

  while ((slots != 0) && (ixP->oldMoved < oldP->arraySize))
  {
    CorHashListItem* itemP = oldP->array[ixP->oldMoved];

    oldP->array[ixP->oldMoved] = NULL;

    while (itemP != NULL)
    {
      CorHashListItem* nextP = itemP->next;
      unsigned int     slot  = itemP->hashCode & ixP->table->mask;

      itemP->next              = ixP->table->array[slot];
      ixP->table->array[slot]  = itemP;
      itemP                    = nextP;
    }

    ixP->oldMoved += 1;
    slots         -= 1;
  }

  if (ixP->oldMoved >= oldP->arraySize)
  {
    corHashRelease(oldP);                            // every slot empty: the arrays only
    ixP->old      = NULL;
    ixP->oldMoved = 0;
  }
}



// -----------------------------------------------------------------------------
//
// grow - when the count passes LOAD per slot, a table GROWTH times bigger takes over
//
static void grow(CorDbDocIndex* ixP)
{
  if (ixP->count <= ixP->slots * DOC_INDEX_LOAD)
    return;

  migrate(ixP, -1);                                   // a growth before the last one's move finished

  CorHashTable* newP = corHashTableCreate(NULL, idHash, idCompare, ixP->slots * DOC_INDEX_GROWTH);

  if (newP == NULL)
    return;                                           // keep the current one; slower, not wrong

  ixP->old      = ixP->table;
  ixP->oldMoved = 0;
  ixP->table    = newP;
  ixP->slots   *= DOC_INDEX_GROWTH;
}



// -----------------------------------------------------------------------------
//
// build - a collection's index, from its list, sized for it
//
static CorDbDocIndex* build(CorDbStore* storeP, CorNode* collP)
{
  int n     = 0;
  int slots = DOC_INDEX_SLOTS;

  for (CorNode* dP = collP->value.head; dP != NULL; dP = dP->next)
    ++n;

  while (n > slots * DOC_INDEX_LOAD)
    slots *= DOC_INDEX_GROWTH;

  CorDbDocIndex* ixP = (CorDbDocIndex*) calloc(1, sizeof(CorDbDocIndex));

  if (ixP == NULL)
    return NULL;

  ixP->table = corHashTableCreate(NULL, idHash, idCompare, slots);

  if (ixP->table == NULL)
  {
    free(ixP);
    return NULL;
  }

  ixP->collP = collP;
  ixP->slots = slots;

  CorNode* prevP = collP;                             // the first document's predecessor: the array

  for (CorNode* dP = collP->value.head; dP != NULL; prevP = dP, dP = dP->next)
  {
    const char* id = docId(dP);

    if (id != NULL)
    {
      corHashItemAdd(ixP->table, id, prevP);
      ++ixP->count;
    }
  }

  ixP->next      = storeP->docIndexes;
  storeP->docIndexes = ixP;

  return ixP;
}



// -----------------------------------------------------------------------------
//
// indexOf - a collection's index (a store has a handful of collections: a list)
//
static CorDbDocIndex* indexOf(CorDbStore* storeP, CorNode* collP, bool create)
{
  for (CorDbDocIndex* ixP = storeP->docIndexes; ixP != NULL; ixP = ixP->next)
  {
    if (ixP->collP == collP)
      return ixP;
  }

  return (create == true) ? build(storeP, collP) : NULL;
}



// -----------------------------------------------------------------------------
//
// entryPrev / entryAdd / entryDel - one id's entry; during a growth it may still be in the old table
//
static CorNode* entryPrev(CorDbDocIndex* ixP, const char* id)
{
  CorNode* prevP = (CorNode*) corHashItemLookup(ixP->table, id);

  if ((prevP == NULL) && (ixP->old != NULL))
    prevP = (CorNode*) corHashItemLookup(ixP->old, id);

  return prevP;
}

static void entryDel(CorDbDocIndex* ixP, const char* id)
{
  if ((corHashItemRemove(ixP->table, id) != 0) && (ixP->old != NULL))
    corHashItemRemove(ixP->old, id);
}

static void entryAdd(CorDbDocIndex* ixP, const char* id, CorNode* prevP)
{
  corHashItemAdd(ixP->table, id, prevP);
}



// -----------------------------------------------------------------------------
//
// corDbDocIndexLookup -
//
CorNode* corDbDocIndexLookup(CorDbStore* storeP, CorNode* collP, const char* id, bool build)
{
  if ((collP == NULL) || (id == NULL))
    return NULL;

  CorDbDocIndex* ixP = indexOf(storeP, collP, build);

  if (ixP == NULL)                                    // not indexed (yet, or out of memory): the walk
  {
    for (CorNode* dP = collP->value.head; dP != NULL; dP = dP->next)
    {
      const char* dId = docId(dP);

      if ((dId != NULL) && (strcmp(dId, id) == 0))
        return dP;
    }

    return NULL;
  }

  CorNode* prevP = entryPrev(ixP, id);

  return (prevP != NULL) ? docAfter(prevP) : NULL;
}



// -----------------------------------------------------------------------------
//
// corDbDocIndexAppend -
//
void corDbDocIndexAppend(CorDbStore* storeP, CorNode* collP, CorNode* docP)
{
  CorDbDocIndex* ixP   = indexOf(storeP, collP, true);
  CorNode*       prevP = (collP->value.head != NULL) ? collP->value.tail : collP;

  corTreeChildAdd(collP, docP);

  if (ixP == NULL)                                    // no index (out of memory): the walk finds it
    return;

  const char* id = docId(docP);

  if (id == NULL)
    return;

  entryAdd(ixP, id, prevP);
  ++ixP->count;

  migrate(ixP, DOC_INDEX_MOVE);
  grow(ixP);
}



// -----------------------------------------------------------------------------
//
// corDbDocIndexUnlink -
//
// Both entries out BEFORE the list changes (each compare reads the list as the entry was made), the
// successor's back in after, naming the unlinked document's predecessor.
//
void corDbDocIndexUnlink(CorDbStore* storeP, CorNode* collP, CorNode* docP)
{
  CorDbDocIndex* ixP   = indexOf(storeP, collP, true);
  const char*    id    = docId(docP);
  CorNode*       succP = docP->next;
  const char*    sId   = docId(succP);
  CorNode*       prevP = NULL;

  if ((ixP != NULL) && (id != NULL))
  {
    prevP = entryPrev(ixP, id);

    if ((prevP != NULL) && (docAfter(prevP) != docP))  // the table disagrees with the list: not trusted
      prevP = NULL;
  }

  if (prevP == NULL)                                  // the walk
  {
    prevP = collP;

    for (CorNode* dP = collP->value.head; (dP != NULL) && (dP != docP); dP = dP->next)
      prevP = dP;

    if (docAfter(prevP) != docP)
      return;                                         // not in this collection
  }

  if (ixP != NULL)
  {
    if (id != NULL)
    {
      entryDel(ixP, id);
      --ixP->count;
    }

    if (sId != NULL)
      entryDel(ixP, sId);
  }

  if (prevP == collP)
    collP->value.head = succP;
  else
    prevP->next = succP;

  if (collP->value.tail == docP)
    collP->value.tail = (prevP == collP) ? NULL : prevP;

  docP->next = NULL;

  if ((ixP != NULL) && (sId != NULL))
    entryAdd(ixP, sId, prevP);

  if (ixP != NULL)
    migrate(ixP, DOC_INDEX_MOVE);
}



// -----------------------------------------------------------------------------
//
// corDbDocIndexSwap -
//
// The node stays where it is - its entry and its successor's stay right - and gets newP's members; newP
// leaves with the old ones. A document's "id" never changes, so the key the entry hashes does not either.
//
void corDbDocIndexSwap(CorNode* storedP, CorNode* newP)
{
  CorValue v = storedP->value;

  storedP->value = newP->value;
  newP->value    = v;

  corTreeFree(newP);
}



// -----------------------------------------------------------------------------
//
// corDbDocIndexFree -
//
void corDbDocIndexFree(CorDbStore* storeP)
{
  CorDbDocIndex* ixP = storeP->docIndexes;

  while (ixP != NULL)
  {
    CorDbDocIndex* nextP = ixP->next;

    corHashRelease(ixP->table);

    if (ixP->old != NULL)
      corHashRelease(ixP->old);

    free(ixP);
    ixP = nextP;
  }

  storeP->docIndexes = NULL;
}
