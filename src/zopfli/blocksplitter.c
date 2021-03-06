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

#include "blocksplitter.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "deflate.h"
#include "lz77.h"
#include "squeeze.h"
#include "tree.h"
#include "util.h"

/*
The "f" for the FindMinimum function below.
i: the current parameter of f(i)
context: for your implementation
*/
typedef size_t FindMinimumFun(size_t i, void* context);

/*
Finds minimum of function f(i) where is is of type size_t, f(i) is of type
size_t, i is in range start-end (excluding end).
*/
static size_t FindMinimum(FindMinimumFun f, void* context,
                          size_t start, size_t end) {
  if (end - start < 1024) {
    size_t best = -1;
    size_t result = start;
    size_t i;
    for (i = start; i < end; i++) {
      size_t v = f(i, context);
      if (v < best) {
        best = v;
        result = i;
      }
    }
    return result;
  } else {
    /* Try to find minimum faster by recursively checking multiple points. */
#define NUM 9  /* Good value: 9. */
    size_t i;
    size_t p[NUM];
    size_t vp[NUM];
    size_t besti;
    size_t best;
    size_t lastbest = -1;
    size_t pos = start;

    for (;;) {
      if (end - start <= NUM) break;

      for (i = 0; i < NUM; i++) {
        p[i] = start + (i + 1) * ((end - start) / (NUM + 1));
        vp[i] = f(p[i], context);
      }
      besti = 0;
      best = vp[0];
      for (i = 1; i < NUM; i++) {
        if (vp[i] < best) {
          best = vp[i];
          besti = i;
        }
      }
      if (best > lastbest) break;

      start = besti == 0 ? start : p[besti - 1];
      end = besti == NUM - 1 ? end : p[besti + 1];

      pos = p[besti];
      lastbest = best;
    }
    return pos;
#undef NUM
  }
}

/*
Returns estimated cost of a block in bits.  It includes the size to encode the
tree and the size to encode all literal, length and distance symbols and their
extra bits.

litlens: lz77 lit/lengths
dists: ll77 distances
lstart: start of block
lend: end of block (not inclusive)
*/
static size_t EstimateCost(const unsigned short* litlens,
                           const unsigned short* dists,
                           size_t lstart, size_t lend) {
  return ZopfliCalculateBlockSize(litlens, dists, lstart, lend, 2);
}

typedef struct SplitCostContext {
  const unsigned short* litlens;
  const unsigned short* dists;
  size_t llsize;
  size_t start;
  size_t end;
} SplitCostContext;


/*
Gets the cost which is the sum of the cost of the left and the right section
of the data.
type: FindMinimumFun
*/
static size_t SplitCost(size_t i, void* context) {
  SplitCostContext* c = (SplitCostContext*)context;
  return EstimateCost(c->litlens, c->dists, c->start, i) +
      EstimateCost(c->litlens, c->dists, i, c->end);
}

#ifdef _MSC_VER
__forceinline
#endif
static void AddSorted(size_t value, size_t** out, size_t* outsize) {
  size_t i;
  size_t *_out, _outsize;
  size_t val;
  _out = *out;
  _outsize = *outsize;
  if (!(_outsize & (_outsize - 1))) {
    /*double alloc size if it's a power of two*/
    void *temp = _outsize == 0 ? realloc(NULL, sizeof(size_t))
                               : realloc(_out, _outsize * 2 * sizeof(size_t));
	_out = (size_t*) temp;
  }
  *out = _out;
  *outsize = _outsize + 1;
  for (i = 0; i < _outsize; i++) {
    if (_out[i] > value) break;
  }
  val = value;
  for (; i < _outsize + 1; i++) {
    size_t v;
    v = _out[i];
    _out[i] = val;
    val = v;
  }
}

