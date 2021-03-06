/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include "lz77.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(__GNUC__) && !defined(_MSC_VER)
void ZopfliInitLZ77Store(ZopfliLZ77Store* store) {
  store->size = 0;
  store->litlens = 0;
  store->dists = 0;
}

void ZopfliCleanLZ77Store(ZopfliLZ77Store* store) {
  free(store->litlens);
  free(store->dists);
}
#endif

#if defined(_MSC_VER) && !defined(DEBUG) && !defined(_DEBUG)
__declspec(naked) void __fastcall
ZopfliCopyLZ77Store(const ZopfliLZ77Store* s, ZopfliLZ77Store* d) {
__asm{
	push esi
	mov esi, ecx
	push edi
	mov edi, dword ptr [free]
	push ebx
	mov eax, [edx]
	mov ecx, [edx+4]
	mov ebx, edx
	push eax
	push ecx
	call edi
	pop ecx
	call edi
	mov eax, [esi+8]
	pop edx
	lea ecx, [eax+eax]
	push ecx
	mov edi, dword ptr [malloc]
	push ecx
	call edi
	xchg eax, edi
	pop ecx
	call eax
	test edi, edi
	jz error_exit
	pop ecx
	test eax, eax
	jz error_exit
	mov edx, [esi+8]
	mov [ebx+4], eax
	mov [ebx+8], edx
	mov [ebx+0], edi /*__emit 0x89 __asm __emit 0x7B __asm __emit 0x00*/
	test edx, edx
	jz loopend
	push ebp
	shr edx, 1
	mov ebx, [esi+4]
	mov esi, [esi]
	jnc loopnext
	mov cx, [esi+4*edx]
	mov bp, [ebx+4*edx]
	mov [edi+4*edx], cx
	mov [eax+4*edx], bp
loopnext:
	dec edx
	mov ecx, [esi+4*edx]
	mov ebp, [ebx+4*edx]
	mov [edi+4*edx], ecx
	mov [eax+4*edx], ebp
	jnz loopnext
	pop ebp
loopend:
	pop ebx
	pop edi
	pop esi
	ret
error_exit:
	push -1
	call dword ptr[exit]
}
}
#else
void ZopfliCopyLZ77Store(
    const ZopfliLZ77Store* source, ZopfliLZ77Store* dest) {
  size_t i;
  ZopfliCleanLZ77Store(dest);
  dest->litlens =
      (unsigned short*)malloc(sizeof(*dest->litlens) * source->size);
  dest->dists = (unsigned short*)malloc(sizeof(*dest->dists) * source->size);

  if (!dest->litlens || !dest->dists) exit(-1); /* Allocation failed. */

  dest->size = source->size;
  for (i = 0; i < source->size; i++) {
    dest->litlens[i] = source->litlens[i];
    dest->dists[i] = source->dists[i];
  }
}
#endif

/*
Appends the length and distance to the LZ77 arrays of the ZopfliLZ77Store.
context must be a ZopfliLZ77Store*.
*/
void ZopfliStoreLitLenDist(unsigned short length, unsigned short dist,
                           ZopfliLZ77Store* store) {
  size_t size = store->size;
  unsigned short *litlens = store->litlens;
  unsigned short *dists = store->dists;
  if (!(size & (size-1))) {
    litlens = (unsigned short *)(size == 0 ? realloc(0, sizeof(unsigned short))
                      : realloc(litlens, size * 2 * sizeof(unsigned short)));
	store->litlens = litlens;
    dists = (unsigned short *)(size == 0 ? realloc(0, sizeof(unsigned short))
                      : realloc(dists, size * 2 * sizeof(unsigned short)));
	store->dists = dists;
  }
  litlens[size] = length;
  dists[size] = dist;
  store->size = size + 1;
}

/*
Gets a score of the length given the distance. Typically, the score of the
length is the length itself, but if the distance is very long, decrease the
score of the length a bit to make up for the fact that long distances use large
amounts of extra bits.

This is not an accurate score, it is a heuristic only for the greedy LZ77
implementation. More accurate cost models are employed later. Making this
heuristic more accurate may hurt rather than improve compression.

The two direct uses of this heuristic are:
-avoid using a length of 3 in combination with a long distance. This only has
 an effect if length == 3.
-make a slightly better choice between the two options of the lazy matching.

Indirectly, this affects:
-the block split points if the default of block splitting first is used, in a
 rather unpredictable way
-the first zopfli run, so it affects the chance of the first run being closer
 to the optimal output
*/
static int GetLengthScore(int length, int distance) {
  /*
  At 1024, the distance uses 9+ extra bits and this seems to be the sweet spot
  on tested files.
  */
  return distance > 1024 ? length - 1 : length;
}