/*
Prints the block split points as decimal and hex values in the terminal.
*/
static void PrintBlockSplitPoints(const unsigned short* litlens,
                                  const unsigned short* dists,
                                  size_t llsize, const size_t* lz77splitpoints,
                                  size_t nlz77points) {
  size_t* splitpoints = 0;
  size_t npoints = 0;
  size_t i;
  /* The input is given as lz77 indices, but we want to see the uncompressed
  index values. */
  size_t pos = 0;
  if (nlz77points > 0) {
    for (i = 0; i < llsize; i++) {
      size_t length = dists[i] == 0 ? 1 : litlens[i];
      if (lz77splitpoints[npoints] == i) {
        ZOPFLI_APPEND_DATA_T(size_t, pos, splitpoints, npoints);
        if (npoints == nlz77points) break;
      }
      pos += length;
    }
  }
  assert(npoints == nlz77points);

  fprintf(stderr, "block split points: ");
  for (i = 0; i < npoints; i++) {
    fprintf(stderr, "%d ", (int)splitpoints[i]);
  }
  fprintf(stderr, "(hex:");
  for (i = 0; i < npoints; i++) {
    fprintf(stderr, " %x", (int)splitpoints[i]);
  }
  fprintf(stderr, ")\n");

  free(splitpoints);
}

/*
Finds next block to try to split, the largest of the available ones.
The largest is chosen to make sure that if only a limited amount of blocks is
requested, their sizes are spread evenly.
llsize: the size of the LL77 data, which is the size of the done array here.
done: array indicating which blocks starting at that position are no longer
    splittable (splitting them increases rather than decreases cost).
splitpoints: the splitpoints found so far.
npoints: the amount of splitpoints found so far.
lstart: output variable, giving start of block.
lend: output variable, giving end of block.
returns 1 if a block was found, 0 if no block found (all are done).
*/
static int FindLargestSplittableBlock(
    size_t llsize, const unsigned char* done,
    const size_t* splitpoints, size_t npoints,
    size_t* lstart, size_t* lend) {
  size_t longest = 0;
  int found = 0;
  size_t i;
  for (i = 0; i <= npoints; i++) {
    size_t start = i == 0 ? 0 : splitpoints[i - 1];
    size_t end = i == npoints ? llsize - 1 : splitpoints[i];
    if (!done[start] && end - start > longest) {
      *lstart = start;
      *lend = end;
      found = 1;
      longest = end - start;
    }
  }
  return found;
}

void ZopfliBlockSplitLZ77(const ZopfliOptions* options,
                          const unsigned short* litlens,
                          const unsigned short* dists,
                          size_t llsize, size_t maxblocks,
                          size_t** splitpoints, size_t* npoints) {
  size_t lstart, lend;
  size_t i;
  size_t llpos = 0;
  size_t numblocks = 1;
  unsigned char* done;
  size_t splitcost, origcost;

  if (llsize < 10) return;  /* This code fails on tiny files. */

  done = (unsigned char*)malloc(llsize);
  if (!done) exit(-1); /* Allocation failed. */
  for (i = 0; i < llsize; i++) done[i] = 0;

  lstart = 0;
  lend = llsize;
  for (;;) {
    SplitCostContext c;

    if (maxblocks > 0 && numblocks >= maxblocks) {
      break;
    }

    c.litlens = litlens;
    c.dists = dists;
    c.llsize = llsize;
    c.start = lstart;
    c.end = lend;
    assert(lstart < lend);
    llpos = FindMinimum(SplitCost, &c, lstart + 1, lend);

    assert(llpos > lstart);
    assert(llpos < lend);

    splitcost = EstimateCost(litlens, dists, lstart, llpos) +
        EstimateCost(litlens, dists, llpos, lend);
    origcost = EstimateCost(litlens, dists, lstart, lend);

    if (splitcost > origcost || llpos == lstart + 1 || llpos == lend) {
      done[lstart] = 1;
    } else {
      AddSorted(llpos, splitpoints, npoints);
      numblocks++;
    }

    if (!FindLargestSplittableBlock(
        llsize, done, *splitpoints, *npoints, &lstart, &lend)) {
      break;  /* No further split will probably reduce compression. */
    }

    if (lend - lstart < 10) {
      break;
    }
  }

  if (options->verbose) {
    PrintBlockSplitPoints(litlens, dists, llsize, *splitpoints, *npoints);
  }

  free(done);
}

void ZopfliBlockSplit(const ZopfliOptions* options,
                      const unsigned char* in, size_t instart, size_t inend,
                      size_t maxblocks, size_t** splitpoints, size_t* npoints) {
  size_t pos = 0;
  size_t i;
  ZopfliBlockState s;
  size_t* lz77splitpoints = 0;
  size_t nlz77points = 0;
  ZopfliLZ77Store store;
  size_t _npoints, *_splitpoints;

  ZopfliInitLZ77Store(&store);

  s.options = options;
  s.blockstart = instart;
  s.blockend = inend;
#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  s.lmc = 0;
#endif

  /* Unintuitively, Using a simple LZ77 method here instead of ZopfliLZ77Optimal
  results in better blocks. */
  ZopfliLZ77Greedy(&s, in, instart, inend, &store);

  ZopfliBlockSplitLZ77(options,
                       store.litlens, store.dists, store.size, maxblocks,
                       &lz77splitpoints, &nlz77points);

  _npoints = 0;
  _splitpoints = 0;
  /* Convert LZ77 positions to positions in the uncompressed input. */
  pos = instart;
  if (nlz77points > 0) {
    for (i = 0; i < store.size; i++) {
      if (lz77splitpoints[_npoints] == i) {
        ZOPFLI_APPEND_DATA_T(size_t, pos, _splitpoints, _npoints);
        if (_npoints == nlz77points) break;
      }
      pos += store.dists[i] == 0 ? 1 : store.litlens[i];
    }
  }
  *splitpoints = _splitpoints;
  *npoints = _npoints;
  assert(*npoints == nlz77points);

  free(lz77splitpoints);
  ZopfliCleanLZ77Store(&store);
}

#if 0 && defined(_MSC_VER) && !defined(DEBUG) && !defined(_DEBUG)
__declspec(naked)
void _ZopfliBlockSplitSimple(size_t instart, size_t inend, size_t blocksize,
                             size_t** splitpoints, size_t* npoints) {
__asm {
	mov ecx, [esp+4]
	mov eax, [esp+8]
	xor edx, edx
	sub eax, ecx
	jbe func_end
	push esi
	push edi
	push ebx
	mov esi, ecx
	mov edi, [esp+12+12]
	lea eax, [eax+edi-1]
	div edi
	mov ecx, [esp+12+20]
	xor ebx, ebx
	lea edx, [eax+ecx-1]
	xchg eax, ebx
	bsf edx, edx
	test ecx, ecx
	jz domalloc
	dec ecx
	mov eax, [esp+12+16]
	bt ecx, edx
	jc skiprealloc
domalloc:
	inc edx
	xor ecx, ecx
	bts ecx, edx
	push ecx
	push eax
	call dword ptr [realloc]
	add esp, 8
	mov ecx, [esp+12+20]
skiprealloc:
loopnext:
	mov [eax+ecx], esi
	add esi, edi
	lea ecx, [ecx+1]
	add ebx, -1
	jnz loopnext
	pop ebx
	pop edi
	pop esi
	mov [esp+16], eax
	mov [esp+20], ecx
func_end:
	ret
}
}
#else
void _ZopfliBlockSplitSimple(size_t instart, size_t inend, size_t blocksize,
                             size_t** splitpoints, size_t* npoints) {
  size_t i = instart;
  size_t *_splitpoints;
  size_t _npoints;

  if (i >= inend) return;
  _splitpoints = *splitpoints;
  _npoints = *npoints;

  __asm _emit 0x05 __asm _emit 0x00 __asm _emit 0x00 __asm _emit 0x00 __asm _emit 0x00
  do {
    ZOPFLI_APPEND_DATA_T(size_t, i, _splitpoints, _npoints);
    *splitpoints = _splitpoints;
    *npoints = _npoints;
    i += blocksize;
  } while(i < inend);

}
#endif