void ZopfliVerifyLenDist(const unsigned char* data, size_t datasize, size_t pos,
                         unsigned short dist, unsigned short length) {

  /* TODO(lode): make this only run in a debug compile, it's for assert only. */
  size_t i;

  assert(pos + length <= datasize);
  for (i = 0; i < length; i++) {
    if (data[pos - dist + i] != data[pos + i]) {
      assert(data[pos - dist + i] == data[pos + i]);
      break;
    }
  }
}

/*
Finds how long the match of scan and match is. Can be used to find how many
bytes starting from scan, and from match, are equal. Returns the last byte
after scan, which is still equal to the correspondinb byte after match.
scan is the position to compare
match is the earlier position to compare.
end is the last possible byte, beyond which to stop looking.
safe_end is a few (8) bytes before end, for comparing multiple bytes at once.
*/
static const unsigned char* GetMatch(const unsigned char* scan,
                                     const unsigned char* match,
                                     const unsigned char* end) {
  const unsigned char* safe_end;

  if (sizeof(size_t) == 8) {
    safe_end = end - 8;
    /* 8 checks at once per array bounds check (size_t is 64-bit). */
    while (scan < safe_end && *((size_t*)scan) == *((size_t*)match)) {
      scan += 8;
      match += 8;
    }
  } else if (sizeof(unsigned int) == 4) {
    safe_end = end - 4;
    /* 4 checks at once per array bounds check (unsigned int is 32-bit). */
    while (scan < safe_end
        && *((unsigned int*)scan) == *((unsigned int*)match)) {
      scan += 4;
      match += 4;
    }
  } else {
    safe_end = end - 8;
    /* do 8 checks at once per array bounds check. */
    while (scan < safe_end && *scan == *match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match) {
      scan++; match++;
    }
  }

  /* The remaining few bytes. */
  while (scan != end && *scan == *match) {
    scan++; match++;
  }

  return scan;
}

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
/*
Gets distance, length and sublen values from the cache if possible.
Returns 1 if it got the values from the cache, 0 if not.
Updates the limit value to a smaller one if possible with more limited
information from the cache.
*/
#ifdef _MSC_VER
__forceinline
#endif
static int TryGetFromLongestMatchCache(ZopfliLongestMatchCache* lmc,
    size_t lmcpos, size_t* limit,
    unsigned short* sublen, unsigned short* distance, unsigned short* length) {
  /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
  unsigned lmclength = lmc->length[lmcpos];
  unsigned lmcdist = lmc->dist[lmcpos];
  unsigned cachedlen;
  if (sublen)
    cachedlen = ZopfliMaxCachedSublen(lmc, lmcpos, lmclength);

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
#define cache_available (lmclength == 0 || lmcdist != 0)
#define limit_ok_for_cache (cache_available && \
    (*limit == ZOPFLI_MAX_MATCH || lmclength <= *limit || \
    (sublen && cachedlen >= *limit)) )

  if (limit_ok_for_cache) {
    if (!sublen || lmclength <= cachedlen) {
      unsigned len, dist;
      len = lmclength;
      if (len > *limit) len = *limit;
      *length = len;
      dist = lmcdist;
      if (sublen) {
        ZopfliCacheToSublen(lmc, lmcpos, len, sublen);
        dist = sublen[len];
        if (*limit == ZOPFLI_MAX_MATCH && len >= ZOPFLI_MIN_MATCH) {
        assert(sublen[len] == lmcdist);
        }
      }
      *distance = dist;
      return 1;
    }
    /* Can't use much of the cache, since the "sublens" need to be calculated,
       but at  least we already know when to stop. */
    *limit = lmclength;
  }
#undef cache_available
#undef limit_ok_for_cache

  return 0;
}

/*
Stores the found sublen, distance and length in the longest match cache, if
possible.
*/
static void StoreInLongestMatchCache(ZopfliLongestMatchCache* lmc,
    size_t lmcpos, size_t limit,
    const unsigned short* sublen,
    unsigned distance, unsigned length) {
  /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
  unsigned lmclength = lmc->length[lmcpos];
  unsigned lmcdist = lmc->dist[lmcpos];

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
#define cache_available (lmclength == 0 || lmcdist != 0)

  if (limit == ZOPFLI_MAX_MATCH && sublen && !cache_available) {
    assert(lmclength == 1 && lmcdist == 0);
    if (length < ZOPFLI_MIN_MATCH) {
      lmcdist = 0;
      lmclength = 0;
    } else {
      lmcdist = distance;
      lmclength = length;
    }
    assert(!(lmclength == 1 && lmcdist == 0));
    lmc->dist[lmcpos] = lmcdist;
    lmc->length[lmcpos] = lmclength;
    ZopfliSublenToCache(sublen, lmcpos, length, lmc);
  }
 
#undef cache_available
}
#endif

void ZopfliFindLongestMatch(ZopfliBlockState* s, const ZopfliHash* h,
    const unsigned char* array,
    size_t pos, size_t size, size_t limit,
    unsigned short* sublen, unsigned short* distance, unsigned short* length) {
  unsigned hpos = pos & ZOPFLI_WINDOW_MASK, p, pp;
  unsigned bestdist = 0;
  unsigned bestlength = 1;
  const unsigned char* scan;
  const unsigned char* match;
  const unsigned char* arrayend;
#if ZOPFLI_MAX_CHAIN_HITS < ZOPFLI_WINDOW_SIZE
  int chain_counter = ZOPFLI_MAX_CHAIN_HITS;  /* For quitting early. */
#endif

  unsigned dist = 0;  /* Not unsigned short on purpose. */

  int* hhead = h->head;
  unsigned short* hprev = h->prev;
  int* hhashval = h->hashval;
  int hval = h->val;

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  size_t lmcpos = pos - s->blockstart;
  ZopfliLongestMatchCache *lmc = s->lmc;
  if (lmc && TryGetFromLongestMatchCache(lmc, lmcpos, &limit, sublen, distance, length)) {
    assert(pos + *length <= size);
    return;
  }
#endif

  assert(limit <= ZOPFLI_MAX_MATCH);
  assert(limit >= ZOPFLI_MIN_MATCH);
  assert(pos < size);

  if (size - pos < ZOPFLI_MIN_MATCH) {
    /* The rest of the code assumes there are at least ZOPFLI_MIN_MATCH bytes to
       try. */
    *length = 0;
    *distance = 0;
    return;
  }

  if (pos + limit > size) {
    limit = size - pos;
  }
  arrayend = &array[pos] + limit;

  assert(hval < 65536);

  pp = hhead[hval];  /* During the whole loop, p == hprev[pp]. */
  p = hprev[pp];

  assert(pp == hpos);

  dist = p < pp ? pp - p : pp - p + ZOPFLI_WINDOW_SIZE;

  /* Go through all distances. */
  while (dist < ZOPFLI_WINDOW_SIZE) {
    unsigned currentlength = 0;

    assert(p < ZOPFLI_WINDOW_SIZE);
    assert(p == hprev[pp]);
    assert(hhashval[p] == hval);

    if (dist > 0) {
      assert(pos < size);
      assert(dist <= pos);
      scan = &array[pos];
      match = &array[pos - dist];

      /* Testing the byte at position bestlength first, goes slightly faster. */
      if (pos + bestlength >= size
          || *(scan + bestlength) == *(match + bestlength)) {

#ifdef ZOPFLI_HASH_SAME
        unsigned same0 = h->same[pos & ZOPFLI_WINDOW_MASK];
        if (same0 > 2 && *scan == *match) {
          unsigned same1 = h->same[(pos - dist) & ZOPFLI_WINDOW_MASK];
          unsigned same = same0 < same1 ? same0 : same1;
          if (same > limit) same = limit;
          scan += same;
          match += same;
        }
#endif
        scan = GetMatch(scan, match, arrayend);
        currentlength = scan - &array[pos];  /* The found length. */
      }

      if (currentlength > bestlength) {
        bestdist = dist;
        if (sublen) {
          unsigned j;
          for (j = bestlength + 1; j <= currentlength; j++) {
            sublen[j] = dist;
          }
        }
        bestlength = currentlength;
        if (currentlength >= limit) break;
      }
    }


#ifdef ZOPFLI_HASH_SAME_HASH
    /* Switch to the other hash once this will be more efficient. */
    if (hhead != h->head2 && bestlength >= h->same[hpos] &&
        h->val2 == h->hashval2[p]) {
      /* Now use the hash that encodes the length and first byte. */
      hhead = h->head2;
      hprev = h->prev2;
      hhashval = h->hashval2;
      hval = h->val2;
    }
#endif

    pp = p;
    p = hprev[p];
    if (p == pp) break;  /* Uninited prev value. */

    dist += p < pp ? pp - p : pp - p + ZOPFLI_WINDOW_SIZE;

#if ZOPFLI_MAX_CHAIN_HITS < ZOPFLI_WINDOW_SIZE
    chain_counter--;
    if (chain_counter <= 0) break;
#endif
  }

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  if (lmc) StoreInLongestMatchCache(lmc, lmcpos, limit, sublen, bestdist, bestlength);
#endif

  assert(bestlength <= limit);

  *distance = bestdist;
  *length = bestlength;
  assert(pos + *length <= size);
}

void ZopfliLZ77Greedy(ZopfliBlockState* s, const unsigned char* in,
                      size_t instart, size_t inend,
                      ZopfliLZ77Store* store) {
  size_t i = 0, j;
  unsigned short leng;
  unsigned short dist;
  int lengthscore;
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;
  unsigned short dummysublen[259];

  ZopfliHash hash;
  ZopfliHash* h = &hash;

#ifdef ZOPFLI_LAZY_MATCHING
  /* Lazy matching. */
  unsigned prev_length = 0;
  unsigned prev_match = 0;
  int prevlengthscore;
  int match_available = 0;
#endif

  if (instart == inend) return;

  ZopfliInitHash(ZOPFLI_WINDOW_SIZE, h);
  ZopfliWarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    ZopfliUpdateHash(in, i, inend, h);
  }

  for (i = instart; i < inend; i++) {
    ZopfliUpdateHash(in, i, inend, h);

    ZopfliFindLongestMatch(s, h, in, i, inend, ZOPFLI_MAX_MATCH, dummysublen,
                           &dist, &leng);
    lengthscore = GetLengthScore(leng, dist);

#ifdef ZOPFLI_LAZY_MATCHING
    /* Lazy matching. */
    prevlengthscore = GetLengthScore(prev_length, prev_match);
    if (match_available) {
      match_available = 0;
      if (lengthscore > prevlengthscore + 1) {
        ZopfliStoreLitLenDist(in[i - 1], 0, store);
        if (lengthscore >= ZOPFLI_MIN_MATCH && leng < ZOPFLI_MAX_MATCH) {
          match_available = 1;
          prev_length = leng;
          prev_match = dist;
          continue;
        }
      } else {
        /* Add previous to output. */
        leng = prev_length;
        dist = prev_match;
        lengthscore = prevlengthscore;
        /* Add to output. */
        ZopfliVerifyLenDist(in, inend, i - 1, dist, leng);
        ZopfliStoreLitLenDist(leng, dist, store);
        for (j = 2; j < leng; j++) {
          assert(i < inend);
          i++;
          ZopfliUpdateHash(in, i, inend, h);
        }
        continue;
      }
    }
    else if (lengthscore >= ZOPFLI_MIN_MATCH && leng < ZOPFLI_MAX_MATCH) {
      match_available = 1;
      prev_length = leng;
      prev_match = dist;
      continue;
    }
    /* End of lazy matching. */
#endif

    /* Add to output. */
    if (lengthscore >= ZOPFLI_MIN_MATCH) {
      ZopfliVerifyLenDist(in, inend, i, dist, leng);
      ZopfliStoreLitLenDist(leng, dist, store);
    } else {
      leng = 1;
      ZopfliStoreLitLenDist(in[i], 0, store);
    }
    for (j = 1; j < leng; j++) {
      assert(i < inend);
      i++;
      ZopfliUpdateHash(in, i, inend, h);
    }
  }

  ZopfliCleanHash(h);
}

void ZopfliLZ77Counts(const unsigned short* litlens,
                      const unsigned short* dists,
                      size_t start, size_t end,
                      size_t* ll_count, size_t* d_count) {
  size_t i;

  for (i = 0; i < 288; i++) {
    ll_count[i] = 0;
  }
  for (i = 0; i < 32; i++) {
    d_count[i] = 0;
  }

  for (i = start; i < end; i++) {
    if (dists[i] == 0) {
      ll_count[litlens[i]]++;
    } else {
      ll_count[ZopfliGetLengthSymbol(litlens[i])]++;
      d_count[ZopfliGetDistSymbol(dists[i])]++;
    }
  }

  ll_count[256] = 1;  /* End symbol. */
}
